/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "tests/Test.h"

#include "include/core/SkBitmap.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/Recording.h"
#include "src/gpu/graphite/Buffer.h"
#include "src/gpu/graphite/Caps.h"
#include "src/gpu/graphite/ComputePipelineDesc.h"
#include "src/gpu/graphite/ComputeTask.h"
#include "src/gpu/graphite/ComputeTypes.h"
#include "src/gpu/graphite/ContextPriv.h"
#include "src/gpu/graphite/DrawParams.h"
#include "src/gpu/graphite/RecorderPriv.h"
#include "src/gpu/graphite/ResourceProvider.h"
#include "src/gpu/graphite/SynchronizeToCpuTask.h"
#include "src/gpu/graphite/UploadTask.h"
#include "src/gpu/graphite/compute/ComputeStep.h"
#include "src/gpu/graphite/compute/DispatchGroup.h"

using namespace skgpu::graphite;

namespace {

static const Transform kTestTransform = Transform::Identity();
static DrawParams fake_draw_params_for_testing() {
    return DrawParams(kTestTransform, {}, {}, DrawOrder({}), nullptr);
}

void* map_bind_buffer(const BindBufferInfo& info) {
    SkASSERT(info.fBuffer);
    auto buffer = sk_ref_sp(info.fBuffer);
    uint8_t* ptr = static_cast<uint8_t*>(buffer->map());
    SkASSERT(ptr);

    return ptr + info.fOffset;
}

}  // namespace

// TODO(b/262427430, b/262429132): Enable this test on other backends once they all support
// compute programs.
DEF_GRAPHITE_TEST_FOR_METAL_CONTEXT(Compute_SingleDispatchTest, reporter, context) {
    constexpr uint32_t kProblemSize = 512;
    constexpr float kFactor = 4.f;

    std::unique_ptr<Recorder> recorder = context->makeRecorder();

    class TestComputeStep : public ComputeStep {
    public:
        TestComputeStep() : ComputeStep(
                /*name=*/"TestArrayMultiply",
                /*localDispatchSize=*/{kProblemSize, 1, 1},
                /*resources=*/{
                    // Input buffer:
                    {
                        /*type=*/ResourceType::kStorageBuffer,
                        /*flow=*/DataFlow::kPrivate,
                        /*policy=*/ResourcePolicy::kMapped,
                    },
                    // Output buffer:
                    {
                        /*type=*/ResourceType::kStorageBuffer,
                        /*flow=*/DataFlow::kShared,  // shared to allow us to access it from the
                                                     // Builder
                        /*policy=*/ResourcePolicy::kMapped,  // mappable for read-back
                        /*slot=*/0,
                    }
                }) {}
        ~TestComputeStep() override = default;

        // A kernel that multiplies a large array of floats by a supplied factor.
        std::string computeSkSL(const ResourceBindingRequirements&, int) const override {
            return R"(
                layout(set=0, binding=0) readonly buffer inputBlock
                {
                    float factor;
                    float in_data[];
                };
                layout(set=0, binding=1) buffer outputBlock
                {
                    float out_data[];
                };
                void main() {
                    out_data[sk_GlobalInvocationID.x] = in_data[sk_GlobalInvocationID.x] * factor;
                }
            )";
        }

        size_t calculateBufferSize(const DrawParams&,
                                   int index,
                                   const ResourceDesc& r) const override {
            if (index == 0) {
                SkASSERT(r.fFlow == DataFlow::kPrivate);
                return sizeof(float) * (kProblemSize + 1);
            }
            SkASSERT(index == 1);
            SkASSERT(r.fSlot == 0);
            SkASSERT(r.fFlow == DataFlow::kShared);
            return sizeof(float) * kProblemSize;
        }

        void prepareBuffer(const DrawParams&,
                           int ssboIndex,
                           int resourceIndex,
                           const ResourceDesc& r,
                           void* buffer,
                           size_t bufferSize) const override {
            // Only initialize the input buffer.
            if (resourceIndex != 0) {
                return;
            }
            SkASSERT(r.fFlow == DataFlow::kPrivate);

            size_t dataCount = sizeof(float) * (kProblemSize + 1);
            SkASSERT(bufferSize == dataCount);
            SkSpan<float> inData(static_cast<float*>(buffer), dataCount);
            inData[0] = kFactor;
            for (unsigned int i = 0; i < kProblemSize; ++i) {
                inData[i + 1] = i + 1;
            }
        }
    } step;

    DispatchGroup::Builder builder(recorder.get());
    if (!builder.appendStep(&step, fake_draw_params_for_testing(), 0)) {
        ERRORF(reporter, "Failed to add ComputeStep to DispatchGroup");
        return;
    }

    // The output buffer should have been placed in the right output slot.
    BindBufferInfo outputInfo = builder.getSharedBufferResource(0);
    if (!outputInfo) {
        ERRORF(reporter, "Failed to allocate an output buffer at slot 0");
        return;
    }

    // Record the compute task
    ComputeTask::DispatchGroupList groups;
    groups.push_back(builder.finalize());
    recorder->priv().add(ComputeTask::Make(std::move(groups)));

    // Ensure the output buffer is synchronized to the CPU once the GPU submission has finished.
    recorder->priv().add(SynchronizeToCpuTask::Make(sk_ref_sp(outputInfo.fBuffer)));

    // Submit the work and wait for it to complete.
    std::unique_ptr<Recording> recording = recorder->snap();
    if (!recording) {
        ERRORF(reporter, "Failed to make recording");
        return;
    }

    InsertRecordingInfo insertInfo;
    insertInfo.fRecording = recording.get();
    context->insertRecording(insertInfo);
    context->submit(SyncToCpu::kYes);

    // Verify the contents of the output buffer.
    float* outData = static_cast<float*>(map_bind_buffer(outputInfo));
    SkASSERT(outputInfo.fBuffer->isMapped() && outData != nullptr);
    for (unsigned int i = 0; i < kProblemSize; ++i) {
        const float expected = (i + 1) * kFactor;
        const float found = outData[i];
        REPORTER_ASSERT(reporter, expected == found, "expected '%f', found '%f'", expected, found);
    }
}

