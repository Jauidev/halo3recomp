// pieces of ReXGlue the ported hooks reference but this runtime does not use: its
// kernel state (the hooks' memory lookups go through rexglue_glue's GuestMemory()).
#include <cstdint>

#include <rex/system/xmemory.h>

#include "xd3d_init.h"

namespace rex::system {
class KernelState;
KernelState* kernel_state() { return nullptr; }
}  // namespace rex::system

namespace rex::memory {
// The heap memory.cpp's LookupHeap returns for every address: the runtime keeps its own page
// table, so the hooks' readability checks only see whole read/write memory.
PageAccess BaseHeap::QueryRangeAccess(uint32_t low_address, uint32_t high_address) {
  (void)low_address;
  (void)high_address;
  return PageAccess::kReadWrite;
}
}  // namespace rex::memory
