#pragma once

#include <cstdint>
#include <utility>

// A game's SPIR-V cache can hold a shader under the hash of another build of the same program; its
// games/<game>/data/shader/shader_aliases.inc lists {container hash, cached hash} pairs (GTA IV's
// has some, a new game none). The hash is replaced where the shader registers, so the hash-based
// families (kVideo, the TV trace) also see the cached one.
namespace rex::graphics::xnative {
inline constexpr std::pair<uint64_t, uint64_t> kShaderAliases[] = {
#if __has_include(<shader/shader_aliases.inc>)
#include <shader/shader_aliases.inc>
#endif
    {0, 0}};

constexpr uint64_t CanonicalShaderHash(uint64_t hash) {
  for (const auto& [from, to] : kShaderAliases) {
    if (hash == from) {
      return to;
    }
  }
  return hash;
}
}  // namespace rex::graphics::xnative
