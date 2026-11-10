// guest virtual and physical memory for the game (NtAllocateVirtualMemory & co).
// UnleashedRecomp left these as stubs because it hooked Sonic's XAPI heap by address; in the title the
// XAPI/the engine allocators run recompiled on top of them. Semantics follow Xenia (xboxkrnl_memory.cc).
//
// The whole 4 GB guest space is already mapped read/write on the host (kernel/memory.cpp), so
// reserve/commit is bookkeeping: one state byte per 4 KB page, and fresh commits are zeroed.
#include <stdafx.h>
#include "memory.h"
#include "function.h"
#include "xdm.h"

namespace
{
    constexpr uint32_t PAGE_SIZE_4K = 0x1000;
    constexpr uint32_t PAGE_SIZE_64K = 0x10000;

    // Game virtual range: 4 KB pages low, 64 KB pages high, like the console (and Xenia).
    constexpr uint32_t VIRTUAL_4K_BEGIN = 0x00010000;
    constexpr uint32_t VIRTUAL_4K_END = 0x40000000;
    constexpr uint32_t VIRTUAL_64K_BEGIN = 0x40000000;
    constexpr uint32_t VIRTUAL_64K_END = 0x70000000; // 0x70000000+ belongs to the runtime heap

    // Game physical range: the console's 512 MB. 0xC0000000-0xDFFFFFFF is the same memory seen
    // through the second view (kernel/memory.cpp); 0xE0000000+ belongs to the runtime physical heap.
    constexpr uint32_t PHYSICAL_BEGIN = 0xA0000000;
    constexpr uint32_t PHYSICAL_END = 0xC0000000;

    constexpr uint32_t X_MEM_COMMIT = 0x00001000;
    constexpr uint32_t X_MEM_RESERVE = 0x00002000;
    constexpr uint32_t X_MEM_DECOMMIT = 0x00004000;
    constexpr uint32_t X_MEM_RELEASE = 0x00008000;
    constexpr uint32_t X_MEM_FREE = 0x00010000;
    constexpr uint32_t X_MEM_PRIVATE = 0x00020000;
    constexpr uint32_t X_MEM_RESET = 0x00080000;
    constexpr uint32_t X_MEM_LARGE_PAGES = 0x20000000;

    constexpr uint32_t X_STATUS_SUCCESS = 0x00000000;
    constexpr uint32_t X_STATUS_INVALID_PARAMETER = 0xC000000D;
    constexpr uint32_t X_STATUS_NO_MEMORY = 0xC0000017;
    constexpr uint32_t X_STATUS_CONFLICTING_ADDRESSES = 0xC0000018;
    constexpr uint32_t X_STATUS_MEMORY_NOT_ALLOCATED = 0xC00000A0;

    enum : uint8_t
    {
        PAGE_FREE = 0,
        PAGE_RESERVED = 1,
        PAGE_COMMITTED = 2,
    };

    struct PageInfo
    {
        uint8_t state;
        uint32_t allocationBase; // base of the reservation this page belongs to
        uint32_t protect;
    };

    struct Range
    {
        uint32_t begin;
        uint32_t end;
        uint32_t pageSize;
        std::vector<PageInfo> pages; // indexed in 4 KB units
        std::unordered_map<uint32_t, uint32_t> sizes; // reservation base -> size
    };

    Mutex g_vmemMutex;
    Range g_virtual4k{ VIRTUAL_4K_BEGIN, VIRTUAL_4K_END, PAGE_SIZE_4K };
    Range g_virtual64k{ VIRTUAL_64K_BEGIN, VIRTUAL_64K_END, PAGE_SIZE_64K };
    Range g_physical{ PHYSICAL_BEGIN, PHYSICAL_END, PAGE_SIZE_4K };

    Range* FindRange(uint32_t address)
    {
        for (Range* range : { &g_virtual4k, &g_virtual64k, &g_physical })
        {
            if (address >= range->begin && address < range->end)
                return range;
        }
        return nullptr;
    }

    PageInfo& Page(Range& range, uint32_t address)
    {
        if (range.pages.empty())
            range.pages.resize((range.end - range.begin) / PAGE_SIZE_4K);
        return range.pages[(address - range.begin) / PAGE_SIZE_4K];
    }