// TODO(b/262427430, b/262429132): Enable this test on other backends once they all support
// compute programs.
DEF_GRAPHITE_TEST_FOR_METAL_CONTEXT(Compute_DispatchGroupTest, reporter, context) {
    constexpr uint32_t kProblemSize = 512;
    constexpr float kFactor1 = 4.f;
    constexpr float kFactor2 = 3.f;

    std::unique_ptr<Recorder> recorder = context->makeRecorder();

    // Define two steps that perform two multiplication passes over the same input.

    class TestComputeStep1 : public ComputeStep {
    public:
        TestComputeStep1() : ComputeStep(
                /*name=*/"TestArrayMultiplyFirstPass",
                /*localDispatchSize=*/{kProblemSize, 1, 1},
                /*resources=*/{
                    // Input buffer:
                    {
                        /*type=*/ResourceType::kStorageBuffer,
                        /*flow=*/DataFlow::kPrivate,
                        /*policy=*/ResourcePolicy::kMapped,  // mappable for read-back
                    },
                    // Output buffers:
                    {
                        /*type=*/ResourceType::kStorageBuffer,
                        /*flow=*/DataFlow::kShared,
                        /*policy=*/ResourcePolicy::kNone,  // GPU-only, read by second step
                        /*slot=*/0,
                    },
                    {
                        /*type=*/ResourceType::kStorageBuffer,
                        /*flow=*/DataFlow::kShared,
                        /*policy=*/ResourcePolicy::kMapped,  // mappable for read-back
                        /*slot=*/1,
                    }
                }) {}
        ~TestComputeStep1() override = default;

        // A kernel that multiplies a large array of floats by a supplied factor.
        std::string computeSkSL(const ResourceBindingRequirements&, int) const override {
            return R"(
                layout(set=0, binding=0) readonly buffer inputBlock
                {
                    float factor;
                    float in_data[];
                };
                layout(set=0, binding=1) buffer outputBlock1
                {
                    float forward_data[];
                };
                layout(set=0, binding=2) buffer outputBlock2
                {
                    float extra_data[2];
                };
                void main() {
                    forward_data[sk_GlobalInvocationID.x] = in_data[sk_GlobalInvocationID.x] * factor;
                    extra_data[0] = factor;
                    extra_data[1] = 2 * factor;
                }
            )";
        }

        size_t calculateBufferSize(const DrawParams&,
                                   int index,
                                   const ResourceDesc& r) const override {
            if (index == 0) {
                SkASSERT(r.fFlow == DataFlow::kPrivate);
                return sizeof(float) * (kProblemSize + 1);
            }
            if (index == 1) {
                SkASSERT(r.fFlow == DataFlow::kShared);
                SkASSERT(r.fSlot == 0);
                return sizeof(float) * kProblemSize;
            }

            SkASSERT(index == 2);
            SkASSERT(r.fSlot == 1);
            SkASSERT(r.fFlow == DataFlow::kShared);
            return 2 * sizeof(float);
        }

        void prepareBuffer(const DrawParams&,
                           int ssboIndex,
                           int resourceIndex,
                           const ResourceDesc& r,
                           void* buffer,
                           size_t bufferSize) const override {
            if (resourceIndex != 0) {
                return;
            }

            size_t dataCount = sizeof(float) * (kProblemSize + 1);
            SkASSERT(bufferSize == dataCount);
            SkSpan<float> inData(static_cast<float*>(buffer), dataCount);
            inData[0] = kFactor1;
            for (unsigned int i = 0; i < kProblemSize; ++i) {
                inData[i + 1] = i + 1;
            }
        }
    } step1;

    class TestComputeStep2 : public ComputeStep {
    public:
        TestComputeStep2() : ComputeStep(
                /*name=*/"TestArrayMultiplySecondPass",
                /*localDispatchSize=*/{kProblemSize, 1, 1},
                /*resources=*/{
                    // Input buffer:
                    {
                        /*type=*/ResourceType::kStorageBuffer,
                        /*flow=*/DataFlow::kShared,
                        /*policy=*/ResourcePolicy::kNone,  // GPU-only
                        /*slot=*/0, // this is the output from the first step
                    },
                    {
                        /*type=*/ResourceType::kStorageBuffer,
                        /*flow=*/DataFlow::kPrivate,
                        /*policy=*/ResourcePolicy::kMapped,
                    },
                    // Output buffer:
                    {
                        /*type=*/ResourceType::kStorageBuffer,
                        /*flow=*/DataFlow::kShared,
                        /*policy=*/ResourcePolicy::kMapped,  // mappable for read-back
                        /*slot=*/2,
                    }
                }) {}
        ~TestComputeStep2() override = default;

        // A kernel that multiplies a large array of floats by a supplied factor.
        std::string computeSkSL(const ResourceBindingRequirements&, int) const override {
            return R"(
                layout(set=0, binding=0) readonly buffer inputBlock
                {
                    float in_data[];
                };
                layout(set=0, binding=1) readonly buffer factorBlock
                {
                    float factor;
                };
                layout(set=0, binding=2) buffer outputBlock
                {
                    float out_data[];
                };
                void main() {
                    out_data[sk_GlobalInvocationID.x] = in_data[sk_GlobalInvocationID.x] * factor;
                }
            )";
        }

        size_t calculateBufferSize(const DrawParams&,
                                   int index,
                                   const ResourceDesc& r) const override {
            if (index == 0) {
                return sizeof(float) * kProblemSize;
            }
            if (index == 1) {
                SkASSERT(r.fFlow == DataFlow::kPrivate);
                return sizeof(float);
            }
            SkASSERT(index == 2);
            SkASSERT(r.fSlot == 2);
            SkASSERT(r.fFlow == DataFlow::kShared);
            return sizeof(float) * kProblemSize;
        }

        void prepareBuffer(const DrawParams&,
                           int ssboIndex,
                           int resourceIndex,
                           const ResourceDesc& r,
                           void* buffer,
                           size_t bufferSize) const override {
            if (resourceIndex != 1) {
                return;
            }
            SkASSERT(r.fFlow == DataFlow::kPrivate);
            *static_cast<float*>(buffer) = kFactor2;
        }
    } step2;

    DispatchGroup::Builder builder(recorder.get());
    builder.appendStep(&step1, fake_draw_params_for_testing(), 0);
    builder.appendStep(&step2, fake_draw_params_for_testing(), 0);

    // Slots 0, 1, and 2 should all contain shared buffers. Slot 1 contains the extra output buffer
    // from step 1 while slot 2 contains the result of the second multiplication pass from step 1.
    // Slot 0 is not mappable.
    REPORTER_ASSERT(reporter,
                    std::holds_alternative<BindBufferInfo>(builder.outputTable().fSharedSlots[0]),
                    "shared resource at slot 0 is missing");
    BindBufferInfo outputInfo = builder.getSharedBufferResource(2);
    if (!outputInfo) {
        ERRORF(reporter, "Failed to allocate an output buffer at slot 0");
        return;
    }

    // Extra output buffer from step 1 (corresponding to 'outputBlock2')
    BindBufferInfo extraOutputInfo = builder.getSharedBufferResource(1);
    if (!extraOutputInfo) {
        ERRORF(reporter, "shared resource at slot 1 is missing");
        return;
    }

    // Record the compute task
    ComputeTask::DispatchGroupList groups;
    groups.push_back(builder.finalize());
    recorder->priv().add(ComputeTask::Make(std::move(groups)));

    // Ensure the output buffers get synchronized to the CPU once the GPU submission has finished.
    recorder->priv().add(SynchronizeToCpuTask::Make(sk_ref_sp(outputInfo.fBuffer)));
    recorder->priv().add(SynchronizeToCpuTask::Make(sk_ref_sp(extraOutputInfo.fBuffer)));

    // Submit the work and wait for it to complete.
    std::unique_ptr<Recording> recording = recorder->snap();
    if (!recording) {
        ERRORF(reporter, "Failed to make recording");
        return;
    }

    InsertRecordingInfo insertInfo;
    insertInfo.fRecording = recording.get();
    context->insertRecording(insertInfo);
    context->submit(SyncToCpu::kYes);

    // Verify the contents of the output buffer from step 2
    float* outData = static_cast<float*>(map_bind_buffer(outputInfo));
    SkASSERT(outputInfo.fBuffer->isMapped() && outData != nullptr);
    for (unsigned int i = 0; i < kProblemSize; ++i) {
        const float expected = (i + 1) * kFactor1 * kFactor2;
        const float found = outData[i];
        REPORTER_ASSERT(reporter, expected == found, "expected '%f', found '%f'", expected, found);
    }

    // Verify the contents of the extra output buffer from step 1
    float* extraOutData = static_cast<float*>(map_bind_buffer(extraOutputInfo));
    SkASSERT(extraOutputInfo.fBuffer->isMapped() && extraOutData != nullptr);
    REPORTER_ASSERT(reporter,
                    kFactor1 == extraOutData[0],
                    "expected '%f', found '%f'",
                    kFactor1,
                    extraOutData[0]);
    REPORTER_ASSERT(reporter,
                    2 * kFactor1 == extraOutData[1],
                    "expected '%f', found '%f'",
                    2 * kFactor2,
                    extraOutData[1]);
}

