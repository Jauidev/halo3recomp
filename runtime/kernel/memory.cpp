#include <game.h>
#include <stdafx.h>
#include "memory.h"
#include <os/thread_dump.h>
#ifndef _WIN32
#include <sys/syscall.h>
#endif

// the console sees its 512 MB of physical RAM at 0xA0000000 and again at 0xC0000000,
// and D3D relies on it: the command lists its per-CPU threads walk link to each other through
// 0xC0000000 pointers (sub_82A4E9B8). Both views map one shared memory object here. The third
// console view, 0xE0000000 (offset by 4 KB), is not reproduced: the runtime's own heap lives there
// (kernel/heap.cpp). The game's physical allocations stay in 0xA0000000-0xBFFFFFFF (kernel/vmem.cpp).
constexpr size_t PHYSICAL_SIZE = 0x20000000;
constexpr size_t PHYSICAL_VIEWS[] = { 0xA0000000, 0xC0000000 };

#ifdef _WIN32
// Placeholders (Windows 10 1803+) let a file mapping replace part of one big reservation.
// Looked up at run time so the build needs no newer import library.
static uint8_t* AllocateWithPhysicalViews(uint8_t*& physical)
{
    using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
    using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
    constexpr ULONG RESERVE_PLACEHOLDER = 0x00040000;  // MEM_RESERVE_PLACEHOLDER
    constexpr ULONG REPLACE_PLACEHOLDER = 0x00004000;  // MEM_REPLACE_PLACEHOLDER
    constexpr ULONG PRESERVE_PLACEHOLDER = 0x00000002; // MEM_PRESERVE_PLACEHOLDER

    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    auto virtualAlloc2 = kernelBase ? (VirtualAlloc2Fn)GetProcAddress(kernelBase, "VirtualAlloc2") : nullptr;
    auto mapViewOfFile3 = kernelBase ? (MapViewOfFile3Fn)GetProcAddress(kernelBase, "MapViewOfFile3") : nullptr;
    if (!virtualAlloc2 || !mapViewOfFile3)
        return nullptr;

    HANDLE process = GetCurrentProcess();
    auto* base = (uint8_t*)virtualAlloc2(process, (void*)0x100000000ull, PPC_MEMORY_SIZE, MEM_RESERVE | RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    if (base == nullptr)
        base = (uint8_t*)virtualAlloc2(process, nullptr, PPC_MEMORY_SIZE, MEM_RESERVE | RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    if (base == nullptr)
        return nullptr;

    // Split the placeholder at every view, then fill the pieces in address order.
    for (size_t view : PHYSICAL_VIEWS)
        VirtualFree(base + view, PHYSICAL_SIZE, MEM_RELEASE | PRESERVE_PLACEHOLDER);

    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, PHYSICAL_SIZE >> 32, PHYSICAL_SIZE & 0xFFFFFFFF, nullptr);
    if (section == nullptr)
        return nullptr;

    size_t address = 0;
    auto commit = [&](size_t end)
    {
        return end == address || virtualAlloc2(process, base + address, end - address, MEM_RESERVE | MEM_COMMIT | REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    };
    for (size_t view : PHYSICAL_VIEWS)
    {
        if (!commit(view) || !mapViewOfFile3(section, process, base + view, 0, PHYSICAL_SIZE, REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
            return nullptr;
        address = view + PHYSICAL_SIZE;
    }
    if (!commit(PPC_MEMORY_SIZE))
        return nullptr;

    // The unprotected view for Xenia's GPU (see Memory::physical).
    physical = (uint8_t*)MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, PHYSICAL_SIZE);
    return base;
}
#endif

Memory::Memory()
{
#ifdef _WIN32
    base = AllocateWithPhysicalViews(physical);

    if (base == nullptr)
    {
        DiagPrint("guest memory: no physical memory views (needs Windows 10 1803+); D3D will stall\n");
        base = (uint8_t*)VirtualAlloc((void*)0x100000000ull, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }

    if (base == nullptr)
        base = (uint8_t*)VirtualAlloc(nullptr, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (base == nullptr)
        return;

    DWORD oldProtect;
    VirtualProtect(base, 4096, PAGE_NOACCESS, &oldProtect);
#else
    // MAP_NORESERVE so the 4 GB are not charged against Android's commit limit; pages
    // become real memory only when touched.
    constexpr int flags = MAP_ANON | MAP_PRIVATE | MAP_NORESERVE;
    base = (uint8_t*)mmap((void*)0x100000000ull, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, flags, -1, 0);

    if (base == (uint8_t*)MAP_FAILED)
        base = (uint8_t*)mmap(NULL, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, flags, -1, 0);

    if (base == (uint8_t*)MAP_FAILED)
    {
        base = nullptr;
        return;
    }

    mprotect(base, 4096, PROT_NONE);

    // The physical views replace their part of the anonymous mapping. memfd_create through
    // syscall(): the libc wrapper needs API 30.
    const int fd = int(syscall(__NR_memfd_create, "xport-physical", 1u /* MFD_CLOEXEC */));
    bool mapped = fd >= 0 && ftruncate(fd, PHYSICAL_SIZE) == 0;
    for (size_t view : PHYSICAL_VIEWS)
    {
        mapped = mapped && mmap(base + view, PHYSICAL_SIZE, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_FIXED | MAP_NORESERVE, fd, 0) != MAP_FAILED;
    }
    if (mapped)
    {
        // Xenia's physical_membase: GPU writes (resolves, fences) go here and so never fault on
        // the write watches the GPU sets in the guest views.
        physical = (uint8_t*)mmap(nullptr, PHYSICAL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, fd, 0);
        if (physical == (uint8_t*)MAP_FAILED)
            physical = nullptr;
    }
    if (!mapped)
        DiagPrint("guest memory: physical memory views failed (errno %d); D3D will stall\n", errno);
    if (fd >= 0)
        close(fd);
#endif

    for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++)
    {
        if (PPCFuncMappings[i].host != nullptr)
            InsertFunction(PPCFuncMappings[i].guest, PPCFuncMappings[i].host);
    }

    game::RegisterExtraFunctions(*this);
}

void* MmGetHostAddress(uint32_t ptr)
{
    return g_memory.Translate(ptr);
}