    bool IsFree(Range& range, uint32_t address, uint32_t size)
    {
        for (uint32_t a = address; a < address + size; a += PAGE_SIZE_4K)
        {
            if (Page(range, a).state != PAGE_FREE)
                return false;
        }
        return true;
    }

    // First fit, searched from the bottom or the top of [minAddress, maxAddress).
    uint32_t FindFree(Range& range, uint32_t size, uint32_t alignment, uint32_t minAddress, uint32_t maxAddress, bool topDown)
    {
        minAddress = std::max(minAddress, range.begin);
        maxAddress = std::min(maxAddress, range.end);
        alignment = std::max(alignment, range.pageSize);
        if (maxAddress <= minAddress || size > maxAddress - minAddress)
            return 0;

        if (topDown)
        {
            uint32_t address = (maxAddress - size) & ~(alignment - 1);
            for (; address >= minAddress && address + size > address; address -= alignment)
            {
                if (IsFree(range, address, size))
                    return address;
                if (address < alignment)
                    break;
            }
        }
        else
        {
            uint32_t address = (minAddress + alignment - 1) & ~(alignment - 1);
            for (; uint64_t(address) + size <= maxAddress; address += alignment)
            {
                if (IsFree(range, address, size))
                    return address;
            }
        }
        return 0;
    }

    void SetState(Range& range, uint32_t address, uint32_t size, uint8_t state, uint32_t allocationBase, uint32_t protect)
    {
        for (uint32_t a = address; a < address + size; a += PAGE_SIZE_4K)
        {
            auto& page = Page(range, a);
            if (state == PAGE_COMMITTED && page.state != PAGE_COMMITTED)
                memset(g_memory.Translate(a), 0, PAGE_SIZE_4K);
            page.state = state;
            page.allocationBase = state == PAGE_FREE ? 0 : allocationBase;
            page.protect = protect;
        }
    }

