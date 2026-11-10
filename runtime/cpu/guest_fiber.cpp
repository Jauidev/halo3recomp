// XAPI fibers on host fibers: see guest_fiber.h. The host side is the Win32 fiber API on the PC
// and, on Android (Bionic has no makecontext/swapcontext), _setjmp/_longjmp with a stack pivot for
// the first entry: the pattern of ReXGlue's fiber_android.cpp (BSD 3-Clause, Tom Clay).
#include <stdafx.h>
#include "guest_fiber.h"
#include "guest_thread.h"
#include "ppc_context.h"
#include <kernel/function.h>
#include <kernel/memory.h>
#include <kernel/heap.h>
#include <os/logger.h>
#include <os/thread_dump.h>
#ifndef _WIN32
#include <csetjmp>
#endif

namespace
{
    // Host stack of a created fiber. The guest asks for small stacks (Halo 3: 32 KB), but each
    // recompiled guest frame is a bigger host frame, and imports run on this stack too.
    constexpr size_t kHostFiberStack = 2 * 1024 * 1024;

    struct HostFiber;
    using HostEntry = void (*)(HostFiber*);

    // ---- host fiber primitive ----------------------------------------------------------------
    struct HostFiber
    {
        uint32_t guest = 0;          // the title's fiber block
        PPCContext* ctx = nullptr;   // its registers (the thread's own context for a thread fiber)
        bool isThread = false;
        bool started = false;        // created fibers: entered at least once
        bool deleted = false;        // freed by the title while running (cannot destroy it then)
        PPCFunc* entry = nullptr;
        uint32_t stackPointer = 0;
        uint32_t argument = 0;
#ifdef _WIN32
        void* handle = nullptr;
#else
        alignas(16) jmp_buf jump{};
        void* stack = nullptr;
        // jump holds a saved context: set when SwitchHost first pivots onto the fiber's stack.
        // Not `started`, which GuestFiberSwitch sets before the first SwitchHost: with it the
        // first switch longjmp'd into the empty jmp_buf (sp 0, SIGSEGV in _longjmp on the phone).
        bool entered = false;
#endif
    };

    thread_local HostFiber* t_current;     // the host fiber running on this thread
    thread_local HostFiber* t_threadFiber; // the one this thread itself became
    // A GuestThreadExit thrown on a created fiber's stack cannot unwind past it: it is carried to
    // the thread fiber, which rethrows it so the host thread unwinds from its own stack.
    thread_local bool t_exitPending;
    thread_local uint32_t t_exitCode;

    void FiberMain(HostFiber* fiber);

#ifdef _WIN32
    void WINAPI FiberStart(void* parameter)
    {
        FiberMain(static_cast<HostFiber*>(parameter));
    }

    bool ConvertHostThread(HostFiber* fiber)
    {
        void* handle = ::ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
        if (!handle && GetLastError() == ERROR_ALREADY_FIBER)
            handle = ::GetCurrentFiber();
        fiber->handle = handle;
        return handle != nullptr;
    }

    bool CreateHostFiber(HostFiber* fiber)
    {
        fiber->handle = ::CreateFiberEx(64 * 1024, kHostFiberStack, FIBER_FLAG_FLOAT_SWITCH,
            FiberStart, fiber);
        return fiber->handle != nullptr;
    }

    void SwitchHost(HostFiber* from, HostFiber* to)
    {
        (void)from;
        t_current = to;
        ::SwitchToFiber(to->handle);
    }

    void DestroyHostFiber(HostFiber* fiber)
    {
        if (fiber->handle && !fiber->isThread)
            ::DeleteFiber(fiber->handle);
    }
#else
    bool ConvertHostThread(HostFiber*)
    {
        return true;
    }

    bool CreateHostFiber(HostFiber* fiber)
    {
        void* stack = nullptr;
        if (posix_memalign(&stack, 16, kHostFiberStack) != 0)
            return false;
        fiber->stack = stack;
        return true;
    }