// Tests the storage texture binding for a compute dispatch that writes the same color to every
// pixel of a storage texture.
DEF_GRAPHITE_TEST_FOR_METAL_CONTEXT(Compute_StorageTexture, reporter, context) {
    std::unique_ptr<Recorder> recorder = context->makeRecorder();

    // For this test we allocate a 16x16 tile which is written to by a single workgroup of the same
    // size.
    constexpr uint32_t kDim = 16;

    class TestComputeStep : public ComputeStep {
    public:
        TestComputeStep() : ComputeStep(
                /*name=*/"TestStorageTextures",
                /*localDispatchSize=*/{kDim, kDim, 1},
                /*resources=*/{
                    {
                        /*type=*/ResourceType::kStorageTexture,
                        /*flow=*/DataFlow::kShared,
                        /*policy=*/ResourcePolicy::kNone,
                        /*slot=*/0,
                    }
                }) {}
        ~TestComputeStep() override = default;

        std::string computeSkSL(const ResourceBindingRequirements&, int) const override {
            return R"(
                layout(binding = 0) writeonly texture2D dest;

                void main() {
                    write(dest, sk_LocalInvocationID.xy, half4(0.0, 1.0, 0.0, 1.0));
                }
            )";
        }

        std::tuple<SkISize, SkColorType> calculateTextureParameters(
                const DrawParams&, int index, const ResourceDesc& r) const override {
            return {{kDim, kDim}, kRGBA_8888_SkColorType};
        }

        WorkgroupSize calculateGlobalDispatchSize(const DrawParams&) const override {
            return WorkgroupSize(1, 1, 1);
        }
    } step;

    DispatchGroup::Builder builder(recorder.get());
    if (!builder.appendStep(&step, fake_draw_params_for_testing(), 0)) {
        ERRORF(reporter, "Failed to add ComputeStep to DispatchGroup");
        return;
    }

    sk_sp<TextureProxy> texture = builder.getSharedTextureResource(0);
    if (!texture) {
        ERRORF(reporter, "Shared resource at slot 0 is missing");
        return;
    }

    // Record the compute task
    ComputeTask::DispatchGroupList groups;
    groups.push_back(builder.finalize());
    recorder->priv().add(ComputeTask::Make(std::move(groups)));

    // Submit the work and wait for it to complete.
    std::unique_ptr<Recording> recording = recorder->snap();
    if (!recording) {
        ERRORF(reporter, "Failed to make recording");
        return;
    }

    InsertRecordingInfo insertInfo;
    insertInfo.fRecording = recording.get();
    context->insertRecording(insertInfo);
    context->submit(SyncToCpu::kYes);

    SkBitmap bitmap;
    SkImageInfo imgInfo =
            SkImageInfo::Make(kDim, kDim, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    bitmap.allocPixels(imgInfo);

    SkPixmap pixels;
    bool peekPixelsSuccess = bitmap.peekPixels(&pixels);
    REPORTER_ASSERT(reporter, peekPixelsSuccess);

    bool readPixelsSuccess = context->priv().readPixels(pixels, texture.get(), imgInfo, 0, 0);
    REPORTER_ASSERT(reporter, readPixelsSuccess);

    for (uint32_t x = 0; x < kDim; ++x) {
        for (uint32_t y = 0; y < kDim; ++y) {
            SkColor4f expected = SkColor4f::FromColor(SK_ColorGREEN);
            SkColor4f color = pixels.getColor4f(x, y);
            REPORTER_ASSERT(reporter, expected == color,
                            "At position {%u, %u}, "
                            "expected {%.1f, %.1f, %.1f, %.1f}, "
                            "found {%.1f, %.1f, %.1f, %.1f}",
                            x, y,
                            expected.fR, expected.fG, expected.fB, expected.fA,
                            color.fR, color.fG, color.fB, color.fA);
        }
    }
}