    uint32_t RoundUp(uint32_t value, uint32_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    // XPORT_LOG_VMEM=1: every NtAllocateVirtualMemory/NtFreeVirtualMemory call with its caller
    // (debugging only; nothing changes without it).
    bool LogVmem()
    {
        static const bool s_enabled = getenv("XPORT_LOG_VMEM") != nullptr;
        return s_enabled;
    }

    uint32_t GuestCaller()
    {
        auto* ctx = GetPPCContext();
        return ctx ? uint32_t(ctx->lr) : 0;
    }
}

static uint32_t NtAllocateVirtualMemoryImpl(be<uint32_t>* baseAddressPtr, be<uint32_t>* regionSizePtr, uint32_t allocationType, uint32_t protect, uint32_t debugMemory);

uint32_t NtAllocateVirtualMemory(be<uint32_t>* baseAddressPtr, be<uint32_t>* regionSizePtr, uint32_t allocationType, uint32_t protect, uint32_t debugMemory)
{
    if (!LogVmem())
        return NtAllocateVirtualMemoryImpl(baseAddressPtr, regionSizePtr, allocationType, protect, debugMemory);
    uint32_t inBase = baseAddressPtr ? uint32_t(*baseAddressPtr) : 0, inSize = regionSizePtr ? uint32_t(*regionSizePtr) : 0;
    uint32_t status = NtAllocateVirtualMemoryImpl(baseAddressPtr, regionSizePtr, allocationType, protect, debugMemory);
    LOGF_WARNING("vmem alloc 0x{:08X}+0x{:X} type 0x{:X} -> 0x{:X} 0x{:08X}+0x{:X} (lr 0x{:08X})", inBase, inSize, allocationType, status,
        baseAddressPtr ? uint32_t(*baseAddressPtr) : 0, regionSizePtr ? uint32_t(*regionSizePtr) : 0, GuestCaller());
    return status;
}

static uint32_t NtAllocateVirtualMemoryImpl(be<uint32_t>* baseAddressPtr, be<uint32_t>* regionSizePtr, uint32_t allocationType, uint32_t protect, uint32_t debugMemory)
{
    if (!baseAddressPtr || !regionSizePtr || !*regionSizePtr)
        return X_STATUS_INVALID_PARAMETER;
    if (!(allocationType & (X_MEM_COMMIT | X_MEM_RESERVE | X_MEM_RESET)))
        return X_STATUS_INVALID_PARAMETER;
    if (allocationType & X_MEM_RESET)
        return X_STATUS_SUCCESS; // contents may be discarded; keeping them is valid

    std::lock_guard lock(g_vmemMutex);

    uint32_t base = *baseAddressPtr;
    Range* range = base ? FindRange(base) : ((allocationType & X_MEM_LARGE_PAGES) ? &g_virtual64k : &g_virtual4k);
    if (!range || range == &g_physical)
        return X_STATUS_INVALID_PARAMETER;

    // Some games pass negative sizes.
    uint32_t size = int32_t(regionSizePtr->get()) < 0 ? uint32_t(-int32_t(regionSizePtr->get())) : regionSizePtr->get();
    size = RoundUp(size, range->pageSize);
    base &= ~(range->pageSize - 1);

    if (base == 0)
    {
        base = FindFree(*range, size, range->pageSize, range->begin, range->end, false);
        if (!base)
            return X_STATUS_NO_MEMORY;
        range->sizes[base] = size;
        SetState(*range, base, size, (allocationType & X_MEM_COMMIT) ? PAGE_COMMITTED : PAGE_RESERVED, base, protect);
    }
    else if (IsFree(*range, base, size))
    {
        range->sizes[base] = size;
        SetState(*range, base, size, (allocationType & X_MEM_COMMIT) ? PAGE_COMMITTED : PAGE_RESERVED, base, protect);
    }
    else
    {
        // Committing inside an existing reservation.
        if (!(allocationType & X_MEM_COMMIT))
            return X_STATUS_CONFLICTING_ADDRESSES;
        uint32_t allocationBase = Page(*range, base).allocationBase;
        for (uint32_t a = base; a < base + size; a += PAGE_SIZE_4K)
        {
            if (Page(*range, a).state == PAGE_FREE)
                return X_STATUS_CONFLICTING_ADDRESSES;
        }
        SetState(*range, base, size, PAGE_COMMITTED, allocationBase, protect);
    }

    *baseAddressPtr = base;
    *regionSizePtr = size;
    return X_STATUS_SUCCESS;
}

static uint32_t NtFreeVirtualMemoryImpl(be<uint32_t>* baseAddressPtr, be<uint32_t>* regionSizePtr, uint32_t freeType, uint32_t debugMemory);

uint32_t NtFreeVirtualMemory(be<uint32_t>* baseAddressPtr, be<uint32_t>* regionSizePtr, uint32_t freeType, uint32_t debugMemory)
{
    if (!LogVmem())
        return NtFreeVirtualMemoryImpl(baseAddressPtr, regionSizePtr, freeType, debugMemory);
    uint32_t inBase = baseAddressPtr ? uint32_t(*baseAddressPtr) : 0, inSize = regionSizePtr ? uint32_t(*regionSizePtr) : 0;
    uint32_t status = NtFreeVirtualMemoryImpl(baseAddressPtr, regionSizePtr, freeType, debugMemory);
    LOGF_WARNING("vmem free 0x{:08X}+0x{:X} type 0x{:X} -> 0x{:X} (lr 0x{:08X})", inBase, inSize, freeType, status, GuestCaller());
    return status;
}

static uint32_t NtFreeVirtualMemoryImpl(be<uint32_t>* baseAddressPtr, be<uint32_t>* regionSizePtr, uint32_t freeType, uint32_t debugMemory)
{
    if (!baseAddressPtr || !*baseAddressPtr)
        return X_STATUS_MEMORY_NOT_ALLOCATED;

    std::lock_guard lock(g_vmemMutex);

    uint32_t base = *baseAddressPtr;
    Range* range = FindRange(base);
    if (!range || range == &g_physical)
        return X_STATUS_MEMORY_NOT_ALLOCATED;

    base &= ~(range->pageSize - 1);
    if (Page(*range, base).state == PAGE_FREE)
        return X_STATUS_MEMORY_NOT_ALLOCATED;

    if (freeType & X_MEM_RELEASE)
    {
        uint32_t allocationBase = Page(*range, base).allocationBase;
        auto it = range->sizes.find(allocationBase);
        uint32_t size = it != range->sizes.end() ? it->second : range->pageSize;
        SetState(*range, allocationBase, size, PAGE_FREE, 0, 0);
        range->sizes.erase(allocationBase);
        *baseAddressPtr = allocationBase;
        if (regionSizePtr)
            *regionSizePtr = size;
    }
    else
    {
        uint32_t size = regionSizePtr && *regionSizePtr ? RoundUp(regionSizePtr->get(), range->pageSize) : range->pageSize;
        for (uint32_t a = base; a < base + size; a += PAGE_SIZE_4K)
        {
            auto& page = Page(*range, a);
            if (page.state == PAGE_COMMITTED)
                page.state = PAGE_RESERVED;
        }
        *baseAddressPtr = base;
        if (regionSizePtr)
            *regionSizePtr = size;
    }
    return X_STATUS_SUCCESS;
}

struct X_MEMORY_BASIC_INFORMATION
{
    be<uint32_t> BaseAddress;
    be<uint32_t> AllocationBase;
    be<uint32_t> AllocationProtect;
    be<uint32_t> RegionSize;
    be<uint32_t> State;
    be<uint32_t> Protect;
    be<uint32_t> Type;
};

uint32_t NtQueryVirtualMemory(uint32_t baseAddress, X_MEMORY_BASIC_INFORMATION* info, uint32_t infoLength, uint32_t debugMemory)
{
#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
    // The export is NtQueryVirtualMemory(BaseAddress, MemoryInformation, DebugMemory), with no length
    // (as in Xenia): Gears of War 3's VirtualQuery passes 0 there, and its UE3 realloc of big blocks
    // copies the RegionSize this returns (0 on failure: the names table lost its contents on growth).
    if (!info)
        return X_STATUS_INVALID_PARAMETER;
#else
    if (!info || infoLength < sizeof(X_MEMORY_BASIC_INFORMATION))
        return X_STATUS_INVALID_PARAMETER;
#endif

    std::lock_guard lock(g_vmemMutex);

    Range* range = FindRange(baseAddress);
    uint32_t address = baseAddress & ~(PAGE_SIZE_4K - 1);
    memset(info, 0, sizeof(*info));
    info->BaseAddress = address;

    if (!range)
    {
        // Image and runtime areas: report committed read/write memory.
        info->AllocationBase = address;
        info->AllocationProtect = PAGE_READWRITE;
        info->RegionSize = PAGE_SIZE_4K;
        info->State = X_MEM_COMMIT;
        info->Protect = PAGE_READWRITE;
        info->Type = X_MEM_PRIVATE;
        return X_STATUS_SUCCESS;
    }

    const PageInfo first = Page(*range, address);
    uint32_t end = address;
    while (end < range->end)
    {
        const PageInfo& page = Page(*range, end);
        if (page.state != first.state || page.allocationBase != first.allocationBase || page.protect != first.protect)
            break;
        end += PAGE_SIZE_4K;
    }

    info->AllocationBase = first.allocationBase;
    info->AllocationProtect = first.protect;
    info->RegionSize = end - address;
    info->State = first.state == PAGE_COMMITTED ? X_MEM_COMMIT : first.state == PAGE_RESERVED ? X_MEM_RESERVE : X_MEM_FREE;
    info->Protect = first.state == PAGE_COMMITTED ? first.protect : 0;
    info->Type = first.state == PAGE_FREE ? 0 : X_MEM_PRIVATE;
    return X_STATUS_SUCCESS;
}

uint32_t MmAllocatePhysicalMemoryEx(uint32_t flags, uint32_t size, uint32_t protect, uint32_t minAddress, uint32_t maxAddress, uint32_t alignment)
{
    std::lock_guard lock(g_vmemMutex);

    // Physical constraints are in physical addresses (0 .. 512 MB); 0 means unbounded.
    uint32_t minVirtual = PHYSICAL_BEGIN + (minAddress & 0x1FFFFFFF);
    uint32_t maxVirtual = maxAddress && (maxAddress & 0x1FFFFFFF) ? PHYSICAL_BEGIN + std::min<uint32_t>(maxAddress, 0x1FFFFFFF) + 1 : PHYSICAL_END;
    // Large page allocations (64 KB / 16 MB) come with the matching alignment request.
    uint32_t pageAlignment = (protect & X_MEM_LARGE_PAGES) ? PAGE_SIZE_64K : PAGE_SIZE_4K;
    uint32_t align = std::max(alignment ? RoundUp(alignment, PAGE_SIZE_4K) : PAGE_SIZE_4K, pageAlignment);
    size = RoundUp(size, PAGE_SIZE_4K);

    uint32_t address = FindFree(g_physical, size, align, minVirtual, maxVirtual, (flags & 0x2) != 0);
    if (!address)
    {
        LOGF_ERROR("out of physical memory: size 0x{:X} align 0x{:X} range 0x{:X}-0x{:X}", size, align, minAddress, maxAddress);
        return 0;
    }

    g_physical.sizes[address] = size;
    SetState(g_physical, address, size, PAGE_COMMITTED, address, protect);
    return address;
}

void MmFreePhysicalMemory(uint32_t type, uint32_t address)
{
    if (!address)
        return;

    std::lock_guard lock(g_vmemMutex);
    auto it = g_physical.sizes.find(address);
    if (it == g_physical.sizes.end())
    {
        LOGF_WARNING("not an allocation: 0x{:X}", address);
        return;
    }
    SetState(g_physical, address, it->second, PAGE_FREE, 0, 0);
    g_physical.sizes.erase(it);
}

// Debugging (VblankThread status): committed game physical memory per 32 MB of the console's 512 MB,
// in MB, and the total.
std::string VmemPhysicalStats()
{
    std::lock_guard lock(g_vmemMutex);
    uint32_t chunks[16] = {};
    uint32_t total = 0;
    for (size_t i = 0; i < g_physical.pages.size(); ++i)
    {
        if (g_physical.pages[i].state == PAGE_COMMITTED)
        {
            ++chunks[std::min<size_t>(i / (0x2000000 / PAGE_SIZE_4K), 15)];
            ++total;
        }
    }
    std::string line = fmt::format("game physical memory {} MB:", total * PAGE_SIZE_4K >> 20);
    for (uint32_t chunk : chunks)
        line += fmt::format(" {}", chunk * PAGE_SIZE_4K >> 20);
    return line;
}

uint32_t MmGetPhysicalAddress(uint32_t address)
{
    return address & 0x1FFFFFFF;
}

uint32_t MmQueryAddressProtect(uint32_t address)
{
    std::lock_guard lock(g_vmemMutex);
    Range* range = FindRange(address);
    if (!range)
        return PAGE_READWRITE;
    const PageInfo& page = Page(*range, address & ~(PAGE_SIZE_4K - 1));
    return page.state == PAGE_COMMITTED ? page.protect : 0;
}

uint32_t MmQueryAllocationSize(uint32_t address)
{
    std::lock_guard lock(g_vmemMutex);
    Range* range = FindRange(address);
    if (!range)
        return 0;
    auto it = range->sizes.find(Page(*range, address & ~(PAGE_SIZE_4K - 1)).allocationBase);
    return it != range->sizes.end() ? it->second : 0;
}

GUEST_FUNCTION_HOOK(__imp__NtAllocateVirtualMemory, NtAllocateVirtualMemory);
GUEST_FUNCTION_HOOK(__imp__NtFreeVirtualMemory, NtFreeVirtualMemory);
GUEST_FUNCTION_HOOK(__imp__NtQueryVirtualMemory, NtQueryVirtualMemory);
GUEST_FUNCTION_HOOK(__imp__MmAllocatePhysicalMemoryEx, MmAllocatePhysicalMemoryEx);
GUEST_FUNCTION_HOOK(__imp__MmFreePhysicalMemory, MmFreePhysicalMemory);
GUEST_FUNCTION_HOOK(__imp__MmGetPhysicalAddress, MmGetPhysicalAddress);
GUEST_FUNCTION_HOOK(__imp__MmQueryAddressProtect, MmQueryAddressProtect);
GUEST_FUNCTION_HOOK(__imp__MmQueryAllocationSize, MmQueryAllocationSize);
