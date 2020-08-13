/*
 * Copyright 2018 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/**************************************************************************************************
 *** This file was autogenerated from GrLinearGradientLayout.fp; do not modify.
 **************************************************************************************************/
#ifndef GrLinearGradientLayout_DEFINED
#define GrLinearGradientLayout_DEFINED

#include "include/core/SkM44.h"
#include "include/core/SkTypes.h"

#include "src/gpu/effects/GrMatrixEffect.h"
#include "src/gpu/gradients/GrGradientShader.h"
#include "src/shaders/gradients/SkLinearGradient.h"

#include "src/gpu/GrFragmentProcessor.h"

class GrLinearGradientLayout : public GrFragmentProcessor {
public:
    static std::unique_ptr<GrFragmentProcessor> Make(const SkLinearGradient& gradient,
                                                     const GrFPArgs& args);
    GrLinearGradientLayout(const GrLinearGradientLayout& src);
    std::unique_ptr<GrFragmentProcessor> clone() const override;
    const char* name() const override { return "LinearGradientLayout"; }

private:
    GrLinearGradientLayout()
            : INHERITED(kGrLinearGradientLayout_ClassID,
                        (OptimizationFlags)kPreservesOpaqueInput_OptimizationFlag) {
        this->setUsesSampleCoordsDirectly();
    }
    GrGLSLFragmentProcessor* onCreateGLSLInstance() const override;
    void onGetGLSLProcessorKey(const GrShaderCaps&, GrProcessorKeyBuilder*) const override;
    bool onIsEqual(const GrFragmentProcessor&) const override;
#if GR_TEST_UTILS
    SkString onDumpInfo() const override;
#endif
    GR_DECLARE_FRAGMENT_PROCESSOR_TEST
    typedef GrFragmentProcessor INHERITED;
};
#endif