// Tests the readonly texture binding for a compute dispatch that random-access reads from a
// CPU-populated texture and copies it to a storage texture.
DEF_GRAPHITE_TEST_FOR_METAL_CONTEXT(Compute_SampledTexture, reporter, context) {
    std::unique_ptr<Recorder> recorder = context->makeRecorder();

    // For this test we allocate a 16x16 tile which is written to by a single workgroup of the same
    // size.
    constexpr uint32_t kDim = 16;

    class TestComputeStep : public ComputeStep {
    public:
        TestComputeStep() : ComputeStep(
                /*name=*/"TestSampledTextures",
                /*localDispatchSize=*/{kDim, kDim, 1},
                /*resources=*/{
                    {
                        /*type=*/ResourceType::kTexture,
                        /*flow=*/DataFlow::kShared,
                        /*policy=*/ResourcePolicy::kNone,
                        /*slot=*/0,
                    },
                    {
                        /*type=*/ResourceType::kStorageTexture,
                        /*flow=*/DataFlow::kShared,
                        /*policy=*/ResourcePolicy::kNone,
                        /*slot=*/1,
                    }
                }) {}
        ~TestComputeStep() override = default;

        std::string computeSkSL(const ResourceBindingRequirements&, int) const override {
            return R"(
                layout(binding = 0) readonly texture2D src;
                layout(binding = 1) writeonly texture2D dest;

                void main() {
                    half4 color = read(src, sk_LocalInvocationID.xy);
                    write(dest, sk_LocalInvocationID.xy, color);
                }
            )";
        }

        std::tuple<SkISize, SkColorType> calculateTextureParameters(
                const DrawParams&, int index, const ResourceDesc& r) const override {
            SkASSERT(index == 1);
            return {{kDim, kDim}, kRGBA_8888_SkColorType};
        }

        WorkgroupSize calculateGlobalDispatchSize(const DrawParams&) const override {
            return WorkgroupSize(1, 1, 1);
        }
    } step;

    // Create and populate an input texture.
    SkBitmap srcBitmap;
    SkImageInfo srcInfo =
            SkImageInfo::Make(kDim, kDim, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    srcBitmap.allocPixels(srcInfo);
    SkPixmap srcPixels;
    bool srcPeekPixelsSuccess = srcBitmap.peekPixels(&srcPixels);
    REPORTER_ASSERT(reporter, srcPeekPixelsSuccess);
    for (uint32_t x = 0; x < kDim; ++x) {
        for (uint32_t y = 0; y < kDim; ++y) {
            *srcPixels.writable_addr32(x, y) =
                    SkColorSetARGB(255, x * 256 / kDim, y * 256 / kDim, 0);
        }
    }

    sk_sp<TextureProxy> srcProxy = TextureProxy::Make(context->priv().caps(),
                                                      {kDim, kDim},
                                                      kRGBA_8888_SkColorType,
                                                      skgpu::Mipmapped::kNo,
                                                      skgpu::Protected::kNo,
                                                      skgpu::Renderable::kNo,
                                                      skgpu::Budgeted::kNo);
    MipLevel mipLevel;
    mipLevel.fPixels = srcPixels.addr();
    mipLevel.fRowBytes = srcPixels.rowBytes();
    UploadInstance upload = UploadInstance::Make(recorder.get(),
                                                 srcProxy,
                                                 srcPixels.info().colorInfo(),
                                                 srcPixels.info().colorInfo(),
                                                 {mipLevel},
                                                 SkIRect::MakeWH(kDim, kDim),
                                                 std::make_unique<ImageUploadContext>());
    if (!upload.isValid()) {
        ERRORF(reporter, "Could not create UploadInstance");
        return;
    }
    recorder->priv().add(UploadTask::Make(std::move(upload)));

    DispatchGroup::Builder builder(recorder.get());

    // Assign the input texture to slot 0. This corresponds to the ComputeStep's "src" texture
    // binding.
    builder.assignSharedTexture(std::move(srcProxy), 0);

    if (!builder.appendStep(&step, fake_draw_params_for_testing(), 0)) {
        ERRORF(reporter, "Failed to add ComputeStep to DispatchGroup");
        return;
    }

    sk_sp<TextureProxy> dst = builder.getSharedTextureResource(1);
    if (!dst) {
        ERRORF(reporter, "shared resource at slot 1 is missing");
        return;
    }

    // Record the compute task
    ComputeTask::DispatchGroupList groups;
    groups.push_back(builder.finalize());
    recorder->priv().add(ComputeTask::Make(std::move(groups)));

    // Submit the work and wait for it to complete.
    std::unique_ptr<Recording> recording = recorder->snap();
    if (!recording) {
        ERRORF(reporter, "Failed to make recording");
        return;
    }

    InsertRecordingInfo insertInfo;
    insertInfo.fRecording = recording.get();
    context->insertRecording(insertInfo);
    context->submit(SyncToCpu::kYes);

    SkBitmap bitmap;
    SkImageInfo imgInfo =
            SkImageInfo::Make(kDim, kDim, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    bitmap.allocPixels(imgInfo);

    SkPixmap pixels;
    bool peekPixelsSuccess = bitmap.peekPixels(&pixels);
    REPORTER_ASSERT(reporter, peekPixelsSuccess);

    bool readPixelsSuccess = context->priv().readPixels(pixels, dst.get(), imgInfo, 0, 0);
    REPORTER_ASSERT(reporter, readPixelsSuccess);

    for (uint32_t x = 0; x < kDim; ++x) {
        for (uint32_t y = 0; y < kDim; ++y) {
            SkColor4f expected = SkColor4f::FromBytes_RGBA(
                    SkColorSetARGB(255, x * 256 / kDim, y * 256 / kDim, 0));
            SkColor4f color = pixels.getColor4f(x, y);
            REPORTER_ASSERT(reporter, expected == color,
                            "At position {%u, %u}, "
                            "expected {%.1f, %.1f, %.1f, %.1f}, "
                            "found {%.1f, %.1f, %.1f, %.1f}",
                            x, y,
                            expected.fR, expected.fG, expected.fB, expected.fA,
                            color.fR, color.fG, color.fB, color.fA);
        }
    }
}

// Tests that a texture written by one compute step can be sampled by a subsequent step.
DEF_GRAPHITE_TEST_FOR_METAL_CONTEXT(Compute_StorageTextureMultipleComputeSteps, reporter, context) {
    std::unique_ptr<Recorder> recorder = context->makeRecorder();

    // For this test we allocate a 16x16 tile which is written to by a single workgroup of the same
    // size.
    constexpr uint32_t kDim = 16;

    // Writes to a texture in slot 0.
    class TestComputeStep1 : public ComputeStep {
    public:
        TestComputeStep1() : ComputeStep(
                /*name=*/"TestStorageTexturesFirstPass",
                /*localDispatchSize=*/{kDim, kDim, 1},
                /*resources=*/{
                    {
                        /*type=*/ResourceType::kStorageTexture,
                        /*flow=*/DataFlow::kShared,
                        /*policy=*/ResourcePolicy::kNone,
                        /*slot=*/0,
                    }
                }) {}
        ~TestComputeStep1() override = default;

        std::string computeSkSL(const ResourceBindingRequirements&, int) const override {
            return R"(
                layout(binding = 0) writeonly texture2D dest;

                void main() {
                    write(dest, sk_LocalInvocationID.xy, half4(0.0, 1.0, 0.0, 1.0));
                }
            )";
        }

        std::tuple<SkISize, SkColorType> calculateTextureParameters(
                const DrawParams&, int index, const ResourceDesc& r) const override {
            SkASSERT(index == 0);
            return {{kDim, kDim}, kRGBA_8888_SkColorType};
        }

        WorkgroupSize calculateGlobalDispatchSize(const DrawParams&) const override {
            return WorkgroupSize(1, 1, 1);
        }
    } step1;

    // Reads from the texture in slot 0 and writes it to another texture in slot 1.
    class TestComputeStep2 : public ComputeStep {
    public:
        TestComputeStep2() : ComputeStep(
                /*name=*/"TestStorageTexturesSecondPass",
                /*localDispatchSize=*/{kDim, kDim, 1},
                /*resources=*/{
                    {
                        /*type=*/ResourceType::kTexture,
                        /*flow=*/DataFlow::kShared,
                        /*policy=*/ResourcePolicy::kNone,
                        /*slot=*/0,
                    },
                    {
                        /*type=*/ResourceType::kStorageTexture,
                        /*flow=*/DataFlow::kShared,
                        /*policy=*/ResourcePolicy::kNone,
                        /*slot=*/1,
                    }
                }) {}
        ~TestComputeStep2() override = default;

        std::string computeSkSL(const ResourceBindingRequirements&, int) const override {
            return R"(
                layout(binding = 0) readonly texture2D src;
                layout(binding = 1) writeonly texture2D dest;

                void main() {
                    half4 color = read(src, sk_LocalInvocationID.xy);
                    write(dest, sk_LocalInvocationID.xy, color);
                }
            )";
        }

        std::tuple<SkISize, SkColorType> calculateTextureParameters(
                const DrawParams&, int index, const ResourceDesc& r) const override {
            SkASSERT(index == 1);
            return {{kDim, kDim}, kRGBA_8888_SkColorType};
        }

        WorkgroupSize calculateGlobalDispatchSize(const DrawParams&) const override {
            return WorkgroupSize(1, 1, 1);
        }
    } step2;

    DispatchGroup::Builder builder(recorder.get());
    builder.appendStep(&step1, fake_draw_params_for_testing(), 0);
    builder.appendStep(&step2, fake_draw_params_for_testing(), 0);

    sk_sp<TextureProxy> dst = builder.getSharedTextureResource(1);
    if (!dst) {
        ERRORF(reporter, "shared resource at slot 1 is missing");
        return;
    }

    // Record the compute task
    ComputeTask::DispatchGroupList groups;
    groups.push_back(builder.finalize());
    recorder->priv().add(ComputeTask::Make(std::move(groups)));

    // Submit the work and wait for it to complete.
    std::unique_ptr<Recording> recording = recorder->snap();
    if (!recording) {
        ERRORF(reporter, "Failed to make recording");
        return;
    }

    InsertRecordingInfo insertInfo;
    insertInfo.fRecording = recording.get();
    context->insertRecording(insertInfo);
    context->submit(SyncToCpu::kYes);

    SkBitmap bitmap;
    SkImageInfo imgInfo =
            SkImageInfo::Make(kDim, kDim, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    bitmap.allocPixels(imgInfo);

    SkPixmap pixels;
    bool peekPixelsSuccess = bitmap.peekPixels(&pixels);
    REPORTER_ASSERT(reporter, peekPixelsSuccess);

    bool readPixelsSuccess = context->priv().readPixels(pixels, dst.get(), imgInfo, 0, 0);
    REPORTER_ASSERT(reporter, readPixelsSuccess);

    for (uint32_t x = 0; x < kDim; ++x) {
        for (uint32_t y = 0; y < kDim; ++y) {
            SkColor4f expected = SkColor4f::FromColor(SK_ColorGREEN);
            SkColor4f color = pixels.getColor4f(x, y);
            REPORTER_ASSERT(reporter, expected == color,
                            "At position {%u, %u}, "
                            "expected {%.1f, %.1f, %.1f, %.1f}, "
                            "found {%.1f, %.1f, %.1f, %.1f}",
                            x, y,
                            expected.fR, expected.fG, expected.fB, expected.fA,
                            color.fR, color.fG, color.fB, color.fA);
        }
    }
}