    __attribute__((noinline, noreturn)) void Trampoline()
    {
        FiberMain(t_current);
        __builtin_trap();
    }

    void SwitchHost(HostFiber* from, HostFiber* to)
    {
        t_current = to;
        if (_setjmp(from->jump) != 0)
            return; // resumed
        if (to->entered || to->isThread)
            _longjmp(to->jump, 1);
        to->entered = true;
        const uintptr_t sp = (reinterpret_cast<uintptr_t>(to->stack) + kHostFiberStack) & ~uintptr_t(15);
        void (*trampoline)() = &Trampoline;
#if defined(__aarch64__)
        __asm__ volatile("mov sp, %[newsp]\n\tbr %[func]\n\t" : : [newsp] "r"(sp), [func] "r"(trampoline) : "memory");
#elif defined(__x86_64__)
        __asm__ volatile("mov %[newsp], %%rsp\n\txor %%rbp, %%rbp\n\tcall *%[func]\n\tud2\n\t"
            : : [newsp] "r"(sp), [func] "a"(trampoline) : "memory");
#else
#error Unsupported architecture for guest fibers
#endif
        __builtin_unreachable();
    }

    void DestroyHostFiber(HostFiber* fiber)
    {
        if (!fiber->isThread)
            free(fiber->stack);
    }
#endif

    // ---- guest fiber blocks -> host fibers ---------------------------------------------------
    std::mutex g_fiberMutex;
    std::unordered_map<uint32_t, HostFiber*> g_fibers;

    HostFiber* Find(uint32_t guest)
    {
        std::lock_guard lock(g_fiberMutex);
        auto it = g_fibers.find(guest);
        return it == g_fibers.end() ? nullptr : it->second;
    }

    void Destroy(HostFiber* fiber)
    {
        if (fiber->isThread)
        {
            // The thread keeps being a host fiber (it may convert again); only the block goes.
            fiber->guest = 0;
            return;
        }
        if (fiber->started)
            UnregisterGuestContext(fiber->ctx);
        DestroyHostFiber(fiber);
        delete fiber->ctx;
        delete fiber;
    }

    // The thread fiber of the calling thread, made on demand (a thread that switches without
    // having converted, or whose block was freed and made again).
    HostFiber* ThreadFiber(PPCContext& ctx)
    {
        if (!t_threadFiber)
        {
            auto* fiber = new HostFiber();
            fiber->isThread = true;
            fiber->started = true;
            if (!ConvertHostThread(fiber))
            {
                LOG_ERROR("could not make the thread a host fiber");
                delete fiber;
                return nullptr;
            }
            t_threadFiber = fiber;
            t_current = fiber;
        }
        t_threadFiber->ctx = &ctx;
        return t_threadFiber;
    }

    void FiberMain(HostFiber* fiber)
    {
        PPCContext& ctx = *fiber->ctx;
        SetPPCContext(ctx);
        ctx.fpscr.loadFromHost();
        try
        {
            fiber->entry(ctx, g_memory.base);
            LOGF_ERROR("fiber 0x{:08X}: its start routine returned", fiber->guest);
        }
        catch (const GuestThreadExit& exit)
        {
            t_exitPending = true;
            t_exitCode = exit.exitCode;
        }
        // A host fiber must never return (on Windows that ends the thread): hand the thread back
        // to its own fiber, which unwinds the thread when an exit is pending.
        for (;;)
        {
            HostFiber* back = t_threadFiber;
            if (!back || back == t_current)
                std::abort();
            SwitchHost(t_current, back);
        }
    }
}

void GuestFiberConvertThread(PPCContext& ctx, uint32_t fiber)
{
    HostFiber* host = ThreadFiber(ctx);
    if (!host)
        return;
    host->guest = fiber;
    std::lock_guard lock(g_fiberMutex);
    g_fibers[fiber] = host;
}

