#pragma once

#include <cstdint>
#include <vector>

namespace rex::graphics::xnative {

// Xcompiler (Halo 3, xnative_nan_safe_minmax): GLSL.std.450 FMin, FMax and FClamp have an
// undefined result when an operand is NaN, so each GPU picks its own (NVIDIA returns the number;
// a Mali may return the NaN). The translated Xenos max/min and the _sat modifier (HLSL saturate,
// FClamp(x, 0, 1)) then let a NaN through on the phone only, and Halo 3's bloom spreads it into a
// white ball. NMin, NMax and NClamp return the other operand (NClamp: the minimum), as D3D and the
// console do. Rewritten in place: same opcode, same operands. Returns the number of rewrites.
inline uint32_t RewriteSpirvNanSafeMinMax(std::vector<uint32_t>& words) {
  constexpr uint16_t kOpExtInstImport = 11, kOpExtInst = 12, kOpFunction = 54;
  constexpr uint32_t kFMin = 37, kFMax = 40, kFClamp = 43;
  constexpr uint32_t kNMin = 79, kNMax = 80, kNClamp = 81;
  if (words.size() < 5 || words[0] != 0x07230203u) return 0;
  uint32_t glsl_set = 0, rewritten = 0;
  for (size_t i = 5; i < words.size();) {
    const uint32_t word_count = words[i] >> 16;
    const uint16_t opcode = uint16_t(words[i] & 0xFFFF);
    if (!word_count || i + word_count > words.size()) return rewritten;
    if (opcode == kOpExtInstImport && word_count >= 3 && words[i + 2] == 0x4C534C47u) {
      // "GLSL.std.450": "GLSL" then ".std.450".
      if (word_count >= 5 && words[i + 3] == 0x6474732Eu && words[i + 4] == 0x3035342Eu)
        glsl_set = words[i + 1];
    } else if (opcode == kOpExtInst && glsl_set && word_count >= 5 && words[i + 3] == glsl_set) {
      uint32_t& instruction = words[i + 4];
      if (instruction == kFMin) instruction = kNMin, ++rewritten;
      else if (instruction == kFMax) instruction = kNMax, ++rewritten;
      else if (instruction == kFClamp) instruction = kNClamp, ++rewritten;
    } else if (opcode == kOpFunction && !glsl_set) {
      return rewritten;  // no GLSL.std.450 import before the code: nothing to rewrite
    }
    i += word_count;
  }
  return rewritten;
}

}  // namespace rex::graphics::xnative
