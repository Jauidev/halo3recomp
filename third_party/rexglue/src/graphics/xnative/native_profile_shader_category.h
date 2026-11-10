#pragma once
#include <bit>
#include <cstdint>
#include <string_view>
#include "native_performance_samples.h"

namespace rex::graphics::xnative::performance {
// Diagnostic fallback only. It never changes render phases, pipelines,
// culling or shader selection. The label explicitly describes shader families
// because the title may submit them outside a tagged render-phase scope.
//
// each rule reads one of the two filenames, so the rules are bits
// (in priority order) that a shader computes once from its own name; draws
// combine the two masks instead of searching both names (2026-10-26: the
// searches were ~2.8 ms of the recorder per frame).
inline bool ProfileShaderNameHas(std::string_view name, std::string_view family) {
  return name.find(family) != std::string_view::npos;
}
inline uint8_t ProfileShaderVertexRules(std::string_view vs) {
  const auto has = [vs](std::string_view family) { return ProfileShaderNameHas(vs, family); };
  uint8_t rules = 0;
  if (has("/shadowzdir/")) rules |= 1u << 0;
  if (has("/shadowz/") || has("/shadowzspot/")) rules |= 1u << 1;
  if (has("/gta_im/")) rules |= 1u << 4;
  if (has("/gta_default/") || has("/gta_normal") || has("/gta_spec/") ||
      has("/gta_vehicle") || has("/gta_ped/") || has("/gta_emissive"))
    rules |= 1u << 6;
  return rules;
}
inline uint8_t ProfileShaderPixelRules(std::string_view ps) {
  const auto has = [ps](std::string_view family) { return ProfileShaderNameHas(ps, family); };
  uint8_t rules = 0;
  if (has("/rage_postfx") || has("/gta_postfx")) rules |= 1u << 2;
  if (has("/sky") || has("/clouds")) rules |= 1u << 3;
  if (has("/gta_im/")) rules |= 1u << 4;
  if (has("/gta_particle") || has("/particle")) rules |= 1u << 5;
  return rules;
}
inline GpuRange ProfileShaderCategoryFromRules(uint8_t rules) {
  constexpr GpuRange kRanges[] = {
      GpuRange::kDirectionalShadowShaders, GpuRange::kLocalShadowShaders, GpuRange::kComposite,
      GpuRange::kSkyShaders, GpuRange::kImmediateShaders, GpuRange::kParticleShaders,
      GpuRange::kMaterialShaders};
  return rules ? kRanges[std::countr_zero(rules)] : GpuRange::kUnattributed;
}
inline GpuRange ProfileShaderCategory(std::string_view vs, std::string_view ps) {
  return ProfileShaderCategoryFromRules(ProfileShaderVertexRules(vs) | ProfileShaderPixelRules(ps));
}
}  // namespace rex::graphics::xnative::performance
