/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/vulkan/vulkan_device.h"

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#else
#include <dlfcn.h>
#include <unwind.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace xe {
namespace ui {
namespace vulkan {

// Arm erratum 3843237 (Mali drivers r18p0 to r53p0, documented by
// MaliBridge as MK-03): an integer vector bitwise operation or shift whose
// operand is a swizzle or a vector built lane by lane is miscompiled. Xenia's
// shaders are full of those (EDRAM addressing, float24 depth packing in the
// render target transfer and host depth store shaders). Every integer vector
// OpShiftRightLogical to OpNot is rewritten as one scalar operation per lane
// joined by an OpCompositeConstruct, in every module given to the driver.
namespace {

PFN_vkCreateShaderModule xport_driver_create_shader_module = nullptr;

// Opcodes whose first two operands are <result type> <result id>.
bool XportSpirvHasResultType(uint32_t opcode) {
  return opcode == 1 || opcode == 12 || (opcode >= 41 && opcode <= 52) ||
         opcode == 57 || opcode == 59 || opcode == 60 || opcode == 61 ||
         (opcode >= 65 && opcode <= 70) || (opcode >= 77 && opcode <= 84) ||
         (opcode >= 86 && opcode <= 107 && opcode != 99) ||
         (opcode >= 109 && opcode <= 124) || (opcode >= 126 && opcode <= 205) ||
         (opcode >= 207 && opcode <= 215) || (opcode >= 227 && opcode <= 242) ||
         opcode == 245;
}

// Returns the rewritten module, or an empty vector if nothing had to change.
std::vector<uint32_t> XportScalarizeIntegerVectorBitwise(const uint32_t* code,
                                                         size_t word_count) {
  constexpr uint32_t kOpTypeInt = 21, kOpTypeVector = 23,
                     kOpCompositeConstruct = 80, kOpCompositeExtract = 81,
                     kOpShiftRightLogical = 194, kOpNot = 200;
  if (word_count < 5 || code[0] != 0x07230203) {
    return {};
  }
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> vector_types;
  std::unordered_map<uint32_t, bool> int_types;
  std::unordered_map<uint32_t, uint32_t> value_types;
  for (size_t i = 5; i < word_count;) {
    const uint32_t words = code[i] >> 16, opcode = code[i] & 0xFFFF;
    if (!words || i + words > word_count) {
      return {};
    }
    if (opcode == kOpTypeInt && words >= 2) {
      int_types[code[i + 1]] = true;
    } else if (opcode == kOpTypeVector && words >= 4) {
      vector_types[code[i + 1]] = {code[i + 2], code[i + 3]};
    } else if (words >= 3 && XportSpirvHasResultType(opcode)) {
      value_types[code[i + 2]] = code[i + 1];
    }
    i += words;
  }
  // The integer vector type of a value, or nullptr.
  auto int_vector_of = [&](uint32_t type) -> const std::pair<uint32_t, uint32_t>* {
    auto it = vector_types.find(type);
    if (it == vector_types.end() || !int_types.count(it->second.first)) {
      return nullptr;
    }
    return &it->second;
  };

  std::vector<uint32_t> out(code, code + 5);
  out.reserve(word_count + word_count / 4);
  uint32_t bound = code[3];
  bool changed = false;
  for (size_t i = 5; i < word_count;) {
    const uint32_t words = code[i] >> 16, opcode = code[i] & 0xFFFF;
    const uint32_t operand_count = opcode == kOpNot ? 1 : 2;
    const std::pair<uint32_t, uint32_t>* result_vector = nullptr;
    const std::pair<uint32_t, uint32_t>* operand_vectors[2] = {};
    bool rewrite = opcode >= kOpShiftRightLogical && opcode <= kOpNot &&
                   words == 3 + operand_count &&
                   (result_vector = int_vector_of(code[i + 1])) != nullptr;
    for (uint32_t j = 0; rewrite && j < operand_count; ++j) {
      auto type = value_types.find(code[i + 3 + j]);
      operand_vectors[j] =
          type != value_types.end() ? int_vector_of(type->second) : nullptr;
      rewrite = operand_vectors[j] &&
                operand_vectors[j]->second == result_vector->second;
    }
    if (!rewrite) {
      out.insert(out.end(), code + i, code + i + words);
      i += words;
      continue;
    }
    const uint32_t lane_count = result_vector->second;
    std::vector<uint32_t> lanes;
    for (uint32_t lane = 0; lane < lane_count; ++lane) {
      uint32_t lane_operands[2];
      for (uint32_t j = 0; j < operand_count; ++j) {
        lane_operands[j] = bound++;
        out.insert(out.end(), {(5u << 16) | kOpCompositeExtract,
                               operand_vectors[j]->first, lane_operands[j],
                               code[i + 3 + j], lane});
      }
      const uint32_t lane_result = bound++;
      out.push_back(((3 + operand_count) << 16) | opcode);
      out.push_back(result_vector->first);
      out.push_back(lane_result);
      out.insert(out.end(), lane_operands, lane_operands + operand_count);
      lanes.push_back(lane_result);
    }
    out.push_back(((3 + lane_count) << 16) | kOpCompositeConstruct);
    out.push_back(code[i + 1]);
    out.push_back(code[i + 2]);
    out.insert(out.end(), lanes.begin(), lanes.end());
    changed = true;
    i += words;
  }
  if (!changed) {
    return {};
  }
  out[3] = bound;
  return out;
}

// Arm erratum 3779215 (Mali drivers r38p1 to r49p0 and r50p0 to
// r51p0, MaliBridge's MK-32): a GLSL.std.450 min / max / clamp with a
// constant zero limit may be miscompiled into a plain min or max. Every such
// zero operand is replaced by a load of a Private variable that each entry
// point sets to that zero first, which the compiler can't fold.
std::vector<uint32_t> XportHideZeroLimits(const uint32_t* code,
                                         size_t word_count) {
  constexpr uint32_t kOpExtInstImport = 11, kOpExtInst = 12,
                     kOpEntryPoint = 15, kOpTypePointer = 32,
                     kOpConstant = 43, kOpConstantComposite = 44,
                     kOpConstantNull = 46, kOpFunction = 54, kOpLabel = 248,
                     kOpVariable = 59, kOpLoad = 61, kOpStore = 62,
                     kStorageClassPrivate = 6;
  if (word_count < 5 || code[0] != 0x07230203) {
    return {};
  }
  uint32_t glsl_std_450 = UINT32_MAX;
  std::unordered_map<uint32_t, uint32_t> zero_constants;  // id -> type
  std::unordered_map<uint32_t, uint32_t> private_pointers;  // type -> pointer
  std::vector<uint32_t> entry_functions;
  std::unordered_map<uint32_t, uint32_t> used_zeros;  // zero id -> variable
  uint32_t bound = code[3];
  for (size_t i = 5; i < word_count;) {
    const uint32_t words = code[i] >> 16, opcode = code[i] & 0xFFFF;
    if (!words || i + words > word_count) {
      return {};
    }
    if (opcode == kOpExtInstImport && words >= 3 &&
        !std::strcmp(reinterpret_cast<const char*>(&code[i + 2]),
                     "GLSL.std.450")) {
      glsl_std_450 = code[i + 1];
    } else if (opcode == kOpEntryPoint && words >= 3) {
      entry_functions.push_back(code[i + 2]);
    } else if (opcode == kOpTypePointer && words == 4 &&
               code[i + 2] == kStorageClassPrivate) {
      private_pointers.emplace(code[i + 3], code[i + 1]);
    } else if (opcode == kOpConstant && words >= 4) {
      bool zero = true;
      for (uint32_t j = 3; j < words; ++j) {
        zero &= code[i + j] == 0;
      }
      if (zero) {
        zero_constants[code[i + 2]] = code[i + 1];
      }
    } else if (opcode == kOpConstantNull && words == 3) {
      zero_constants[code[i + 2]] = code[i + 1];
    } else if (opcode == kOpConstantComposite && words >= 4) {
      bool zero = true;
      for (uint32_t j = 3; j < words; ++j) {
        zero &= zero_constants.count(code[i + j]) != 0;
      }
      if (zero) {
        zero_constants[code[i + 2]] = code[i + 1];
      }
    } else if (opcode == kOpExtInst && words >= 6 &&
               code[i + 3] == glsl_std_450) {
      // FMin 37 ... SClamp 45, NMin 79 ... NClamp 81.
      const uint32_t instruction = code[i + 4];
      if ((instruction >= 37 && instruction <= 45) ||
          (instruction >= 79 && instruction <= 81)) {
        for (uint32_t j = 5; j < words; ++j) {
          if (zero_constants.count(code[i + j])) {
            used_zeros.emplace(code[i + j], 0);
          }
        }
      }
    }
    i += words;
  }
  if (used_zeros.empty() || entry_functions.empty()) {
    return {};
  }
  for (auto& [zero, variable] : used_zeros) {
    variable = bound++;
  }

  const bool interface_lists_globals = code[1] >= 0x00010400;
  std::vector<uint32_t> out(code, code + 5);
  out.reserve(word_count + used_zeros.size() * 16);
  bool globals_emitted = false;
  // In an entry point: waiting for its first block's non-variable code.
  bool store_pending = false;
  for (size_t i = 5; i < word_count;) {
    const uint32_t words = code[i] >> 16, opcode = code[i] & 0xFFFF;
    if (opcode == kOpFunction && !globals_emitted) {
      // The end of the global section: the pointer types and the variables.
      for (const auto& [zero, variable] : used_zeros) {
        const uint32_t type = zero_constants[zero];
        auto pointer = private_pointers.find(type);
        if (pointer == private_pointers.end()) {
          pointer = private_pointers.emplace(type, bound++).first;
          out.insert(out.end(), {(4u << 16) | kOpTypePointer, pointer->second,
                                 kStorageClassPrivate, type});
        }
        out.insert(out.end(), {(4u << 16) | kOpVariable, pointer->second,
                               variable, kStorageClassPrivate});
      }
      globals_emitted = true;
    }
    if (opcode == kOpFunction && words >= 3) {
      store_pending = std::find(entry_functions.begin(), entry_functions.end(),
                                code[i + 2]) != entry_functions.end();
    }
    if (store_pending && opcode != kOpLabel && opcode != kOpFunction &&
        opcode != 55 /* OpFunctionParameter */ && opcode != kOpVariable &&
        opcode != 8 /* OpLine */ && opcode != 317 /* OpNoLine */) {
      for (const auto& [zero, variable] : used_zeros) {
        out.insert(out.end(), {(3u << 16) | kOpStore, variable, zero});
      }
      store_pending = false;
    }
    if (opcode == kOpEntryPoint && interface_lists_globals) {
      out.push_back(((words + uint32_t(used_zeros.size())) << 16) |
                    kOpEntryPoint);
      out.insert(out.end(), code + i + 1, code + i + words);
      for (const auto& [zero, variable] : used_zeros) {
        out.push_back(variable);
      }
      i += words;
      continue;
    }
    if (opcode == kOpExtInst && words >= 6 && code[i + 3] == glsl_std_450 &&
        ((code[i + 4] >= 37 && code[i + 4] <= 45) ||
         (code[i + 4] >= 79 && code[i + 4] <= 81))) {
      std::vector<uint32_t> instruction(code + i, code + i + words);
      for (uint32_t j = 5; j < words; ++j) {
        auto used = used_zeros.find(code[i + j]);
        if (used != used_zeros.end()) {
          const uint32_t loaded = bound++;
          out.insert(out.end(), {(4u << 16) | kOpLoad,
                                 zero_constants[code[i + j]], loaded,
                                 used->second});
          instruction[j] = loaded;
        }
      }
      out.insert(out.end(), instruction.begin(), instruction.end());
      i += words;
      continue;
    }
    out.insert(out.end(), code + i, code + i + words);
    i += words;
  }
  out[3] = bound;
  return out;
}

// XPORT_XENIA_FP16 (see XportCreateShaderModule): in the pixel shaders
// translated from the game, decorates 32-bit float arithmetic results
// (OpFNegate...OpDot, GLSL.std.450 extended instructions, OpSelect, OpPhi)
// with RelaxedPrecision, so the Mali can run them at 16 bits (twice the
// arithmetic rate), except the components of values that something
// precision-critical needs. Those are found by walking the data flow
// backwards, per vector component, from:
// - the coordinates, gradients, LOD and offsets of image instructions (Xenia
//   computes them from the bits of the fetch constants);
// - float to integer conversions and bitcasts (addressing, format emulation);
// - derivatives, and stores to built-in outputs (depth, sample mask);
// - everything computed from gl_FragCoord (deferred passes rebuild positions
//   from it and the sampled depth).
// Float comparisons are not seeds: Xenia compares every multiplication operand
// with 0 (Xenos returns 0 for 0 * infinity), which works the same at 16 bits.
// The Xenos compiler reuses registers for unrelated values, so values are
// followed through Xenia's register array (xe_var_registers) with reaching
// definitions per element and component over the control flow graph.
// tools/perf/fp16_precision.py is the Python prototype (same results).
// Returns the rewritten module (empty if nothing changes); relaxed_count gets
// the number of decorated results out of candidate_count.
std::vector<uint32_t> XportRelaxFragmentPrecision(const uint32_t* code,
                                                 size_t word_count,
                                                 uint32_t& relaxed_count,
                                                 uint32_t& candidate_count) {
  // Opcodes.
  constexpr uint32_t kSourceExtension = 4, kExtInst = 12, kEntryPoint = 15,
                     kTypeBool = 20, kTypeInt = 21, kTypeFloat = 22,
                     kTypeVector = 23, kTypeArray = 28, kTypePointer = 32,
                     kConstant = 43, kFunctionCall = 57, kVariable = 59,
                     kLoad = 61, kStore = 62, kAccessChain = 65,
                     kInBoundsAccessChain = 66, kDecorate = 71,
                     kVectorShuffle = 79, kCompositeConstruct = 80,
                     kCompositeExtract = 81, kCompositeInsert = 82,
                     kCopyObject = 83, kImageWrite = 99, kConvertFToU = 109,
                     kConvertFToS = 110, kConvertSToF = 111, kConvertUToF = 112,
                     kFConvert = 115, kBitcast = 124, kVectorTimesScalar = 142,
                     kSelect = 169, kPhi = 245, kLabel = 248, kBranch = 249,
                     kBranchConditional = 250, kSwitch = 251;
  constexpr uint32_t kExecutionModelFragment = 4, kDecorationRelaxedPrecision = 0,
                     kDecorationBuiltIn = 11, kBuiltInFragCoord = 15,
                     kStorageClassInput = 1;
  // GLSL.std.450 instructions that work per component.
  static const std::vector<uint8_t> glsl_componentwise = [] {
    std::vector<uint8_t> table(82, 0);
    for (uint32_t op : {1u,  2u,  3u,  4u,  5u,  6u,  7u,  8u,  9u,  10u, 11u,
                        12u, 13u, 14u, 15u, 16u, 17u, 18u, 19u, 20u, 21u, 22u,
                        23u, 25u, 26u, 27u, 28u, 29u, 30u, 31u, 32u, 37u, 38u,
                        39u, 40u, 41u, 42u, 43u, 44u, 45u, 46u, 48u, 49u, 50u,
                        53u, 79u, 80u, 81u}) {
      table[op] = 1;
    }
    return table;
  }();
  constexpr uint32_t kNone = UINT32_MAX - 1, kAny = UINT32_MAX;
  relaxed_count = candidate_count = 0;
  if (word_count < 5 || code[0] != 0x07230203) {
    return {};
  }
  const uint32_t bound = code[3];
  if (bound > (1u << 22)) {
    return {};
  }
  bool guest = false, fragment = false;
  size_t first_type = 0;
  std::vector<uint32_t> definition(bound, 0);  // Offset of the defining instruction.
  std::vector<uint8_t> width(bound, 4);        // Components of a value / type.
  std::vector<uint8_t> is_float32(bound, 0);   // Float scalar / vector types and values.
  std::vector<uint8_t> builtin_frag_coord(bound, 0), builtin(bound, 0);
  std::vector<uint32_t> pointee(bound, 0), array_element(bound, 0);
  std::unordered_map<uint32_t, uint32_t> int_constants;
  struct Location {
    uint32_t var, element, component;
  };
  std::unordered_map<uint32_t, Location> locations;  // Pointer id -> location.
  std::vector<uint8_t> var_width(bound, 4), dynamic_var(bound, 0);
  std::vector<uint8_t> frag_coord_var(bound, 0);
  struct Store {
    Location location;
    uint32_t object;
  };
  std::vector<Store> stores;
  // Per block: events (store index, or load id | 0x80000000) and successors.
  struct Block {
    std::vector<uint32_t> events, successors;
  };
  std::vector<Block> blocks;
  std::unordered_map<uint32_t, uint32_t> block_of_label;
  std::vector<std::pair<uint32_t, uint32_t>> seeds;  // (id, component mask)
  std::vector<uint32_t> candidates;
  int32_t current_block = -1;
  auto block_index = [&](uint32_t label) {
    auto it = block_of_label.emplace(label, uint32_t(blocks.size()));
    if (it.second) {
      blocks.emplace_back();
    }
    return it.first->second;
  };

  for (size_t i = 5; i < word_count;) {
    const uint32_t words = code[i] >> 16, opcode = code[i] & 0xFFFF;
    if (!words || i + words > word_count) {
      return {};
    }
    const uint32_t* w = code + i;
    switch (opcode) {
      case kEntryPoint:
        if (words < 2 || w[1] != kExecutionModelFragment) {
          return {};
        }
        fragment = true;
        break;
      case kSourceExtension: {
        const char* text = reinterpret_cast<const char*>(w + 1);
        guest |= words >= 2 &&
                 strnlen(text, (words - 1) * sizeof(uint32_t)) == 14 &&
                 std::memcmp(text, "xport-guest", 14) == 0;
      } break;
      case kDecorate:
        if (words >= 4 && w[1] < bound && w[2] == kDecorationBuiltIn) {
          builtin[w[1]] = 1;
          builtin_frag_coord[w[1]] = w[3] == kBuiltInFragCoord;
        }
        break;
      case kTypeFloat:
        if (words >= 3 && w[1] < bound) {
          width[w[1]] = 1;
          is_float32[w[1]] = w[2] == 32;
        }
        break;
      case kTypeInt:
      case kTypeBool:
        if (words >= 2 && w[1] < bound) {
          width[w[1]] = 1;
        }
        break;
      case kTypeVector:
        if (words >= 4 && w[1] < bound && w[2] < bound) {
          width[w[1]] = uint8_t(std::min(w[3], 4u));
          is_float32[w[1]] = is_float32[w[2]];
        }
        break;
      case kTypeArray:
        if (words >= 3 && w[1] < bound) {
          array_element[w[1]] = w[2];
        }
        break;
      case kTypePointer:
        if (words >= 4 && w[1] < bound) {
          pointee[w[1]] = w[3];
        }
        break;
      default:
        break;
    }
    if (!first_type && opcode >= 19 && opcode <= 39) {
      first_type = i;
    }
    const bool has_result = words >= 3 && XportSpirvHasResultType(opcode) &&
                            w[1] < bound && w[2] < bound;
    if (has_result) {
      definition[w[2]] = uint32_t(i);
      width[w[2]] = width[w[1]];
      is_float32[w[2]] = is_float32[w[1]];
    }
    if (opcode == kLabel && words >= 2) {
      current_block = int32_t(block_index(w[1]));
    } else if (opcode == kConstant && has_result && words >= 4) {
      int_constants[w[2]] = w[3];
    } else if (opcode == kVariable && has_result && words >= 4) {
      uint32_t type = pointee[w[1]];
      if (type < bound && array_element[type]) {
        type = array_element[type];
      }
      var_width[w[2]] = type < bound ? width[type] : 4;
      frag_coord_var[w[2]] =
          w[3] == kStorageClassInput && builtin_frag_coord[w[2]];
      locations[w[2]] = {w[2], kNone, kNone};
    } else if ((opcode == kAccessChain || opcode == kInBoundsAccessChain) &&
               has_result && words >= 4) {
      Location location = {w[3], kAny, kNone};
      auto base = locations.find(w[3]);
      if (base != locations.end()) {
        location = base->second;
      }
      for (uint32_t index = 4; index < words; ++index) {
        auto constant = int_constants.find(w[index]);
        const uint32_t value =
            constant != int_constants.end() ? constant->second : kAny;
        if (value == kAny && location.var < bound) {
          dynamic_var[location.var] = 1;
        }
        if (location.element == kNone) {
          location.element = value;
        } else if (location.component == kNone) {
          location.component = value;
        }
      }
      locations[w[2]] = location;
    } else if (opcode == kLoad && has_result && current_block >= 0) {
      blocks[current_block].events.push_back(w[2] | 0x80000000u);
    } else if (opcode == kStore && words >= 3 && current_block >= 0) {
      auto location = locations.find(w[1]);
      if (location != locations.end()) {
        blocks[current_block].events.push_back(uint32_t(stores.size()));
        stores.push_back({location->second, w[2]});
        if (location->second.var < bound && builtin[location->second.var]) {
          seeds.push_back({w[2], 15});
        }
      }
    } else if ((opcode == kBranch || opcode == kBranchConditional ||
                opcode == kSwitch) &&
               current_block >= 0) {
      // Targets: OpBranch %label; OpBranchConditional %cond %true %false;
      // OpSwitch %selector %default (literal %label)... (32-bit selectors).
      // block_index may grow blocks, so the indices are taken first.
      std::vector<uint32_t> targets;
      if (opcode == kBranch && words >= 2) {
        targets.push_back(block_index(w[1]));
      } else if (opcode == kBranchConditional && words >= 4) {
        targets.push_back(block_index(w[2]));
        targets.push_back(block_index(w[3]));
      } else if (opcode == kSwitch && words >= 3) {
        targets.push_back(block_index(w[2]));
        for (uint32_t index = 4; index < words; index += 2) {
          targets.push_back(block_index(w[index]));
        }
      }
      auto& successors = blocks[current_block].successors;
      successors.insert(successors.end(), targets.begin(), targets.end());
    }
    // Seeds.
    const bool image = (opcode >= 87 && opcode <= 98) ||
                       (opcode >= 103 && opcode <= 107) ||
                       (opcode >= 305 && opcode <= 315);
    if (image && has_result) {
      for (uint32_t index = 4; index < words; ++index) {
        seeds.push_back({w[index], 15});
      }
    } else if (opcode == kImageWrite) {
      for (uint32_t index = 2; index < words; ++index) {
        seeds.push_back({w[index], 15});
      }
    } else if ((opcode == kConvertFToU || opcode == kConvertFToS ||
                opcode == kBitcast || (opcode >= 207 && opcode <= 215)) &&
               has_result && words >= 4) {
      seeds.push_back({w[3], 15});
    } else if (opcode == kFunctionCall && has_result) {
      for (uint32_t index = 4; index < words; ++index) {
        seeds.push_back({w[index], 15});
      }
    }
    const bool arithmetic = (opcode >= 127 && opcode <= 148) ||
                            opcode == kExtInst || opcode == kSelect ||
                            opcode == kPhi;
    if (arithmetic && has_result && is_float32[w[1]]) {
      candidates.push_back(w[2]);
    }
    i += words;
  }
  if (!guest || !fragment || candidates.empty() || !first_type ||
      blocks.empty()) {
    return {};
  }

  // Keys: (variable, element, component) of locations without dynamic
  // indexing. Each store defines the keys of its components.
  std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint32_t> key_indices;
  auto location_keys = [&](const Location& location,
                           std::vector<uint32_t>& keys) {
    keys.clear();
    if (location.var >= bound || dynamic_var[location.var] ||
        location.element == kAny || location.component == kAny) {
      return;
    }
    if (location.component != kNone) {
      keys.push_back(key_indices
                         .emplace(std::make_tuple(location.var, location.element,
                                                  location.component),
                                  uint32_t(key_indices.size()))
                         .first->second);
      return;
    }
    for (uint32_t c = 0; c < var_width[location.var]; ++c) {
      keys.push_back(
          key_indices
              .emplace(std::make_tuple(location.var, location.element, c),
                       uint32_t(key_indices.size()))
              .first->second);
    }
  };
  std::vector<std::vector<uint32_t>> store_keys(stores.size());
  for (size_t s = 0; s < stores.size(); ++s) {
    location_keys(stores[s].location, store_keys[s]);
  }
  // Reaching definitions. State: per key, a bitset of store indices.
  const size_t store_words = (stores.size() + 63) / 64;
  using State = std::vector<std::vector<uint64_t>>;
  auto merge = [&](State& into, const State& from) {
    if (into.size() < from.size()) {
      into.resize(from.size());
    }
    for (size_t k = 0; k < from.size(); ++k) {
      if (from[k].empty()) {
        continue;
      }
      if (into[k].empty()) {
        into[k].assign(store_words, 0);
      }
      for (size_t x = 0; x < store_words; ++x) {
        into[k][x] |= from[k][x];
      }
    }
  };
  auto apply_store = [&](State& state, uint32_t s) {
    for (uint32_t key : store_keys[s]) {
      if (state.size() <= key) {
        state.resize(key + 1);
      }
      state[key].assign(store_words, 0);
      state[key][s / 64] |= uint64_t(1) << (s % 64);
    }
  };
  std::vector<std::vector<uint32_t>> predecessors(blocks.size());
  for (uint32_t b = 0; b < blocks.size(); ++b) {
    for (uint32_t successor : blocks[b].successors) {
      predecessors[successor].push_back(b);
    }
  }
  std::vector<State> block_out(blocks.size());
  for (bool changed = true; changed;) {
    changed = false;
    for (uint32_t b = 0; b < blocks.size(); ++b) {
      State state;
      for (uint32_t p : predecessors[b]) {
        merge(state, block_out[p]);
      }
      for (uint32_t event : blocks[b].events) {
        if (!(event & 0x80000000u)) {
          apply_store(state, event);
        }
      }
      // Normalize trailing empty keys before comparing.
      while (!state.empty() && state.back().empty()) {
        state.pop_back();
      }
      if (state != block_out[b]) {
        block_out[b] = std::move(state);
        changed = true;
      }
    }
  }
  // For every load from a tracked location: per component, the stores that
  // reach it. Loads from untracked (dynamically indexed) variables depend on
  // every store to the variable.
  struct LoadInfo {
    Location location;
    bool tracked;
    std::vector<std::vector<uint64_t>> reaching;  // Per component.
  };
  std::unordered_map<uint32_t, LoadInfo> loads;
  std::vector<uint32_t> keys;
  for (uint32_t b = 0; b < blocks.size(); ++b) {
    State state;
    for (uint32_t p : predecessors[b]) {
      merge(state, block_out[p]);
    }
    for (uint32_t event : blocks[b].events) {
      if (!(event & 0x80000000u)) {
        apply_store(state, event);
        continue;
      }
      const uint32_t load_id = event & 0x7FFFFFFFu;
      const uint32_t* w = code + definition[load_id];
      auto location = locations.find(w[3]);
      if (location == locations.end()) {
        continue;
      }
      LoadInfo info;
      info.location = location->second;
      location_keys(info.location, keys);
      info.tracked = !keys.empty();
      for (uint32_t key : keys) {
        info.reaching.push_back(key < state.size() && !state[key].empty()
                                    ? state[key]
                                    : std::vector<uint64_t>(store_words, 0));
      }
      loads.emplace(load_id, std::move(info));
    }
  }

  // Values computed from gl_FragCoord are seeds (forward taint, also through
  // the register array).
  {
    std::vector<std::vector<uint32_t>> users(bound);
    for (uint32_t id = 0; id < bound; ++id) {
      if (!definition[id]) {
        continue;
      }
      const uint32_t* w = code + definition[id];
      for (uint32_t index = 3; index < (w[0] >> 16); ++index) {
        if (w[index] < bound) {
          users[w[index]].push_back(id);
        }
      }
    }
    std::vector<std::vector<uint32_t>> store_loads(stores.size());
    std::unordered_map<uint32_t, std::vector<uint32_t>> object_stores;
    for (const auto& [load_id, info] : loads) {
      for (const auto& bits : info.reaching) {
        for (size_t s = 0; s < stores.size(); ++s) {
          if (bits[s / 64] >> (s % 64) & 1) {
            store_loads[s].push_back(load_id);
          }
        }
      }
    }
    for (size_t s = 0; s < stores.size(); ++s) {
      object_stores[stores[s].object].push_back(uint32_t(s));
    }
    std::vector<uint8_t> tainted(bound, 0);
    std::vector<uint32_t> stack;
    for (uint32_t id = 0; id < bound; ++id) {
      if (!definition[id]) {
        continue;
      }
      const uint32_t* w = code + definition[id];
      const uint32_t opcode = w[0] & 0xFFFF;
      if ((opcode == kLoad || opcode == kAccessChain ||
           opcode == kInBoundsAccessChain) &&
          (w[0] >> 16) >= 4) {
        auto location = locations.find(w[3]);
        if (location != locations.end() && location->second.var < bound &&
            frag_coord_var[location->second.var]) {
          stack.push_back(id);
        }
      }
    }
    while (!stack.empty()) {
      const uint32_t id = stack.back();
      stack.pop_back();
      if (tainted[id]) {
        continue;
      }
      tainted[id] = 1;
      if (is_float32[id]) {
        seeds.push_back({id, 15});
      }
      for (uint32_t user : users[id]) {
        stack.push_back(user);
      }
      auto it = object_stores.find(id);
      if (it != object_stores.end()) {
        for (uint32_t s : it->second) {
          for (uint32_t load_id : store_loads[s]) {
            stack.push_back(load_id);
          }
        }
      }
    }
  }

  // Backward walk with component masks.
  std::vector<uint8_t> needed(bound, 0);
  std::vector<std::pair<uint32_t, uint32_t>> work(seeds);
  auto need = [&](uint32_t id, uint32_t mask) {
    if (id < bound && definition[id] && (mask & ~needed[id])) {
      work.push_back({id, mask});
    }
  };
  while (!work.empty()) {
    auto [id, mask] = work.back();
    work.pop_back();
    if (id >= bound || !definition[id]) {
      continue;
    }
    const uint32_t value_mask = (1u << width[id]) - 1;
    const uint32_t added = mask & value_mask & ~needed[id];
    if (!added) {
      continue;
    }
    needed[id] |= uint8_t(added);
    const uint32_t* w = code + definition[id];
    const uint32_t words = w[0] >> 16, opcode = w[0] & 0xFFFF;
    auto operand_mask = [&](uint32_t operand) {
      return operand < bound && width[operand] > 1 ? added : 1u;
    };
    switch (opcode) {
      case kLoad: {
        auto it = loads.find(id);
        if (it == loads.end()) {
          break;
        }
        const LoadInfo& info = it->second;
        if (!info.tracked) {
          for (const Store& store : stores) {
            if (store.location.var == info.location.var) {
              need(store.object, 15);
            }
          }
          break;
        }
        for (uint32_t c = 0; c < info.reaching.size(); ++c) {
          if (!(added >> c & 1) && info.reaching.size() > 1) {
            continue;
          }
          for (size_t s = 0; s < stores.size(); ++s) {
            if (!(info.reaching[c][s / 64] >> (s % 64) & 1)) {
              continue;
            }
            const Location& stored = stores[s].location;
            if (stored.component == kNone && info.location.component == kNone) {
              need(stores[s].object, 1u << c);
            } else if (stored.component != kNone &&
                       info.location.component == kNone) {
              need(stores[s].object, 1);
            } else if (stored.component == kNone) {
              need(stores[s].object, 1u << info.location.component);
            } else {
              need(stores[s].object, 15);
            }
          }
        }
      } break;
      case kVariable:
      case kConstant:
        break;
      case kCompositeExtract:
        if (words == 5) {
          need(w[3], w[4] < 4 ? 1u << w[4] : 15);
        } else {
          need(w[3], 15);
        }
        break;
      case kCompositeConstruct: {
        uint32_t position = 0;
        for (uint32_t index = 3; index < words; ++index) {
          const uint32_t part = w[index] < bound ? width[w[index]] : 1;
          const uint32_t sub = (added >> position) & ((1u << part) - 1);
          if (sub) {
            need(w[index], sub);
          }
          position += part;
        }
      } break;
      case kVectorShuffle: {
        if (words < 5) {
          break;
        }
        const uint32_t first = w[3] < bound ? width[w[3]] : 4;
        for (uint32_t j = 0; j + 5 < words + 0 && j < 4; ++j) {
          const uint32_t component = w[5 + j];
          if (!(added >> j & 1) || component == 0xFFFFFFFFu) {
            continue;
          }
          if (component < first) {
            need(w[3], 1u << component);
          } else {
            need(w[4], 1u << (component - first));
          }
        }
      } break;
      case kCompositeInsert:
        if (words == 6 && w[5] < 4) {
          if (added >> w[5] & 1) {
            need(w[3], 1);
          }
          if (added & ~(1u << w[5])) {
            need(w[4], added & ~(1u << w[5]));
          }
        } else {
          for (uint32_t index = 3; index < words; ++index) {
            need(w[index], 15);
          }
        }
        break;
      case kVectorTimesScalar:
        if (words >= 5) {
          need(w[3], added);
          need(w[4], 1);
        }
        break;
      case kPhi:
        for (uint32_t index = 3; index + 1 < words; index += 2) {
          need(w[index], operand_mask(w[index]));
        }
        break;
      case kExtInst:
        if (words >= 5 && w[4] < glsl_componentwise.size() &&
            glsl_componentwise[w[4]]) {
          for (uint32_t index = 5; index < words; ++index) {
            need(w[index], operand_mask(w[index]));
          }
        } else {
          for (uint32_t index = 5; index < words; ++index) {
            need(w[index], 15);
          }
        }
        break;
      default: {
        const bool componentwise =
            opcode == kSelect || opcode == kCopyObject ||
            opcode == kConvertSToF || opcode == kConvertUToF ||
            opcode == kFConvert || (opcode >= 127 && opcode <= 141) ||
            (opcode >= 164 && opcode <= 168) || (opcode >= 180 && opcode <= 191) ||
            opcode == 156 || opcode == 157;
        for (uint32_t index = 3; index < words; ++index) {
          need(w[index], componentwise ? operand_mask(w[index]) : 15);
        }
      } break;
    }
  }

  std::vector<uint32_t> relaxed;
  for (uint32_t id : candidates) {
    if (!needed[id]) {
      relaxed.push_back(id);
    }
  }
  candidate_count = uint32_t(candidates.size());
  relaxed_count = uint32_t(relaxed.size());
  if (relaxed.empty()) {
    return {};
  }
  std::vector<uint32_t> out(code, code + first_type);
  for (uint32_t id : relaxed) {
    out.push_back((3u << 16) | kDecorate);
    out.push_back(id);
    out.push_back(kDecorationRelaxedPrecision);
  }
  out.insert(out.end(), code + first_type, code + word_count);
  return out;
}

VKAPI_ATTR VkResult VKAPI_CALL XportCreateShaderModule(
    VkDevice device, const VkShaderModuleCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkShaderModule* shader_module) {
  const uint32_t* source = create_info->pCode;
  size_t source_words = create_info->codeSize / sizeof(uint32_t);
  std::vector<uint32_t> code =
      XportScalarizeIntegerVectorBitwise(source, source_words);
  if (!code.empty()) {
    source = code.data();
    source_words = code.size();
  }
  static const bool hide_zero_limits = [] {
    const char* setting = std::getenv("XPORT_XENIA_CLAMP_ZERO_FIX");
    return !setting || setting[0] != '0';
  }();
  if (hide_zero_limits) {
    std::vector<uint32_t> hidden = XportHideZeroLimits(source, source_words);
    if (!hidden.empty()) {
      code = std::move(hidden);
      source = code.data();
      source_words = code.size();
    }
  }
  // XPORT_XENIA_FP16=1 relaxes every guest pixel shader; =m:<hex> only those
  // whose code hash falls in the set bits of a 16-bucket mask (bisecting the
  // shaders that break at 16 bits); =x:<hash>,<hash>,... all but those. Each
  // relaxed shader is logged with its hash and bucket.
  static const char* relax_setting = std::getenv("XPORT_XENIA_FP16");
  // XPORT_XENIA_SPIRV_DUMP=<directory>: every shader module as given by Xenia
  // (before the rewrites), as <hash>.spv, for spirv-dis.
  static const char* dump_directory = std::getenv("XPORT_XENIA_SPIRV_DUMP");
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < create_info->codeSize / sizeof(uint32_t); ++i) {
    hash = (hash ^ create_info->pCode[i]) * 16777619u;
  }
  if (dump_directory && *dump_directory) {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%08X.spv", dump_directory, hash);
    if (FILE* file = std::fopen(path, "wb")) {
      std::fwrite(create_info->pCode, 1, create_info->codeSize, file);
      std::fclose(file);
    }
  }
  if (relax_setting && *relax_setting && relax_setting[0] != '0') {
    const uint32_t bucket = hash % 16;
    bool selected = true;
    if (relax_setting[0] == 'm' && relax_setting[1] == ':') {
      selected = (std::strtoul(relax_setting + 2, nullptr, 16) >> bucket) & 1;
    } else if (relax_setting[0] == 'x' && relax_setting[1] == ':') {
      char hash_text[16];
      std::snprintf(hash_text, sizeof(hash_text), "%08X", hash);
      selected = std::strstr(relax_setting, hash_text) == nullptr;
    }
    if (selected) {
      uint32_t relaxed_count, candidate_count;
      std::vector<uint32_t> relaxed = XportRelaxFragmentPrecision(
          source, source_words, relaxed_count, candidate_count);
      if (!relaxed.empty()) {
        XELOGI(
            "xport: fp16 pixel shader {:08X} bucket {}: {} of {} float "
            "results relaxed",
            hash, bucket, relaxed_count, candidate_count);
        if (dump_directory && *dump_directory) {
          char path[512];
          std::snprintf(path, sizeof(path), "%s/%08X_fp16.spv", dump_directory,
                        hash);
          if (FILE* file = std::fopen(path, "wb")) {
            std::fwrite(relaxed.data(), sizeof(uint32_t), relaxed.size(), file);
            std::fclose(file);
          }
        }
        code = std::move(relaxed);
      }
    }
  }
  if (code.empty()) {
    return xport_driver_create_shader_module(device, create_info, allocator,
                                                shader_module);
  }
  VkShaderModuleCreateInfo rewritten_info = *create_info;
  rewritten_info.codeSize = code.size() * sizeof(uint32_t);
  rewritten_info.pCode = code.data();
  return xport_driver_create_shader_module(device, &rewritten_info,
                                              allocator, shader_module);
}

}  // namespace

// XPORT_XENIA_MEMORY_LOG=1 (debugging RAM on phones): every
// vkAllocateMemory is tallied by its caller (4 return addresses, as module
// offsets for llvm-symbolizer), XportDeviceMemoryReport lists them.
namespace {
PFN_vkAllocateMemory xport_driver_allocate_memory = nullptr;
PFN_vkFreeMemory xport_driver_free_memory = nullptr;
struct XportMemoryCaller {
  uint64_t bytes = 0;
  uint32_t count = 0;
};
std::mutex xport_memory_mutex;
std::unordered_map<VkDeviceMemory, std::pair<uint64_t, std::string>>
    xport_memory_allocations;
std::map<std::string, XportMemoryCaller> xport_memory_callers;

std::string XportDescribeAddress(void* address) {
#if XE_PLATFORM_WIN32
  HMODULE module = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(address), &module) &&
      module) {
    return fmt::format("+0x{:X}", reinterpret_cast<uintptr_t>(address) -
                                      reinterpret_cast<uintptr_t>(module));
  }
#else
  Dl_info info;
  if (dladdr(address, &info) && info.dli_fbase) {
    return fmt::format("+0x{:X}", reinterpret_cast<uintptr_t>(address) -
                                      reinterpret_cast<uintptr_t>(info.dli_fbase));
  }
#endif
  return fmt::format("0x{:X}", reinterpret_cast<uintptr_t>(address));
}