void GuestFiberCreate(uint32_t fiber, PPCFunc* entry, uint32_t stackPointer, uint32_t argument)
{
    auto* host = new HostFiber();
    host->guest = fiber;
    host->ctx = new PPCContext();
    host->entry = entry;
    host->stackPointer = stackPointer;
    host->argument = argument;
    if (!CreateHostFiber(host))
    {
        LOGF_ERROR("fiber 0x{:08X}: no host fiber", fiber);
        delete host->ctx;
        delete host;
        return;
    }
    HostFiber* old = nullptr;
    {
        std::lock_guard lock(g_fiberMutex);
        auto [it, inserted] = g_fibers.try_emplace(fiber, host);
        if (!inserted)
        {
            old = it->second; // a block freed without DeleteFiber and allocated again
            it->second = host;
        }
    }
    if (old && old != t_current)
        Destroy(old);
}

void GuestFiberSwitch(PPCContext& ctx, uint32_t target)
{
    HostFiber* to = Find(target);
    HostFiber* from = t_current ? t_current : ThreadFiber(ctx);
    if (!to || !from)
    {
        LOGF_ERROR("SwitchToFiber(0x{:08X}): not a fiber", target);
        return;
    }
    if (to == from)
        return;

    const uint32_t rounding = ctx.fpscr.loadFromHost();
    if (!to->started)
    {
        // Enters the start thunk: the switcher's context (r2, r13, rounding) with the fiber's own
        // stack and argument, as the console's XAPI start thunk receives them.
        *to->ctx = ctx;
        to->ctx->r1.u64 = to->stackPointer;
        to->ctx->r3.u64 = to->argument;
        to->ctx->r31.u64 = to->argument;
        to->ctx->lr = 0;
        RegisterGuestContext(to->ctx);
        to->started = true;
    }
    else
    {
        to->ctx->r13 = ctx.r13; // the thread it resumes on
    }

    SwitchHost(from, to);

    // Back on this fiber.
    SetPPCContext(ctx);
    ctx.fpscr.loadFromHost();
    ctx.fpscr.storeFromGuest(rounding);
    if (t_exitPending && t_current == t_threadFiber)
    {
        t_exitPending = false;
        throw GuestThreadExit{ t_exitCode };
    }
}

void GuestFiberDelete(uint32_t fiber)
{
    HostFiber* host = nullptr;
    {
        std::lock_guard lock(g_fiberMutex);
        auto it = g_fibers.find(fiber);
        if (it == g_fibers.end())
            return;
        host = it->second;
        g_fibers.erase(it);
    }
    if (host == t_current && !host->isThread)
    {
        host->deleted = true; // the title exits the thread right after (XAPI DeleteFiber)
        return;
    }
    Destroy(host);
}

// ---- kernel stacks (XAPI CreateFiber / DeleteFiber) ------------------------------------------
// MmCreateKernelStack(size, flags) returns the stack's top; MmDeleteKernelStack(top, bottom).
static std::mutex g_kernelStackMutex;
static std::set<uint32_t> g_kernelStacks; // bottoms

static uint32_t MmCreateKernelStack(uint32_t size, uint32_t flags)
{
    (void)flags;
    size = (std::max<uint32_t>(size, 0x4000) + 0xFFF) & ~0xFFFu;
    void* stack = g_userHeap.Alloc(size);
    if (!stack)
        return 0;
    memset(stack, 0, size);
    const uint32_t bottom = g_memory.MapVirtual(stack);
    std::lock_guard lock(g_kernelStackMutex);
    g_kernelStacks.insert(bottom);
    return bottom + size;
}

static uint32_t MmDeleteKernelStack(uint32_t top, uint32_t bottom)
{
    (void)top;
    {
        std::lock_guard lock(g_kernelStackMutex);
        if (!g_kernelStacks.erase(bottom))
            return 0;
    }
    g_userHeap.Free(g_memory.Translate(bottom));
    return 0;
}

GUEST_FUNCTION_HOOK(__imp__MmCreateKernelStack, MmCreateKernelStack);
GUEST_FUNCTION_HOOK(__imp__MmDeleteKernelStack, MmDeleteKernelStack);
