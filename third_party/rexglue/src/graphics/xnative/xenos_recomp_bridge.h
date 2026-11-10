#pragma once

// XenosRecomp's HLSL front end for the runtime shader translator (shader_translator.cpp). Its
// library (rex_xenosrecomp) has its own fmt and a global be<>, so only these types cross over.

#include <cstdint>
#include <string>
#include <string_view>

namespace rex::graphics::xnative {

struct XenosRecompOutput {
  std::string hlsl;
  std::string error;
  bool pixel = false;
  uint32_t spec_constants_mask = 0;
  uint32_t used_texture_mask = 0;
  // Xcompiler (Halo 3): the vertex fetch slots a computed-fetch vertex shader reads (else 0).
  uint32_t vertex_fetch_slots[3] = {};
  // Xcompiler (Halo 3, 2026-11-10): the patch fetch plan (ShaderCacheEntry::patchPlan).
  uint32_t patch_plan[32] = {};
};

// container: a Xenos shader container as the game hands it to D3D (big-endian). include: the
// renderer's ABI header (tools/shaders/xenos_shader_common.h).
bool RecompileXenosShader(const uint8_t* container, std::string_view include,
                          XenosRecompOutput& output);

}  // namespace rex::graphics::xnative
