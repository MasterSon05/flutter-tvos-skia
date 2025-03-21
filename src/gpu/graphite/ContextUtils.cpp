/*
 * Copyright 2021 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/graphite/ContextUtils.h"

#include <string>
#include "include/private/SkSLString.h"
#include "src/core/SkBlenderBase.h"
#include "src/gpu/graphite/Caps.h"
#include "src/gpu/graphite/GraphicsPipelineDesc.h"
#include "src/gpu/graphite/KeyContext.h"
#include "src/gpu/graphite/PaintParams.h"
#include "src/gpu/graphite/PipelineData.h"
#include "src/gpu/graphite/RecorderPriv.h"
#include "src/gpu/graphite/Renderer.h"
#include "src/gpu/graphite/ResourceProvider.h"
#include "src/gpu/graphite/ShaderCodeDictionary.h"
#include "src/gpu/graphite/UniformManager.h"
#include "src/gpu/graphite/UniquePaintParamsID.h"

namespace skgpu::graphite {

std::tuple<UniquePaintParamsID, const UniformDataBlock*, const TextureDataBlock*>
ExtractPaintData(Recorder* recorder,
                 PipelineDataGatherer* gatherer,
                 PaintParamsKeyBuilder* builder,
                 const Layout layout,
                 const SkM44& local2Dev,
                 const PaintParams& p,
                 const SkColorInfo& targetColorInfo) {
    SkDEBUGCODE(builder->checkReset());

    gatherer->resetWithNewLayout(layout);

    KeyContext keyContext(recorder, local2Dev, targetColorInfo, p.color());
    p.toKey(keyContext, builder, gatherer);

    auto entry = recorder->priv().shaderCodeDictionary()->findOrCreate(builder);
    if (!entry) {
        return { UniquePaintParamsID::InvalidID(), nullptr, nullptr };
    }

    UniformDataCache* uniformDataCache = recorder->priv().uniformDataCache();
    TextureDataCache* textureDataCache = recorder->priv().textureDataCache();
    const UniformDataBlock* uniforms =
            gatherer->hasUniforms() ? uniformDataCache->insert(gatherer->finishUniformDataBlock())
                                    : nullptr;
    const TextureDataBlock* textures =
            gatherer->hasTextures() ? textureDataCache->insert(gatherer->textureDataBlock())
                                    : nullptr;

    return { entry->uniqueID(), uniforms, textures };
}

std::tuple<const UniformDataBlock*, const TextureDataBlock*> ExtractRenderStepData(
        UniformDataCache* uniformDataCache,
        TextureDataCache* textureDataCache,
        PipelineDataGatherer* gatherer,
        const Layout layout,
        const RenderStep* step,
        const DrawParams& params) {
    gatherer->resetWithNewLayout(layout);
    step->writeUniformsAndTextures(params, gatherer);

    const UniformDataBlock* uniforms =
            gatherer->hasUniforms() ? uniformDataCache->insert(gatherer->finishUniformDataBlock())
                                    : nullptr;
    const TextureDataBlock* textures =
            gatherer->hasTextures() ? textureDataCache->insert(gatherer->textureDataBlock())
                                    : nullptr;

    return { uniforms, textures };
}

namespace {
std::string get_uniform_header(int bufferID, const char* name) {
    std::string result;

    SkSL::String::appendf(&result, "layout (binding=%d) uniform %sUniforms {\n", bufferID, name);

    return result;
}

std::string get_uniforms(Layout layout,
                         SkSpan<const Uniform> uniforms,
                         int* offset,
                         int manglingSuffix) {
    std::string result;
    UniformOffsetCalculator offsetter(layout, *offset);

    for (const Uniform& u : uniforms) {
        SkSL::String::appendf(&result,
                              "    layout(offset=%zu) %s %s",
                              offsetter.advanceOffset(u.type(), u.count()),
                              SkSLTypeString(u.type()),
                              u.name());
        if (manglingSuffix >= 0) {
            result.append("_");
            result.append(std::to_string(manglingSuffix));
        }
        if (u.count()) {
            result.append("[");
            result.append(std::to_string(u.count()));
            result.append("]");
        }
        result.append(";\n");
    }

    *offset = offsetter.size();
    return result;
}
}  // anonymous namespace

std::string EmitPaintParamsUniforms(int bufferID,
                                    const char* name,
                                    const Layout layout,
                                    const std::vector<PaintParamsKey::BlockReader>& readers) {
    int offset = 0;

    std::string result = get_uniform_header(bufferID, name);
    for (int i = 0; i < (int) readers.size(); ++i) {
        SkSpan<const Uniform> uniforms = readers[i].entry()->fUniforms;

        if (!uniforms.empty()) {
            SkSL::String::appendf(&result, "// %s uniforms\n", readers[i].entry()->fName);
            result += get_uniforms(layout, uniforms, &offset, i);
        }
    }
    result.append("};\n\n");

    return result;
}

std::string EmitRenderStepUniforms(int bufferID,
                                   const char* name,
                                   const Layout layout,
                                   SkSpan<const Uniform> uniforms) {
    int offset = 0;

    std::string result = get_uniform_header(bufferID, name);
    result += get_uniforms(layout, uniforms, &offset, -1);
    result.append("};\n\n");

    return result;
}

std::string EmitPaintParamsStorageBuffer(
        int bufferID,
        const char* bufferTypePrefix,
        const char* bufferNamePrefix,
        const std::vector<PaintParamsKey::BlockReader>& readers) {

    std::string result;
    SkSL::String::appendf(&result, "struct %sUniformData {\n", bufferTypePrefix);
    for (int i = 0; i < (int)readers.size(); ++i) {
        SkSpan<const Uniform> uniforms = readers[i].entry()->fUniforms;
        if (uniforms.empty()) {
            continue;
        }
        SkSL::String::appendf(&result, "// %s uniforms\n", readers[i].entry()->fName);
        int manglingSuffix = i;
        for (const Uniform& u : uniforms) {
            SkSL::String::appendf(
                    &result, "    %s %s_%d", SkSLTypeString(u.type()), u.name(), manglingSuffix);
            if (u.count()) {
                SkSL::String::appendf(&result, "[%u]", u.count());
            }
            result.append(";\n");
        }
    }
    result.append("};\n\n");

    SkSL::String::appendf(&result,
                          "layout (binding=%d) buffer %sUniforms {\n"
                          "    %sUniformData %sUniformData[];\n"
                          "};\n",
                          bufferID,
                          bufferTypePrefix,
                          bufferTypePrefix,
                          bufferNamePrefix);
    return result;
}

std::string EmitStorageBufferAccess(const char* bufferNamePrefix,
                                    const char* ssboIndex,
                                    const char* uniformName) {
    return SkSL::String::printf("%sUniformData[%s].%s", bufferNamePrefix, ssboIndex, uniformName);
}

std::string EmitTexturesAndSamplers(const ResourceBindingRequirements& bindingReqs,
                                    const std::vector<PaintParamsKey::BlockReader>& readers,
                                    int* binding) {
    std::string result;
    for (int i = 0; i < (int) readers.size(); ++i) {
        SkSpan<const TextureAndSampler> samplers = readers[i].entry()->fTexturesAndSamplers;

        if (!samplers.empty()) {
            SkSL::String::appendf(&result, "// %s samplers\n", readers[i].entry()->fName);

            for (const TextureAndSampler& t : samplers) {
                result += EmitSamplerLayout(bindingReqs, binding);
                SkSL::String::appendf(&result, " uniform sampler2D %s_%d;\n", t.name(), i);
            }
        }
    }

    return result;
}

std::string EmitSamplerLayout(const ResourceBindingRequirements& bindingReqs, int* binding) {
    std::string result;

    // If fDistinctIndexRanges is false, then texture and sampler indices may clash with other
    // resource indices. Graphite assumes that they will be placed in descriptor set (Vulkan) and
    // bind group (Dawn) index 1.
    if (bindingReqs.fSeparateTextureAndSamplerBinding) {
        int samplerIndex = (*binding)++;
        int textureIndex = (*binding)++;
        SkSL::String::appendf(&result,
                              "layout(wgsl, %ssampler=%d, texture=%d)",
                              bindingReqs.fDistinctIndexRanges ? "" : "set=1, ",
                              samplerIndex,
                              textureIndex);
    } else {
        SkSL::String::appendf(&result,
                              "layout(%sbinding=%d)",
                              bindingReqs.fDistinctIndexRanges ? "" : "set=1, ",
                              *binding);
        (*binding)++;
    }
    return result;
}

namespace {
std::string emit_attributes(SkSpan<const Attribute> vertexAttrs,
                            SkSpan<const Attribute> instanceAttrs) {
    std::string result;

    int attr = 0;
    auto add_attrs = [&](SkSpan<const Attribute> attrs) {
        for (auto a : attrs) {
            SkSL::String::appendf(&result, "    layout(location=%d) in ", attr++);
            result.append(SkSLTypeString(a.gpuType()));
            SkSL::String::appendf(&result, " %s;\n", a.name());
        }
    };

    if (!vertexAttrs.empty()) {
        result.append("// vertex attrs\n");
        add_attrs(vertexAttrs);
    }
    if (!instanceAttrs.empty()) {
        result.append("// instance attrs\n");
        add_attrs(instanceAttrs);
    }

    return result;
}
}  // anonymous namespace

std::string EmitVaryings(const RenderStep* step,
                         const char* direction,
                         bool emitShadingSsboIndexVarying,
                         bool emitLocalCoordsVarying) {
    std::string result;
    int location = 0;

    if (emitShadingSsboIndexVarying) {
        SkSL::String::appendf(&result,
                              "    layout(location=%d) %s int shadingSsboIndexVar;\n",
                              location++,
                              direction);
    }

    if (emitLocalCoordsVarying) {
        SkSL::String::appendf(&result, "    layout(location=%d) %s ", location++, direction);
        result.append(SkSLTypeString(SkSLType::kFloat2));
        SkSL::String::appendf(&result, " localCoordsVar;\n");
    }

    for (auto v : step->varyings()) {
        SkSL::String::appendf(&result, "    layout(location=%d) %s ", location++, direction);
        result.append(SkSLTypeString(v.fType));
        SkSL::String::appendf(&result, " %s;\n", v.fName);
    }

    return result;
}

std::string GetSkSLVS(const ResourceBindingRequirements& bindingReqs,
                      const RenderStep* step,
                      bool defineShadingSsboIndexVarying,
                      bool defineLocalCoordsVarying) {
    // TODO: To more completely support end-to-end rendering, this will need to be updated so that
    // the RenderStep shader snippet can produce a device coord, a local coord, and depth.
    // If the paint combination doesn't need the local coord it can be ignored, otherwise we need
    // a varying for it. The fragment function's output will need to be updated to have a color and
    // the depth, or when there's no combination, just the depth. Lastly, we also should add the
    // static/intrinsic uniform binding point so that we can handle normalizing the device position
    // produced by the RenderStep automatically.

    // Fixed program header
    std::string sksl =
        "layout (binding=0) uniform intrinsicUniforms {\n"
        "    layout(offset=0) float4 rtAdjust;\n"
        "};\n"
        "\n";

    if (step->numVertexAttributes() > 0 || step->numInstanceAttributes() > 0) {
        sksl += emit_attributes(step->vertexAttributes(), step->instanceAttributes());
    }

    // Uniforms needed by RenderStep
    // The uniforms are mangled by having their index in 'fEntries' as a suffix (i.e., "_%d")
    // TODO: replace hard-coded bufferID with the backend's renderstep uniform-buffer index.
    if (step->numUniforms() > 0) {
        sksl += EmitRenderStepUniforms(
                1, "Step", bindingReqs.fUniformBufferLayout, step->uniforms());
    }

    // Varyings needed by RenderStep
    sksl += EmitVaryings(step, "out", defineShadingSsboIndexVarying, defineLocalCoordsVarying);

    // Vertex shader function declaration
    sksl += "void main() {";
    // Create stepLocalCoords which render steps can write to.
    sksl += "float2 stepLocalCoords = float2(0);";
    // Vertex shader body
    sksl += step->vertexSkSL();
    sksl += "sk_Position = float4(devPosition.xy * rtAdjust.xy + devPosition.ww * rtAdjust.zw,"
            "devPosition.zw);";

    if (defineShadingSsboIndexVarying) {
        // Assign SSBO index value to the SSBO index varying
        SkSL::String::appendf(&sksl, "shadingSsboIndexVar = %s;", step->ssboIndex());
    }

    if (defineLocalCoordsVarying) {
        // Assign Render Step's stepLocalCoords to the localCoordsVar varying.
        sksl += "localCoordsVar = stepLocalCoords;";
    }
    sksl += "}";

    return sksl;
}

FragSkSLInfo GetSkSLFS(const ResourceBindingRequirements& bindingReqs,
                       const ShaderCodeDictionary* dict,
                       const RuntimeEffectDictionary* rteDict,
                       const RenderStep* step,
                       UniquePaintParamsID paintID,
                       bool useStorageBuffers,
                       skgpu::Swizzle writeSwizzle) {
    if (!paintID.isValid()) {
        // TODO: we should return the error shader code here
        return {};
    }

    FragSkSLInfo result;

    const char* shadingSsboIndexVar = useStorageBuffers ? "shadingSsboIndexVar" : nullptr;
    ShaderInfo shaderInfo(rteDict, shadingSsboIndexVar);

    dict->getShaderInfo(paintID, &shaderInfo);
    result.fBlendInfo = shaderInfo.blendInfo();
    result.fRequiresLocalCoords = shaderInfo.needsLocalCoords();

    // Extra RenderStep uniforms are always backed by a UBO. Uniforms for the PaintParams are either
    // UBO or SSBO backed based on `useStorageBuffers`.
    result.fSkSL =
            shaderInfo.toSkSL(bindingReqs,
                              step,
                              useStorageBuffers,
                              /*defineLocalCoordsVarying=*/result.fRequiresLocalCoords,
                              /*numTexturesAndSamplersUsed=*/&result.fNumTexturesAndSamplers,
                              writeSwizzle);

    return result;
}

} // namespace skgpu::graphite
