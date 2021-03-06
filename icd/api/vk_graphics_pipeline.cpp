/*
 *******************************************************************************
 *
 * Copyright (c) 2014-2017 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "include/stencil_ops_combiner.h"
#include "include/vk_conv.h"
#include "include/vk_device.h"
#include "include/vk_graphics_pipeline.h"
#include "include/vk_instance.h"
#include "include/vk_memory.h"
#include "include/vk_pipeline_cache.h"
#include "include/vk_pipeline_layout.h"
#include "include/vk_object.h"
#include "include/vk_render_pass.h"
#include "include/vk_shader.h"
#include "include/vk_cmdbuffer.h"

#include "palAutoBuffer.h"
#include "palCmdBuffer.h"
#include "palDevice.h"
#include "palPipeline.h"
#include "palShader.h"
#include "palInlineFuncs.h"

#include "llpc.h"

#include <float.h>
#include <math.h>

using namespace Util;

namespace vk
{

// =====================================================================================================================
// Returns true if the given blend factor is a dual source blend factor
bool IsDualSourceBlend(Pal::Blend blend)
{
    switch (blend)
    {
    case Pal::Blend::Src1Color:
    case Pal::Blend::OneMinusSrc1Color:
    case Pal::Blend::Src1Alpha:
    case Pal::Blend::OneMinusSrc1Alpha:
        return true;
    default:
        return false;
    }
}

// =====================================================================================================================
// Returns true if src alpha is used in blending
bool IsSrcAlphaUsedInBlend(
    VkBlendFactor blend)
{
    switch (blend)
    {
    case VK_BLEND_FACTOR_SRC_ALPHA:
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
    case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:
    case VK_BLEND_FACTOR_SRC1_ALPHA:
    case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA:
        return true;
    default:
        return false;
    }
}

// =====================================================================================================================
// Parses input pipeline rasterization create info state.
VkResult GraphicsPipeline::BuildRasterizationState(
    Device*                             pDevice,
    const VkPipelineRasterizationStateCreateInfo* pIn,
    CreateInfo*                         pInfo,
    ImmedInfo*                          pImmedInfo,
    const bool                          dynamicStateFlags[])
{
    VkResult result = VK_SUCCESS;

    union
    {
        const VkStructHeader*                                       pHeader;
        const VkPipelineRasterizationStateCreateInfo*               pRs;
        const VkPipelineRasterizationStateRasterizationOrderAMD*    pRsOrder;
    };

    // By default rasterization is disabled, unless rasterization creation info is present
    pInfo->pipeline.rsState.rasterizerDiscardEnable = true;

    const VkPhysicalDeviceLimits& limits = pDevice->VkPhysicalDevice()->GetLimits();

    // Enable perpendicular end caps if we report strictLines semantics
    pInfo->pipeline.rsState.perpLineEndCapsEnable = (limits.strictLines == VK_TRUE);

    for (pRs = pIn; (pHeader != nullptr) && (result == VK_SUCCESS); pHeader = pHeader->pNext)
    {
        switch (pHeader->sType)
        {
        case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO:
            {
                // When depth clamping is enabled, depth clipping should be disabled, and vice versa
                pInfo->pipeline.vpState.depthClipEnable = (pRs->depthClampEnable == VK_FALSE);

                pInfo->pipeline.rsState.rasterizerDiscardEnable = (pRs->rasterizerDiscardEnable != VK_FALSE);

                pImmedInfo->triangleRasterState.fillMode  = VkToPalFillMode(pRs->polygonMode);
                pImmedInfo->triangleRasterState.cullMode  = VkToPalCullMode(pRs->cullMode);
                pImmedInfo->triangleRasterState.frontFace = VkToPalFaceOrientation(pRs->frontFace);
                pImmedInfo->triangleRasterState.flags.depthBiasEnable = pRs->depthBiasEnable;

                pImmedInfo->depthBiasParams.depthBias = pRs->depthBiasConstantFactor;
                pImmedInfo->depthBiasParams.depthBiasClamp = pRs->depthBiasClamp;
                pImmedInfo->depthBiasParams.slopeScaledDepthBias = pRs->depthBiasSlopeFactor;

                if (pRs->depthBiasEnable && (dynamicStateFlags[VK_DYNAMIC_STATE_DEPTH_BIAS] == false))
                {
                    pImmedInfo->staticStateMask |= 1 << VK_DYNAMIC_STATE_DEPTH_BIAS;
                }

                // point size must be set via gl_PointSize, otherwise it must be 1.0f.
                constexpr float DefaultPointSize = 1.0f;

                pImmedInfo->pointLineRasterParams.lineWidth    = pRs->lineWidth;
                pImmedInfo->pointLineRasterParams.pointSize    = DefaultPointSize;
                pImmedInfo->pointLineRasterParams.pointSizeMin = limits.pointSizeRange[0];
                pImmedInfo->pointLineRasterParams.pointSizeMax = limits.pointSizeRange[1];

                if (dynamicStateFlags[VK_DYNAMIC_STATE_LINE_WIDTH] == false)
                {
                    pImmedInfo->staticStateMask |= 1 << VK_DYNAMIC_STATE_LINE_WIDTH;
                }
            }
            break;

        default:
            // Handle  extension specific structures
            // (a separate switch-case is used to allow the main switch-case to be optimized into a lookup table)
            switch (static_cast<int32>(pHeader->sType))
            {
            case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_RASTERIZATION_ORDER_AMD:
                {
                    // VK_AMD_rasterization_order must be enabled
                    VK_ASSERT(pDevice->IsExtensionEnabled(DeviceExtensions::AMD_RASTERIZATION_ORDER));

                    pInfo->pipeline.rsState.outOfOrderPrimsEnable =
                        VkToPalRasterizationOrder(pRsOrder->rasterizationOrder);
                }
                break;

            default:
                // Skip any unknown extension structures
                break;
            }
            break;

        }
    }

    return result;
}

// =====================================================================================================================
// Parses input pipeline create info state and creates patched versions of the input AMDIL shaders based on it.
VkResult GraphicsPipeline::BuildPatchedShaders(
    Device*                             pDevice,
    PipelineCache*                      pPipelineCache,
    const VkGraphicsPipelineCreateInfo* pIn,
    CreateInfo*                         pInfo,
    ImmedInfo*                          pImmedInfo,
    VbBindingInfo*                      pVbInfo,
    void**                              ppTempBuffer,
    void**                              ppTempShaderBuffer,
    size_t*                             pPipelineBinarySize,
    const void**                        ppPipelineBinary)
{
    const RuntimeSettings& settings = pDevice->GetRuntimeSettings();

    VkResult result = VK_SUCCESS;
    Pal::Result palResult = Pal::Result::Success;

    const VkPipelineVertexInputStateCreateInfo* pVertexInput = nullptr;
    const PipelineLayout* pLayout = nullptr;
    VkFormat cbFormat[Pal::MaxColorTargets] = {};
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;

    // Fill in necessary non-zero defaults in case some information is missing
    pInfo->pipeline.rsState.numSamples  = 1;
    pInfo->msaa.coverageSamples         = 1;
    pInfo->msaa.pixelShaderSamples      = 1;
    pInfo->msaa.depthStencilSamples     = 1;
    pInfo->msaa.shaderExportMaskSamples = 1;
    pInfo->msaa.sampleClusters          = 1;
    pInfo->msaa.alphaToCoverageSamples  = 1;
    pInfo->msaa.occlusionQuerySamples   = 1;
    pInfo->msaa.sampleMask              = 1;
    pInfo->sampleCoverage               = 1;

#if ICD_BUILD_APPPROFILE
    // This is a key structure for the pipeline profile to identify this pipeline and its shaders by hash, etc.
    PipelineOptimizerKey pipelineProfileKey = {};
#endif

    void* pTempBuffer = nullptr;

    // Tracks seen shader stages during parsing.  We'll use these later to build per-stage pipeline
    // shader infos.
    uint32_t activeStageCount = 0;
    const VkPipelineShaderStageCreateInfo* pActiveStages = nullptr;

    EXTRACT_VK_STRUCTURES_0(
        gfxPipeline,
        GraphicsPipelineCreateInfo,
        pIn,
        GRAPHICS_PIPELINE_CREATE_INFO)

    // Set the states which are allowed to call CmdSetxxx outside of the PSO
    bool dynamicStateFlags[uint32_t(DynamicStatesInternal::DynamicStatesInternalCount)];

    memset(dynamicStateFlags, 0, sizeof(dynamicStateFlags));

    if (pGraphicsPipelineCreateInfo != nullptr)
    {
        activeStageCount    = pGraphicsPipelineCreateInfo->stageCount;
        pActiveStages       = pGraphicsPipelineCreateInfo->pStages;

        VK_IGNORE(pGraphicsPipelineCreateInfo->flags & VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT);

        if (pGraphicsPipelineCreateInfo->layout != VK_NULL_HANDLE)
        {
            pLayout = PipelineLayout::ObjectFromHandle(pGraphicsPipelineCreateInfo->layout);

            // Allocate space needed to build auxiliary structures for PAL descriptor
            // mappings
            if (pLayout->GetPipelineInfo()->tempBufferSize > 0)
            {
                pTempBuffer = pDevice->VkInstance()->AllocMem(
                    pLayout->GetPipelineInfo()->tempBufferSize,
                    VK_DEFAULT_MEM_ALIGN,
                    VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);

                if (pTempBuffer == nullptr)
                {
                    result = VK_ERROR_OUT_OF_HOST_MEMORY;
                }
            }

            pInfo->pLayout = pLayout;
        }

        if (result == VK_SUCCESS)
        {
            switch (settings.pipelineLinkTimeOptMode)
            {
            case PipelineLinkTimeOptApiControlled:
                pInfo->pipeline.flags.disableOptimization =
                    ((pGraphicsPipelineCreateInfo->flags & VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT) != 0);
                break;
            case PipelineLinkTimeOptAlwaysEnabled:
                pInfo->pipeline.flags.disableOptimization = 0;
                break;
            case PipelineLinkTimeOptAlwaysDisabled:
                pInfo->pipeline.flags.disableOptimization = 1;
                break;
            default:
                VK_NEVER_CALLED();
                break;
            }

            switch (settings.pipelineFastCompileMode)
            {
            case PipelineFastCompileApiControlled:
                if ((pGraphicsPipelineCreateInfo->flags & VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT) != 0)
                {
                    pInfo->pipeline.flags.disableOptimizationC0 = 0;
                    pInfo->pipeline.flags.disableOptimizationC1 = 1;
                    pInfo->pipeline.flags.disableOptimizationC2 = 1;
                    pInfo->pipeline.flags.disableOptimizationC3 = 1;
                    pInfo->pipeline.flags.disableOptimizationC4 = 1;
                }
                break;
            case PipelineFastCompileAlwaysFast:
                pInfo->pipeline.flags.disableOptimizationC0 = 0;
                pInfo->pipeline.flags.disableOptimizationC1 = 1;
                pInfo->pipeline.flags.disableOptimizationC2 = 1;
                pInfo->pipeline.flags.disableOptimizationC3 = 1;
                pInfo->pipeline.flags.disableOptimizationC4 = 1;
                break;
            case PipelineFastCompileAlwaysOptimized:
                pInfo->pipeline.flags.disableOptimizationC0 = 1;
                pInfo->pipeline.flags.disableOptimizationC1 = 0;
                pInfo->pipeline.flags.disableOptimizationC2 = 1;
                pInfo->pipeline.flags.disableOptimizationC3 = 1;
                pInfo->pipeline.flags.disableOptimizationC4 = 1;
                break;
            default:
                VK_NEVER_CALLED();
                break;
            }

            pInfo->pipeline.flags.sm5_1ResourceBinding = 1;

            pVertexInput = pGraphicsPipelineCreateInfo->pVertexInputState;

            const VkPipelineInputAssemblyStateCreateInfo* pIa = pGraphicsPipelineCreateInfo->pInputAssemblyState;

            // According to the spec this should never be null
            VK_ASSERT(pIa != nullptr);

            pImmedInfo->inputAssemblyState.primitiveRestartEnable = (pIa->primitiveRestartEnable != VK_FALSE);
            pImmedInfo->inputAssemblyState.primitiveRestartIndex  = (pIa->primitiveRestartEnable != VK_FALSE)
                                                            ? 0xFFFFFFFF : 0;
            pImmedInfo->inputAssemblyState.topology = VkToPalPrimitiveTopology(pIa->topology);

            VkToPalPrimitiveTypeAdjacency(
                pIa->topology,
                &pInfo->pipeline.iaState.topologyInfo.primitiveType,
                &pInfo->pipeline.iaState.topologyInfo.adjacency);
            topology = pIa->topology;
            pInfo->pipeline.iaState.disableVertexReuse = false;

            EXTRACT_VK_STRUCTURES_1(
                Tess,
                PipelineTessellationStateCreateInfo,
                PipelineTessellationDomainOriginStateCreateInfoKHR,
                pGraphicsPipelineCreateInfo->pTessellationState,
                PIPELINE_TESSELLATION_STATE_CREATE_INFO,
                PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO_KHR)

            if (pPipelineTessellationStateCreateInfo != nullptr)
            {
                pInfo->pipeline.iaState.topologyInfo.patchControlPoints = pPipelineTessellationStateCreateInfo->patchControlPoints;
            }

            if (pPipelineTessellationDomainOriginStateCreateInfoKHR)
            {
                // Vulkan 1.0 incorrectly specified the tessellation u,v coordinate origin as lower left even though
                // framebuffer and image coordinate origins are in the upper left.  This has since been fixed, but
                // an extension exists to use the previous behavior.  Doing so with flat shading would likely appear
                // incorrect, but Vulkan specifies that the provoking vertex is undefined when tessellation is active.
                if (pPipelineTessellationDomainOriginStateCreateInfoKHR->domainOrigin == VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT_KHR)
                {
                    pInfo->pipeline.hs.flags.switchWinding = true;
                }
            }

            pImmedInfo->staticStateMask = 0;

            const VkPipelineDynamicStateCreateInfo* pDy = pGraphicsPipelineCreateInfo->pDynamicState;

            if (pDy != nullptr)
            {
                for (uint32_t i = 0; i < pDy->dynamicStateCount; ++i)
                {
                    if (pDy->pDynamicStates[i] < VK_DYNAMIC_STATE_RANGE_SIZE)
                    {
                        dynamicStateFlags[pDy->pDynamicStates[i]] = true;
                    }
                    else
                    {
                        switch (static_cast<uint32_t>(pDy->pDynamicStates[i]))
                        {
                        case VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_EXT:
                            dynamicStateFlags[static_cast<uint32_t>(DynamicStatesInternal::SAMPLE_LOCATIONS_EXT)] = true;
                            break;

                        default:
                            // skip unknown dynamic state
                            break;
                        }
                    }
                }
            }

            const VkPipelineViewportStateCreateInfo* pVp = pGraphicsPipelineCreateInfo->pViewportState;

            if (pVp != nullptr)
            {
                // From the spec, "scissorCount is the number of scissors and must match the number of viewports."
                VK_ASSERT(pVp->viewportCount <= Pal::MaxViewports);
                VK_ASSERT(pVp->scissorCount <= Pal::MaxViewports);
                VK_ASSERT(pVp->scissorCount == pVp->viewportCount);

                pImmedInfo->viewportParams.count     = pVp->viewportCount;
                pImmedInfo->scissorRectParams.count  = pVp->scissorCount;

                if (dynamicStateFlags[VK_DYNAMIC_STATE_VIEWPORT] == false)
                {
                    VK_ASSERT(pVp->pViewports != nullptr);

                    for (uint32_t i = 0; i < pVp->viewportCount; ++i)
                    {
                        VkToPalViewport(
                            pVp->pViewports[i],
                            i,
                            pDevice->IsExtensionEnabled(DeviceExtensions::KHR_MAINTENANCE1),
                            &pImmedInfo->viewportParams);
                    }

                    pImmedInfo->staticStateMask |= 1 << VK_DYNAMIC_STATE_VIEWPORT;
                }

                if (dynamicStateFlags[VK_DYNAMIC_STATE_SCISSOR] == false)
                {
                    VK_ASSERT(pVp->pScissors != nullptr);

                    for (uint32_t i = 0; i < pVp->scissorCount; ++i)
                    {
                        VkToPalScissorRect(pVp->pScissors[i], i, &pImmedInfo->scissorRectParams);
                    }

                    pImmedInfo->staticStateMask |= 1 << VK_DYNAMIC_STATE_SCISSOR;
                }
            }

            // Always use D3D viewport coordinate conventions
            pInfo->pipeline.vpState.depthRange = Pal::DepthRange::ZeroToOne;

            if (result == VK_SUCCESS)
            {
                result = BuildRasterizationState(pDevice,
                                                    pGraphicsPipelineCreateInfo->pRasterizationState,
                                                    pInfo,
                                                    pImmedInfo,
                                                    dynamicStateFlags);
            }

            {
                pInfo->pipeline.ps.psOnlyPointCoordEnable = 0xffffffff;
            }

            pInfo->pipeline.rsState.pointCoordOrigin  = Pal::PointOrigin::UpperLeft;
            pInfo->pipeline.rsState.shadeMode         = Pal::ShadeMode::Flat;
            pInfo->pipeline.rsState.rasterizeLastLinePixel = 0;

            // Pipeline Binning Override
            switch (settings.pipelineBinningMode)
            {
                case PipelineBinningModeEnable:
                    pInfo->pipeline.rsState.binningOverride = Pal::BinningOverride::Enable;
                break;

                case PipelineBinningModeDisable:
                    pInfo->pipeline.rsState.binningOverride = Pal::BinningOverride::Disable;
                break;

                case PipelineBinningModeDefault:
                default:
                    pInfo->pipeline.rsState.binningOverride = Pal::BinningOverride::Default;
                break;
            }

            bool multisampleEnable = false;
            uint32_t rasterizationSampleCount = 0;

            const RenderPass* pRenderPass = RenderPass::ObjectFromHandle(pGraphicsPipelineCreateInfo->renderPass);
            const VkPipelineMultisampleStateCreateInfo* pMs = pGraphicsPipelineCreateInfo->pMultisampleState;

            if (pMs != nullptr)
            {
                multisampleEnable = (pMs->rasterizationSamples != 1);

                if (multisampleEnable)
                {
                    VK_ASSERT(pRenderPass != nullptr);

                    rasterizationSampleCount            = pMs->rasterizationSamples;
                    uint32_t subpassCoverageSampleCount = pRenderPass->GetSubpassMaxSampleCount(pGraphicsPipelineCreateInfo->subpass);
                    uint32_t subpassColorSampleCount    = pRenderPass->GetSubpassColorSampleCount(pGraphicsPipelineCreateInfo->subpass);
                    uint32_t subpassDepthSampleCount    = pRenderPass->GetSubpassDepthSampleCount(pGraphicsPipelineCreateInfo->subpass);

                    // subpassCoverageSampleCount would be equal to zero if there are zero attachments.
                    subpassCoverageSampleCount = subpassCoverageSampleCount == 0 ? rasterizationSampleCount : subpassCoverageSampleCount;

                    // In case we are rendering to color only, we make sure to set the DepthSampleCount to CoverageSampleCount.
                    // CoverageSampleCount is really the ColorSampleCount in this case. This makes sure we have a consistent
                    // sample count and that we get correct MSAA behavior.
                    // Similar thing for when we are rendering to depth only. The expectation in that case is that all
                    // sample counts should match.
                    // This shouldn't interfere with EQAA. For EQAA, if ColorSampleCount is not equal to DepthSampleCount
                    // and they are both greater than one, then we do not force them to match.
                    subpassColorSampleCount = subpassColorSampleCount == 0 ? subpassCoverageSampleCount : subpassColorSampleCount;
                    subpassDepthSampleCount = subpassDepthSampleCount == 0 ? subpassCoverageSampleCount : subpassDepthSampleCount;

                    VK_ASSERT(rasterizationSampleCount == subpassCoverageSampleCount);

                    pInfo->msaa.coverageSamples = subpassCoverageSampleCount;
                    pInfo->msaa.exposedSamples  = subpassCoverageSampleCount;

                    if (pMs->sampleShadingEnable && (pMs->minSampleShading > 0.0f))
                    {
                        pInfo->msaa.pixelShaderSamples =
                            Pow2Pad(static_cast<uint32_t>(ceil(subpassColorSampleCount * pMs->minSampleShading)));
                    }
                    else
                    {
                        pInfo->msaa.pixelShaderSamples = 1;
                    }

                    pInfo->pipeline.rsState.numSamples = rasterizationSampleCount;

                    // NOTE: The sample pattern index here is actually the offset of sample position pair. This is
                    // different from the field of creation info of image view. For image view, the sample pattern
                    // index is really table index of the sample pattern.
                    pInfo->pipeline.rsState.samplePatternIdx =
                        Device::GetDefaultSamplePatternIndex(subpassCoverageSampleCount) * Pal::MaxMsaaRasterizerSamples;

                    pInfo->msaa.depthStencilSamples = subpassDepthSampleCount;
                    pInfo->msaa.shaderExportMaskSamples = subpassCoverageSampleCount;
                    pInfo->msaa.sampleMask = pMs->pSampleMask != nullptr
                                           ? pMs->pSampleMask[0]
                                           : 0xffffffff;
                    pInfo->msaa.sampleClusters         = subpassCoverageSampleCount;
                    pInfo->msaa.alphaToCoverageSamples = subpassCoverageSampleCount;
                    pInfo->msaa.occlusionQuerySamples  = subpassDepthSampleCount;
                    pInfo->sampleCoverage              = subpassCoverageSampleCount;

                    /// Sample Locations
                    EXTRACT_VK_STRUCTURES_1(
                        SampleLocations,
                        PipelineMultisampleStateCreateInfo,
                        PipelineSampleLocationsStateCreateInfoEXT,
                        pMs,
                        PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                        PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT)

                    bool customSampleLocations = false;

                    if (pPipelineSampleLocationsStateCreateInfoEXT != nullptr)
                    {
                        customSampleLocations = pPipelineSampleLocationsStateCreateInfoEXT->sampleLocationsEnable == VK_TRUE
                            ? true
                            : customSampleLocations;
                    }

                    if (customSampleLocations &&
                        (dynamicStateFlags[static_cast<uint32_t>(DynamicStatesInternal::SAMPLE_LOCATIONS_EXT)] == false))
                    {
                        // We store the custom sample locations if custom sample locations are enabled and the
                        // sample locations state is static.
                        pImmedInfo->samplePattern.sampleCount =
                            (uint32_t)pPipelineSampleLocationsStateCreateInfoEXT->sampleLocationsInfo.sampleLocationsPerPixel;

                        ConvertToPalMsaaQuadSamplePattern(
                            &pPipelineSampleLocationsStateCreateInfoEXT->sampleLocationsInfo,
                            &pImmedInfo->samplePattern.locations);

                        VK_ASSERT(pImmedInfo->samplePattern.sampleCount == rasterizationSampleCount);

                        pImmedInfo->staticStateMask |=
                            1 << static_cast<uint32_t>(DynamicStatesInternal::SAMPLE_LOCATIONS_EXT);
                    }
                    else if (customSampleLocations == false)
                    {
                        // We store the standard sample locations if custom sample locations are not enabled.
                        pImmedInfo->samplePattern.sampleCount = rasterizationSampleCount;
                        pImmedInfo->samplePattern.locations =
                            *Device::GetDefaultQuadSamplePattern(rasterizationSampleCount);

                        pImmedInfo->staticStateMask |=
                            1 << static_cast<uint32_t>(DynamicStatesInternal::SAMPLE_LOCATIONS_EXT);
                    }
                }

                pInfo->pipeline.cbState.alphaToCoverageEnable = (pMs->alphaToCoverageEnable == VK_TRUE);
            }

            const VkPipelineColorBlendStateCreateInfo* pCb = pGraphicsPipelineCreateInfo->pColorBlendState;

            bool blendingEnabled = false;
            bool dualSourceBlend = false;

            if (pCb == nullptr)
            {
                pInfo->pipeline.cbState.logicOp = Pal::LogicOp::Copy;
            }
            else
            {
                pInfo->pipeline.cbState.logicOp = (pCb->logicOpEnable) ? VkToPalLogicOp(pCb->logicOp) :
                    Pal::LogicOp::Copy;

                const uint32_t numColorTargets = Min(pCb->attachmentCount, Pal::MaxColorTargets);

                for (uint32_t i = 0; i < numColorTargets; ++i)
                {
                    const VkPipelineColorBlendAttachmentState& src = pCb->pAttachments[i];

                    auto pCbDst    = &pInfo->pipeline.cbState.target[i];
                    auto pBlendDst = &pInfo->blend.targets[i];

                    if (pRenderPass)
                    {
                        cbFormat[i] = pRenderPass->GetColorAttachmentFormat(pGraphicsPipelineCreateInfo->subpass, i);
                        pCbDst->swizzledFormat = VkToPalFormat(cbFormat[i]);
                    }

                    // If the sub pass attachment format is UNDEFINED, then it means that that subpass does not
                    // want to write to any attachment for that output (VK_ATTACHMENT_UNUSED).  Under such cases,
                    // disable shader writes through that target.
                    if (pCbDst->swizzledFormat.format != Pal::ChNumFormat::Undefined)
                    {
                        pCbDst->channelWriteMask     = src.colorWriteMask;
                        pCbDst->blendEnable          = (src.blendEnable == VK_TRUE);
                        pCbDst->blendSrcAlphaToColor = IsSrcAlphaUsedInBlend(src.srcAlphaBlendFactor) ||
                                                        IsSrcAlphaUsedInBlend(src.dstAlphaBlendFactor) ||
                                                        IsSrcAlphaUsedInBlend(src.srcColorBlendFactor) ||
                                                        IsSrcAlphaUsedInBlend(src.dstColorBlendFactor);
                        blendingEnabled = blendingEnabled || pCbDst->blendEnable;
                    }
                    else
                    {
                        pCbDst->channelWriteMask = 0;
                        pCbDst->blendEnable      = false;
                    }

                    pBlendDst->blendEnable    = pCbDst->blendEnable;
                    pBlendDst->srcBlendColor  = VkToPalBlend(src.srcColorBlendFactor);
                    pBlendDst->dstBlendColor  = VkToPalBlend(src.dstColorBlendFactor);
                    pBlendDst->blendFuncColor = VkToPalBlendFunc(src.colorBlendOp);
                    pBlendDst->srcBlendAlpha  = VkToPalBlend(src.srcAlphaBlendFactor);
                    pBlendDst->dstBlendAlpha  = VkToPalBlend(src.dstAlphaBlendFactor);
                    pBlendDst->blendFuncAlpha = VkToPalBlendFunc(src.alphaBlendOp);

                    dualSourceBlend |= IsDualSourceBlend(pBlendDst->srcBlendColor);
                    dualSourceBlend |= IsDualSourceBlend(pBlendDst->dstBlendColor);
                    dualSourceBlend |= IsDualSourceBlend(pBlendDst->srcBlendAlpha);
                    dualSourceBlend |= IsDualSourceBlend(pBlendDst->dstBlendAlpha);
                }
            }

            pInfo->pipeline.cbState.dualSourceBlendEnable = dualSourceBlend;

            if (blendingEnabled == true && dynamicStateFlags[VK_DYNAMIC_STATE_BLEND_CONSTANTS] == false)
            {
                static_assert(sizeof(pImmedInfo->blendConstParams) == sizeof(pCb->blendConstants),
                                "Blend constant structure size mismatch!");

                memcpy(&pImmedInfo->blendConstParams, pCb->blendConstants, sizeof(pCb->blendConstants));

                pImmedInfo->staticStateMask |= 1 << VK_DYNAMIC_STATE_BLEND_CONSTANTS;
            }

            if (pRenderPass != nullptr)
            {
                pInfo->pipeline.dbState.swizzledFormat = VkToPalFormat(pRenderPass->GetDepthStencilAttachmentFormat(
                    pGraphicsPipelineCreateInfo->subpass));
            }

            // If the sub pass attachment format is UNDEFINED, then it means that that subpass does not
            // want to write any depth-stencil data (VK_ATTACHMENT_UNUSED).  Under such cases, I think we have to
            // disable depth testing as well as depth writes.
            const VkPipelineDepthStencilStateCreateInfo* pDs = pGraphicsPipelineCreateInfo->pDepthStencilState;

            if ((pInfo->pipeline.dbState.swizzledFormat.format != Pal::ChNumFormat::Undefined) &&
                (pDs != nullptr))
            {
                pInfo->ds.stencilEnable     = (pDs->stencilTestEnable == VK_TRUE);
                pInfo->ds.depthEnable       = (pDs->depthTestEnable == VK_TRUE);
                pInfo->ds.depthWriteEnable  = (pDs->depthWriteEnable == VK_TRUE);
                pInfo->ds.depthFunc         = VkToPalCompareFunc(pDs->depthCompareOp);
                pInfo->ds.depthBoundsEnable = (pDs->depthBoundsTestEnable == VK_TRUE);

                if (pInfo->ds.depthBoundsEnable && dynamicStateFlags[VK_DYNAMIC_STATE_DEPTH_BOUNDS] == false)
                {
                    pImmedInfo->staticStateMask |= 1 << VK_DYNAMIC_STATE_DEPTH_BOUNDS;
                }

                // According to Graham, we should program the stencil state at PSO bind time,
                // regardless of whether this PSO enables\disables Stencil. This allows a second PSO
                // to inherit the first PSO's settings
                if (dynamicStateFlags[VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK] == false)
                {
                    pImmedInfo->staticStateMask |= 1 << VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK;
                }

                if (dynamicStateFlags[VK_DYNAMIC_STATE_STENCIL_WRITE_MASK] == false)
                {
                    pImmedInfo->staticStateMask |= 1 << VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
                }

                if (dynamicStateFlags[VK_DYNAMIC_STATE_STENCIL_REFERENCE] == false)
                {
                    pImmedInfo->staticStateMask |= 1 << VK_DYNAMIC_STATE_STENCIL_REFERENCE;
                }
            }
            else
            {
                pInfo->ds.depthEnable       = false;
                pInfo->ds.depthWriteEnable  = false;
                pInfo->ds.depthFunc         = Pal::CompareFunc::Always;
                pInfo->ds.depthBoundsEnable = false;
                pInfo->ds.stencilEnable     = false;
            }

            constexpr uint8_t DefaultStencilOpValue = 1;

            if (pDs != nullptr)
            {
                pInfo->ds.front.stencilFailOp      = VkToPalStencilOp(pDs->front.failOp);
                pInfo->ds.front.stencilPassOp      = VkToPalStencilOp(pDs->front.passOp);
                pInfo->ds.front.stencilDepthFailOp = VkToPalStencilOp(pDs->front.depthFailOp);
                pInfo->ds.front.stencilFunc        = VkToPalCompareFunc(pDs->front.compareOp);
                pInfo->ds.back.stencilFailOp       = VkToPalStencilOp(pDs->back.failOp);
                pInfo->ds.back.stencilPassOp       = VkToPalStencilOp(pDs->back.passOp);
                pInfo->ds.back.stencilDepthFailOp  = VkToPalStencilOp(pDs->back.depthFailOp);
                pInfo->ds.back.stencilFunc         = VkToPalCompareFunc(pDs->back.compareOp);

                pImmedInfo->stencilRefMasks.frontRef       = static_cast<uint8_t>(pDs->front.reference);
                pImmedInfo->stencilRefMasks.frontReadMask  = static_cast<uint8_t>(pDs->front.compareMask);
                pImmedInfo->stencilRefMasks.frontWriteMask = static_cast<uint8_t>(pDs->front.writeMask);
                pImmedInfo->stencilRefMasks.backRef        = static_cast<uint8_t>(pDs->back.reference);
                pImmedInfo->stencilRefMasks.backReadMask   = static_cast<uint8_t>(pDs->back.compareMask);
                pImmedInfo->stencilRefMasks.backWriteMask  = static_cast<uint8_t>(pDs->back.writeMask);

                pImmedInfo->depthBoundParams.min = pDs->minDepthBounds;
                pImmedInfo->depthBoundParams.max = pDs->maxDepthBounds;
            }

            pImmedInfo->stencilRefMasks.frontOpValue   = DefaultStencilOpValue;
            pImmedInfo->stencilRefMasks.backOpValue    = DefaultStencilOpValue;

            pInfo->pipeline.viewInstancingDesc = Pal::ViewInstancingDescriptor { };

            if (pRenderPass->IsMultiviewEnabled())
            {
                pInfo->pipeline.viewInstancingDesc.viewInstanceCount = Pal::MaxViewInstanceCount;
                pInfo->pipeline.viewInstancingDesc.enableMasking     = true;

                for (uint32 viewIndex = 0; viewIndex < Pal::MaxViewInstanceCount; ++viewIndex)
                {
                    pInfo->pipeline.viewInstancingDesc.viewId[viewIndex] = viewIndex;
                }
            }
        }
    }

    bool enableLlpc = false;
    bool buildLlpcPipeline = false;

    if (result == VK_SUCCESS)
    {
        Llpc::GraphicsPipelineBuildInfo pipelineBuildInfo = {};
        Llpc::GraphicsPipelineBuildOut  piplineOut = {};
        {
            buildLlpcPipeline = true;
        }

        if (buildLlpcPipeline)
        {
            Llpc::PipelineShaderInfo* shaderInfos[] =
            {
                &pipelineBuildInfo.vs,
                &pipelineBuildInfo.tcs,
                &pipelineBuildInfo.tes,
                &pipelineBuildInfo.gs,
                &pipelineBuildInfo.fs
            };

            // Apply patches
            pipelineBuildInfo.pInstance      = pDevice->VkPhysicalDevice()->VkInstance();
            pipelineBuildInfo.pfnOutputAlloc = AllocateShaderOutput;
            pipelineBuildInfo.pUserData      = ppTempShaderBuffer;

            if ((pPipelineCache != nullptr) && (pPipelineCache->GetPipelineCacheType() == PipelineCacheTypeLlpc))
            {
                pipelineBuildInfo.pShaderCache = pPipelineCache->GetShaderCache(DefaultDeviceIndex).pLlpcShaderCache;
            }

            pipelineBuildInfo.pVertexInput   = pVertexInput;

            pipelineBuildInfo.iaState.topology                = topology;
            pipelineBuildInfo.iaState.patchControlPoints      = pInfo->pipeline.iaState.topologyInfo.patchControlPoints;
            pipelineBuildInfo.iaState.disableVertexReuse      =  pInfo->pipeline.iaState.disableVertexReuse;
            pipelineBuildInfo.vpState.depthClipEnable         = pInfo->pipeline.vpState.depthClipEnable;
            pipelineBuildInfo.rsState.rasterizerDiscardEnable = pInfo->pipeline.rsState.rasterizerDiscardEnable;
            pipelineBuildInfo.rsState.perSampleShading        = (pInfo->msaa.pixelShaderSamples > 1);
            pipelineBuildInfo.rsState.numSamples              = pInfo->pipeline.rsState.numSamples;
            pipelineBuildInfo.rsState.samplePatternIdx        = pInfo->pipeline.rsState.samplePatternIdx;
            pipelineBuildInfo.rsState.usrClipPlaneMask        = pInfo->pipeline.rsState.usrClipPlaneMask;
            pipelineBuildInfo.cbState.alphaToCoverageEnable   = pInfo->pipeline.cbState.alphaToCoverageEnable;
            pipelineBuildInfo.cbState.dualSourceBlendEnable   = pInfo->pipeline.cbState.dualSourceBlendEnable;

            for (uint32_t rt = 0; rt < Pal::MaxColorTargets; ++rt)
            {
                pipelineBuildInfo.cbState.target[rt].blendEnable =
                    pInfo->pipeline.cbState.target[rt].blendEnable;
                pipelineBuildInfo.cbState.target[rt].blendSrcAlphaToColor =
                    pInfo->pipeline.cbState.target[rt].blendSrcAlphaToColor;
                pipelineBuildInfo.cbState.target[rt].format = cbFormat[rt];
            }

            for (uint32_t stage = 0; stage < activeStageCount; ++stage)
            {
                auto pStage      = &pActiveStages[stage];
                auto pShader     = ShaderModule::ObjectFromHandle(pStage->module);
                auto shaderStage = ShaderFlagBitToStage(pStage->stage);
                auto pShaderInfo = shaderInfos[shaderStage];

                pShaderInfo->pModuleData         = pShader->GetLlpcShaderData();
                pShaderInfo->pSpecializatonInfo  = pStage->pSpecializationInfo;
                pShaderInfo->pEntryTarget        = pStage->pName;

                // Build the resource mapping description for LLPC.  This data contains things about how shader
                // inputs like descriptor set bindings are communicated to this pipeline in a form that LLPC can
                // understand.
                if (pLayout != nullptr)
                {
                    const bool vertexShader = (shaderStage == ShaderStageVertex);
                    result = pLayout->BuildLlpcPipelineMapping(
                        shaderStage,
                        pTempBuffer,
                        vertexShader ? pVertexInput : nullptr,
                        pShaderInfo,
                        vertexShader ? pVbInfo : nullptr);
                }
            }
        }

        uint64_t pipeHash = 0;
        enableLlpc = true;

        if (result == VK_SUCCESS)
        {
            if (enableLlpc)
            {
                Llpc::Result llpcResult = pDevice->GetCompiler()->BuildGraphicsPipeline(&pipelineBuildInfo, &piplineOut);
                if (llpcResult != Llpc::Result::Success)
                {
                    {
                        result = VK_ERROR_INITIALIZATION_FAILED;
                    }
                }
            }
            else if (settings.enablePipelineDump)
            {
                // LLPC isn't enabled but pipeline dump is required, call LLPC dump interface explicitly
                pDevice->GetCompiler()->DumpGraphicsPipeline(&pipelineBuildInfo);
            }

            if (enableLlpc)
            {
                if (result == VK_SUCCESS)
                {
                    // Update pipeline create info with PAL shader object
                    pInfo->pipeline.ps.psOnlyPointCoordEnable = 0;
                    pInfo->pipeline.pPipelineBinary    = static_cast<const uint8_t*>(piplineOut.pipelineBin.pCode);
                    pInfo->pipeline.pipelineBinarySize = static_cast<uint32_t>(piplineOut.pipelineBin.codeSize);

                    *ppPipelineBinary    = pInfo->pipeline.pPipelineBinary;
                    *pPipelineBinarySize = pInfo->pipeline.pipelineBinarySize;
                }
            }
        }
    }

#ifdef ICD_BUILD_APPPROFILE
    // Override the Pal::GraphicsPipelineCreateInfo parameters based on any active app profile
    pDevice->GetShaderOptimizer()->OverrideGraphicsPipelineCreateInfo(pipelineProfileKey,
                                                                      &pInfo->pipeline,
                                                                      &pImmedInfo->graphicsWaveLimitParams);
#endif

    if (result == VK_SUCCESS)
    {
        *ppTempBuffer = pTempBuffer;
    }
    else
    {
        if (pTempBuffer != nullptr)
        {
            pDevice->VkInstance()->FreeMem(pTempBuffer);
        }
    }

    return result;
}

// =====================================================================================================================
// Create a graphics pipeline object.
VkResult GraphicsPipeline::Create(
    Device*                                 pDevice,
    PipelineCache*                          pPipelineCache,
    const VkGraphicsPipelineCreateInfo*     pCreateInfo,
    const VkAllocationCallbacks*            pAllocator,
    VkPipeline*                             pPipeline)
{
    // Parse the create info and build patched AMDIL shaders
    CreateInfo createInfo       = {};
    ImmedInfo immedInfo         = {};
    VbBindingInfo vbInfo        = {};
    void* pTempBuffer           = nullptr;
    void* pTempShaderBuffer     = nullptr;
    size_t pipelineBinarySize   = 0;
    const void* pPipelineBinary = nullptr;
    Pal::Result palResult       = Pal::Result::Success;

    VkResult result = BuildPatchedShaders(
        pDevice,
        pPipelineCache,
        pCreateInfo,
        &createInfo,
        &immedInfo,
        &vbInfo,
        &pTempBuffer,
        &pTempShaderBuffer,
        &pipelineBinarySize,
        &pPipelineBinary);

    // See which graphics shader stage is setting a wave limit
    if (result == VK_SUCCESS)
    {
        for (uint32_t stage = 0; stage < pCreateInfo->stageCount; ++stage)
        {
            auto pStage = &pCreateInfo->pStages[stage];

            if (pStage->pNext != nullptr)
            {
                ShaderStage shaderStage = ShaderFlagBitToStage(pStage->stage);

                Pal::PipelineShaderInfo* pShaderInfo = nullptr;

                switch (shaderStage)
                {
                case ShaderStageVertex:
                    pShaderInfo = &createInfo.pipeline.vs;
                    break;
                case ShaderStageTessControl:
                    pShaderInfo = &createInfo.pipeline.hs;
                    break;
                case ShaderStageTessEvaluation:
                    pShaderInfo = &createInfo.pipeline.ds;
                    break;
                case ShaderStageGeometry:
                    pShaderInfo = &createInfo.pipeline.gs;
                    break;
                case ShaderStageFragment:
                    pShaderInfo = &createInfo.pipeline.ps;
                    break;
                default:
                    VK_NOT_IMPLEMENTED;
                    break;
                }
            }
        }
    }

    const uint32_t numPalDevices = pDevice->NumPalDevices();

    RenderStateCache* pRSCache = pDevice->GetRenderStateCache();

    // Get the pipeline size from PAL and allocate memory.
    size_t palSize = 0;
    size_t pipelineSize     [MaxPalDevices];

    // Create the PAL pipeline object.
    void*                    pSystemMem = nullptr;
    Pal::IPipeline*          pPalPipeline[MaxPalDevices] = {};
    Pal::IMsaaState*         pPalMsaa[MaxPalDevices] = {};
    Pal::IColorBlendState*   pPalColorBlend[MaxPalDevices] = {};
    Pal::IDepthStencilState* pPalDepthStencil[MaxPalDevices] = {};

    if (result == VK_SUCCESS)
    {
        for (uint32_t deviceIdx = 0; deviceIdx < numPalDevices; deviceIdx++)
        {
            Pal::IDevice* pPalDevice = pDevice->PalDevice(deviceIdx);

            pipelineSize[deviceIdx] = pPalDevice->GetGraphicsPipelineSize(
                createInfo.pipeline, &palResult);
            VK_ASSERT(palResult == Pal::Result::Success);

            palSize += pipelineSize[deviceIdx];
        }

        // Allocate system memory for all objects
        pSystemMem = pAllocator->pfnAllocation(
            pAllocator->pUserData,
            sizeof(GraphicsPipeline) + palSize,
            VK_DEFAULT_MEM_ALIGN,
            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

        if (pSystemMem == nullptr)
        {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    size_t palOffset = sizeof(GraphicsPipeline);

    for (uint32_t deviceIdx = 0; deviceIdx < numPalDevices; deviceIdx++)
    {
        Pal::IDevice* pPalDevice = pDevice->PalDevice(deviceIdx);

        if (result == VK_SUCCESS)
        {
            if ((pPipelineCache != nullptr) && (pPipelineCache->GetPipelineCacheType() == PipelineCacheTypePal))
            {
                createInfo.pipeline.pShaderCache = pPipelineCache->GetShaderCache(deviceIdx).pPalShaderCache;
            }

            for (uint32_t stage = 0; stage < ShaderGfxStageCount; ++stage)
            {
                if (createInfo.pPalShaders[deviceIdx][stage] != nullptr)
                {
                    Pal::IShader* pShader = createInfo.pPalShaders[deviceIdx][stage];

                    switch (stage)
                    {
                    case ShaderStageVertex:
                        createInfo.pipeline.vs.pShader = pShader;
                        break;
                    case ShaderStageTessControl:
                        createInfo.pipeline.hs.pShader = pShader;
                        break;
                    case ShaderStageTessEvaluation:
                        createInfo.pipeline.ds.pShader = pShader;
                        break;
                    case ShaderStageGeometry:
                        createInfo.pipeline.gs.pShader = pShader;
                        break;
                    case ShaderStageFragment:
                        createInfo.pipeline.ps.pShader = pShader;
                        break;
                    default:
                        VK_NOT_IMPLEMENTED;
                        break;
                    }
                }
            }

            palResult = pPalDevice->CreateGraphicsPipeline(
                createInfo.pipeline,
                Util::VoidPtrInc(pSystemMem, palOffset),
                &pPalPipeline[deviceIdx]);

            if (palResult != Pal::Result::Success)
            {
                result = PalToVkResult(palResult);
            }

            palOffset += pipelineSize[deviceIdx];
        }

        // Create the PAL MSAA state object
        if (result == VK_SUCCESS)
        {
            palResult = pRSCache->CreateMsaaState(
                createInfo.msaa,
                pAllocator,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,
                pPalMsaa);
        }

        // Create the PAL color blend state object
        if (result == VK_SUCCESS)
        {
            palResult = pRSCache->CreateColorBlendState(
                createInfo.blend,
                pAllocator,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,
                pPalColorBlend);
        }

        // Create the PAL depth stencil state object
        if (result == VK_SUCCESS)
        {
            palResult = pRSCache->CreateDepthStencilState(
                createInfo.ds,
                pAllocator,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,
                pPalDepthStencil);
        }
    }

    PipelineBinaryInfo* pBinaryInfo = nullptr;

    if (pDevice->IsExtensionEnabled(DeviceExtensions::AMD_SHADER_INFO) && (result == VK_SUCCESS))
    {
        // The CreateLegacyPathElfBinary() function is temporary.  It can go away once LLPC/SCPC paths are enabled.
        void* pLegacyBinary = nullptr;

        if (pPipelineBinary == nullptr)
        {
            Pipeline::CreateLegacyPathElfBinary(pDevice, true, pPalPipeline[DefaultDeviceIndex], &pipelineBinarySize,
                &pLegacyBinary);

            pPipelineBinary = pLegacyBinary;
        }

        // (This call is not temporary)
        pBinaryInfo = PipelineBinaryInfo::Create(pipelineBinarySize, pPipelineBinary, pAllocator);

        if (pLegacyBinary != nullptr)
        {
            pDevice->VkInstance()->FreeMem(pLegacyBinary);
        }
    }

    pDevice->VkInstance()->FreeMem(pTempBuffer);
    pDevice->VkInstance()->FreeMem(pTempShaderBuffer);

    // On success, wrap it up in a Vulkan object.
    if (result == VK_SUCCESS)
    {
        VK_PLACEMENT_NEW(pSystemMem) GraphicsPipeline(
            pDevice,
            pPalPipeline,
            createInfo.pLayout,
            immedInfo,
            vbInfo,
            pPalMsaa,
            pPalColorBlend,
            pPalDepthStencil,
            createInfo.sampleCoverage,
            pBinaryInfo);

        *pPipeline = GraphicsPipeline::HandleFromVoidPointer(pSystemMem);
    }

    // Free PAL shader object and related memory
    Pal::IShader* pPalShaders[ShaderGfxStageCount] = {};

    pPalShaders[ShaderStageVertex]          = createInfo.pipeline.vs.pShader;
    pPalShaders[ShaderStageTessControl]     = createInfo.pipeline.hs.pShader;
    pPalShaders[ShaderStageTessEvaluation]  = createInfo.pipeline.ds.pShader;
    pPalShaders[ShaderStageGeometry]        = createInfo.pipeline.gs.pShader;
    pPalShaders[ShaderStageFragment]        = createInfo.pipeline.ps.pShader;

    for (uint32_t i = 0; i < ShaderGfxStageCount; ++i)
    {
        if (pPalShaders[i] != nullptr)
        {
            pPalShaders[i]->Destroy();
        }
    }

    if (createInfo.pShaderMem != nullptr)
    {
        pDevice->VkInstance()->FreeMem(createInfo.pShaderMem);
    }

    if (result != VK_SUCCESS)
    {
        pRSCache->DestroyMsaaState(pPalMsaa, pAllocator);
        pRSCache->DestroyColorBlendState(pPalColorBlend, pAllocator);
        pRSCache->DestroyDepthStencilState(pPalDepthStencil, pAllocator);

        // Something went wrong with creating the PAL object. Free memory and return error.
        for (uint32_t deviceIdx = 0; deviceIdx < pDevice->NumPalDevices(); deviceIdx++)
        {
            if (pPalPipeline[deviceIdx] != nullptr)
            {
                pPalPipeline[deviceIdx]->Destroy();
            }
        }

        if (pBinaryInfo != nullptr)
        {
            pBinaryInfo->Destroy(pAllocator);
        }

        pAllocator->pfnFree(pAllocator->pUserData, pSystemMem);
    }

    return result;
}

// =====================================================================================================================
GraphicsPipeline::GraphicsPipeline(
    Device* const                          pDevice,
    Pal::IPipeline**                       pPalPipeline,
    const PipelineLayout*                  pLayout,
    const ImmedInfo&                       immedInfo,
    const VbBindingInfo&                   vbInfo,
    Pal::IMsaaState**                      pPalMsaa,
    Pal::IColorBlendState**                pPalColorBlend,
    Pal::IDepthStencilState**              pPalDepthStencil,
    uint32_t                               coverageSamples,
    PipelineBinaryInfo*                    pBinary)
    :
    Pipeline(pDevice, pPalPipeline, pLayout, pBinary),
    m_info(immedInfo),
    m_vbInfo(vbInfo),
    m_coverageSamples(coverageSamples)
{
    memcpy(m_pPalMsaa,         pPalMsaa,         sizeof(pPalMsaa[0])         * pDevice->NumPalDevices());
    memcpy(m_pPalColorBlend,   pPalColorBlend,   sizeof(pPalColorBlend[0])   * pDevice->NumPalDevices());
    memcpy(m_pPalDepthStencil, pPalDepthStencil, sizeof(pPalDepthStencil[0]) * pDevice->NumPalDevices());

    CreateStaticState();
}

// =====================================================================================================================
// Creates instances of static pipeline state.  Much of this information can be cached at the device-level to help speed
// up pipeline-bind operations.
void GraphicsPipeline::CreateStaticState()
{
    RenderStateCache* pCache = m_pDevice->GetRenderStateCache();
    auto* pStaticTokens      = &m_info.staticTokens;

    pStaticTokens->inputAssemblyState         = pCache->CreateInputAssemblyState(m_info.inputAssemblyState);
    pStaticTokens->triangleRasterState        = pCache->CreateTriangleRasterState(m_info.triangleRasterState);
    pStaticTokens->pointLineRasterState       = DynamicRenderStateToken;
    pStaticTokens->depthBias                  = DynamicRenderStateToken;
    pStaticTokens->blendConst                 = DynamicRenderStateToken;
    pStaticTokens->depthBounds                = DynamicRenderStateToken;
    pStaticTokens->viewport                   = DynamicRenderStateToken;
    pStaticTokens->scissorRect                = DynamicRenderStateToken;
    pStaticTokens->samplePattern              = DynamicRenderStateToken;
    pStaticTokens->waveLimits                 = DynamicRenderStateToken;

    if (PipelineSetsState(DynamicStatesInternal::LINE_WIDTH))
    {
        pStaticTokens->pointLineRasterState = pCache->CreatePointLineRasterState(
            m_info.pointLineRasterParams);
    }

    if (PipelineSetsState(DynamicStatesInternal::DEPTH_BIAS))
    {
        pStaticTokens->depthBias = pCache->CreateDepthBias(m_info.depthBiasParams);
    }

    if (PipelineSetsState(DynamicStatesInternal::BLEND_CONSTANTS))
    {
        pStaticTokens->blendConst = pCache->CreateBlendConst(m_info.blendConstParams);
    }

    if (PipelineSetsState(DynamicStatesInternal::DEPTH_BOUNDS))
    {
        pStaticTokens->depthBounds = pCache->CreateDepthBounds(m_info.depthBoundParams);
    }

    if (PipelineSetsState(DynamicStatesInternal::VIEWPORT))
    {
        pStaticTokens->viewport = pCache->CreateViewport(m_info.viewportParams);
    }

    if (PipelineSetsState(DynamicStatesInternal::SCISSOR))
    {
        pStaticTokens->scissorRect = pCache->CreateScissorRect(m_info.scissorRectParams);
    }

    if (PipelineSetsState(DynamicStatesInternal::SAMPLE_LOCATIONS_EXT))
    {
        pStaticTokens->samplePattern = pCache->CreateSamplePattern(m_info.samplePattern);
    }

}

// =====================================================================================================================
// Destroys static pipeline state.
void GraphicsPipeline::DestroyStaticState(
    const VkAllocationCallbacks* pAllocator)
{
    RenderStateCache* pCache = m_pDevice->GetRenderStateCache();

    pCache->DestroyMsaaState(m_pPalMsaa, pAllocator);
    pCache->DestroyColorBlendState(m_pPalColorBlend, pAllocator);
    pCache->DestroyDepthStencilState(m_pPalDepthStencil, pAllocator);

    pCache->DestroyInputAssemblyState(m_info.inputAssemblyState,
                                      m_info.staticTokens.inputAssemblyState);

    pCache->DestroyTriangleRasterState(m_info.triangleRasterState,
                                       m_info.staticTokens.triangleRasterState);

    pCache->DestroyPointLineRasterState(m_info.pointLineRasterParams,
                                        m_info.staticTokens.pointLineRasterState);

    pCache->DestroyDepthBias(m_info.depthBiasParams,
                             m_info.staticTokens.depthBias);

    pCache->DestroyBlendConst(m_info.blendConstParams,
                              m_info.staticTokens.blendConst);

    pCache->DestroyDepthBounds(m_info.depthBoundParams,
                               m_info.staticTokens.depthBounds);

    pCache->DestroyViewport(m_info.viewportParams,
                            m_info.staticTokens.viewport);

    pCache->DestroyScissorRect(m_info.scissorRectParams,
                               m_info.staticTokens.scissorRect);

    pCache->DestroySamplePattern(m_info.samplePattern,
                                 m_info.staticTokens.samplePattern);
}

// =====================================================================================================================
VkResult GraphicsPipeline::Destroy(
    Device*                      pDevice,
    const VkAllocationCallbacks* pAllocator)
{
    DestroyStaticState(pAllocator);

    return Pipeline::Destroy(pDevice, pAllocator);
}

// =====================================================================================================================
GraphicsPipeline::~GraphicsPipeline()
{

}

// =====================================================================================================================
// Binds this graphics pipeline's state to the given command buffer (using waveLimits created from the pipeline)
void GraphicsPipeline::BindToCmdBuffer(
    CmdBuffer*                             pCmdBuffer,
    CmdBufferRenderState*                  pRenderState,
    StencilOpsCombiner*                    pStencilCombiner) const
{
    BindToCmdBuffer(pCmdBuffer, pRenderState, pStencilCombiner, m_info.graphicsWaveLimitParams);
}

// =====================================================================================================================
// Binds this graphics pipeline's state to the given command buffer (with passed in wavelimits)
void GraphicsPipeline::BindToCmdBuffer(
    CmdBuffer*                             pCmdBuffer,
    CmdBufferRenderState*                  pRenderState,
    StencilOpsCombiner*                    pStencilCombiner,
    const Pal::DynamicGraphicsShaderInfos& graphicsShaderInfos) const
{
    // If the viewport/scissor counts changed, we need to resend the current viewport/scissor state to PAL
    bool viewportCountDirty = (pRenderState->allGpuState.viewport.count != m_info.viewportParams.count);
    bool scissorCountDirty  = (pRenderState->allGpuState.scissor.count  != m_info.scissorRectParams.count);

    // Update current viewport/scissor count
    pRenderState->allGpuState.viewport.count = m_info.viewportParams.count;
    pRenderState->allGpuState.scissor.count  = m_info.scissorRectParams.count;

    // Get this pipeline's static tokens
    const auto& newTokens = m_info.staticTokens;

    // Get the old static tokens.  Copy these by value because in MGPU cases we update the new token state in a loop.
    const auto oldTokens = pRenderState->allGpuState.staticTokens;

    // Program static pipeline state.

    // This code will attempt to skip programming state state based on redundant value checks.  These checks are often
    // represented as token compares, where the tokens are two perfect hashes of previously compiled pipelines' static
    // parameter values.
    if (PipelineSetsState(DynamicStatesInternal::VIEWPORT) &&
        CmdBuffer::IsStaticStateDifferent(oldTokens.viewports, newTokens.viewport))
    {
        pCmdBuffer->SetAllViewports(m_info.viewportParams, newTokens.viewport);
        viewportCountDirty = false;
    }

    if (PipelineSetsState(DynamicStatesInternal::SCISSOR) &&
        CmdBuffer::IsStaticStateDifferent(oldTokens.scissorRect, newTokens.scissorRect))
    {
        pCmdBuffer->SetAllScissors(m_info.scissorRectParams, newTokens.scissorRect);
        scissorCountDirty = false;
    }

    utils::IterateMask deviceGroup(pCmdBuffer->GetDeviceMask());
    while (deviceGroup.Iterate())
    {
        const uint32_t deviceIdx = deviceGroup.Index();

        Pal::ICmdBuffer* pPalCmdBuf = pCmdBuffer->PalCmdBuffer(deviceIdx);

        if (pRenderState->allGpuState.pGraphicsPipeline != nullptr)
        {
            const uint64_t oldHash =
                pRenderState->allGpuState.pGraphicsPipeline->PalPipeline(deviceIdx)->GetInfo().pipelineHash;
            const uint64_t newHash = m_pPalPipeline[deviceIdx]->GetInfo().pipelineHash;

            if (oldHash != newHash)
            {
                Pal::PipelineBindParams params = {};
                params.pipelineBindPoint = Pal::PipelineBindPoint::Graphics;
                params.pPipeline         = m_pPalPipeline[deviceIdx];
                params.graphics          = graphicsShaderInfos;

                pPalCmdBuf->CmdBindPipeline(params);
            }
        }
        else
        {
            Pal::PipelineBindParams params = {};
            params.pipelineBindPoint = Pal::PipelineBindPoint::Graphics;
            params.pPipeline         = m_pPalPipeline[deviceIdx];
            params.graphics          = graphicsShaderInfos;

            pPalCmdBuf->CmdBindPipeline(params);
        }

        // Bind state objects that are always static; these are redundancy checked by the pointer in the command buffer.
        pCmdBuffer->PalCmdBindDepthStencilState(deviceIdx, m_pPalDepthStencil[deviceIdx]);
        pCmdBuffer->PalCmdBindColorBlendState(deviceIdx, m_pPalColorBlend[deviceIdx]);
        pCmdBuffer->PalCmdBindMsaaState(deviceIdx, m_pPalMsaa[deviceIdx]);

        // Write parameters that are marked static pipeline state.  Redundancy check these based on static tokens:
        // skip the write if the previously written static token matches.
        if (CmdBuffer::IsStaticStateDifferent(oldTokens.inputAssemblyState, newTokens.inputAssemblyState))
        {
            pPalCmdBuf->CmdSetInputAssemblyState(m_info.inputAssemblyState);
            pRenderState->allGpuState.staticTokens.inputAssemblyState = newTokens.inputAssemblyState;
        }

        if (CmdBuffer::IsStaticStateDifferent(oldTokens.triangleRasterState, newTokens.triangleRasterState))
        {
            pPalCmdBuf->CmdSetTriangleRasterState(m_info.triangleRasterState);
            pRenderState->allGpuState.staticTokens.triangleRasterState = newTokens.triangleRasterState;
        }

        if (PipelineSetsState(DynamicStatesInternal::LINE_WIDTH) &&
            CmdBuffer::IsStaticStateDifferent(oldTokens.pointLineRasterState, newTokens.pointLineRasterState))
        {
            pPalCmdBuf->CmdSetPointLineRasterState(m_info.pointLineRasterParams);
            pRenderState->allGpuState.staticTokens.pointLineRasterState = newTokens.pointLineRasterState;
        }

        if (PipelineSetsState(DynamicStatesInternal::DEPTH_BIAS) &&
            CmdBuffer::IsStaticStateDifferent(oldTokens.depthBiasState, newTokens.depthBias))
        {
            pPalCmdBuf->CmdSetDepthBiasState(m_info.depthBiasParams);
            pRenderState->allGpuState.staticTokens.depthBiasState = newTokens.depthBias;
        }

        if (PipelineSetsState(DynamicStatesInternal::BLEND_CONSTANTS) &&
            CmdBuffer::IsStaticStateDifferent(oldTokens.blendConst, newTokens.blendConst))
        {
            pPalCmdBuf->CmdSetBlendConst(m_info.blendConstParams);
            pRenderState->allGpuState.staticTokens.blendConst = newTokens.blendConst;
        }

        if (PipelineSetsState(DynamicStatesInternal::DEPTH_BOUNDS) &&
            CmdBuffer::IsStaticStateDifferent(oldTokens.depthBounds, newTokens.depthBounds))
        {
            pPalCmdBuf->CmdSetDepthBounds(m_info.depthBoundParams);
            pRenderState->allGpuState.staticTokens.depthBounds = newTokens.depthBounds;
        }

        if (PipelineSetsState(DynamicStatesInternal::SAMPLE_LOCATIONS_EXT) &&
            CmdBuffer::IsStaticStateDifferent(oldTokens.samplePattern, newTokens.samplePattern))
        {
            pCmdBuffer->PalCmdSetMsaaQuadSamplePattern(
                m_info.samplePattern.sampleCount, m_info.samplePattern.locations);
            pRenderState->allGpuState.staticTokens.samplePattern = newTokens.samplePattern;
        }
        // If we still need to rebind viewports but the pipeline state did not already do it, resend the state to PAL
        // (note that we are only reprogramming the previous state here, so no need to update tokens)
        if (viewportCountDirty)
        {
            pPalCmdBuf->CmdSetViewports(pRenderState->allGpuState.viewport);
        }

        if (scissorCountDirty)
        {
            pPalCmdBuf->CmdSetScissorRects(pRenderState->allGpuState.scissor);
        }
    }

    const bool stencilMasks = PipelineSetsState(DynamicStatesInternal::STENCIL_COMPARE_MASK) |
                              PipelineSetsState(DynamicStatesInternal::STENCIL_WRITE_MASK)   |
                              PipelineSetsState(DynamicStatesInternal::STENCIL_REFERENCE);

    // Until we expose Stencil Op Value, we always inherit the PSO value, which is currently Default == 1
    pStencilCombiner->Set(StencilRefMaskParams::FrontOpValue, m_info.stencilRefMasks.frontOpValue);
    pStencilCombiner->Set(StencilRefMaskParams::BackOpValue,  m_info.stencilRefMasks.backOpValue);

    if (stencilMasks)
    {
        // We don't have to use tokens for these since the combiner does a redundancy check on the full value
        if (PipelineSetsState(DynamicStatesInternal::STENCIL_COMPARE_MASK))
        {
            pStencilCombiner->Set(StencilRefMaskParams::FrontReadMask, m_info.stencilRefMasks.frontReadMask);
            pStencilCombiner->Set(StencilRefMaskParams::BackReadMask,  m_info.stencilRefMasks.backReadMask);
        }
        if (PipelineSetsState(DynamicStatesInternal::STENCIL_WRITE_MASK))
        {
            pStencilCombiner->Set(StencilRefMaskParams::FrontWriteMask, m_info.stencilRefMasks.frontWriteMask);
            pStencilCombiner->Set(StencilRefMaskParams::BackWriteMask,  m_info.stencilRefMasks.backWriteMask);
        }
        if (PipelineSetsState(DynamicStatesInternal::STENCIL_REFERENCE))
        {
            pStencilCombiner->Set(StencilRefMaskParams::FrontRef, m_info.stencilRefMasks.frontRef);
            pStencilCombiner->Set(StencilRefMaskParams::BackRef,  m_info.stencilRefMasks.backRef);
        }

        // Generate the PM4 if any of the Stencil state is to be statically bound
        // knowing we will likely overwrite it.
        pStencilCombiner->PalCmdSetStencilState(pCmdBuffer);
    }
}

// =====================================================================================================================
// Binds a null pipeline to PAL
void GraphicsPipeline::BindNullPipeline(CmdBuffer* pCmdBuffer)
{
    const uint32_t numDevices = pCmdBuffer->VkDevice()->NumPalDevices();

    Pal::PipelineBindParams params = {};
    params.pipelineBindPoint = Pal::PipelineBindPoint::Graphics;

    for (uint32_t deviceIdx = 0; deviceIdx < numDevices; deviceIdx++)
    {
        Pal::ICmdBuffer* pPalCmdBuf = pCmdBuffer->PalCmdBuffer(deviceIdx);

        pPalCmdBuf->CmdBindPipeline(params);
        pPalCmdBuf->CmdBindMsaaState(nullptr);
        pPalCmdBuf->CmdBindColorBlendState(nullptr);
        pPalCmdBuf->CmdBindDepthStencilState(nullptr);
    }
}

} // namespace vk