// TODO(b/260622403): The shader tested here is identical to
// `resources/sksl/compute/AtomicsOperations.compute`. It would be nice to be able to exercise SkSL
// features like this as part of SkSLTest.cpp instead of as a graphite test.
// TODO(b/262427430, b/262429132): Enable this test on other backends once they all support
// compute programs.
DEF_GRAPHITE_TEST_FOR_METAL_CONTEXT(Compute_AtomicOperationsTest, reporter, context) {
    std::unique_ptr<Recorder> recorder = context->makeRecorder();

    constexpr uint32_t kWorkgroupCount = 32;
    constexpr uint32_t kWorkgroupSize = 1024;

    class TestComputeStep : public ComputeStep {
    public:
        TestComputeStep() : ComputeStep(
                /*name=*/"TestAtomicOperations",
                /*localDispatchSize=*/{kWorkgroupSize, 1, 1},
                /*resources=*/{
                    {
                        /*type=*/ResourceType::kStorageBuffer,
                        /*flow=*/DataFlow::kShared,
                        /*policy=*/ResourcePolicy::kMapped,
                        /*slot=*/0,
                    }
                }) {}
        ~TestComputeStep() override = default;

        // A kernel that increments a global (device memory) counter across multiple workgroups.
        // Each workgroup maintains its own independent tally in a workgroup-shared counter which
        // is then added to the global count.
        //
        // This exercises atomic store/load/add and coherent reads and writes over memory in storage
        // and workgroup address spaces.
        std::string computeSkSL(const ResourceBindingRequirements&, int) const override {
            return R"(
                layout(metal, binding = 0) buffer ssbo {
                    atomicUint globalCounter;
                };

                workgroup atomicUint localCounter;

                void main() {
                    // Initialize the local counter.
                    if (sk_LocalInvocationID.x == 0) {
                        atomicStore(localCounter, 0);
                    }

                    // Synchronize the threads in the workgroup so they all see the initial value.
                    workgroupBarrier();

                    // All threads increment the counter.
                    atomicAdd(localCounter, 1);

                    // Synchronize the threads again to ensure they have all executed the increment
                    // and the following load reads the same value across all threads in the
                    // workgroup.
                    workgroupBarrier();

                    // Add the workgroup-only tally to the global counter.
                    if (sk_LocalInvocationID.x == 0) {
                        atomicAdd(globalCounter, atomicLoad(localCounter));
                    }
                }
            )";
        }

        size_t calculateBufferSize(const DrawParams&,
                                   int index,
                                   const ResourceDesc& r) const override {
            SkASSERT(index == 0);
            SkASSERT(r.fSlot == 0);
            SkASSERT(r.fFlow == DataFlow::kShared);
            return sizeof(uint32_t);
        }

        WorkgroupSize calculateGlobalDispatchSize(const DrawParams&) const override {
            return WorkgroupSize(kWorkgroupCount, 1, 1);
        }

        void prepareBuffer(const DrawParams&,
                           int ssboIndex,
                           int resourceIndex,
                           const ResourceDesc& r,
                           void* buffer,
                           size_t bufferSize) const override {
            SkASSERT(resourceIndex == 0);
            *static_cast<uint32_t*>(buffer) = 0;
        }
    } step;

    DispatchGroup::Builder builder(recorder.get());
    builder.appendStep(&step, fake_draw_params_for_testing(), 0);

    BindBufferInfo info = builder.getSharedBufferResource(0);
    if (!info) {
        ERRORF(reporter, "shared resource at slot 0 is missing");
        return;
    }

    // Record the compute pass task.
    ComputeTask::DispatchGroupList groups;
    groups.push_back(builder.finalize());
    recorder->priv().add(ComputeTask::Make(std::move(groups)));

    // Ensure the output buffer is synchronized to the CPU once the GPU submission has finished.
    recorder->priv().add(SynchronizeToCpuTask::Make(sk_ref_sp(info.fBuffer)));

    // Submit the work and wait for it to complete.
    std::unique_ptr<Recording> recording = recorder->snap();
    if (!recording) {
        ERRORF(reporter, "Failed to make recording");
        return;
    }

    InsertRecordingInfo insertInfo;
    insertInfo.fRecording = recording.get();
    context->insertRecording(insertInfo);
    context->submit(SyncToCpu::kYes);

    // Verify the contents of the output buffer.
    constexpr uint32_t kExpectedCount = kWorkgroupCount * kWorkgroupSize;
    const uint32_t result = static_cast<const uint32_t*>(map_bind_buffer(info))[0];
    REPORTER_ASSERT(reporter,
                    result == kExpectedCount,
                    "expected '%d', found '%d'",
                    kExpectedCount,
                    result);
}

