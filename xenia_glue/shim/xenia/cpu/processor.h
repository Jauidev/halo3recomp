// stand-in for Xenia's CPU. The recompiled game is the CPU here; the GPU only asks for
// a guest stack trace in a debug log, which this answers with "none".
#ifndef XENIA_CPU_PROCESSOR_H_
#define XENIA_CPU_PROCESSOR_H_

#include <cstdint>

namespace xe {
namespace cpu {
namespace backend {

constexpr uint32_t MAX_GUEST_PSEUDO_STACKTRACE_ENTRIES = 32;

struct GuestPseudoStackTrace {
  uint32_t count = 0;
  uint32_t truncated_flag = 0;
  uint32_t return_addrs[MAX_GUEST_PSEUDO_STACKTRACE_ENTRIES] = {};
};

class Backend {
 public:
  bool PopulatePseudoStacktrace(GuestPseudoStackTrace*) { return false; }
};

}  // namespace backend

class Processor {
 public:
  backend::Backend* backend() { return &backend_; }

 private:
  backend::Backend backend_;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_PROCESSOR_H_
