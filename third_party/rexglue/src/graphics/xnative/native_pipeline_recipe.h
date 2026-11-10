#pragma once
#include <atomic>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>

#include <rex/ui/vulkan/api.h>

namespace rex::graphics::xnative {

// TEMP (Android perf investigation): request driver shader statistics.
inline std::atomic<bool> g_capture_pipeline_statistics{false};

// Xcompiler (Halo 3, 2026-11-10, performance): the vertex stage's specialization when its shader
// reads vertex data itself (XenosRecomp's computed fetches): constant 0 as every stage, and per
// vertex input location L constant 100 + L, the declaration element's format word (XcFetchVertexDecl's
// "declared"; 0 for a location without one element). Built in place: info points into the object.
constexpr uint32_t kNativeVertexFetchFormatCount = 64;
constexpr uint32_t kNativeVertexFetchFormatConstantBase = 100;
struct NativeVertexFetchSpecialization {
  struct Data {
    uint32_t value = 0;
    std::array<uint32_t, kNativeVertexFetchFormatCount> formats{};
  } data;
  std::array<VkSpecializationMapEntry, 1 + kNativeVertexFetchFormatCount> entries{};
  VkSpecializationInfo info{};

  NativeVertexFetchSpecialization() = default;
  NativeVertexFetchSpecialization(const NativeVertexFetchSpecialization&) = delete;
  NativeVertexFetchSpecialization& operator=(const NativeVertexFetchSpecialization&) = delete;

  void Build(uint32_t value, const std::array<uint32_t, kNativeVertexFetchFormatCount>& formats) {
    data.value = value;
    data.formats = formats;
    entries[0] = {0, uint32_t(offsetof(Data, value)), sizeof(uint32_t)};
    for (uint32_t i = 0; i < kNativeVertexFetchFormatCount; ++i) {
      entries[1 + i] = {kNativeVertexFetchFormatConstantBase + i,
                        uint32_t(offsetof(Data, formats) + i * sizeof(uint32_t)), sizeof(uint32_t)};
    }
    info.mapEntryCount = uint32_t(entries.size());
    info.pMapEntries = entries.data();
    info.dataSize = sizeof(Data);
    info.pData = &data;
  }
};

// Complete effective state of the native draw pipeline. No guest memory,
// surface object, vector storage or borrowed pNext chain reaches the compiler.
// The fixed limits are checked at capture; larger future interfaces use the
// original synchronous creation path. Disk snapshots are tied to the executable
// build, Vulkan cache identity, schema, and exact SPIR-V content fingerprints.
class NativePipelineRecipe {
 public:
  struct ShaderIdentity {
    uint64_t title_hash = 0;
    uint64_t code_hash = 0;
    uint32_t variant = 0;  // stock early/late, override early/late
    uint32_t specialization_enabled = 0;
  };
  struct Snapshot {
    uint32_t stage_count = 0;
    std::array<ShaderIdentity, 2> shaders{};
    uint32_t specialization = 0;
    uint32_t binding_count = 0;
    uint32_t attribute_count = 0;
    uint32_t color_count = 0;
    std::array<VkVertexInputBindingDescription, 32> bindings{};
    std::array<VkVertexInputAttributeDescription, 32> attributes{};
    std::array<VkFormat, 4> color_formats{};
    std::array<VkPipelineColorBlendAttachmentState, 4> color_attachments{};
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    VkFormat stencil_format = VK_FORMAT_UNDEFINED;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkBool32 primitive_restart = VK_FALSE;
    VkBool32 negative_one_to_one = VK_FALSE;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkSampleMask sample_mask = 0;
    uint32_t indexed_descriptors = 0;
    // The vertex stage takes NativeVertexFetchSpecialization with these formats.
    uint32_t vertex_fetch_formats_enabled = 0;
    std::array<uint32_t, kNativeVertexFetchFormatCount> vertex_fetch_formats{};
  };
  static_assert(std::is_trivially_copyable_v<Snapshot>);

  Snapshot data{};
  std::array<VkShaderModule, 2> modules{};
  VkPipelineLayout layout = VK_NULL_HANDLE;