// TODO(b/260622403): The shader tested here is identical to
// `resources/sksl/compute/AtomicsOperationsOverArrayAndStruct.compute`. It would be nice to be able
// to exercise SkSL features like this as part of SkSLTest.cpp instead of as a graphite test.
// TODO(b/262427430, b/262429132): Enable this test on other backends once they all support
// compute programs.
DEF_GRAPHITE_TEST_FOR_METAL_CONTEXT(Compute_AtomicOperationsOverArrayAndStructTest,
                                    reporter,
                                    context) {
    std::unique_ptr<Recorder> recorder = context->makeRecorder();

    constexpr uint32_t kWorkgroupCount = 32;
    constexpr uint32_t kWorkgroupSize = 1024;

    class TestComputeStep : public ComputeStep {
    public:
        TestComputeStep() : ComputeStep(
                /*name=*/"TestAtomicOperationsOverArrayAndStruct",
                /*localDispatchSize=*/{kWorkgroupSize, 1, 1},
                /*resources=*/{
                    {
                        /*type=*/ResourceType::kStorageBuffer,
                        /*flow=*/DataFlow::kShared,
                        /*policy=*/ResourcePolicy::kMapped,
                        /*slot=*/0,
                    }
                }) {}
        ~TestComputeStep() override = default;

        // Construct a kernel that increments a two global (device memory) counters across multiple
        // workgroups. Each workgroup maintains its own independent tallies in workgroup-shared
        // counters which are then added to the global counts.
        //
        // This exercises atomic store/load/add and coherent reads and writes over memory in storage
        // and workgroup address spaces.
        std::string computeSkSL(const ResourceBindingRequirements&, int) const override {
            return R"(
                const uint WORKGROUP_SIZE = 1024;

                struct GlobalCounts {
                    atomicUint firstHalfCount;
                    atomicUint secondHalfCount;
                };
                layout(metal, binding = 0) buffer ssbo {
                    GlobalCounts globalCounts;
                };

                workgroup atomicUint localCounts[2];

                void main() {
                    // Initialize the local counts.
                    if (sk_LocalInvocationID.x == 0) {
                        atomicStore(localCounts[0], 0);
                        atomicStore(localCounts[1], 0);
                    }

                    // Synchronize the threads in the workgroup so they all see the initial value.
                    workgroupBarrier();

                    // Each thread increments one of the local counters based on its invocation
                    // index.
                    uint idx = sk_LocalInvocationID.x < (WORKGROUP_SIZE / 2) ? 0 : 1;
                    atomicAdd(localCounts[idx], 1);

                    // Synchronize the threads again to ensure they have all executed the increments
                    // and the following load reads the same value across all threads in the
                    // workgroup.
                    workgroupBarrier();

                    // Add the workgroup-only tally to the global counter.
                    if (sk_LocalInvocationID.x == 0) {
                        atomicAdd(globalCounts.firstHalfCount, atomicLoad(localCounts[0]));
                        atomicAdd(globalCounts.secondHalfCount, atomicLoad(localCounts[1]));
                    }
                }
            )";
        }

        size_t calculateBufferSize(const DrawParams&,
                                   int index,
                                   const ResourceDesc& r) const override {
            SkASSERT(index == 0);
            SkASSERT(r.fSlot == 0);
            SkASSERT(r.fFlow == DataFlow::kShared);
            return 2 * sizeof(uint32_t);
        }

        WorkgroupSize calculateGlobalDispatchSize(const DrawParams&) const override {
            return WorkgroupSize(kWorkgroupCount, 1, 1);
        }

        void prepareBuffer(const DrawParams&,
                           int ssboIndex,
                           int resourceIndex,
                           const ResourceDesc& r,
                           void* buffer,
                           size_t bufferSize) const override {
            SkASSERT(resourceIndex == 0);
            uint32_t* data = static_cast<uint32_t*>(buffer);
            data[0] = 0;
            data[1] = 0;
        }
    } step;

    DispatchGroup::Builder builder(recorder.get());
    builder.appendStep(&step, fake_draw_params_for_testing(), 0);

    BindBufferInfo info = builder.getSharedBufferResource(0);
    if (!info) {
        ERRORF(reporter, "shared resource at slot 0 is missing");
        return;
    }

    // Record the compute pass task.
    ComputeTask::DispatchGroupList groups;
    groups.push_back(builder.finalize());
    recorder->priv().add(ComputeTask::Make(std::move(groups)));

    // Ensure the output buffer is synchronized to the CPU once the GPU submission has finished.
    recorder->priv().add(SynchronizeToCpuTask::Make(sk_ref_sp(info.fBuffer)));

    // Submit the work and wait for it to complete.
    std::unique_ptr<Recording> recording = recorder->snap();
    if (!recording) {
        ERRORF(reporter, "Failed to make recording");
        return;
    }

    InsertRecordingInfo insertInfo;
    insertInfo.fRecording = recording.get();
    context->insertRecording(insertInfo);
    context->submit(SyncToCpu::kYes);

    // Verify the contents of the output buffer.
    constexpr uint32_t kExpectedCount = kWorkgroupCount * kWorkgroupSize / 2;

    const uint32_t* ssboData = static_cast<const uint32_t*>(map_bind_buffer(info));
    const uint32_t firstHalfCount = ssboData[0];
    const uint32_t secondHalfCount = ssboData[1];
    REPORTER_ASSERT(reporter,
                    firstHalfCount == kExpectedCount,
                    "expected '%d', found '%d'",
                    kExpectedCount,
                    firstHalfCount);
    REPORTER_ASSERT(reporter,
                    secondHalfCount == kExpectedCount,
                    "expected '%d', found '%d'",
                    kExpectedCount,
                    secondHalfCount);
}
