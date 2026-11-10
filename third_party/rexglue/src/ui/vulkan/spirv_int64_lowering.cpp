// 64-bit shader integers as uvec2 pairs for devices without shaderInt64 (see the header).
// Covers what the Liberty shader cache and the override cache use (2026-10-29: 2,062 + 636 modules,
// all SPIR-V 1.0 from DXC): 64-bit loads and constants, IAdd/ISub/IMul/SNegate, the bitwise
// operations and shifts, UConvert/SConvert in both directions, Bitcast and ConvertUToPtr/PtrToU,
// and the equality and unsigned comparisons. Anything else that touches a 64-bit integer (vectors of
// them, spec constants, division, signed comparisons, extended instructions, atomics, a 64-bit
// switch) is refused, and the module is left alone.
#include <rex/ui/vulkan/spirv_int64_lowering.h>

#include <algorithm>
#include <initializer_list>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace rex {
namespace ui {
namespace vulkan {

namespace {

// SPIR-V opcodes and enumerants (spirv.core.grammar.json).
enum : uint32_t {
  kOpUndef = 1,
  kOpSourceContinued = 2,
  kOpSource = 3,
  kOpSourceExtension = 4,
  kOpName = 5,
  kOpMemberName = 6,
  kOpString = 7,
  kOpLine = 8,
  kOpExtension = 10,
  kOpExtInstImport = 11,
  kOpExtInst = 12,
  kOpMemoryModel = 14,
  kOpEntryPoint = 15,
  kOpExecutionMode = 16,
  kOpCapability = 17,
  kOpTypeVoid = 19,
  kOpTypeBool = 20,
  kOpTypeInt = 21,
  kOpTypeFloat = 22,
  kOpTypeVector = 23,
  kOpTypeMatrix = 24,
  kOpTypeImage = 25,
  kOpTypeSampler = 26,
  kOpTypeSampledImage = 27,
  kOpTypeArray = 28,
  kOpTypeRuntimeArray = 29,
  kOpTypeStruct = 30,
  kOpTypePointer = 32,
  kOpTypeFunction = 33,
  kOpTypeForwardPointer = 39,
  kOpConstantTrue = 41,
  kOpConstantFalse = 42,
  kOpConstant = 43,
  kOpConstantComposite = 44,
  kOpConstantNull = 46,
  kOpSpecConstantTrue = 48,
  kOpSpecConstantFalse = 49,
  kOpSpecConstant = 50,
  kOpSpecConstantComposite = 51,
  kOpSpecConstantOp = 52,
  kOpFunction = 54,
  kOpFunctionParameter = 55,
  kOpFunctionEnd = 56,
  kOpFunctionCall = 57,
  kOpVariable = 59,
  kOpLoad = 61,
  kOpStore = 62,
  kOpAccessChain = 65,
  kOpInBoundsAccessChain = 66,
  kOpPtrAccessChain = 67,
  kOpInBoundsPtrAccessChain = 70,
  kOpDecorate = 71,
  kOpMemberDecorate = 72,
  kOpGroupDecorate = 74,
  kOpGroupMemberDecorate = 75,
  kOpVectorShuffle = 79,
  kOpCompositeConstruct = 80,
  kOpCompositeExtract = 81,
  kOpCompositeInsert = 82,
  kOpCopyObject = 83,
  kOpImageFirst = 86,  // OpSampledImage
  kOpImageWrite = 99,
  kOpImageLast = 107,  // OpImageQuerySamples
  kOpUConvert = 113,
  kOpSConvert = 114,
  kOpConvertPtrToU = 117,
  kOpConvertUToPtr = 120,
  kOpBitcast = 124,
  kOpSNegate = 126,
  kOpIAdd = 128,
  kOpISub = 130,
  kOpIMul = 132,
  kOpIAddCarry = 149,
  kOpISubBorrow = 150,
  kOpUMulExtended = 151,
  kOpAny = 154,
  kOpAll = 155,
  kOpLogicalOr = 166,
  kOpLogicalAnd = 167,
  kOpSelect = 169,
  kOpIEqual = 170,
  kOpINotEqual = 171,
  kOpUGreaterThan = 172,
  kOpUGreaterThanEqual = 174,
  kOpULessThan = 176,
  kOpULessThanEqual = 178,
  kOpShiftRightLogical = 194,
  kOpShiftRightArithmetic = 195,
  kOpShiftLeftLogical = 196,
  kOpBitwiseOr = 197,
  kOpBitwiseXor = 198,
  kOpBitwiseAnd = 199,
  kOpNot = 200,
  kOpPhi = 245,
  kOpLoopMerge = 246,
  kOpSelectionMerge = 247,
  kOpLabel = 248,
  kOpBranch = 249,
  kOpBranchConditional = 250,
  kOpSwitch = 251,
  kOpReturnValue = 254,
  kOpNoLine = 317,
  kOpModuleProcessed = 330,
  kOpExecutionModeId = 331,

