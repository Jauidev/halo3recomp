#include "xenos_recomp_bridge.h"

#include "shader_recompiler.h"

namespace rex::graphics::xnative {

bool RecompileXenosShader(const uint8_t* container, std::string_view include,
                          XenosRecompOutput& output) {
  ShaderRecompiler recompiler;
  try {
    recompiler.recompile(container, include);
  } catch (const std::exception& error) {
    output.error = error.what();
    return false;
  }
  output.hlsl = std::move(recompiler.out);
  output.pixel = recompiler.isPixelShader;
  output.spec_constants_mask = recompiler.specConstantsMask;
  output.used_texture_mask = recompiler.usedTextureMask;
  for (int i = 0; i < 3; ++i) output.vertex_fetch_slots[i] = recompiler.vertexFetchSlots[i];
  static_assert(ShaderRecompiler::kPatchPlanCapacity == 32);
  for (int i = 0; i < 32; ++i) output.patch_plan[i] = recompiler.patchPlan[i];
  return true;
}

}  // namespace rex::graphics::xnative
