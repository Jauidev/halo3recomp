// rex::memory::Memory over the runtime's guest memory, for the native renderer, which
// only translates guest addresses (TranslateVirtual / TranslatePhysical). ReXGlue's real Memory
// (heaps, views, MMIO) is not built: the runtime's kernel/memory.cpp already maps every view,
// including the physical ones at 0xA0000000 / 0xC0000000, so virtual translation is plain
// base + address. LookupHeap answers one inert heap for every address (host offset 0, and
// native_stubs.cpp's QueryRangeAccess reports read/write).
#include <cstdlib>
#include <cstring>
#include <new>

#include <rex/system/xmemory.h>

#include "rexglue_glue.h"

namespace rex::memory {

const BaseHeap* Memory::LookupHeap(uint32_t address) const {
  (void)address;
  // Zeroed and never constructed, like the Memory itself: only host_address_offset() (0) and the
  // non-virtual QueryRangeAccess are used.
  alignas(BaseHeap) static unsigned char heap[sizeof(BaseHeap)] = {};
  return reinterpret_cast<const BaseHeap*>(heap);
}

}  // namespace rex::memory

namespace xnative {

rex::memory::Memory* CreateMemory(uint8_t* guest_base, uint8_t* physical_base) {
  // Never constructed or destroyed: the heaps and the MMIO handler it declares are not built, and
  // only the two bases are read. Zeroed storage keeps every other member inert.
  void* storage = std::calloc(1, sizeof(rex::memory::Memory));
  auto* memory = static_cast<rex::memory::Memory*>(storage);
  memory->XportSetBases(guest_base, physical_base);
  return memory;
}

}  // namespace xnative
