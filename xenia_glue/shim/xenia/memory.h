// stand-in for Xenia's xenia/memory.h. Xenia's GPU only needs a small part of xe::Memory:
// address translation, the physical heap's page queries and the physical memory write watches
// (invalidation callbacks). This version keeps those names and types and implements them on the
// runtime's memory (xenia_glue/memory.cc); the guest heaps themselves stay in runtime/kernel.
#ifndef XENIA_MEMORY_H_
#define XENIA_MEMORY_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/mmio_handler.h"

namespace xe {

class Memory;

enum MemoryAllocationFlag : uint32_t {
  kMemoryAllocationReserve = 1 << 0,
  kMemoryAllocationCommit = 1 << 1,
};

enum MemoryProtectFlag : uint32_t {
  kMemoryProtectRead = 1 << 0,
  kMemoryProtectWrite = 1 << 1,
  kMemoryProtectNoCache = 1 << 2,
  kMemoryProtectWriteCombine = 1 << 3,
  kMemoryProtectNoAccess = 0,
};

// Equivalent to the Win32 MEMORY_BASIC_INFORMATION struct.
struct HeapAllocationInfo {
  uint32_t base_address;
  uint32_t allocation_base;
  uint32_t allocation_protect;
  uint32_t allocation_size;
  uint32_t region_size;
  uint32_t state;
  uint32_t protect;
};

// The game's physical memory (guest physical addresses 0 - 512 MB).
class BaseHeap {
 public:
  virtual ~BaseHeap() = default;
  uint32_t host_address_offset() const { return 0; }
  uint32_t page_size() const { return 4096; }
  // Committed pages are read/write; anything else is reported as no access.
  bool QueryRegionInfo(uint32_t base_address, HeapAllocationInfo* out_info);
  xe::memory::PageAccess QueryRangeAccess(uint32_t low_address,
                                          uint32_t high_address);
};

class VirtualHeap : public BaseHeap {};
class PhysicalHeap : public BaseHeap {};

class Memory {
 public:
  // kBufferSize of SharedMemory and friends: the console's 512 MB.
  static constexpr uint32_t kPhysicalSize = 0x20000000;

  // virtual_membase: g_memory.base; physical_membase: an unprotected view of the
  // same physical memory (GPU writes through it never hit the write watches).
  Memory(uint8_t* virtual_membase, uint8_t* physical_membase);

  uint8_t* virtual_membase() const { return virtual_membase_; }
  uint8_t* physical_membase() const { return physical_membase_; }

  template <typename T = uint8_t*>
  inline T TranslateVirtual(uint32_t guest_address) const {
    return reinterpret_cast<T>(virtual_membase_ + guest_address);
  }
  template <typename T>
  inline xe::be<T>* TranslateVirtualBE(uint32_t guest_address) const {
    return TranslateVirtual<xe::be<T>*>(guest_address);
  }
  template <typename T = uint8_t*>
  inline T TranslatePhysical(uint32_t guest_address) const {
    return reinterpret_cast<T>(physical_membase_ +
                               (guest_address & 0x1FFFFFFF));
  }

  // Physical memory write watches (see Xenia's memory.h for the full contract).
  // Watched pages are write-protected in the guest's physical views (0xA0000000
  // and 0xC0000000); the first write faults, the callbacks run, the page opens.
  typedef std::pair<uint32_t, uint32_t> (*PhysicalMemoryInvalidationCallback)(
      void* context_ptr, uint32_t physical_address_start, uint32_t length,
      bool exact_range);
  void* RegisterPhysicalMemoryInvalidationCallback(
      PhysicalMemoryInvalidationCallback callback, void* callback_context);
  void UnregisterPhysicalMemoryInvalidationCallback(void* callback_handle);
  void EnablePhysicalMemoryAccessCallbacks(uint32_t physical_address,
                                           uint32_t length,
                                           bool enable_invalidation_notifications,
                                           bool enable_data_providers);
  // For host code about to write guest physical memory (file reads): fires and
  // removes the watches of the range first, as a fault would.
  void TriggerPhysicalMemoryCallbacks(uint32_t physical_address,
                                      uint32_t length);
  // Diagnostics (XPORT_XENIA_INVALIDATE=1): every GPU copy of physical memory
  // is stale, whatever the watches say; all pages are unwatched.
  void InvalidateAllPhysicalMemory();
  // Called by the fault handler; true if the address was a watched page.
  bool HandleWriteFault(void* host_address);

  VirtualHeap* GetPhysicalHeap() { return &physical_heap_; }

  bool AddVirtualMappedRange(uint32_t virtual_address, uint32_t mask,
                             uint32_t size, void* context,
                             cpu::MMIOReadCallback read_callback,
                             cpu::MMIOWriteCallback write_callback) {
    return true;
  }

  // Write protection of physical pages in both guest views (memory.cc).
  void Protect(uint32_t first_page, uint32_t page_count, bool watched);

 private:
  uint8_t* virtual_membase_;
  uint8_t* physical_membase_;
  VirtualHeap physical_heap_;
  std::vector<uint64_t> watched_;  // one bit per 4 KB physical page
  std::vector<std::pair<PhysicalMemoryInvalidationCallback, void*>*>
      invalidation_callbacks_;
};

}  // namespace xe

#endif  // XENIA_MEMORY_H_