#if !XE_PLATFORM_WIN32
struct XportUnwindState {
  void** frames;
  int count;
  int max;
};
_Unwind_Reason_Code XportUnwindCallback(_Unwind_Context* context, void* arg) {
  auto& state = *static_cast<XportUnwindState*>(arg);
  if (state.count >= state.max) {
    return _URC_END_OF_STACK;
  }
  state.frames[state.count++] =
      reinterpret_cast<void*>(_Unwind_GetIP(context));
  return _URC_NO_REASON;
}
#endif

std::string XportCallerKey() {
  void* frames[6] = {};
  int count;
#if XE_PLATFORM_WIN32
  count = RtlCaptureStackBackTrace(2, 4, frames, nullptr);
#else
  XportUnwindState state = {frames, 0, 6};
  _Unwind_Backtrace(XportUnwindCallback, &state);
  // Skip this function and the wrapper.
  count = std::max(state.count - 2, 0);
  for (int i = 0; i < count; ++i) {
    frames[i] = frames[i + 2];
  }
  count = std::min(count, 4);
#endif
  std::string key;
  for (int i = 0; i < count; ++i) {
    key += (i ? " " : "") + XportDescribeAddress(frames[i]);
  }
  return key;
}

VKAPI_ATTR VkResult VKAPI_CALL XportAllocateMemory(
    VkDevice device, const VkMemoryAllocateInfo* allocate_info,
    const VkAllocationCallbacks* allocator, VkDeviceMemory* memory) {
  VkResult result = xport_driver_allocate_memory(device, allocate_info,
                                                    allocator, memory);
  if (result == VK_SUCCESS) {
    std::string key = XportCallerKey();
    std::lock_guard<std::mutex> lock(xport_memory_mutex);
    xport_memory_allocations[*memory] = {allocate_info->allocationSize,
                                            key};
    XportMemoryCaller& caller = xport_memory_callers[key];
    caller.bytes += allocate_info->allocationSize;
    ++caller.count;
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL XportFreeMemory(
    VkDevice device, VkDeviceMemory memory,
    const VkAllocationCallbacks* allocator) {
  if (memory != VK_NULL_HANDLE) {
    std::lock_guard<std::mutex> lock(xport_memory_mutex);
    auto it = xport_memory_allocations.find(memory);
    if (it != xport_memory_allocations.end()) {
      XportMemoryCaller& caller = xport_memory_callers[it->second.second];
      caller.bytes -= it->second.first;
      --caller.count;
      xport_memory_allocations.erase(it);
    }
  }
  xport_driver_free_memory(device, memory, allocator);
}
}  // namespace

std::string XportDeviceMemoryReport() {
  if (!xport_driver_allocate_memory) {
    return std::string();
  }
  std::lock_guard<std::mutex> lock(xport_memory_mutex);
  std::vector<std::pair<uint64_t, const std::string*>> sorted;
  uint64_t total = 0;
  for (const auto& [key, caller] : xport_memory_callers) {
    if (caller.count) {
      sorted.emplace_back(caller.bytes, &key);
      total += caller.bytes;
    }
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  std::string report =
      fmt::format("device memory {} MB in {} allocations", total >> 20,
                  xport_memory_allocations.size());
  for (size_t i = 0; i < std::min(sorted.size(), size_t(12)); ++i) {
    const XportMemoryCaller& caller = xport_memory_callers[*sorted[i].second];
    report += fmt::format("\n  {:5.1f} MB in {:4} from {}",
                          double(caller.bytes) / (1 << 20), caller.count,
                          *sorted[i].second);
  }
  return report;
}

// XPORT_XENIA_MASK="feature,feature" hides device features (debugging:
// makes a desktop GPU behave like the phone's).
static bool XportFeatureMasked(const char* name) {
  const char* list = std::getenv("XPORT_XENIA_MASK");
  if (!list) {
    return false;
  }
  const std::string padded = "," + std::string(list) + ",";
  return padded.find("," + std::string(name) + ",") != std::string::npos;
}

template <typename Structure, VkStructureType StructureType>
struct VulkanFeatures {
  Structure supported = {StructureType};
  Structure enabled = {StructureType};

  void Link(VkPhysicalDeviceFeatures2& supported_features_2,
            VkDeviceCreateInfo& device_create_info) {
    supported.pNext = supported_features_2.pNext;
    supported_features_2.pNext = &supported;
    enabled.pNext = const_cast<void*>(device_create_info.pNext);
    device_create_info.pNext = &enabled;
  }
};

std::unique_ptr<VulkanDevice> VulkanDevice::CreateIfSupported(
    const VulkanInstance* const vulkan_instance,
    const VkPhysicalDevice physical_device, const bool with_gpu_emulation,
    const bool with_swapchain) {
  assert_not_null(vulkan_instance);
  assert_not_null(physical_device);

  const VulkanInstance::Functions& ifn = vulkan_instance->functions();

  // Get supported Vulkan 1.0 properties and features.

  VkPhysicalDeviceProperties properties = {};
  ifn.vkGetPhysicalDeviceProperties(physical_device, &properties);

  // From the VkApplicationInfo specification:
  //
  // "The Khronos validation layers will treat apiVersion as the highest API
  // version the application targets, and will validate API usage against the
  // minimum of that version and the implementation version (instance or device,
  // depending on context). If an application tries to use functionality from a
  // greater version than this, a validation error will be triggered."
  //
  // "Vulkan 1.0 implementations were required to return
  // VK_ERROR_INCOMPATIBLE_DRIVER if apiVersion was larger than 1.0."
  //
  // Make sure that all usages of the API version in Xenia receive the highest
  // minor version that Xenia has been tested on.
  // Libraries such as the Vulkan Memory Allocator also may expect a minor
  // version that is known to them.
  const uint32_t unclamped_api_version = properties.apiVersion;
  const uint32_t clamped_api_minor_version = std::min(
      VK_MAKE_API_VERSION(VK_API_VERSION_VARIANT(unclamped_api_version),
                          VK_API_VERSION_MAJOR(unclamped_api_version),
                          VK_API_VERSION_MINOR(unclamped_api_version), 0),
      vulkan_instance->api_version() >= VK_MAKE_API_VERSION(0, 1, 1, 0)
          ? kHighestUsedApiMinorVersion
          : VK_MAKE_API_VERSION(0, 1, 0, 0));
  properties.apiVersion =
      VK_MAKE_API_VERSION(VK_API_VERSION_VARIANT(clamped_api_minor_version),
                          VK_API_VERSION_MAJOR(clamped_api_minor_version),
                          VK_API_VERSION_MINOR(clamped_api_minor_version),
                          VK_API_VERSION_PATCH(unclamped_api_version));

  VkPhysicalDeviceFeatures supported_features = {};
  ifn.vkGetPhysicalDeviceFeatures(physical_device, &supported_features);

  if (with_gpu_emulation) {
    if (!supported_features.independentBlend) {
      // Not trivial to work around:
      // - Affects not only the blend equation, but also the color write mask.
      // - Can't reuse the blend state of the first attachment for all because
      //   some attachments may have a format that doesn't support blending.
      // - Not possible to split the draw into per-attachment draws because of
      //   depth / stencil.
      // Not supported only on the proprietary driver for the Qualcomm
      // Adreno 4xx, where the driver is largely experimental and doesn't expose
      // a lot of the functionality available in the hardware.
      XELOGW(
          "Vulkan device '{}' doesn't support the independentBlend feature "
          "required for GPU emulation",
          properties.deviceName);
      return nullptr;
    }
  }

  // Enable needed extensions.

  std::unique_ptr<VulkanDevice> device(
      new VulkanDevice(vulkan_instance, physical_device));

  const bool get_physical_device_properties2_supported =
      vulkan_instance->extensions().ext_1_1_KHR_get_physical_device_properties2;

  // Name pointers from `requested_extensions` will be used in the enabled
  // extensions vector.
  std::unordered_map<std::string, bool*> requested_extensions;

  const auto request_promoted_extension =
      [&](const char* const name, uint32_t const major, uint32_t const minor,
          bool* const supported_ptr) {
        assert_not_null(supported_ptr);
        if (properties.apiVersion >= VK_MAKE_API_VERSION(0, major, minor, 0)) {
          *supported_ptr = true;
        } else {
          requested_extensions.emplace(name, supported_ptr);
        }
      };

#define XE_UI_VULKAN_STRUCT_EXTENSION(name) \
  requested_extensions.emplace("VK_" #name, &device->extensions_.ext_##name);
#define XE_UI_VULKAN_LOCAL_EXTENSION(name) \
  requested_extensions.emplace("VK_" #name, &ext_##name);
#define XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(name, major, minor) \
  request_promoted_extension(                                      \
      "VK_" #name, major, minor,                                   \
      &device->extensions_.ext_##major##_##minor##_##name);
#define XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(name, major, minor) \
  request_promoted_extension("VK_" #name, major, minor,           \
                             &ext_##major##_##minor##_##name);

  bool ext_KHR_portability_subset = false;
  bool ext_1_2_KHR_driver_properties = false;
  if (get_physical_device_properties2_supported) {
    // #164. Must be enabled according to the specification if the physical
    // device is a portability subset one.
    XE_UI_VULKAN_LOCAL_EXTENSION(KHR_portability_subset)
    // #197
    XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(KHR_driver_properties, 1, 2)
  }

  // Used by the Vulkan Memory Allocator and potentially by Xenia.
  // #128.
  XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_dedicated_allocation, 1, 1)
  // #147. Also must be enabled for VK_KHR_dedicated_allocation and
  // VK_KHR_sampler_ycbcr_conversion.
  XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_get_memory_requirements2, 1, 1)
  // #158. Also must be enabled for VK_KHR_sampler_ycbcr_conversion.
  XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_bind_memory2, 1, 1)
  if (get_physical_device_properties2_supported) {
    // #238.
    XE_UI_VULKAN_STRUCT_EXTENSION(EXT_memory_budget)
  }
  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    // #414.
    XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_maintenance4, 1, 3)
  }

  if (with_swapchain) {
    // #2.
    XE_UI_VULKAN_STRUCT_EXTENSION(KHR_swapchain)
#if XE_PLATFORM_WIN32
    // #256. Windows-only extension to control fullscreen exclusive behavior.
    // Used to prevent HDR state corruption during fullscreen transitions.
    XE_UI_VULKAN_STRUCT_EXTENSION(EXT_full_screen_exclusive)
#endif
  }

  bool ext_1_2_KHR_sampler_mirror_clamp_to_edge = false;
  bool ext_1_2_EXT_host_query_reset = false;
  bool ext_1_1_KHR_maintenance1 = false;
  bool ext_1_2_KHR_shader_float_controls = false;
  bool ext_EXT_fragment_shader_interlock = false;
  bool ext_1_3_EXT_shader_demote_to_helper_invocation = false;
  bool ext_EXT_non_seamless_cube_map = false;
  bool ext_EXT_custom_border_color = false;
  if (with_gpu_emulation) {
    // #15.
    XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(KHR_sampler_mirror_clamp_to_edge, 1,
                                          2)
    // #70. Must be enabled for VK_KHR_sampler_ycbcr_conversion.
    XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(KHR_maintenance1, 1, 1)
    // #141.
    XE_UI_VULKAN_STRUCT_EXTENSION(EXT_shader_stencil_export)
    // #148.
    XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_image_format_list, 1, 2)
    if (get_physical_device_properties2_supported) {
      // #144.
      XE_UI_VULKAN_STRUCT_EXTENSION(EXT_sample_locations)
      // #157.
      XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_sampler_ycbcr_conversion, 1, 1)
      // #198. Also must be enabled for VK_KHR_spirv_1_4.
      XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(KHR_shader_float_controls, 1, 2)
      XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(EXT_host_query_reset, 1, 2)
      // #82.
      XE_UI_VULKAN_STRUCT_EXTENSION(EXT_conditional_rendering)
      // #284.
      XE_UI_VULKAN_STRUCT_EXTENSION(EXT_depth_bias_control)
      // #252.
      XE_UI_VULKAN_LOCAL_EXTENSION(EXT_fragment_shader_interlock)
      // #277.
      XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(
          EXT_shader_demote_to_helper_invocation, 1, 3)
      // #423.
      XE_UI_VULKAN_LOCAL_EXTENSION(EXT_non_seamless_cube_map)
      // #288. Custom sampler border colors (for YCbCr border colors).
      XE_UI_VULKAN_LOCAL_EXTENSION(EXT_custom_border_color)
    }
    if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
      // #237.
      XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_spirv_1_4, 1, 2)
    }
  }

