#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rex::graphics::xnative {

// Xcompiler (Halo 3): a vertex shader with memory export, rebuilt as a compute shader for GPUs
// without vertexPipelineStoresAndAtomics. The Mali-G615 (r44p1) does not offer it and drops the
// vertex shaders' stores: the export regions kept zeros and 0xCDCDCDCD fill, and what reads them
// (Halo 3's particles and light glows) drew giant white balls. The XcFetchVertex shaders read
// everything from memory by the vertex index, so one invocation per vertex reproduces the draw's
// exports. The outputs (position, interpolators, clip distance, point size) become private
// variables nobody reads. Returns false (error set) for a shape it does not handle, and the draw
// then stays a draw.
//
// One dispatch runs many draws (kMemExportComputeRecordBytes): the shader's push constants are a
// single address, of a table with one record per invocation, {vertex constants, pixel constants,
// shared constants, vertex index} (three uint64 and a uint32, 32 bytes). The prologue copies the
// record's three addresses into the old push constant block, now a private variable, so the rest
// of the shader reads its own draw's constants. Halo 3 dispatched ~120 particle updates a frame of
// 1-16 vertices each, and the Mali ran them one after another (~0.17 ms each, ~20 ms a frame);
// one dispatch runs them side by side. LocalSize is kMemExportComputeGroupSize (a Mali warp): the
// renderer pads the table with records whose addresses are a zero block (no fetch table, no
// export region table: those invocations read zeros and export nothing).
constexpr uint32_t kMemExportComputeGroupSize = 16;
constexpr uint32_t kMemExportComputeRecordBytes = 32;