  bool Valid() const {
    if (!data.stage_count || data.stage_count > data.shaders.size() ||
        data.binding_count > data.bindings.size() ||
        data.attribute_count > data.attributes.size() ||
        data.color_count > data.color_formats.size() || data.indexed_descriptors > VK_TRUE ||
        data.vertex_fetch_formats_enabled > VK_TRUE ||
        (data.vertex_fetch_formats_enabled && !data.shaders[0].specialization_enabled) ||
        data.rasterization.pNext || data.depth_stencil.pNext || data.rasterization.flags ||
        data.depth_stencil.flags ||
        data.rasterization.sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO ||
        data.depth_stencil.sType != VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO ||
        data.topology < VK_PRIMITIVE_TOPOLOGY_POINT_LIST ||
        data.topology > VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ||
        data.primitive_restart > VK_TRUE || data.negative_one_to_one > VK_TRUE ||
        data.rasterization.depthClampEnable > VK_TRUE ||
        data.rasterization.rasterizerDiscardEnable != VK_FALSE ||
        (data.rasterization.polygonMode != VK_POLYGON_MODE_FILL &&
         data.rasterization.polygonMode != VK_POLYGON_MODE_LINE) ||
        data.rasterization.cullMode > VK_CULL_MODE_FRONT_AND_BACK ||
        uint32_t(data.rasterization.frontFace) > VK_FRONT_FACE_CLOCKWISE ||
        data.rasterization.depthBiasEnable > VK_TRUE || data.rasterization.lineWidth != 1.0f ||
        data.depth_stencil.depthTestEnable > VK_TRUE ||
        data.depth_stencil.depthWriteEnable > VK_TRUE ||
        data.depth_stencil.depthBoundsTestEnable != VK_FALSE ||
        data.depth_stencil.stencilTestEnable > VK_TRUE ||
        uint32_t(data.depth_stencil.depthCompareOp) > VK_COMPARE_OP_ALWAYS) {
      return false;
    }
    switch (data.samples) {
      case VK_SAMPLE_COUNT_1_BIT:
      case VK_SAMPLE_COUNT_2_BIT:
      case VK_SAMPLE_COUNT_4_BIT:
      case VK_SAMPLE_COUNT_8_BIT:
      case VK_SAMPLE_COUNT_16_BIT:
      case VK_SAMPLE_COUNT_32_BIT:
        break;
      default:
        return false;
    }
    // Native draws currently use core formats only. Future extension formats
    // fail closed to the original exact creation path until explicitly added.
    const auto core_format = [](VkFormat format) {
      return uint32_t(format) <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
    };
    if (!core_format(data.depth_format) || !core_format(data.stencil_format)) {
      return false;
    }
    // Variants 0-3: early/late and their overrides; 4-7: early-fragment-test modules; 8-11:
    // colorless modules (the module_code_hashes / replay module table order). A limit of 3 failed
    // every early-test or colorless pixel shader recipe in Create(), so those pipelines compiled
    // synchronously on the recording thread and were dropped from the saved recipes on load.
    for (uint32_t i = 0; i < data.stage_count; ++i) {
      if (data.shaders[i].variant > 11 || data.shaders[i].specialization_enabled > VK_TRUE) {
        return false;
      }
    }
    for (uint32_t i = 0; i < data.binding_count; ++i) {
      if (data.bindings[i].binding >= data.bindings.size() ||
          uint32_t(data.bindings[i].inputRate) > VK_VERTEX_INPUT_RATE_INSTANCE) {
        return false;
      }
    }
    for (uint32_t i = 0; i < data.attribute_count; ++i) {
      if (data.attributes[i].location >= data.attributes.size() ||
          data.attributes[i].binding >= data.bindings.size() ||
          !core_format(data.attributes[i].format) ||
          data.attributes[i].format == VK_FORMAT_UNDEFINED) {
        return false;
      }
    }
    for (const auto& stencil : {data.depth_stencil.front, data.depth_stencil.back}) {
      if (uint32_t(stencil.failOp) > VK_STENCIL_OP_DECREMENT_AND_WRAP ||
          uint32_t(stencil.passOp) > VK_STENCIL_OP_DECREMENT_AND_WRAP ||
          uint32_t(stencil.depthFailOp) > VK_STENCIL_OP_DECREMENT_AND_WRAP ||
          uint32_t(stencil.compareOp) > VK_COMPARE_OP_ALWAYS) {
        return false;
      }
    }
    for (uint32_t i = 0; i < data.color_count; ++i) {
      const auto& blend = data.color_attachments[i];
      if (!core_format(data.color_formats[i]) || blend.blendEnable > VK_TRUE ||
          uint32_t(blend.srcColorBlendFactor) > VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA ||
          uint32_t(blend.dstColorBlendFactor) > VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA ||
          uint32_t(blend.srcAlphaBlendFactor) > VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA ||
          uint32_t(blend.dstAlphaBlendFactor) > VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA ||
          uint32_t(blend.colorBlendOp) > VK_BLEND_OP_MAX ||
          uint32_t(blend.alphaBlendOp) > VK_BLEND_OP_MAX ||
          (blend.colorWriteMask & ~(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT))) {
        return false;
      }
    }
    return true;
  }