#undef XE_UI_VULKAN_STRUCT_EXTENSION
#undef XE_UI_VULKAN_LOCAL_EXTENSION
#undef XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION
#undef XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION

  std::vector<const char*> enabled_extensions;
  {
    uint32_t supported_extension_count = 0;
    const VkResult get_supported_extension_count_result =
        ifn.vkEnumerateDeviceExtensionProperties(
            physical_device, nullptr, &supported_extension_count, nullptr);
    if (get_supported_extension_count_result != VK_SUCCESS &&
        get_supported_extension_count_result != VK_INCOMPLETE) {
      XELOGW("Failed to get the Vulkan device '{}' extension count",
             properties.deviceName);
      return nullptr;
    }
    if (supported_extension_count) {
      std::vector<VkExtensionProperties> supported_extensions(
          supported_extension_count);
      if (ifn.vkEnumerateDeviceExtensionProperties(
              physical_device, nullptr, &supported_extension_count,
              supported_extensions.data()) != VK_SUCCESS) {
        XELOGW("Failed to get the Vulkan device '{}' extensions",
               properties.deviceName);
        return nullptr;
      }
      assert_true(supported_extension_count == supported_extensions.size());
      for (const VkExtensionProperties& supported_extension :
           supported_extensions) {
        const auto requested_extension_it =
            requested_extensions.find(supported_extension.extensionName);
        if (requested_extension_it == requested_extensions.cend()) {
          continue;
        }
        assert_not_null(requested_extension_it->second);
        if (!*requested_extension_it->second) {
          enabled_extensions.emplace_back(
              requested_extension_it->first.c_str());
          *requested_extension_it->second = true;
        }
      }
    }
  }

  if (with_swapchain && !device->extensions_.ext_KHR_swapchain) {
    XELOGW("Vulkan device '{}' doesn't support swapchains",
           properties.deviceName);
    return nullptr;
  }

  VkDeviceCreateInfo device_create_info = {
      VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};

  device_create_info.enabledExtensionCount =
      uint32_t(enabled_extensions.size());
  device_create_info.ppEnabledExtensionNames = enabled_extensions.data();

  // Get supported Vulkan 1.1+ and extension properties and features.
  //
  // The property and feature structures are initialized to zero or to the
  // minimum / maximum requirements for the simplicity of handling unavailable
  // VK_KHR_get_physical_device_properties2.

  VkPhysicalDeviceProperties2 properties_2 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};

  VkPhysicalDeviceFeatures2 supported_features_2 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};

  VulkanFeatures<VkPhysicalDeviceVulkan12Features,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES>
      features_1_2;
  VulkanFeatures<VkPhysicalDeviceHostQueryResetFeatures,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES>
      features_EXT_host_query_reset;
  VulkanFeatures<VkPhysicalDeviceVulkan13Features,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES>
      features_1_3;
  VulkanFeatures<
      VkPhysicalDevicePortabilitySubsetFeaturesKHR,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR>
      features_KHR_portability_subset;
  VkPhysicalDeviceDriverPropertiesKHR properties_1_2_KHR_driver_properties = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  VkPhysicalDeviceSampleLocationsPropertiesEXT properties_EXT_sample_locations =
      {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLE_LOCATIONS_PROPERTIES_EXT};
  VkPhysicalDeviceFloatControlsProperties
      properties_1_2_KHR_shader_float_controls = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
  VulkanFeatures<
      VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT>
      features_EXT_fragment_shader_interlock;
  VulkanFeatures<
      VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT>
      features_1_3_EXT_shader_demote_to_helper_invocation;
  VulkanFeatures<
      VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT>
      features_EXT_non_seamless_cube_map;
  VulkanFeatures<
      VkPhysicalDeviceCustomBorderColorFeaturesEXT,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT>
      features_EXT_custom_border_color;
  VulkanFeatures<
      VkPhysicalDeviceConditionalRenderingFeaturesEXT,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT>
      features_EXT_conditional_rendering;
  VulkanFeatures<
      VkPhysicalDeviceDepthBiasControlFeaturesEXT,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT>
      features_EXT_depth_bias_control;

  if (get_physical_device_properties2_supported) {
    if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
      features_1_2.Link(supported_features_2, device_create_info);
    } else if (ext_1_2_EXT_host_query_reset) {
      features_EXT_host_query_reset.Link(supported_features_2,
                                         device_create_info);
    }
    if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0)) {
      features_1_3.Link(supported_features_2, device_create_info);
    } else {
      if (ext_1_3_EXT_shader_demote_to_helper_invocation) {
        features_1_3_EXT_shader_demote_to_helper_invocation.Link(
            supported_features_2, device_create_info);
      }
    }
    if (ext_KHR_portability_subset) {
      features_KHR_portability_subset.Link(supported_features_2,
                                           device_create_info);
    }
    if (ext_1_2_KHR_driver_properties) {
      properties_1_2_KHR_driver_properties.pNext = properties_2.pNext;
      properties_2.pNext = &properties_1_2_KHR_driver_properties;
    }
    if (ext_1_2_KHR_shader_float_controls) {
      properties_1_2_KHR_shader_float_controls.pNext = properties_2.pNext;
      properties_2.pNext = &properties_1_2_KHR_shader_float_controls;
    }
    if (device->extensions_.ext_EXT_sample_locations) {
      properties_EXT_sample_locations.pNext = properties_2.pNext;
      properties_2.pNext = &properties_EXT_sample_locations;
    }
    if (ext_EXT_fragment_shader_interlock) {
      features_EXT_fragment_shader_interlock.Link(supported_features_2,
                                                  device_create_info);
    }
    if (ext_EXT_non_seamless_cube_map) {
      features_EXT_non_seamless_cube_map.Link(supported_features_2,
                                              device_create_info);
    }
    if (ext_EXT_custom_border_color) {
      features_EXT_custom_border_color.Link(supported_features_2,
                                            device_create_info);
    }
    if (device->extensions_.ext_EXT_conditional_rendering) {
      features_EXT_conditional_rendering.Link(supported_features_2,
                                              device_create_info);
    }
    if (device->extensions_.ext_EXT_depth_bias_control) {
      features_EXT_depth_bias_control.Link(supported_features_2,
                                           device_create_info);
    }
    ifn.vkGetPhysicalDeviceProperties2(physical_device, &properties_2);
    ifn.vkGetPhysicalDeviceFeatures2(physical_device, &supported_features_2);
  }

  uint32_t queue_family_count = 0;
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(physical_device,
                                               &queue_family_count, nullptr);
  std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(
      physical_device, &queue_family_count, queue_families.data());

  device->queue_families_.resize(queue_family_count);

  uint32_t first_queue_family_graphics_compute_sparse_binding = UINT32_MAX;
  uint32_t first_queue_family_graphics_compute = UINT32_MAX;
  uint32_t first_queue_family_sparse_binding = UINT32_MAX;
  bool has_presentation_queue_family = false;

  for (uint32_t queue_family_index = 0; queue_family_index < queue_family_count;
       ++queue_family_index) {
    QueueFamily& queue_family = device->queue_families_[queue_family_index];
    const VkQueueFamilyProperties& queue_family_properties =
        queue_families[queue_family_index];

    const VkQueueFlags queue_unsupported_flags =
        ~queue_family_properties.queueFlags;

    if (!(queue_unsupported_flags &
          (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
      first_queue_family_graphics_compute =
          std::min(queue_family_index, first_queue_family_graphics_compute);
    }

    if (with_gpu_emulation && supported_features.sparseBinding &&
        !(queue_unsupported_flags & VK_QUEUE_SPARSE_BINDING_BIT)) {
      first_queue_family_sparse_binding =
          std::min(queue_family_index, first_queue_family_sparse_binding);
      if (!(queue_unsupported_flags &
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
        first_queue_family_graphics_compute_sparse_binding =
            std::min(queue_family_index,
                     first_queue_family_graphics_compute_sparse_binding);
      }
    }

    if (with_swapchain) {
#if XE_PLATFORM_WIN32
      queue_family.may_support_presentation =
          vulkan_instance->extensions().ext_KHR_win32_surface &&
          ifn.vkGetPhysicalDeviceWin32PresentationSupportKHR(
              physical_device, queue_family_index);
#else
      queue_family.may_support_presentation = true;
#endif
      if (queue_family.may_support_presentation) {
        queue_family.queues.resize(
            std::max(size_t(1), queue_family.queues.size()));
        has_presentation_queue_family = true;
      }
    }
  }

  if (first_queue_family_graphics_compute == UINT32_MAX) {
    // Not valid according to the Vulkan specification, but for safety.
    XELOGW(
        "Vulkan device '{}' doesn't provide a graphics and compute queue "
        "family",
        properties.deviceName);
    return nullptr;
  }

  if (with_swapchain && !has_presentation_queue_family) {
    XELOGW(
        "Vulkan device '{}' doesn't provide a queue family that supports "
        "presentation",
        properties.deviceName);
    return nullptr;
  }

  // Get the queues to create.

  if (first_queue_family_sparse_binding == UINT32_MAX) {
    // Not valid not to provide a sparse binding queue if the sparseBinding
    // feature is supported according to the Vulkan specification, but for
    // safety and simplicity.
    supported_features.sparseBinding = VK_FALSE;
  }
  if (!supported_features.sparseBinding) {
    supported_features.sparseResidencyBuffer = VK_FALSE;
    supported_features.sparseResidencyImage2D = VK_FALSE;
    supported_features.sparseResidencyImage3D = VK_FALSE;
    supported_features.sparseResidency2Samples = VK_FALSE;
    supported_features.sparseResidency4Samples = VK_FALSE;
    supported_features.sparseResidency8Samples = VK_FALSE;
    supported_features.sparseResidency16Samples = VK_FALSE;
    supported_features.sparseResidencyAliased = VK_FALSE;
  }

  // Prefer using one queue for everything whenever possible for simplicity.
  // TODO(Triang3l): Research if separate queues for purposes like composition,
  // swapchain image presentation, and sparse binding, may be beneficial.

  if (first_queue_family_graphics_compute_sparse_binding != UINT32_MAX) {
    device->queue_family_graphics_compute_ =
        first_queue_family_graphics_compute_sparse_binding;
    device->queue_family_sparse_binding_ =
        first_queue_family_graphics_compute_sparse_binding;
  } else {
    device->queue_family_graphics_compute_ =
        first_queue_family_graphics_compute;
    device->queue_family_sparse_binding_ = first_queue_family_sparse_binding;
  }

  device->queue_families_[device->queue_family_graphics_compute_].queues.resize(
      std::max(size_t(1),
               device->queue_families_[device->queue_family_graphics_compute_]
                   .queues.size()));
  if (device->queue_family_sparse_binding_ != UINT32_MAX) {
    device->queue_families_[device->queue_family_sparse_binding_].queues.resize(
        std::max(size_t(1),
                 device->queue_families_[device->queue_family_sparse_binding_]
                     .queues.size()));
  }

  size_t max_enabled_queues_per_family = 0;
  for (const QueueFamily& queue_family : device->queue_families_) {
    max_enabled_queues_per_family =
        std::max(queue_family.queues.size(), max_enabled_queues_per_family);
  }
  const std::vector<float> queue_priorities(max_enabled_queues_per_family,
                                            1.0f);
  std::vector<VkDeviceQueueCreateInfo> queue_create_infos;
  for (size_t queue_family_index = 0;
       queue_family_index < device->queue_families_.size();
       ++queue_family_index) {
    const QueueFamily& queue_family =
        device->queue_families_[queue_family_index];
    if (queue_family.queues.empty()) {
      continue;
    }
    VkDeviceQueueCreateInfo& queue_create_info =
        queue_create_infos.emplace_back();
    queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_info.pNext = nullptr;
    queue_create_info.flags = 0;
    queue_create_info.queueFamilyIndex = uint32_t(queue_family_index);
    queue_create_info.queueCount = uint32_t(queue_family.queues.size());
    queue_create_info.pQueuePriorities = queue_priorities.data();
  }
  device_create_info.queueCreateInfoCount = uint32_t(queue_create_infos.size());
  device_create_info.pQueueCreateInfos = queue_create_infos.data();

  // Enable needed features and copy the properties.
  //
  // Enabling only actually used features because drivers may take more optimal
  // paths when certain features are disabled. Also, in VK_EXT_shader_object,
  // the state that the application must set for the draw depends on which
  // features are enabled.

  device->properties_.apiVersion = properties.apiVersion;
  device->properties_.driverVersion = properties.driverVersion;
  device->properties_.vendorID = properties.vendorID;
  device->properties_.deviceID = properties.deviceID;
  std::strcpy(device->properties_.deviceName, properties.deviceName);

  XELOGI(
      "Vulkan device '{}': API {}.{}.{} ({}.{} used), vendor 0x{:04X}, device "
      "0x{:04X}, driver version 0x{:X}",
      properties.deviceName, VK_VERSION_MAJOR(unclamped_api_version),
      VK_VERSION_MINOR(unclamped_api_version),
      VK_VERSION_PATCH(properties.apiVersion),
      VK_VERSION_MAJOR(properties.apiVersion),
      VK_VERSION_MINOR(properties.apiVersion), properties.vendorID,
      properties.deviceID, properties.driverVersion);

  XELOGI("Enabled Vulkan device extensions:");
  for (uint32_t enabled_extension_index = 0;
       enabled_extension_index < device_create_info.enabledExtensionCount;
       ++enabled_extension_index) {
    XELOGI("* {}",
           device_create_info.ppEnabledExtensionNames[enabled_extension_index]);
  }

  XELOGI("Vulkan device properties and enabled features:");

  VkPhysicalDeviceFeatures enabled_features = {};
  device_create_info.pEnabledFeatures = &enabled_features;

#define XE_UI_VULKAN_LIMIT(name)                     \
  device->properties_.name = properties.limits.name; \
  XELOGI("* " #name ": {}", properties.limits.name);
#define XE_UI_VULKAN_ENUM_LIMIT(name, type)          \
  device->properties_.name = properties.limits.name; \
  XELOGI("* " #name ": {}", vk::to_string(vk::type(properties.limits.name)));
#define XE_UI_VULKAN_FEATURE(name)                    \
  if (XportFeatureMasked(#name)) {                     \
    supported_features.name = VK_FALSE;               \
  }                                                   \
  enabled_features.name = supported_features.name;    \
  device->properties_.name = supported_features.name; \
  if (supported_features.name) {                      \
    XELOGI("* " #name);                               \
  }
#define XE_UI_VULKAN_PROPERTY_2(structure, name) \
  device->properties_.name = structure.name;     \
  XELOGI("* " #name ": {}", structure.name);
#define XE_UI_VULKAN_ENUM_PROPERTY_2(structure, name, type) \
  device->properties_.name = structure.name;                \
  XELOGI("* " #name ": {}", vk::to_string(vk::type(structure.name)));
#define XE_UI_VULKAN_FEATURE_2(structure, name)        \
  if (XportFeatureMasked(#name)) {                      \
    structure.supported.name = VK_FALSE;               \
  }                                                    \
  structure.enabled.name = structure.supported.name;   \
  device->properties_.name = structure.supported.name; \
  if (structure.supported.name) {                      \
    XELOGI("* " #name);                                \
  }
#define XE_UI_VULKAN_FEATURE_IMPLIED(name) \
  device->properties_.name = true;         \
  XELOGI("* " #name);

  if (ext_1_2_KHR_driver_properties) {
    XE_UI_VULKAN_ENUM_PROPERTY_2(properties_1_2_KHR_driver_properties, driverID,
                                 DriverId);
    XELOGI("* driverName: {}", properties_1_2_KHR_driver_properties.driverName);
    if (properties_1_2_KHR_driver_properties.driverInfo[0]) {
      XELOGI("* driverInfo: {}",
             properties_1_2_KHR_driver_properties.driverInfo);
    }
    XELOGI("* conformanceVersion: {}.{}.{}.{}",
           properties_1_2_KHR_driver_properties.conformanceVersion.major,
           properties_1_2_KHR_driver_properties.conformanceVersion.minor,
           properties_1_2_KHR_driver_properties.conformanceVersion.subminor,
           properties_1_2_KHR_driver_properties.conformanceVersion.patch);
  }

  XE_UI_VULKAN_LIMIT(maxImageDimension2D)
  XE_UI_VULKAN_LIMIT(maxImageDimension3D)
  XE_UI_VULKAN_LIMIT(maxImageDimensionCube)
  XE_UI_VULKAN_LIMIT(maxImageArrayLayers)
  XE_UI_VULKAN_LIMIT(maxStorageBufferRange)
  XE_UI_VULKAN_LIMIT(maxSamplerAllocationCount)
  XE_UI_VULKAN_LIMIT(maxPerStageDescriptorSamplers)
  XE_UI_VULKAN_LIMIT(maxPerStageDescriptorStorageBuffers)
  XE_UI_VULKAN_LIMIT(maxPerStageDescriptorSampledImages)
  XE_UI_VULKAN_LIMIT(maxPerStageResources)
  XE_UI_VULKAN_LIMIT(maxVertexOutputComponents)
  XE_UI_VULKAN_LIMIT(maxTessellationEvaluationOutputComponents)
  XE_UI_VULKAN_LIMIT(maxGeometryInputComponents)
  XE_UI_VULKAN_LIMIT(maxGeometryOutputComponents)
  XE_UI_VULKAN_LIMIT(maxFragmentInputComponents)
  XE_UI_VULKAN_LIMIT(maxFragmentCombinedOutputResources)
  XE_UI_VULKAN_LIMIT(maxSamplerAnisotropy)
  XE_UI_VULKAN_LIMIT(maxViewportDimensions[0])
  XE_UI_VULKAN_LIMIT(maxViewportDimensions[1])
  XE_UI_VULKAN_LIMIT(minUniformBufferOffsetAlignment)
  XE_UI_VULKAN_LIMIT(minStorageBufferOffsetAlignment)
  XE_UI_VULKAN_LIMIT(maxFramebufferWidth)
  XE_UI_VULKAN_LIMIT(maxFramebufferHeight)
  XE_UI_VULKAN_ENUM_LIMIT(framebufferColorSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(framebufferDepthSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(framebufferStencilSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(framebufferNoAttachmentsSampleCounts,
                          SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(sampledImageColorSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(sampledImageIntegerSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(sampledImageDepthSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(sampledImageStencilSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_LIMIT(standardSampleLocations)
  XE_UI_VULKAN_LIMIT(optimalBufferCopyOffsetAlignment)
  XE_UI_VULKAN_LIMIT(optimalBufferCopyRowPitchAlignment)
  XE_UI_VULKAN_LIMIT(nonCoherentAtomSize)

  if (with_gpu_emulation) {
    XE_UI_VULKAN_FEATURE(robustBufferAccess)
    XE_UI_VULKAN_FEATURE(fullDrawIndexUint32)
    XE_UI_VULKAN_FEATURE(independentBlend)
    XE_UI_VULKAN_FEATURE(geometryShader)
    XE_UI_VULKAN_FEATURE(tessellationShader)
    XE_UI_VULKAN_FEATURE(sampleRateShading)
    XE_UI_VULKAN_FEATURE(depthClamp)
    XE_UI_VULKAN_FEATURE(fillModeNonSolid)
    XE_UI_VULKAN_FEATURE(samplerAnisotropy)
    XE_UI_VULKAN_FEATURE(occlusionQueryPrecise)
    XE_UI_VULKAN_FEATURE(vertexPipelineStoresAndAtomics)
    XE_UI_VULKAN_FEATURE(fragmentStoresAndAtomics)
    XE_UI_VULKAN_FEATURE(shaderClipDistance)
    XE_UI_VULKAN_FEATURE(shaderCullDistance)
    XE_UI_VULKAN_FEATURE(sparseBinding)
    XE_UI_VULKAN_FEATURE(sparseResidencyBuffer)
  }

  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_1_2, samplerMirrorClampToEdge);
      XE_UI_VULKAN_FEATURE_2(features_1_2, uniformBufferStandardLayout);
      XE_UI_VULKAN_FEATURE_2(features_1_2, scalarBlockLayout);
      XE_UI_VULKAN_FEATURE_2(features_1_2, hostQueryReset);
    }
  } else {
    if (ext_1_2_KHR_sampler_mirror_clamp_to_edge) {
      XE_UI_VULKAN_FEATURE_IMPLIED(samplerMirrorClampToEdge)
    }
    if (ext_1_2_EXT_host_query_reset && with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_EXT_host_query_reset, hostQueryReset);
    }
  }
  device->extensions_.ext_1_2_EXT_host_query_reset =
      ext_1_2_EXT_host_query_reset;

  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0)) {
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_1_3, shaderDemoteToHelperInvocation);
    }
  } else {
    if (ext_1_3_EXT_shader_demote_to_helper_invocation) {
      if (with_gpu_emulation) {
        XE_UI_VULKAN_FEATURE_2(
            features_1_3_EXT_shader_demote_to_helper_invocation,
            shaderDemoteToHelperInvocation);
      }
    }
  }

  if (ext_KHR_portability_subset) {
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset,
                             constantAlphaColorBlendFactors)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset,
                             imageViewFormatReinterpretation)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset,
                             imageViewFormatSwizzle)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset, pointPolygons)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset,
                             separateStencilMaskRef)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset,
                             shaderSampleRateInterpolationFunctions)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset, triangleFans)
    }
  } else {
    // Not a portability subset device.
    XE_UI_VULKAN_FEATURE_IMPLIED(constantAlphaColorBlendFactors)
    XE_UI_VULKAN_FEATURE_IMPLIED(imageViewFormatReinterpretation)
    XE_UI_VULKAN_FEATURE_IMPLIED(imageViewFormatSwizzle)
    XE_UI_VULKAN_FEATURE_IMPLIED(pointPolygons)
    XE_UI_VULKAN_FEATURE_IMPLIED(separateStencilMaskRef)
    XE_UI_VULKAN_FEATURE_IMPLIED(shaderSampleRateInterpolationFunctions)
    XE_UI_VULKAN_FEATURE_IMPLIED(triangleFans)
  }

  if (ext_1_2_KHR_shader_float_controls) {
    XE_UI_VULKAN_PROPERTY_2(properties_1_2_KHR_shader_float_controls,
                            shaderSignedZeroInfNanPreserveFloat32);
    XE_UI_VULKAN_PROPERTY_2(properties_1_2_KHR_shader_float_controls,
                            shaderDenormFlushToZeroFloat32);
    XE_UI_VULKAN_PROPERTY_2(properties_1_2_KHR_shader_float_controls,
                            shaderRoundingModeRTEFloat32);
  }

  if (ext_EXT_fragment_shader_interlock) {
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_EXT_fragment_shader_interlock,
                             fragmentShaderSampleInterlock)
      XE_UI_VULKAN_FEATURE_2(features_EXT_fragment_shader_interlock,
                             fragmentShaderPixelInterlock)
    }
  }

  if (device->extensions_.ext_EXT_sample_locations) {
    XE_UI_VULKAN_PROPERTY_2(properties_EXT_sample_locations,
                            sampleLocationSampleCounts);
    XE_UI_VULKAN_PROPERTY_2(properties_EXT_sample_locations,
                            variableSampleLocations);
  }

  if (ext_EXT_non_seamless_cube_map) {
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_EXT_non_seamless_cube_map,
                             nonSeamlessCubeMap)
    }
  }

  if (ext_EXT_custom_border_color) {
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_EXT_custom_border_color,
                             customBorderColors)
      XE_UI_VULKAN_FEATURE_2(features_EXT_custom_border_color,
                             customBorderColorWithoutFormat)
    }
  }

  if (device->extensions_.ext_EXT_conditional_rendering) {
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_EXT_conditional_rendering,
                             conditionalRendering)
    }
  }

  if (device->extensions_.ext_EXT_depth_bias_control) {
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_EXT_depth_bias_control, depthBiasControl)
      XE_UI_VULKAN_FEATURE_2(features_EXT_depth_bias_control,
                             leastRepresentableValueForceUnormRepresentation)
      XE_UI_VULKAN_FEATURE_2(features_EXT_depth_bias_control, depthBiasExact)
    }
  }

