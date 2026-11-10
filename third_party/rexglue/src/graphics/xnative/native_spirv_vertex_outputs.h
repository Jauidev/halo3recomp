#pragma once
// Vertex shader variants without the outputs the pixel shader of a pipeline does not read.
//
// XenosRecomp's vertex shaders all write the same 23 interpolator locations (0-22), whatever
// pixel shader they are paired with; Halo 3's pixel shaders declare 2-14 of them (8 on average,
// measured over the 8432 cached shaders on 2026-11-09). On the Mali every written output is
// computed and stored to memory per vertex (IDVS varying shading): the Mali Offline Compiler gave
// a typical vertex shader 26 load/store cycles in its varying variant with 23 outputs, 10 with 8,
// and Streamline showed the GPU vertex-bound (vertex queue 70-76 %, ~2 GB/s of load/store writes).
//
// SpirvInterfaceLocationMask lists the locations of a stage's Input or Output variables.
// StripSpirvUnreadVertexOutputs turns the Output variables whose location is not in keep_mask
// into Private ones, like StripSpirvColorOutputs does for colors (native_spirv_colorless.h): the
// computation feeding them then feeds nothing and the driver drops it. Builtins (position, point
// size, clip distances) and outputs without a location are never touched.
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rex::graphics::xnative {

// The locations (bit L = location L) of the non-builtin variables of `storage` (1 Input, 3 Output)
// that carry a Location decoration. nullopt when one cannot be described by such a mask (not a
// scalar or vector, so it may span several locations, or a location above 63): callers then keep
// every output.
inline std::optional<uint64_t> SpirvInterfaceLocationMask(const std::vector<uint32_t>& in,
                                                          uint32_t storage) {
  constexpr uint16_t kOpTypeBool = 20, kOpTypeInt = 21, kOpTypeFloat = 22, kOpTypeVector = 23,
                     kOpTypePointer = 32, kOpVariable = 59, kOpDecorate = 71;
  constexpr uint32_t kDecorationBuiltIn = 11, kDecorationLocation = 30;
  if (in.size() < 5 || in[0] != 0x07230203u) return std::nullopt;
  std::unordered_set<uint32_t> simple_types;  // scalars and vectors: one location
  std::unordered_map<uint32_t, uint32_t> pointer_pointee;
  std::unordered_map<uint32_t, uint32_t> locations;
  std::unordered_set<uint32_t> builtins;
  struct Variable { uint32_t id, type; };
  std::vector<Variable> variables;
  for (size_t i = 5; i < in.size();) {
    const uint32_t count = in[i] >> 16;
    const uint16_t op = uint16_t(in[i] & 0xFFFF);
    if (!count || i + count > in.size()) return std::nullopt;
    const uint32_t* w = in.data() + i;
    if ((op == kOpTypeBool || op == kOpTypeInt || op == kOpTypeFloat || op == kOpTypeVector) &&
        count >= 2)
      simple_types.insert(w[1]);
    if (op == kOpTypePointer && count == 4 && w[2] == storage) pointer_pointee[w[1]] = w[3];
    if (op == kOpDecorate && count >= 4 && w[2] == kDecorationLocation) locations[w[1]] = w[3];
    if (op == kOpDecorate && count >= 3 && w[2] == kDecorationBuiltIn) builtins.insert(w[1]);
    if (op == kOpVariable && count >= 4 && w[3] == storage) variables.push_back({w[2], w[1]});
    i += count;
  }
  uint64_t mask = 0;
  for (const Variable& variable : variables) {
    if (builtins.count(variable.id)) continue;
    const auto location = locations.find(variable.id);
    if (location == locations.end()) continue;
    const auto pointee = pointer_pointee.find(variable.type);
    if (pointee == pointer_pointee.end() || !simple_types.count(pointee->second) ||
        location->second >= 64)
      return std::nullopt;
    mask |= uint64_t(1) << location->second;
  }
  return mask;
}

// nullopt: nothing to strip, or an instruction this pass does not handle uses one of them (the
// shader is then used as it is).
inline std::optional<std::vector<uint32_t>> StripSpirvUnreadVertexOutputs(
    const std::vector<uint32_t>& in, uint64_t keep_mask) {
  constexpr uint16_t kOpEntryPoint = 15, kOpTypePointer = 32, kOpFunctionParameter = 55,
                     kOpVariable = 59, kOpAccessChain = 65, kOpInBoundsAccessChain = 66,
                     kOpDecorate = 71;
  constexpr uint32_t kOutput = 3, kPrivate = 6, kDecorationBuiltIn = 11,
                     kDecorationLocation = 30, kDecorationComponent = 31, kDecorationIndex = 32;
  // Interpolation and interface decorations, invalid on Private variables.
  constexpr uint32_t kDecorationNoPerspective = 13, kDecorationFlat = 14, kDecorationPatch = 15,
                     kDecorationCentroid = 16, kDecorationSample = 17, kDecorationInvariant = 18;
  if (in.size() < 5 || in[0] != 0x07230203u) return std::nullopt;
  const uint32_t version = in[1];

  struct PointerType { uint32_t storage, pointee; };
  std::unordered_map<uint32_t, PointerType> pointer_types;
  std::unordered_set<uint32_t> builtin_ids;
  std::unordered_map<uint32_t, uint32_t> locations;
  std::unordered_set<uint32_t> converted;  // variables and access chains into them
  std::unordered_set<uint32_t> needed_types;
  for (size_t i = 5; i < in.size();) {
    const uint32_t count = in[i] >> 16;
    const uint16_t op = uint16_t(in[i] & 0xFFFF);
    if (!count || i + count > in.size()) return std::nullopt;
    const uint32_t* w = in.data() + i;
    if (op == kOpTypePointer && count == 4) pointer_types[w[1]] = {w[2], w[3]};
    if (op == kOpDecorate && count >= 3 && w[2] == kDecorationBuiltIn) builtin_ids.insert(w[1]);
    if (op == kOpDecorate && count >= 4 && w[2] == kDecorationLocation) locations[w[1]] = w[3];
    if (op == kOpVariable && count >= 4 && w[3] == kOutput && !builtin_ids.count(w[2])) {
      // Annotations come before the variables in a module, so the location is known here.
      const auto location = locations.find(w[2]);
      if (location != locations.end() && location->second < 64 &&
          !((keep_mask >> location->second) & 1)) {
        converted.insert(w[2]);
        needed_types.insert(w[1]);
      }
    }
    if ((op == kOpAccessChain || op == kOpInBoundsAccessChain) && count >= 4 &&
        converted.count(w[3])) {
      converted.insert(w[2]);
      needed_types.insert(w[1]);
    }
    i += count;
  }
  if (converted.empty()) return std::nullopt;
  // Any other instruction that can take a pointer (function arguments, copies, pointer casts,
  // atomics) is not handled: keep the shader as it is. Only those opcodes are checked, since
  // literals elsewhere may equal ids.
  for (size_t i = 5; i < in.size();) {
    const uint32_t count = in[i] >> 16;
    const uint16_t op = uint16_t(in[i] & 0xFFFF);
    const uint32_t* w = in.data() + i;
    const bool pointer_operands =
        op == 57 /*FunctionCall*/ || op == 63 /*CopyMemory*/ || op == 64 /*CopyMemorySized*/ ||
        op == 67 /*PtrAccessChain*/ || op == 68 /*ArrayLength*/ || op == 83 /*CopyObject*/ ||
        op == 117 /*ConvertPtrToU*/ || op == 124 /*Bitcast*/ || op == 169 /*Select*/ ||
        (op >= 227 && op <= 242) /*atomics*/ || op == 245 /*Phi*/ || op == 401 /*PtrEqual*/ ||
        op == 402 /*PtrNotEqual*/ || op == 403 /*PtrDiff*/;
    if (pointer_operands) {
      for (uint32_t k = 1; k < count; ++k)
        if (converted.count(w[k])) return std::nullopt;
    }
    if (op == 12 /*ExtInst*/) {
      for (uint32_t k = 5; k < count; ++k)
        if (converted.count(w[k])) return std::nullopt;
    }
    if (op == kOpFunctionParameter && count >= 3) {
      const auto type = pointer_types.find(w[1]);
      if (type != pointer_types.end() && type->second.storage == kOutput) return std::nullopt;
    }
    i += count;
  }
  uint32_t bound = in[3];
  std::unordered_map<uint32_t, uint32_t> private_types;
  for (uint32_t type : needed_types) {
    const auto found = pointer_types.find(type);
    if (found == pointer_types.end() || found->second.storage != kOutput) return std::nullopt;
    private_types[type] = bound++;
  }

  std::vector<uint32_t> out;
  out.reserve(in.size() + private_types.size() * 4);
  out.insert(out.end(), in.begin(), in.begin() + 5);
  out[3] = bound;
  for (size_t i = 5; i < in.size();) {
    const uint32_t count = in[i] >> 16;
    const uint16_t op = uint16_t(in[i] & 0xFFFF);
    const uint32_t* w = in.data() + i;
    if (op == kOpEntryPoint && version < 0x00010400u) {
      // Before SPIR-V 1.4 the interface lists only Input and Output.
      size_t name_end = 3;
      while (name_end < count && (w[name_end] >> 24) != 0) ++name_end;
      ++name_end;  // word holding the string terminator
      std::vector<uint32_t> entry(w, w + std::min<size_t>(name_end, count));
      for (size_t k = name_end; k < count; ++k)
        if (!converted.count(w[k])) entry.push_back(w[k]);
      entry[0] = uint32_t(entry.size()) << 16 | kOpEntryPoint;
      out.insert(out.end(), entry.begin(), entry.end());
    } else if (op == kOpDecorate && count >= 3 && converted.count(w[1]) &&
               (w[2] == kDecorationLocation || w[2] == kDecorationComponent ||
                w[2] == kDecorationIndex || w[2] == kDecorationNoPerspective ||
                w[2] == kDecorationFlat || w[2] == kDecorationPatch ||
                w[2] == kDecorationCentroid || w[2] == kDecorationSample ||
                w[2] == kDecorationInvariant)) {
      // Interface decorations are invalid on Private variables.
    } else if (op == kOpVariable && count >= 4 && converted.count(w[2])) {
      out.insert(out.end(), w, w + count);
      out[out.size() - count + 1] = private_types.at(w[1]);
      out[out.size() - count + 3] = kPrivate;
    } else if ((op == kOpAccessChain || op == kOpInBoundsAccessChain) && count >= 4 &&
               converted.count(w[2])) {
      out.insert(out.end(), w, w + count);
      out[out.size() - count + 1] = private_types.at(w[1]);
    } else {
      out.insert(out.end(), w, w + count);
      if (op == kOpTypePointer && count == 4) {
        if (const auto found = private_types.find(w[1]); found != private_types.end())
          out.insert(out.end(), {uint32_t(4) << 16 | kOpTypePointer, found->second, kPrivate, w[3]});
      }
    }
    i += count;
  }
  return out;
}

}  // namespace rex::graphics::xnative
