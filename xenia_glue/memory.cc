// xe::Memory for Xenia's GPU (see shim/xenia/memory.h) on the runtime's memory.
// The game's physical memory is one shared memory object seen at 0xA0000000 and 0xC0000000 in
// the guest space (runtime/kernel/memory.cpp) and once more, unprotected, at physical_membase.
// Write watches follow Xenia's PhysicalHeap::TriggerCallbacks: watched pages are read-only in
// both guest views (xe::memory::Protect: mprotect or VirtualProtect); the first write faults (runtime/os/crash.cpp asks HandleWriteFault first),
// every invalidation callback hears about the page, and the range all of them agree on (at
// least the page, at most its aligned 4 MB) is unwatched and writable again.
#include "xenia/memory.h"

#include <algorithm>

namespace xe {

namespace {
constexpr uint32_t kPageShift = 12;
constexpr uint32_t kPageSize = 1u << kPageShift;
constexpr uint32_t kPageCount = Memory::kPhysicalSize >> kPageShift;
constexpr uint32_t kGuestViews[] = {0xA0000000, 0xC0000000};
constexpr uint32_t kMaxUnwatchExcess = 4 * 1024 * 1024;
}  // namespace

bool BaseHeap::QueryRegionInfo(uint32_t base_address,
                               HeapAllocationInfo* out_info) {
  // The runtime's physical allocator (runtime/kernel/vmem.cpp) is not visible
  // from here; physical memory is reported as committed and writable, which is
  // what the GPU's callers check before writing resolved data back.
  const uint32_t page = base_address & ~(kPageSize - 1);
  out_info->base_address = page;
  out_info->allocation_base = page;
  out_info->allocation_protect = kMemoryProtectRead | kMemoryProtectWrite;
  // One region up to the end of physical memory: the resolve readback checks
  // that the whole written range fits in the queried region.
  out_info->allocation_size = Memory::kPhysicalSize - (page & 0x1FFFFFFF);
  out_info->region_size = out_info->allocation_size;
  out_info->state = kMemoryAllocationReserve | kMemoryAllocationCommit;
  out_info->protect = kMemoryProtectRead | kMemoryProtectWrite;
  return true;
}

xe::memory::PageAccess BaseHeap::QueryRangeAccess(uint32_t, uint32_t) {
  return xe::memory::PageAccess::kReadWrite;
}

Memory::Memory(uint8_t* virtual_membase, uint8_t* physical_membase)
    : virtual_membase_(virtual_membase),
      physical_membase_(physical_membase),
      watched_(kPageCount / 64) {}

void* Memory::RegisterPhysicalMemoryInvalidationCallback(
    PhysicalMemoryInvalidationCallback callback, void* callback_context) {
  auto* entry = new std::pair<PhysicalMemoryInvalidationCallback, void*>(
      callback, callback_context);
  auto global_lock = xe::global_critical_region::Acquire();
  invalidation_callbacks_.push_back(entry);
  return entry;
}

void Memory::UnregisterPhysicalMemoryInvalidationCallback(
    void* callback_handle) {
  auto* entry =
      static_cast<std::pair<PhysicalMemoryInvalidationCallback, void*>*>(
          callback_handle);
  {
    auto global_lock = xe::global_critical_region::Acquire();
    invalidation_callbacks_.erase(
        std::remove(invalidation_callbacks_.begin(),
                    invalidation_callbacks_.end(), entry),
        invalidation_callbacks_.end());
  }
  delete entry;
}

void Memory::Protect(uint32_t first_page, uint32_t page_count, bool watched) {
  for (uint32_t view : kGuestViews) {
    xe::memory::Protect(
        virtual_membase_ + view + (size_t(first_page) << kPageShift),
        size_t(page_count) << kPageShift,
        watched ? xe::memory::PageAccess::kReadOnly
                : xe::memory::PageAccess::kReadWrite,
        nullptr);
  }
}

void Memory::EnablePhysicalMemoryAccessCallbacks(
    uint32_t physical_address, uint32_t length,
    bool enable_invalidation_notifications, bool enable_data_providers) {
  if (!enable_invalidation_notifications || length == 0) {
    return;
  }
  physical_address &= kPhysicalSize - 1;
  const uint32_t first = physical_address >> kPageShift;
  const uint32_t last = std::min(
      uint32_t((uint64_t(physical_address) + length - 1) >> kPageShift),
      kPageCount - 1);
  auto global_lock = xe::global_critical_region::Acquire();
  // Protect only the runs of pages that are not watched yet.
  uint32_t run_first = UINT32_MAX;
  for (uint32_t page = first; page <= last + 1; ++page) {
    const bool newly_watched =
        page <= last && !(watched_[page >> 6] & (uint64_t(1) << (page & 63)));
    if (newly_watched) {
      watched_[page >> 6] |= uint64_t(1) << (page & 63);
      if (run_first == UINT32_MAX) {
        run_first = page;
      }
    } else if (run_first != UINT32_MAX) {
      Protect(run_first, page - run_first, true);
      run_first = UINT32_MAX;
    }
  }
}

// Fires the callbacks for physical pages [first, last] and unwatches what they
// allow. Called with the global critical region held (Xenia's SharedMemory takes the same
// lock around its calls into Memory, so one lock keeps the order).
static void TriggerLocked(
    Memory& memory, std::vector<uint64_t>& watched,
    const std::vector<std::pair<Memory::PhysicalMemoryInvalidationCallback,
                                void*>*>& callbacks,
    uint32_t first, uint32_t last,
    void (Memory::*protect)(uint32_t, uint32_t, bool)) {
  const uint32_t start = first << kPageShift;
  const uint32_t length = (last - first + 1) << kPageShift;
  uint32_t unwatch_first = 0;
  uint32_t unwatch_last = UINT32_MAX;
  for (auto* callback : callbacks) {
    const auto range = callback->first(callback->second, start, length, false);
    unwatch_first = std::max(unwatch_first, range.first);
    unwatch_last = std::min(
        unwatch_last,
        uint32_t(std::min<uint64_t>(
            uint64_t(range.first) + std::max(range.second, uint32_t(1)) - 1,
            UINT32_MAX)));
  }
  unwatch_first = std::min(unwatch_first, start);
  unwatch_last = std::max(unwatch_last, start + length - 1);
  unwatch_first = std::max(unwatch_first, start & ~(kMaxUnwatchExcess - 1));
  unwatch_last =
      std::min(unwatch_last, (start + length - 1) | (kMaxUnwatchExcess - 1));
  unwatch_last = std::min(unwatch_last, Memory::kPhysicalSize - 1);

  const uint32_t page_first = unwatch_first >> kPageShift;
  const uint32_t page_last = unwatch_last >> kPageShift;
  uint32_t run_first = UINT32_MAX;
  for (uint32_t page = page_first; page <= page_last + 1; ++page) {
    const bool was_watched =
        page <= page_last && (watched[page >> 6] & (uint64_t(1) << (page & 63)));
    if (was_watched) {
      watched[page >> 6] &= ~(uint64_t(1) << (page & 63));
      if (run_first == UINT32_MAX) {
        run_first = page;
      }
    } else if (run_first != UINT32_MAX) {
      (memory.*protect)(run_first, page - run_first, false);
      run_first = UINT32_MAX;
    }
  }
}

bool Memory::HandleWriteFault(void* host_address) {
  const auto* address = static_cast<const uint8_t*>(host_address);
  uint32_t physical = UINT32_MAX;
  for (uint32_t view : kGuestViews) {
    const uint8_t* view_base = virtual_membase_ + view;
    if (address >= view_base && address < view_base + kPhysicalSize) {
      physical = uint32_t(address - view_base);
    }
  }
  if (physical == UINT32_MAX) {
    return false;
  }
  const uint32_t page = physical >> kPageShift;
  auto global_lock = xe::global_critical_region::Acquire();
  if (watched_[page >> 6] & (uint64_t(1) << (page & 63))) {
    TriggerLocked(*this, watched_, invalidation_callbacks_, page, page,
                  &Memory::Protect);
  }
  // Not watched (any more): another thread opened the page between the fault
  // and the lock. Either way the write can be retried.
  return true;
}

void Memory::TriggerPhysicalMemoryCallbacks(uint32_t physical_address,
                                            uint32_t length) {
  if (length == 0) {
    return;
  }
  physical_address &= kPhysicalSize - 1;
  const uint32_t first = physical_address >> kPageShift;
  const uint32_t last = std::min(
      uint32_t((uint64_t(physical_address) + length - 1) >> kPageShift),
      kPageCount - 1);
  auto global_lock = xe::global_critical_region::Acquire();
  bool any = false;
  for (uint32_t page = first; page <= last && !any; ++page) {
    any = (watched_[page >> 6] & (uint64_t(1) << (page & 63))) != 0;
  }
  if (any) {
    TriggerLocked(*this, watched_, invalidation_callbacks_, first, last,
                  &Memory::Protect);
  }
}

void Memory::InvalidateAllPhysicalMemory() {
  auto global_lock = xe::global_critical_region::Acquire();
  for (auto* callback : invalidation_callbacks_) {
    callback->first(callback->second, 0, kPhysicalSize, true);
  }
  std::fill(watched_.begin(), watched_.end(), 0);
  Protect(0, kPageCount, false);
}

}  // namespace xe