#undef XE_UI_VULKAN_LIMIT
#undef XE_UI_VULKAN_ENUM_LIMIT
#undef XE_UI_VULKAN_FEATURE
#undef XE_UI_VULKAN_PROPERTY_2
#undef XE_UI_VULKAN_ENUM_PROPERTY_2
#undef XE_UI_VULKAN_FEATURE_2

  // Create the device.

  const VkResult device_create_result = ifn.vkCreateDevice(
      physical_device, &device_create_info, nullptr, &device->device_);
  if (device_create_result != VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan logical device from the physical device "
        "'{}': {}",
        properties.deviceName, vk::to_string(vk::Result(device_create_result)));
    return nullptr;
  }

  // Load device functions.

  bool functions_loaded = true;

  Functions& dfn = device->functions_;

#define XE_UI_VULKAN_FUNCTION(name)                                   \
  functions_loaded &= (dfn.name = PFN_##name(ifn.vkGetDeviceProcAddr( \
                           device->device_, #name))) != nullptr;

  // Vulkan 1.0.
#include "xenia/ui/vulkan/functions/device_1_0.inc"

  // Extensions promoted to a Vulkan version supported by the device.
#define XE_UI_VULKAN_FUNCTION_PROMOTED(extension_name, core_name) \
  functions_loaded &=                                             \
      (dfn.core_name = PFN_##core_name(                           \
           ifn.vkGetDeviceProcAddr(device->device_, #core_name))) != nullptr;
  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
#include "xenia/ui/vulkan/functions/device_1_1_khr_bind_memory2.inc"
#include "xenia/ui/vulkan/functions/device_1_1_khr_get_memory_requirements2.inc"
  }
  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
#include "xenia/ui/vulkan/functions/device_1_2_ext_host_query_reset.inc"
  }
  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0)) {
#include "xenia/ui/vulkan/functions/device_1_3_khr_maintenance4.inc"
  }