  kCapabilityInt64 = 11,
  kCapabilityInt64Atomics = 12,
  kStorageClassFunction = 7,
  kVersion14 = 0x00010400,
  kVersion15 = 0x00010500,
};

// Instructions whose first two operands are a result type and a result id (core grammar).
bool HasResultType(uint32_t op) {
  return op == 1 || op == 12 || (op >= 41 && op <= 46) || (op >= 48 && op <= 52) || op == 54 ||
         op == 55 || op == 57 || (op >= 59 && op <= 61) || (op >= 65 && op <= 70) ||
         (op >= 77 && op <= 84) || (op >= 86 && op <= 107 && op != kOpImageWrite) ||
         (op >= 109 && op <= 124) || (op >= 126 && op <= 152) || (op >= 154 && op <= 205) ||
         (op >= 207 && op <= 215) || op == 227 || (op >= 229 && op <= 242) || op == kOpPhi ||
         (op >= 333 && op <= 366);
}

struct Instruction {
  uint32_t opcode;
  std::vector<uint32_t> operands;
};

class Int64Lowering {
 public:
  bool Run(const uint32_t* code, size_t word_count, std::vector<uint32_t>& out);
  std::string failure;

 private:
  bool Fail(std::string reason) {
    if (failure.empty()) failure = std::move(reason);
    return false;
  }
  uint32_t NewId() { return bound_++; }
  bool Is64Type(uint32_t id) const { return int64_types_.count(id) != 0; }
  bool Is64Value(uint32_t id) const { return int64_values_.count(id) != 0; }
  uint32_t Remap(uint32_t type) const { return Is64Type(type) ? v2uint_ : type; }

  // Types and constants the lowering emits at the place of the first 64-bit type declaration.
  void PlaceHelperTypes();
  uint32_t BoolType();
  uint32_t V2BoolType();
  uint32_t PairType();
  uint32_t UintConstant(uint32_t value);

  bool LowerGlobal(const Instruction& inst);
  bool LowerFunctionInstruction(const Instruction& inst);
  void FixEntryPointInterfaces();

  // Function body emission.
  uint32_t Op(uint32_t opcode, uint32_t type, std::initializer_list<uint32_t> args,
              uint32_t result = 0) {
    if (!result) result = NewId();
    Instruction inst{opcode, {type, result}};
    inst.operands.insert(inst.operands.end(), args);
    functions_.push_back(std::move(inst));
    type_of_[result] = type;
    return result;
  }
  uint32_t Lo(uint32_t value) { return Op(kOpCompositeExtract, uint_, {value, 0}); }
  uint32_t Hi(uint32_t value) { return Op(kOpCompositeExtract, uint_, {value, 1}); }
  void Pack(uint32_t lo, uint32_t hi, uint32_t result) {
    Op(kOpCompositeConstruct, v2uint_, {lo, hi}, result);
  }
  // A 32-bit integer value as uint (0 if it is not a 32-bit integer).
  uint32_t AsUint32(uint32_t value);
  bool LowerShift(const Instruction& inst);
  bool LowerCompare(const Instruction& inst);

  std::vector<Instruction> in_;
  size_t first_function_ = 0;
  uint32_t version_ = 0;
  uint32_t bound_ = 0;

  std::unordered_set<uint32_t> int64_types_;
  std::unordered_set<uint32_t> int64_values_;
  std::unordered_map<uint32_t, uint64_t> int64_constants_;
  std::unordered_map<uint32_t, uint32_t> type_of_;
  std::unordered_map<uint32_t, uint32_t> int_width_;  // OpTypeInt id -> width | signed << 8
  size_t helper_index_ = 0;                            // first 64-bit type declaration
  std::unordered_set<size_t> moved_;                   // declarations moved to the helpers

