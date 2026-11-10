#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include <spirv/unified1/spirv.hpp11>

namespace rex::graphics::xnative {

// Vulkan lets a driver assume that floats are never NaN or infinite and ignore the sign of zero,
// unless the entry point declares SignedZeroInfNanPreserve ("Precision and Operation of SPIR-V
// Instructions"). NVIDIA keeps IEEE behaviour anyway; a mobile compiler may fold isnan/isinf to
// false and drop clamps of infinities, so the NaN guard of the colour epilogue and the translated
// Xenos ops (clamp(rcp(x), -FLT_MAX, FLT_MAX), xcInfToZero) can vanish there. Xenia's own SPIR-V
// translator declares the same mode (spirv_translator.cpp). Adds the capability, the extension
// for SPIR-V before 1.4 and the 32-bit execution mode for every entry point. Returns nullopt for a
// malformed module, one without an entry point, or one that already declares the mode.
inline std::optional<std::vector<uint32_t>> AddSpirvSignedZeroInfNanPreserve(
    std::span<const uint32_t> input) {
  constexpr uint32_t kCapability = uint32_t(spv::Capability::SignedZeroInfNanPreserve);
  constexpr uint32_t kMode = uint32_t(spv::ExecutionMode::SignedZeroInfNanPreserve);
  constexpr char kExtension[] = "SPV_KHR_float_controls";
  if (input.size() < 5 || input[0] != spv::MagicNumber) return std::nullopt;
  const auto string_equals = [&](size_t first, size_t count, const char* text) {
    const size_t length = std::strlen(text) + 1;
    if ((count - 1) * 4 < length) return false;
    return std::memcmp(&input[first], text, length) == 0;
  };
  std::vector<uint32_t> entries;
  bool has_capability = false, has_extension = false;
  for (size_t i = 5; i < input.size();) {
    const uint32_t count = input[i] >> 16;
    const auto op = spv::Op(input[i] & 0xFFFF);
    if (!count || i + count > input.size()) return std::nullopt;
    if (op == spv::Op::OpCapability && count >= 2 && input[i + 1] == kCapability) {
      has_capability = true;
    } else if (op == spv::Op::OpExtension && string_equals(i + 1, count, kExtension)) {
      has_extension = true;
    } else if (op == spv::Op::OpEntryPoint && count >= 3) {
      entries.push_back(input[i + 2]);
    } else if (op == spv::Op::OpExecutionMode && count >= 3 && input[i + 2] == kMode) {
      return std::nullopt;
    }
    i += count;
  }
  if (entries.empty()) return std::nullopt;

  std::vector<uint32_t> output(input.begin(), input.begin() + 5);
  output.reserve(input.size() + 16 + entries.size() * 4);
  bool capability_done = has_capability;
  bool extension_done = has_extension || input[1] >= 0x00010400u;
  bool entry_seen = false, modes_done = false;
  for (size_t i = 5; i < input.size();) {
    const uint32_t count = input[i] >> 16;
    const auto op = spv::Op(input[i] & 0xFFFF);
    // Logical layout: capabilities, extensions, imports, memory model, entry points, modes.
    if (!capability_done && op != spv::Op::OpCapability) {
      output.push_back((2u << 16) | uint32_t(spv::Op::OpCapability));
      output.push_back(kCapability);
      capability_done = true;
    }
    if (!extension_done && op != spv::Op::OpCapability && op != spv::Op::OpExtension) {
      const size_t words = (sizeof(kExtension) + 3) / 4;
      output.push_back(uint32_t((words + 1) << 16) | uint32_t(spv::Op::OpExtension));
      const size_t at = output.size();
      output.resize(at + words, 0);
      std::memcpy(&output[at], kExtension, sizeof(kExtension));
      extension_done = true;
    }
    if (entry_seen && !modes_done && op != spv::Op::OpEntryPoint &&
        op != spv::Op::OpExecutionMode && op != spv::Op::OpExecutionModeId) {
      for (const uint32_t entry : entries) {
        output.push_back((4u << 16) | uint32_t(spv::Op::OpExecutionMode));
        output.push_back(entry);
        output.push_back(kMode);
        output.push_back(32);
      }
      modes_done = true;
    }
    entry_seen |= op == spv::Op::OpEntryPoint;
    output.insert(output.end(), input.begin() + i, input.begin() + i + count);
    i += count;
  }
  if (!modes_done) return std::nullopt;
  return output;
}

}  // namespace rex::graphics::xnative