#undef XE_UI_VULKAN_FUNCTION_PROMOTED

  // Non-promoted extensions, and extensions promoted to a Vulkan version not
  // supported by the device.
#define XE_UI_VULKAN_FUNCTION_PROMOTED(extension_name, core_name) \
  functions_loaded &=                                             \
      (dfn.core_name = PFN_##core_name(ifn.vkGetDeviceProcAddr(   \
           device->device_, #extension_name))) != nullptr;
  if (properties.apiVersion < VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    if (device->extensions_.ext_1_1_KHR_get_memory_requirements2) {
#include "xenia/ui/vulkan/functions/device_1_1_khr_get_memory_requirements2.inc"
    }
    if (device->extensions_.ext_1_1_KHR_bind_memory2) {
#include "xenia/ui/vulkan/functions/device_1_1_khr_bind_memory2.inc"
    }
  }
  if (properties.apiVersion < VK_MAKE_API_VERSION(0, 1, 2, 0)) {
    if (device->extensions_.ext_1_2_EXT_host_query_reset) {
#include "xenia/ui/vulkan/functions/device_1_2_ext_host_query_reset.inc"
    }
  }
  if (properties.apiVersion < VK_MAKE_API_VERSION(0, 1, 3, 0)) {
    if (device->extensions_.ext_1_3_KHR_maintenance4) {
#include "xenia/ui/vulkan/functions/device_1_3_khr_maintenance4.inc"
    }
  }
  if (device->extensions_.ext_KHR_swapchain) {
#include "xenia/ui/vulkan/functions/device_khr_swapchain.inc"
  }
  if (device->extensions_.ext_EXT_conditional_rendering) {
#include "xenia/ui/vulkan/functions/device_ext_conditional_rendering.inc"
  }
  if (device->extensions_.ext_EXT_depth_bias_control) {
#include "xenia/ui/vulkan/functions/device_ext_depth_bias_control.inc"
  }
#undef XE_UI_VULKAN_FUNCTION_PROMOTED

#undef XE_UI_VULKAN_FUNCTION

  if (!functions_loaded) {
    XELOGE("Failed to get all Vulkan device function pointers for '{}'",
           properties.deviceName);
    return nullptr;
  }

  // the Arm erratum 3843237 workaround (see above) on Arm GPUs;
  // XPORT_XENIA_BITWISE_FIX=1 / 0 forces it on (to test it elsewhere) or off.
  {
    const char* bitwise_fix = std::getenv("XPORT_XENIA_BITWISE_FIX");
    const bool use_bitwise_fix = bitwise_fix ? bitwise_fix[0] == '1'
                                             : properties.vendorID == 0x13B5;
    if (use_bitwise_fix) {
      xport_driver_create_shader_module = dfn.vkCreateShaderModule;
      dfn.vkCreateShaderModule = XportCreateShaderModule;
      XELOGI("xport: integer vector bitwise operations scalarized in shaders "
             "(Arm erratum 3843237)");
    }
  }

  // see XportDeviceMemoryReport.
  if (std::getenv("XPORT_XENIA_MEMORY_LOG")) {
    xport_driver_allocate_memory = dfn.vkAllocateMemory;
    xport_driver_free_memory = dfn.vkFreeMemory;
    dfn.vkAllocateMemory = XportAllocateMemory;
    dfn.vkFreeMemory = XportFreeMemory;
  }

  // Get the queues.

  for (size_t queue_family_index = 0;
       queue_family_index < device->queue_families_.size();
       ++queue_family_index) {
    QueueFamily& queue_family = device->queue_families_[queue_family_index];
    for (size_t queue_index = 0; queue_index < queue_family.queues.size();
         ++queue_index) {
      VkQueue queue;
      dfn.vkGetDeviceQueue(device->device_, uint32_t(queue_family_index),
                           uint32_t(queue_index), &queue);
      queue_family.queues[queue_index] = std::make_unique<Queue>(queue);
    }
  }

  // Get the memory types.

  VkPhysicalDeviceMemoryProperties memory_properties;
  ifn.vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
  for (uint32_t memory_type_index = 0;
       memory_type_index < memory_properties.memoryTypeCount;
       ++memory_type_index) {
    const uint32_t memory_type_bit = uint32_t(1) << memory_type_index;
    const VkMemoryPropertyFlags memory_type_flags =
        memory_properties.memoryTypes[memory_type_index].propertyFlags;
    if (memory_type_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
      device->memory_types_.device_local |= memory_type_bit;
    }
    if (memory_type_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      device->memory_types_.host_visible |= memory_type_bit;
    }
    if (memory_type_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
      device->memory_types_.host_coherent |= memory_type_bit;
    }
    if (memory_type_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {
      device->memory_types_.host_cached |= memory_type_bit;
    }
  }

  return device;
}

VulkanDevice::~VulkanDevice() {
  if (device_) {
    vulkan_instance_->functions().vkDestroyDevice(device_, nullptr);
  }
}

VulkanDevice::VulkanDevice(const VulkanInstance* const vulkan_instance,
                           const VkPhysicalDevice physical_device)
    : vulkan_instance_(vulkan_instance), physical_device_(physical_device) {
  assert_not_null(vulkan_instance);
  assert_not_null(physical_device);
}

}  // namespace vulkan
}  // namespace ui
}  // namespace xe