  uint32_t uint_ = 0, bool_ = 0, v2uint_ = 0, v2bool_ = 0, pair_ = 0;
  std::unordered_map<uint32_t, uint32_t> uint_constants_;  // value -> id usable at the helpers

  std::vector<Instruction> globals_;  // lowered global instructions
  size_t helpers_at_ = 0;             // where in globals_ the helpers go
  std::vector<Instruction> helpers_;
  std::vector<Instruction> functions_;
};

void Int64Lowering::PlaceHelperTypes() {
  // uint, bool and their 2-component vectors: an existing declaration after the first 64-bit type
  // moves up to the helpers, so everything that used the 64-bit types can use them.
  size_t uint_index = SIZE_MAX, bool_index = SIZE_MAX, v2uint_index = SIZE_MAX,
         v2bool_index = SIZE_MAX;
  for (size_t i = 0; i < first_function_; ++i) {
    const Instruction& inst = in_[i];
    const auto& w = inst.operands;
    if (inst.opcode == kOpTypeInt && w.size() >= 3 && w[1] == 32 && w[2] == 0 && !uint_) {
      uint_ = w[0];
      uint_index = i;
    } else if (inst.opcode == kOpTypeBool && w.size() >= 1 && !bool_) {
      bool_ = w[0];
      bool_index = i;
    }
  }
  for (size_t i = 0; i < first_function_; ++i) {
    const Instruction& inst = in_[i];
    const auto& w = inst.operands;
    if (inst.opcode == kOpTypeVector && w.size() >= 3 && w[2] == 2) {
      if (uint_ && w[1] == uint_ && !v2uint_) {
        v2uint_ = w[0];
        v2uint_index = i;
      } else if (bool_ && w[1] == bool_ && !v2bool_) {
        v2bool_ = w[0];
        v2bool_index = i;
      }
    }
  }
  auto place = [&](uint32_t& id, size_t index, Instruction declaration) {
    if (id && index < helper_index_) return;
    if (id) {
      moved_.insert(index);
      declaration.operands[0] = id;
    } else {
      id = declaration.operands[0] = NewId();
    }
    helpers_.push_back(std::move(declaration));
  };
  place(uint_, uint_index, {kOpTypeInt, {0, 32, 0}});
  place(v2uint_, v2uint_index, {kOpTypeVector, {0, uint_, 2}});
  if (bool_) place(bool_, bool_index, {kOpTypeBool, {0}});
  if (v2bool_) place(v2bool_, v2bool_index, {kOpTypeVector, {0, bool_, 2}});
  // uint constants declared before the helpers can be shared.
  for (size_t i = 0; i < helper_index_; ++i) {
    const auto& w = in_[i].operands;
    if (in_[i].opcode == kOpConstant && w.size() == 3 && w[0] == uint_) {
      uint_constants_.emplace(w[2], w[1]);
    }
  }
}

uint32_t Int64Lowering::BoolType() {
  if (!bool_) {
    bool_ = NewId();
    helpers_.push_back({kOpTypeBool, {bool_}});
  }
  return bool_;
}

uint32_t Int64Lowering::V2BoolType() {
  if (!v2bool_) {
    const uint32_t component = BoolType();
    v2bool_ = NewId();
    helpers_.push_back({kOpTypeVector, {v2bool_, component, 2}});
  }
  return v2bool_;
}

uint32_t Int64Lowering::PairType() {
  if (!pair_) {
    pair_ = NewId();
    helpers_.push_back({kOpTypeStruct, {pair_, uint_, uint_}});
  }
  return pair_;
}

uint32_t Int64Lowering::UintConstant(uint32_t value) {
  auto it = uint_constants_.find(value);
  if (it != uint_constants_.end()) return it->second;
  const uint32_t id = NewId();
  helpers_.push_back({kOpConstant, {uint_, id, value}});
  type_of_[id] = uint_;
  uint_constants_.emplace(value, id);
  return id;
}

uint32_t Int64Lowering::AsUint32(uint32_t value) {
  const auto type = type_of_.find(value);
  if (type == type_of_.end()) return 0;
  if (type->second == uint_) return value;
  const auto width = int_width_.find(type->second);
  if (width == int_width_.end() || (width->second & 0xFF) != 32) return 0;
  return Op(kOpBitcast, uint_, {value});
}

bool Int64Lowering::LowerGlobal(const Instruction& inst) {
  const uint32_t op = inst.opcode;
  const auto& w = inst.operands;
  Instruction out = inst;
  switch (op) {
    case kOpCapability:
      if (!w.empty() && w[0] == kCapabilityInt64Atomics) return Fail("Int64Atomics capability");
      if (!w.empty() && w[0] == kCapabilityInt64) return true;
      break;
    case kOpName:
    case kOpMemberName:
      if (!w.empty() && Is64Type(w[0])) return true;
      break;
    case kOpDecorate:
    case kOpMemberDecorate:
      if (!w.empty() && Is64Type(w[0])) return Fail("decorated 64-bit integer type");
      break;
    case kOpGroupDecorate:
    case kOpGroupMemberDecorate:
      for (size_t k = 1; k < w.size(); ++k) {
        if (Is64Type(w[k])) return Fail("group-decorated 64-bit integer type");
      }
      break;
    case kOpSourceContinued:
    case kOpSource:
    case kOpSourceExtension:
    case kOpString:
    case kOpLine:
    case kOpNoLine:
    case kOpExtension:
    case kOpExtInstImport:
    case kOpMemoryModel:
    case kOpEntryPoint:
    case kOpExecutionMode:
    case kOpExecutionModeId:
    case kOpModuleProcessed:
      break;
    case kOpTypeInt:
      if (w.size() >= 2 && w[1] == 64) return true;  // replaced by the uvec2
      break;
    case kOpTypeVector:
      if (w.size() >= 2 && Is64Type(w[1])) return Fail("vector of 64-bit integers");
      break;
    case kOpTypeImage:
      if (w.size() >= 2 && Is64Type(w[1])) return Fail("64-bit integer image");
      break;
    case kOpTypePointer:
      if (w.size() >= 3) out.operands[2] = Remap(w[2]);
      break;
    case kOpTypeStruct:
    case kOpTypeFunction:
      for (size_t k = 1; k < w.size(); ++k) out.operands[k] = Remap(w[k]);
      break;
    case kOpTypeArray:
    case kOpTypeRuntimeArray:
      if (w.size() >= 2) out.operands[1] = Remap(w[1]);
      if (op == kOpTypeArray && w.size() >= 3 && Is64Value(w[2])) {
        const auto length = int64_constants_.find(w[2]);
        if (length == int64_constants_.end() || length->second >> 32) {
          return Fail("64-bit array length");
        }
        out.operands[2] = UintConstant(uint32_t(length->second));
      }
      break;
    case kOpConstant:
      if (w.size() >= 3 && Is64Type(w[0])) {
        const uint32_t high = w.size() >= 4 ? w[3] : 0;
        out = {kOpConstantComposite, {v2uint_, w[1], UintConstant(w[2]), UintConstant(high)}};
      }
      break;
    case kOpConstantNull:
    case kOpUndef:
      if (!w.empty()) out.operands[0] = Remap(w[0]);
      break;
    case kOpConstantComposite:
    case kOpVariable:
    case kOpTypeVoid:
    case kOpTypeBool:
    case kOpTypeFloat:  // literal width
    case kOpTypeMatrix:  // float columns, literal count
    case kOpTypeSampler:
    case kOpTypeSampledImage:
    case kOpTypeForwardPointer:  // literal storage class
    case kOpConstantTrue:
    case kOpConstantFalse:
    case 73:  // OpDecorationGroup
      break;
    case kOpSpecConstantTrue:
    case kOpSpecConstantFalse:
    case kOpSpecConstant:
      if (!w.empty() && Is64Type(w[0])) return Fail("64-bit specialization constant");
      break;
    case kOpSpecConstantComposite:
    case kOpSpecConstantOp:
      if (!w.empty() && Is64Type(w[0])) return Fail("64-bit specialization constant");
      for (size_t k = op == kOpSpecConstantOp ? 3 : 2; k < w.size(); ++k) {
        if (Is64Value(w[k])) return Fail("specialization constant of a 64-bit integer");
      }
      break;
    default:
      for (uint32_t word : w) {
        if (Is64Type(word)) return Fail("global opcode " + std::to_string(op) + " uses a 64-bit type");
      }
      break;
  }
  globals_.push_back(std::move(out));
  return true;
}

bool Int64Lowering::LowerShift(const Instruction& inst) {
  const auto& w = inst.operands;
  if (w.size() != 4) return Fail("malformed shift");
  const uint32_t result = w[1], base = w[2], shift = w[3];
  const uint32_t amount = Is64Value(shift) ? Lo(shift) : AsUint32(shift);
  if (!amount) return Fail("shift amount is not a 32-bit integer");
  // Shifts of 64 or more are undefined in SPIR-V, so the amount is taken as 0..63: t = amount & 31,
  // and amounts from 32 on move the shifted word across. The bits crossing words are moved in two
  // steps ((x >> 1) >> (31 - t)) so no 32-bit shift ever reaches 32.
  const uint32_t c1 = UintConstant(1), c31 = UintConstant(31), c32 = UintConstant(32);
  const uint32_t c0 = UintConstant(0);
  const uint32_t t = Op(kOpBitwiseAnd, uint_, {amount, c31});
  const uint32_t big = Op(kOpUGreaterThanEqual, BoolType(), {amount, c32});
  const uint32_t inverse = Op(kOpISub, uint_, {c31, t});
  const uint32_t lo = Lo(base), hi = Hi(base);
  if (inst.opcode == kOpShiftLeftLogical) {
    const uint32_t lo_shifted = Op(kOpShiftLeftLogical, uint_, {lo, t});
    const uint32_t hi_shifted = Op(kOpShiftLeftLogical, uint_, {hi, t});
    const uint32_t crossing =
        Op(kOpShiftRightLogical, uint_, {Op(kOpShiftRightLogical, uint_, {lo, c1}), inverse});
    const uint32_t small_hi = Op(kOpBitwiseOr, uint_, {hi_shifted, crossing});
    Pack(Op(kOpSelect, uint_, {big, c0, lo_shifted}),
         Op(kOpSelect, uint_, {big, lo_shifted, small_hi}), result);
    return true;
  }
  const bool arithmetic = inst.opcode == kOpShiftRightArithmetic;
  const uint32_t hi_shifted = Op(arithmetic ? kOpShiftRightArithmetic : kOpShiftRightLogical,
                                 uint_, {hi, t});
  const uint32_t lo_shifted = Op(kOpShiftRightLogical, uint_, {lo, t});
  const uint32_t crossing =
      Op(kOpShiftLeftLogical, uint_, {Op(kOpShiftLeftLogical, uint_, {hi, c1}), inverse});
  const uint32_t small_lo = Op(kOpBitwiseOr, uint_, {lo_shifted, crossing});
  const uint32_t fill = arithmetic ? Op(kOpShiftRightArithmetic, uint_, {hi, c31}) : c0;
  Pack(Op(kOpSelect, uint_, {big, hi_shifted, small_lo}), Op(kOpSelect, uint_, {big, fill, hi_shifted}),
       result);
  return true;
}

bool Int64Lowering::LowerCompare(const Instruction& inst) {
  const auto& w = inst.operands;
  if (w.size() != 4) return Fail("malformed comparison");
  if (!bool_ || w[0] != bool_) return Fail("vector comparison of 64-bit integers");
  const uint32_t result = w[1];
  uint32_t a = w[2], b = w[3];
  if (!Is64Value(a) || !Is64Value(b)) return Fail("comparison of mixed widths");
  switch (inst.opcode) {
    case kOpIEqual:
    case kOpINotEqual: {
      const uint32_t components = Op(inst.opcode, V2BoolType(), {a, b});
      Op(inst.opcode == kOpIEqual ? kOpAll : kOpAny, bool_, {components}, result);
      return true;
    }
    case kOpUGreaterThan:
    case kOpUGreaterThanEqual:
      std::swap(a, b);
      [[fallthrough]];
    case kOpULessThan:
    case kOpULessThanEqual: {
      const bool or_equal =
          inst.opcode == kOpULessThanEqual || inst.opcode == kOpUGreaterThanEqual;
      const uint32_t a_lo = Lo(a), a_hi = Hi(a), b_lo = Lo(b), b_hi = Hi(b);
      const uint32_t hi_less = Op(kOpULessThan, bool_, {a_hi, b_hi});
      const uint32_t hi_equal = Op(kOpIEqual, bool_, {a_hi, b_hi});
      const uint32_t lo_less = Op(or_equal ? kOpULessThanEqual : kOpULessThan, bool_, {a_lo, b_lo});
      Op(kOpLogicalOr, bool_, {hi_less, Op(kOpLogicalAnd, bool_, {hi_equal, lo_less})}, result);
      return true;
    }
    default:
      return Fail("signed comparison of 64-bit integers");
  }
}

bool Int64Lowering::LowerFunctionInstruction(const Instruction& inst) {
  const uint32_t op = inst.opcode;
  const auto& w = inst.operands;
  const bool typed = HasResultType(op) && w.size() >= 2;
  const bool result64 = typed && Is64Type(w[0]);
  if (result64) int64_values_.insert(w[1]);
  if (typed) type_of_[w[1]] = Remap(w[0]);
  auto any64 = [&](size_t from, size_t to) {
    for (size_t k = from; k < std::min(to, w.size()); ++k) {
      if (Is64Value(w[k]) || Is64Type(w[k])) return true;
    }
    return false;
  };
  auto copy_remapped = [&]() {
    Instruction out = inst;
    if (typed) out.operands[0] = Remap(w[0]);
    functions_.push_back(std::move(out));
    return true;
  };
  switch (op) {
    // Same instruction on the uvec2: only the result type changes.
    case kOpUndef:
    case kOpCopyObject:
    case kOpPhi:
    case kOpSelect:
    case kOpFunctionParameter:
    case kOpFunctionCall:
    case kOpCompositeExtract:  // a 64-bit member of a struct
    case kOpLoad:
    case kOpBitwiseAnd:
    case kOpBitwiseOr:
    case kOpBitwiseXor:
    case kOpNot:
    case kOpFunction:
      if (op == kOpLoad && any64(2, 3)) return Fail("load through a 64-bit integer");
      if (op == kOpCompositeExtract && any64(2, 3)) return Fail("extract from a 64-bit integer");
      return copy_remapped();
    case kOpStore:
    case kOpReturnValue:
    case kOpCompositeConstruct:  // of a struct with 64-bit members
    case kOpCompositeInsert:
    case kOpVariable:
    case kOpLabel:
    case kOpBranch:
    case kOpLoopMerge:
    case kOpSelectionMerge:
    case kOpBranchConditional:
    case kOpFunctionEnd:
    case kOpLine:
    case kOpNoLine:
      if (op == kOpStore && any64(0, 1)) return Fail("store through a 64-bit integer");
      if (op == kOpBranchConditional && any64(0, 1)) return Fail("64-bit branch condition");
      functions_.push_back(inst);
      return true;
    case kOpSwitch:
      if (any64(0, 1)) return Fail("64-bit switch");
      functions_.push_back(inst);
      return true;
    case kOpVectorShuffle:
      if (any64(0, 4)) return Fail("vector shuffle of 64-bit integers");
      functions_.push_back(inst);
      return true;
    case kOpExtInst:
      if (result64 || any64(4, w.size())) return Fail("extended instruction on a 64-bit integer");
      functions_.push_back(inst);
      return true;
    case kOpAccessChain:
    case kOpInBoundsAccessChain:
    case kOpPtrAccessChain:
    case kOpInBoundsPtrAccessChain: {
      // A 64-bit index is used by its low word.
      Instruction out = inst;
      for (size_t k = 3; k < out.operands.size(); ++k) {
        if (Is64Value(out.operands[k])) out.operands[k] = Lo(out.operands[k]);
      }
      if (any64(2, 3)) return Fail("access chain on a 64-bit integer");
      functions_.push_back(std::move(out));
      return true;
    }
    case kOpIAdd:
    case kOpISub: {
      if (!result64) break;
      const uint32_t a = w[2], b = w[3];
      const uint32_t pair = Op(op == kOpIAdd ? kOpIAddCarry : kOpISubBorrow, PairType(), {Lo(a), Lo(b)});
      const uint32_t lo = Op(kOpCompositeExtract, uint_, {pair, 0});
      const uint32_t carry = Op(kOpCompositeExtract, uint_, {pair, 1});
      const uint32_t hi = Op(op, uint_, {Hi(a), Hi(b)});
      Pack(lo, Op(op, uint_, {hi, carry}), w[1]);
      return true;
    }
    case kOpSNegate: {
      if (!result64) break;
      const uint32_t c0 = UintConstant(0);
      const uint32_t pair = Op(kOpISubBorrow, PairType(), {c0, Lo(w[2])});
      const uint32_t lo = Op(kOpCompositeExtract, uint_, {pair, 0});
      const uint32_t borrow = Op(kOpCompositeExtract, uint_, {pair, 1});
      const uint32_t hi = Op(kOpISub, uint_, {c0, Hi(w[2])});
      Pack(lo, Op(kOpISub, uint_, {hi, borrow}), w[1]);
      return true;
    }
    case kOpIMul: {
      if (!result64) break;
      const uint32_t a_lo = Lo(w[2]), a_hi = Hi(w[2]), b_lo = Lo(w[3]), b_hi = Hi(w[3]);
      const uint32_t product = Op(kOpUMulExtended, PairType(), {a_lo, b_lo});
      const uint32_t lo = Op(kOpCompositeExtract, uint_, {product, 0});
      const uint32_t carry = Op(kOpCompositeExtract, uint_, {product, 1});
      const uint32_t cross = Op(kOpIAdd, uint_, {Op(kOpIMul, uint_, {a_lo, b_hi}),
                                                 Op(kOpIMul, uint_, {a_hi, b_lo})});
      Pack(lo, Op(kOpIAdd, uint_, {carry, cross}), w[1]);
      return true;
    }
    case kOpShiftLeftLogical:
    case kOpShiftRightLogical:
    case kOpShiftRightArithmetic:
      if (!result64) break;
      return LowerShift(inst);
    case kOpIEqual:
    case kOpINotEqual:
    case kOpULessThan:
    case kOpULessThanEqual:
    case kOpUGreaterThan:
    case kOpUGreaterThanEqual:
    case 173:  // OpSGreaterThan
    case 175:  // OpSGreaterThanEqual
    case 177:  // OpSLessThan
    case 179:  // OpSLessThanEqual
      if (!any64(2, 4)) break;
      return LowerCompare(inst);
    case kOpUConvert:
    case kOpSConvert: {
      if (result64) {
        const uint32_t low = AsUint32(w[2]);
        if (!low) return Fail("conversion to 64 bits from a non-32-bit integer");
        const uint32_t high = op == kOpSConvert
                                  ? Op(kOpShiftRightArithmetic, uint_, {low, UintConstant(31)})
                                  : UintConstant(0);
        Pack(low, high, w[1]);
        return true;
      }
      if (!Is64Value(w[2])) break;
      if (w[0] == uint_) {
        Op(kOpCompositeExtract, uint_, {w[2], 0}, w[1]);
        return true;
      }
      const auto width = int_width_.find(w[0]);
      if (width == int_width_.end() || (width->second & 0xFF) != 32) {
        return Fail("conversion from 64 bits to a non-32-bit integer");
      }
      Op(kOpBitcast, w[0], {Lo(w[2])}, w[1]);
      return true;
    }
    case kOpBitcast:
    case kOpConvertUToPtr:
    case kOpConvertPtrToU: {
      const bool source64 = Is64Value(w[2]);
      if (!result64 && !source64) break;
      if (result64 && source64) {
        Op(kOpCopyObject, v2uint_, {w[2]}, w[1]);
      } else if (result64 && type_of_.count(w[2]) && type_of_[w[2]] == v2uint_) {
        Op(kOpCopyObject, v2uint_, {w[2]}, w[1]);
      } else {
        // uvec2 <-> pointer or another 64-bit type: an OpBitcast (SPIR-V 1.5).
        Op(kOpBitcast, Remap(w[0]), {w[2]}, w[1]);
      }
      return true;
    }
    default:
      if (op >= kOpImageFirst && op <= kOpImageLast) {
        // Image operand masks are literals; the ids before them are the image and coordinates.
        if (result64 || any64(op == kOpImageWrite ? 0 : 2, op == kOpImageWrite ? 3 : 4)) {
          return Fail("image instruction on a 64-bit integer");
        }
        functions_.push_back(inst);
        return true;
      }
      break;
  }
  // Not lowered: it must not touch a 64-bit integer.
  if (result64 || any64(typed ? 2 : 0, w.size())) {
    return Fail("opcode " + std::to_string(op) + " on a 64-bit integer");
  }
  functions_.push_back(inst);
  return true;
}

void Int64Lowering::FixEntryPointInterfaces() {
  // SPIR-V 1.4: the interface lists every global variable the entry point uses, not only the
  // Input/Output ones. Listing all of them is the allowed superset.
  std::vector<uint32_t> variables;
  for (const Instruction& inst : globals_) {
    if (inst.opcode == kOpVariable && inst.operands.size() >= 3 &&
        inst.operands[2] != kStorageClassFunction) {
      variables.push_back(inst.operands[1]);
    }
  }
  for (Instruction& inst : globals_) {
    if (inst.opcode != kOpEntryPoint || inst.operands.size() < 3) continue;
    auto& w = inst.operands;
    size_t interface_start = 2;
    while (interface_start < w.size()) {
      const uint32_t word = w[interface_start++];
      if (!(word & 0xFF) || !(word & 0xFF00) || !(word & 0xFF0000) || !(word & 0xFF000000)) break;
    }
    std::unordered_set<uint32_t> listed(w.begin() + std::min(interface_start, w.size()), w.end());
    for (uint32_t variable : variables) {
      if (listed.insert(variable).second) w.push_back(variable);
    }
  }
}

bool Int64Lowering::Run(const uint32_t* code, size_t word_count, std::vector<uint32_t>& out) {
  if (word_count < 5 || code[0] != 0x07230203) return false;
  version_ = code[1];
  bound_ = code[3];
  // Parse.
  for (size_t i = 5; i < word_count;) {
    const uint32_t length = code[i] >> 16;
    if (!length || i + length > word_count) return Fail("malformed module");
    in_.push_back({code[i] & 0xFFFF, std::vector<uint32_t>(code + i + 1, code + i + length)});
    i += length;
  }
  first_function_ = in_.size();
  helper_index_ = SIZE_MAX;
  for (size_t i = 0; i < in_.size(); ++i) {
    const Instruction& inst = in_[i];
    const auto& w = inst.operands;
    if (inst.opcode == kOpFunction) {
      first_function_ = i;
      break;
    }
    if (inst.opcode == kOpTypeInt && w.size() >= 3) {
      int_width_[w[0]] = w[1] | (w[2] << 8);
      if (w[1] == 64) {
        int64_types_.insert(w[0]);
        helper_index_ = std::min(helper_index_, i);
      }
    }
  }
  if (int64_types_.empty()) return false;
  // Types and values of the globals.
  for (size_t i = 0; i < first_function_; ++i) {
    const auto& w = in_[i].operands;
    if (!HasResultType(in_[i].opcode) || w.size() < 2) continue;
    type_of_[w[1]] = w[0];
    if (Is64Type(w[0])) {
      int64_values_.insert(w[1]);
      if (in_[i].opcode == kOpConstant && w.size() >= 3) {
        int64_constants_[w[1]] = w[2] | (w.size() >= 4 ? uint64_t(w[3]) << 32 : 0);
      }
    }
  }
  PlaceHelperTypes();
  for (auto& entry : type_of_) entry.second = Remap(entry.second);
  // Globals, with the helpers at the first 64-bit type.
  for (size_t i = 0; i < first_function_; ++i) {
    if (i == helper_index_) helpers_at_ = globals_.size();
    if (moved_.count(i)) continue;
    if (!LowerGlobal(in_[i])) return false;
  }
  // Functions.
  for (size_t i = first_function_; i < in_.size(); ++i) {
    if (!LowerFunctionInstruction(in_[i])) return false;
  }
  if (version_ < kVersion14) FixEntryPointInterfaces();
  // Write the module.
  out.clear();
  out.reserve(word_count + word_count / 4);
  out.insert(out.end(), {code[0], std::max<uint32_t>(version_, kVersion15), code[2], bound_, code[4]});
  auto write = [&](const Instruction& inst) {
    out.push_back(uint32_t(inst.operands.size() + 1) << 16 | inst.opcode);
    out.insert(out.end(), inst.operands.begin(), inst.operands.end());
  };
  for (size_t i = 0; i < globals_.size(); ++i) {
    if (i == helpers_at_) {
      for (const Instruction& helper : helpers_) write(helper);
    }
    write(globals_[i]);
  }
  if (helpers_at_ >= globals_.size()) {
    for (const Instruction& helper : helpers_) write(helper);
  }
  for (const Instruction& inst : functions_) write(inst);
  return true;
}

}  // namespace

bool LowerSpirvInt64(const uint32_t* code, size_t word_count, std::vector<uint32_t>& lowered,
                     std::string* failure) {
  Int64Lowering lowering;
  const bool done = lowering.Run(code, word_count, lowered);
  if (failure) *failure = lowering.failure;
  return done;
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
