// stand-in for Xenia's KernelState: the GPU asks it for memory, the CPU (only for a
// debug stack trace), the emulator (only for the window title) and the title module.
#ifndef XENIA_KERNEL_KERNEL_STATE_H_
#define XENIA_KERNEL_KERNEL_STATE_H_

#include <cstdint>

#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

namespace xe {
namespace kernel {

class KernelState {
 public:
  KernelState(Memory* memory, uint32_t title_id)
      : memory_(memory), module_(title_id) {}

  Memory* memory() const { return memory_; }
  cpu::Processor* processor() { return &processor_; }
  Emulator* emulator() { return &emulator_; }
  uint32_t GetIdleProcess() const { return 0; }
  UserModule* GetExecutableModule() { return &module_; }
  bool is_title_open() const { return true; }

 private:
  Memory* memory_;
  cpu::Processor processor_;
  Emulator emulator_;
  UserModule module_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_KERNEL_STATE_H_