  // vertex_fetch_formats: the vertex stage's info is a NativeVertexFetchSpecialization built
  // from specialization and these formats (the caller's word for it), else null.
  static std::optional<NativePipelineRecipe> Capture(
      const VkGraphicsPipelineCreateInfo& info, uint32_t specialization,
      const std::array<uint32_t, kNativeVertexFetchFormatCount>* vertex_fetch_formats = nullptr) {
    if (info.flags || info.renderPass || info.pTessellationState || !info.pStages ||
        !info.stageCount || info.stageCount > 2 ||
        !info.pNext || !info.pVertexInputState || !info.pInputAssemblyState ||
        !info.pRasterizationState || !info.pDepthStencilState || !info.pColorBlendState ||
        !info.pMultisampleState || !info.pViewportState || !info.pDynamicState) {
      return std::nullopt;
    }
    const auto& rendering = *static_cast<const VkPipelineRenderingCreateInfo*>(info.pNext);
    const auto& vertex = *info.pVertexInputState;
    const auto& blend = *info.pColorBlendState;
    const auto& multisample = *info.pMultisampleState;
    if (rendering.sType != VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO || rendering.pNext ||
        rendering.viewMask || vertex.pNext || vertex.flags ||
        vertex.vertexBindingDescriptionCount > 32 || vertex.vertexAttributeDescriptionCount > 32 ||
        rendering.colorAttachmentCount > 4 || blend.attachmentCount != rendering.colorAttachmentCount ||
        blend.pNext || blend.flags || blend.logicOpEnable || info.pRasterizationState->pNext ||
        info.pDepthStencilState->pNext || info.pInputAssemblyState->pNext ||
        info.pInputAssemblyState->flags || info.pViewportState->flags ||
        info.pDynamicState->pNext || info.pDynamicState->flags ||
        !info.pDynamicState->pDynamicStates ||
        (vertex.vertexBindingDescriptionCount && !vertex.pVertexBindingDescriptions) ||
        (vertex.vertexAttributeDescriptionCount && !vertex.pVertexAttributeDescriptions) ||
        (rendering.colorAttachmentCount && (!rendering.pColorAttachmentFormats || !blend.pAttachments)) ||
        multisample.pNext || multisample.flags || multisample.sampleShadingEnable ||
        multisample.alphaToCoverageEnable || multisample.alphaToOneEnable || !multisample.pSampleMask ||
        uint32_t(multisample.rasterizationSamples) > 32 ||
        info.pViewportState->viewportCount != 1 || info.pViewportState->scissorCount != 1 ||
        info.pViewportState->pViewports || info.pViewportState->pScissors ||
        info.pDynamicState->dynamicStateCount != kDynamicStates.size() ||
        std::memcmp(info.pDynamicState->pDynamicStates, kDynamicStates.data(),
                    sizeof(kDynamicStates)) != 0) {
      return std::nullopt;
    }
    NativePipelineRecipe recipe;
    recipe.layout = info.layout;
    auto& out = recipe.data;
    out.stage_count = info.stageCount;
    out.specialization = specialization;
    if (vertex_fetch_formats) {
      if (!info.pStages[0].pSpecializationInfo) {
        return std::nullopt;
      }
      out.vertex_fetch_formats_enabled = VK_TRUE;
      out.vertex_fetch_formats = *vertex_fetch_formats;
    }
    for (uint32_t i = 0; i < info.stageCount; ++i) {
      const auto& stage = info.pStages[i];
      if (stage.pNext || stage.flags || !stage.pName || std::strcmp(stage.pName, "shaderMain") ||
          stage.stage != (i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT)) {
        return std::nullopt;
      }
      if (stage.pSpecializationInfo && !(i == 0 && vertex_fetch_formats) &&
          (stage.pSpecializationInfo->mapEntryCount != 1 ||
           !stage.pSpecializationInfo->pMapEntries || !stage.pSpecializationInfo->pData ||
           stage.pSpecializationInfo->dataSize != sizeof(specialization) ||
           stage.pSpecializationInfo->pMapEntries[0].constantID != 0 ||
           stage.pSpecializationInfo->pMapEntries[0].offset != 0 ||
           stage.pSpecializationInfo->pMapEntries[0].size != sizeof(specialization) ||
           std::memcmp(stage.pSpecializationInfo->pData, &specialization, sizeof(specialization)))) {
        return std::nullopt;
      }
      recipe.modules[i] = stage.module;
      out.shaders[i].specialization_enabled = stage.pSpecializationInfo != nullptr;
    }
    out.binding_count = vertex.vertexBindingDescriptionCount;
    out.attribute_count = vertex.vertexAttributeDescriptionCount;
    out.color_count = rendering.colorAttachmentCount;
    if (out.binding_count) {
      std::copy_n(vertex.pVertexBindingDescriptions, out.binding_count, out.bindings.begin());
    }
    if (out.attribute_count) {
      std::copy_n(vertex.pVertexAttributeDescriptions, out.attribute_count, out.attributes.begin());
    }
    if (out.color_count) {
      std::copy_n(rendering.pColorAttachmentFormats, out.color_count, out.color_formats.begin());
      std::copy_n(blend.pAttachments, out.color_count, out.color_attachments.begin());
    }
    out.depth_format = rendering.depthAttachmentFormat;
    out.stencil_format = rendering.stencilAttachmentFormat;
    out.topology = info.pInputAssemblyState->topology;
    out.primitive_restart = info.pInputAssemblyState->primitiveRestartEnable;
    if (info.pViewportState->pNext) {
      const auto& clip = *static_cast<const VkPipelineViewportDepthClipControlCreateInfoEXT*>(
          info.pViewportState->pNext);
      if (clip.sType != VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT ||
          clip.pNext) {
        return std::nullopt;
      }
      out.negative_one_to_one = clip.negativeOneToOne;
    }
    out.rasterization = *info.pRasterizationState;
    out.depth_stencil = *info.pDepthStencilState;
    // These fields are dynamic, so canonicalize them before persistence.
    out.depth_stencil.front.compareMask = out.depth_stencil.front.writeMask =
        out.depth_stencil.front.reference = 0;
    out.depth_stencil.back.compareMask = out.depth_stencil.back.writeMask =
        out.depth_stencil.back.reference = 0;
    out.samples = multisample.rasterizationSamples;
    out.sample_mask = *multisample.pSampleMask;
    return recipe.Valid() ? std::optional(std::move(recipe)) : std::nullopt;
  }