inline bool ConvertMemExportVertexSpirvToCompute(const std::vector<uint32_t>& in,
                                                 std::vector<uint32_t>& out, std::string* error) {
  constexpr uint32_t kMagic = 0x07230203u;
  constexpr uint16_t kOpTypeInt = 21, kOpTypeVector = 23, kOpTypeStruct = 30,
                     kOpTypePointer = 32, kOpConstant = 43, kOpFunction = 54, kOpVariable = 59,
                     kOpLoad = 61, kOpStore = 62, kOpAccessChain = 65, kOpDecorate = 71,
                     kOpMemberDecorate = 72, kOpDecorationGroup = 73, kOpGroupDecorate = 74,
                     kOpGroupMemberDecorate = 75, kOpCompositeExtract = 81, kOpUConvert = 113,
                     kOpConvertUToPtr = 120, kOpBitcast = 124, kOpIAdd = 128, kOpIMul = 132,
                     kOpLabel = 248, kOpLine = 8, kOpNoLine = 317, kOpEntryPoint = 15,
                     kOpExecutionMode = 16, kOpCapability = 17, kOpExecutionModeId = 331,
                     kOpDecorateId = 332, kOpDecorateString = 5632, kOpMemberDecorateString = 5633;
  constexpr uint32_t kStorageInput = 1, kStorageOutput = 3, kStoragePrivate = 6,
                     kStoragePushConstant = 9, kStoragePhysicalStorageBuffer = 5349;
  constexpr uint32_t kDecorationBlock = 2, kDecorationBuiltIn = 11, kDecorationOffset = 35;
  constexpr uint32_t kBuiltInVertexIndex = 42, kBuiltInInstanceIndex = 43,
                     kBuiltInGlobalInvocationId = 28;
  constexpr uint32_t kModelVertex = 0, kModelGLCompute = 5, kModeLocalSize = 17;
  constexpr uint32_t kCapabilityClipDistance = 32, kCapabilityCullDistance = 33;
  constexpr uint32_t kMemoryAccessAligned = 2;
  const auto fail = [error](const char* reason) {
    if (error) *error = reason;
    return false;
  };
  if (in.size() < 5 || in[0] != kMagic) return fail("not SPIR-V");
  const uint32_t version = in[1];
  uint32_t bound = in[3];

  // Pass 1: the entry point, the interface variables, the builtins, the push constant block and
  // the types needed.
  size_t entry_index = 0;
  uint32_t entry_function = 0;
  uint32_t vertex_index_variable = 0, instance_index_variable = 0;
  uint32_t uint_type = 0, ulong_type = 0, v3uint_type = 0, uint_zero = 0;
  uint32_t push_variable = 0, push_pointer_type = 0;
  struct Pointer {
    uint32_t id, storage, pointee;
  };
  std::vector<Pointer> pointers;
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> structs;  // id -> member types
  std::vector<uint32_t> interface_variables;                       // global Input/Output variables
  std::vector<std::pair<uint32_t, uint32_t>> variable_type;        // variable id -> pointer type
  size_t first_function = 0;
  for (size_t i = 5; i < in.size();) {
    const uint32_t count = in[i] >> 16;
    const uint16_t op = uint16_t(in[i] & 0xFFFF);
    if (!count || i + count > in.size()) return fail("truncated instruction");
    switch (op) {
      case kOpEntryPoint:
        if (entry_index) return fail("several entry points");
        if (count < 4 || in[i + 1] != kModelVertex) return fail("not a vertex shader");
        entry_index = i;
        entry_function = in[i + 2];
        break;
      case kOpDecorate:
        if (count >= 4 && in[i + 2] == kDecorationBuiltIn) {
          if (in[i + 3] == kBuiltInVertexIndex) vertex_index_variable = in[i + 1];
          else if (in[i + 3] == kBuiltInInstanceIndex) instance_index_variable = in[i + 1];
          else if (in[i + 3] >= 4424 && in[i + 3] <= 4426) return fail("draw parameters");
        }
        break;
      case kOpTypeInt:
        if (count == 4 && in[i + 2] == 32 && in[i + 3] == 0 && !uint_type) uint_type = in[i + 1];
        if (count == 4 && in[i + 2] == 64 && in[i + 3] == 0 && !ulong_type) ulong_type = in[i + 1];
        break;
      case kOpTypeVector:
        if (count == 4 && uint_type && in[i + 2] == uint_type && in[i + 3] == 3 && !v3uint_type)
          v3uint_type = in[i + 1];
        break;
      case kOpTypeStruct:
        structs.emplace_back(in[i + 1], std::vector<uint32_t>(in.begin() + i + 2,
                                                              in.begin() + i + count));
        break;
      case kOpConstant:
        if (count == 4 && uint_type && in[i + 1] == uint_type && in[i + 3] == 0 && !uint_zero)
          uint_zero = in[i + 2];
        break;
      case kOpTypePointer:
        if (count == 4) pointers.push_back({in[i + 1], in[i + 2], in[i + 3]});
        break;
      case kOpVariable:
        if (!first_function && count >= 4 &&
            (in[i + 3] == kStorageInput || in[i + 3] == kStorageOutput)) {
          interface_variables.push_back(in[i + 2]);
          variable_type.emplace_back(in[i + 2], in[i + 1]);
        }
        if (!first_function && count >= 4 && in[i + 3] == kStoragePushConstant) {
          if (push_variable) return fail("several push constant blocks");
          push_variable = in[i + 2];
          push_pointer_type = in[i + 1];
        }
        break;
      case kOpFunction:
        if (!first_function) first_function = i;
        break;
      default:
        break;
    }
    i += count;
  }
  if (!entry_index || !entry_function || !first_function) return fail("no entry point");
  if (!vertex_index_variable) return fail("no gl_VertexIndex");
  if (!uint_type || !ulong_type) return fail("no 32/64-bit unsigned integer type");
  if (!push_variable) return fail("no push constant block");
  const auto pointee_of = [&](uint32_t pointer) -> uint32_t {
    for (const Pointer& entry : pointers)
      if (entry.id == pointer) return entry.pointee;
    return 0;
  };
  const auto pointee_of_variable = [&](uint32_t variable) -> uint32_t {
    for (const auto& [id, type] : variable_type)
      if (id == variable) return pointee_of(type);
    return 0;
  };
  const auto is_interface = [&](uint32_t id) {
    for (uint32_t variable : interface_variables)
      if (variable == id) return true;
    return false;
  };
  const uint32_t vertex_index_type = pointee_of_variable(vertex_index_variable);
  if (!vertex_index_type) return fail("gl_VertexIndex type");
  // XenosRecomp's PushConstants: VertexShaderConstants, PixelShaderConstants, SharedConstants.
  const uint32_t push_struct = pointee_of(push_pointer_type);
  bool push_layout = false;
  for (const auto& [id, members] : structs) {
    if (id != push_struct) continue;
    push_layout = members.size() == 3 && members[0] == ulong_type && members[1] == ulong_type &&
                  members[2] == ulong_type;
  }
  if (!push_layout) return fail("push constants are not three uint64 addresses");
  // A member pointer the shader already has (PushConstant uint64, made Private below).
  uint32_t private_ulong_pointer = 0;
  for (const Pointer& entry : pointers)
    if (entry.storage == kStoragePushConstant && entry.pointee == ulong_type)
      private_ulong_pointer = entry.id;

  const uint32_t new_v3uint = v3uint_type ? 0 : bound++;
  const uint32_t v3uint_id = v3uint_type ? v3uint_type : new_v3uint;
  const uint32_t new_zero = (instance_index_variable && !uint_zero) ? bound++ : 0;
  const uint32_t zero_id = uint_zero ? uint_zero : new_zero;
  const uint32_t input_pointer = bound++;
  const uint32_t invocation_variable = bound++;
  const uint32_t new_private_ulong_pointer = private_ulong_pointer ? 0 : bound++;
  const uint32_t member_pointer = private_ulong_pointer ? private_ulong_pointer
                                                        : new_private_ulong_pointer;
  const uint32_t table_struct = bound++, table_struct_pointer = bound++,
                 table_member_pointer = bound++, table_variable = bound++;
  const uint32_t psb_ulong_pointer = bound++, psb_uint_pointer = bound++;
  const uint32_t member_index[3] = {bound++, bound++, bound++};
  const uint32_t ulong_8 = bound++, ulong_16 = bound++, ulong_24 = bound++,
                 ulong_record = bound++;
  const uint32_t loaded_id = bound++, index_id = bound++, table_pointer = bound++,
                 table_id = bound++, index64_id = bound++, offset_id = bound++,
                 record_id = bound++;
  uint32_t field_address[4], field_pointer[4], field_value[4], member_destination[3];
  field_address[0] = record_id;
  for (uint32_t k = 1; k < 4; ++k) field_address[k] = bound++;
  for (uint32_t k = 0; k < 4; ++k) field_pointer[k] = bound++;
  for (uint32_t k = 0; k < 4; ++k) field_value[k] = bound++;
  for (uint32_t k = 0; k < 3; ++k) member_destination[k] = bound++;
  const uint32_t cast_id = vertex_index_type != uint_type ? bound++ : 0;
  const uint32_t instance_type =
      instance_index_variable ? pointee_of_variable(instance_index_variable) : 0;
  const uint32_t instance_cast = instance_index_variable && instance_type != uint_type ? bound++ : 0;

  // Pass 2: the compute module.
  out.clear();
  out.reserve(in.size() + 256);
  out.insert(out.end(), in.begin(), in.begin() + 5);
  out[3] = bound;
  bool in_entry_function = false, entry_prologue_done = false, decoration_written = false;
  bool annotations_written = false;
  for (size_t i = 5; i < in.size();) {
    const uint32_t count = in[i] >> 16;
    const uint16_t op = uint16_t(in[i] & 0xFFFF);
    const uint32_t* instruction = in.data() + i;
    i += count;
    if (op == kOpCapability &&
        (instruction[1] == kCapabilityClipDistance || instruction[1] == kCapabilityCullDistance)) {
      continue;
    }
    if (op == kOpEntryPoint) {
      // Name: literal string from word 3, up to and including the word holding its NUL.
      size_t name_end = 3;
      while (name_end < count) {
        const uint32_t word = instruction[name_end++];
        if (!(word & 0xFF000000u) || !(word & 0xFF0000u) || !(word & 0xFF00u) || !(word & 0xFFu))
          break;
      }
      // SPIR-V 1.4 and later list every global the entry point uses; 1.0-1.3 only Input/Output.
      const bool all_globals = version >= 0x00010400u;
      std::vector<uint32_t> entry_interface;
      entry_interface.push_back(invocation_variable);
      if (all_globals) {
        // The old interface (now Private variables), the new push constant block.
        entry_interface.insert(entry_interface.end(), instruction + name_end, instruction + count);
        entry_interface.push_back(table_variable);
      }
      const uint32_t new_count = uint32_t(name_end + entry_interface.size());
      out.push_back((new_count << 16) | kOpEntryPoint);
      out.push_back(kModelGLCompute);
      out.push_back(entry_function);
      out.insert(out.end(), instruction + 3, instruction + name_end);
      out.insert(out.end(), entry_interface.begin(), entry_interface.end());
      out.push_back((6u << 16) | kOpExecutionMode);
      out.push_back(entry_function);
      out.push_back(kModeLocalSize);
      out.push_back(kMemExportComputeGroupSize);
      out.push_back(1);
      out.push_back(1);
      continue;
    }
    if ((op == kOpExecutionMode || op == kOpExecutionModeId) && instruction[1] == entry_function) {
      continue;  // vertex execution modes (none from DXC)
    }
    const bool annotation = op == kOpDecorate || op == kOpMemberDecorate ||
                            op == kOpDecorationGroup || op == kOpGroupDecorate ||
                            op == kOpGroupMemberDecorate || op == kOpDecorateId ||
                            op == kOpDecorateString || op == kOpMemberDecorateString;
    if (annotation && !annotations_written) {
      // The record table's push constant block.
      out.insert(out.end(), {(3u << 16) | kOpDecorate, table_struct, kDecorationBlock});
      out.insert(out.end(),
                 {(5u << 16) | kOpMemberDecorate, table_struct, 0u, kDecorationOffset, 0u});
      annotations_written = true;
    }
    if (op == kOpDecorate && is_interface(instruction[1])) {
      if (instruction[1] == vertex_index_variable && !decoration_written) {
        out.push_back((4u << 16) | kOpDecorate);
        out.push_back(invocation_variable);
        out.push_back(kDecorationBuiltIn);
        out.push_back(kBuiltInGlobalInvocationId);
        decoration_written = true;
      }
      continue;
    }
    if (op == kOpMemberDecorate && count >= 4 && instruction[3] == kDecorationBuiltIn) {
      continue;  // a gl_PerVertex block's builtin members
    }
    // The old push constant block becomes a private variable: no explicit layout.
    if ((op == kOpDecorate || op == kOpMemberDecorate) && instruction[1] == push_struct) {
      continue;
    }
    if (op == kOpTypePointer && count == 4 &&
        (instruction[2] == kStorageInput || instruction[2] == kStorageOutput ||
         instruction[2] == kStoragePushConstant)) {
      out.insert(out.end(), instruction, instruction + count);
      out[out.size() - 2] = kStoragePrivate;
      continue;
    }
    if (op == kOpVariable && count >= 4 &&
        (is_interface(instruction[2]) || instruction[2] == push_variable)) {
      out.insert(out.end(), instruction, instruction + count);
      out[out.size() - count + 3] = kStoragePrivate;
      continue;
    }
    if (op == kOpFunction && instruction == in.data() + first_function) {
      if (new_v3uint) {
        out.insert(out.end(), {(4u << 16) | kOpTypeVector, new_v3uint, uint_type, 3u});
      }
      if (new_zero) {
        out.insert(out.end(), {(4u << 16) | kOpConstant, uint_type, new_zero, 0u});
      }
      if (new_private_ulong_pointer) {
        out.insert(out.end(), {(4u << 16) | kOpTypePointer, new_private_ulong_pointer,
                               kStoragePrivate, ulong_type});
      }
      out.insert(out.end(), {(4u << 16) | kOpTypePointer, input_pointer, kStorageInput, v3uint_id});
      out.insert(out.end(), {(4u << 16) | kOpVariable, input_pointer, invocation_variable,
                             kStorageInput});
      out.insert(out.end(), {(3u << 16) | kOpTypeStruct, table_struct, ulong_type});
      out.insert(out.end(), {(4u << 16) | kOpTypePointer, table_struct_pointer,
                             kStoragePushConstant, table_struct});
      out.insert(out.end(), {(4u << 16) | kOpTypePointer, table_member_pointer,
                             kStoragePushConstant, ulong_type});
      out.insert(out.end(), {(4u << 16) | kOpVariable, table_struct_pointer, table_variable,
                             kStoragePushConstant});
      out.insert(out.end(), {(4u << 16) | kOpTypePointer, psb_ulong_pointer,
                             kStoragePhysicalStorageBuffer, ulong_type});
      out.insert(out.end(), {(4u << 16) | kOpTypePointer, psb_uint_pointer,
                             kStoragePhysicalStorageBuffer, uint_type});
      for (uint32_t k = 0; k < 3; ++k) {
        out.insert(out.end(), {(4u << 16) | kOpConstant, uint_type, member_index[k], k});
      }
      out.insert(out.end(), {(5u << 16) | kOpConstant, ulong_type, ulong_8, 8u, 0u});
      out.insert(out.end(), {(5u << 16) | kOpConstant, ulong_type, ulong_16, 16u, 0u});
      out.insert(out.end(), {(5u << 16) | kOpConstant, ulong_type, ulong_24, 24u, 0u});
      out.insert(out.end(), {(5u << 16) | kOpConstant, ulong_type, ulong_record,
                             kMemExportComputeRecordBytes, 0u});
    }
    if (op == kOpFunction) {
      in_entry_function = instruction[2] == entry_function;
    }
    // The entry function's first block: after its OpLabel and OpVariables, this invocation's
    // record: the draw's constant addresses into the old push constant block, then the vertex.
    if (in_entry_function && !entry_prologue_done && op != kOpFunction && op != kOpLabel &&
        op != kOpVariable && op != kOpLine && op != kOpNoLine && op != 55 /* OpFunctionParameter */) {
      out.insert(out.end(), {(4u << 16) | kOpLoad, v3uint_id, loaded_id, invocation_variable});
      out.insert(out.end(), {(5u << 16) | kOpCompositeExtract, uint_type, index_id, loaded_id, 0u});
      out.insert(out.end(), {(5u << 16) | kOpAccessChain, table_member_pointer, table_pointer,
                             table_variable, member_index[0]});
      out.insert(out.end(), {(4u << 16) | kOpLoad, ulong_type, table_id, table_pointer});
      out.insert(out.end(), {(4u << 16) | kOpUConvert, ulong_type, index64_id, index_id});
      out.insert(out.end(), {(5u << 16) | kOpIMul, ulong_type, offset_id, index64_id, ulong_record});
      out.insert(out.end(), {(5u << 16) | kOpIAdd, ulong_type, record_id, table_id, offset_id});
      const uint32_t field_offset[4] = {0, ulong_8, ulong_16, ulong_24};
      for (uint32_t k = 1; k < 4; ++k) {
        out.insert(out.end(), {(5u << 16) | kOpIAdd, ulong_type, field_address[k], record_id,
                               field_offset[k]});
      }
      for (uint32_t k = 0; k < 3; ++k) {
        out.insert(out.end(), {(4u << 16) | kOpConvertUToPtr, psb_ulong_pointer, field_pointer[k],
                               field_address[k]});
        out.insert(out.end(), {(6u << 16) | kOpLoad, ulong_type, field_value[k], field_pointer[k],
                               kMemoryAccessAligned, 8u});
        out.insert(out.end(), {(5u << 16) | kOpAccessChain, member_pointer, member_destination[k],
                               push_variable, member_index[k]});
        out.insert(out.end(), {(3u << 16) | kOpStore, member_destination[k], field_value[k]});
      }
      out.insert(out.end(), {(4u << 16) | kOpConvertUToPtr, psb_uint_pointer, field_pointer[3],
                             field_address[3]});
      out.insert(out.end(), {(6u << 16) | kOpLoad, uint_type, field_value[3], field_pointer[3],
                             kMemoryAccessAligned, 4u});
      if (cast_id) {
        out.insert(out.end(), {(4u << 16) | kOpBitcast, vertex_index_type, cast_id, field_value[3]});
      }
      out.insert(out.end(), {(3u << 16) | kOpStore, vertex_index_variable,
                             cast_id ? cast_id : field_value[3]});
      if (instance_index_variable) {
        if (instance_cast) {
          out.insert(out.end(), {(4u << 16) | kOpBitcast, instance_type, instance_cast, zero_id});
        }
        out.insert(out.end(), {(3u << 16) | kOpStore, instance_index_variable,
                               instance_cast ? instance_cast : zero_id});
      }
      entry_prologue_done = true;
    }
    out.insert(out.end(), instruction, instruction + count);
  }
  if (!decoration_written) return fail("gl_VertexIndex decoration");
  if (!entry_prologue_done) return fail("entry function body");
  if (!annotations_written) return fail("no annotations");
  return true;
}

}  // namespace rex::graphics::xnative
