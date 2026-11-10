#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rex {
namespace ui {
namespace vulkan {

// for devices without shaderInt64 (Qualcomm's own Adreno drivers, 2026-10-29). The
// recompiled shaders keep their constant buffer addresses in 64-bit integers (three uint64_t push
// constants, read with vk::RawBufferLoad(address + offset)) and test bool constants with 64-bit
// masks. This rewrites every 64-bit integer value as a uvec2 (x = low word, y = high word): additions
// and multiplications become 32-bit ones with carry, conversions build or drop the high word, and an
// address becomes a pointer by an OpBitcast from the uvec2 (SPIR-V 1.5; older modules are raised to
// 1.5 and get every global variable in their entry point interfaces, as 1.4 requires). The memory
// layout does not change: a uvec2 holds the same 8 bytes as a uint64_t, so the push constants and
// buffers the shaders read stay as they are.
// Returns false when the module has no 64-bit integers, or uses them in a way this does not cover;
// `failure` then names the reason (empty when there was nothing to lower).
bool LowerSpirvInt64(const uint32_t* code, size_t word_count, std::vector<uint32_t>& lowered,
                     std::string* failure = nullptr);

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