  VkResult Create(PFN_vkCreateGraphicsPipelines create, VkDevice device, VkPipelineCache cache,
                  VkPipeline* pipeline) const {
    if (!Valid() || !layout || !modules[0] || (data.stage_count == 2 && !modules[1])) {
      return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkSpecializationMapEntry entry{0, 0, sizeof(data.specialization)};
    VkSpecializationInfo specialization{1, &entry, sizeof(data.specialization), &data.specialization};
    NativeVertexFetchSpecialization vertex_fetch_specialization;
    if (data.vertex_fetch_formats_enabled) {
      vertex_fetch_specialization.Build(data.specialization, data.vertex_fetch_formats);
    }
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (uint32_t i = 0; i < data.stage_count; ++i) {
      stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
      stages[i].module = modules[i];
      stages[i].pName = "shaderMain";
      stages[i].pSpecializationInfo = data.shaders[i].specialization_enabled ? &specialization : nullptr;
    }
    if (data.vertex_fetch_formats_enabled) {
      stages[0].pSpecializationInfo = &vertex_fetch_specialization.info;
    }
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = data.binding_count;
    vertex.pVertexBindingDescriptions = data.bindings.data();
    vertex.vertexAttributeDescriptionCount = data.attribute_count;
    vertex.pVertexAttributeDescriptions = data.attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = data.topology;
    assembly.primitiveRestartEnable = data.primitive_restart;
    VkPipelineViewportDepthClipControlCreateInfoEXT clip{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    clip.negativeOneToOne = data.negative_one_to_one;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.pNext = data.negative_one_to_one ? &clip : nullptr;
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = data.samples;
    multisample.pSampleMask = &data.sample_mask;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = data.color_count;
    blend.pAttachments = data.color_attachments.data();
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = uint32_t(kDynamicStates.size());
    dynamic.pDynamicStates = kDynamicStates.data();
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = data.color_count;
    rendering.pColorAttachmentFormats = data.color_formats.data();
    rendering.depthAttachmentFormat = data.depth_format;
    rendering.stencilAttachmentFormat = data.stencil_format;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = data.stage_count;
    info.pStages = stages.data();
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &data.rasterization;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &data.depth_stencil;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    if (g_capture_pipeline_statistics.load(std::memory_order_relaxed))
      info.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    return create(device, cache, 1, &info, nullptr, pipeline);
  }

 private:
  static constexpr std::array<VkDynamicState, 7> kDynamicStates = {
      VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
      VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
      VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
};

}  // namespace rex::graphics::xnative
