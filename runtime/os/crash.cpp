// crash reporter. Prints the host fault, the guest address when the fault is inside the
// 4 GB guest space, and the guest registers of the faulting thread (LR tells which recompiled
// sub_XXXXXXXX was running). Symbolize the host offset with llvm-symbolizer against the game's .exe / libgame.so.
#include <stdafx.h>
#include <cpu/ppc_context.h>
#include <kernel/memory.h>
#include "thread_dump.h"
#include "paths.h"
#ifdef _WIN32
#include <dbghelp.h>
#include <csignal>
#include <exception>
#else
#include <signal.h>
#include <dlfcn.h>
#include <sys/prctl.h>
#include <unwind.h>
#endif
#include <xport_gpu.h>

static void DumpGuestState(uint64_t faultAddress)
{
    if (g_memory.base && faultAddress >= uint64_t(g_memory.base) && faultAddress < uint64_t(g_memory.base) + PPC_MEMORY_SIZE)
        DiagPrint("  guest address: 0x%08X\n", uint32_t(faultAddress - uint64_t(g_memory.base)));

    PPCContext* ctx = GetPPCContext();
    if (!ctx)
    {
        DiagPrint("  no guest context on this thread\n");
        return;
    }
#ifndef PPC_CONFIG_SKIP_LR
    const uint32_t lr = uint32_t(ctx->lr);
#else
    const uint32_t lr = 0;
#endif
#ifndef PPC_CONFIG_CTR_AS_LOCAL
    const uint32_t ctr = ctx->ctr.u32;
#else
    const uint32_t ctr = 0;
#endif
#ifndef PPC_CONFIG_NON_ARGUMENT_AS_LOCAL
    const uint32_t r11 = ctx->r11.u32;
    const uint32_t r12 = ctx->r12.u32;
#else
    const uint32_t r11 = 0;
    const uint32_t r12 = 0;
#endif
    DiagPrint("  guest lr=0x%08X ctr=0x%08X r1=0x%08X r13=0x%08X\n", lr, ctr, ctx->r1.u32, ctx->r13.u32);
    DiagPrint("  r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X r7=0x%08X r8=0x%08X r9=0x%08X r10=0x%08X r11=0x%08X r12=0x%08X\n",
        ctx->r3.u32, ctx->r4.u32, ctx->r5.u32, ctx->r6.u32, ctx->r7.u32, ctx->r8.u32, ctx->r9.u32, ctx->r10.u32, r11, r12);
#ifndef PPC_CONFIG_NON_VOLATILE_AS_LOCAL
    DiagPrint("  r14=0x%08X r15=0x%08X r16=0x%08X r17=0x%08X r18=0x%08X r19=0x%08X r20=0x%08X r21=0x%08X\n", ctx->r14.u32, ctx->r15.u32,
        ctx->r16.u32, ctx->r17.u32, ctx->r18.u32, ctx->r19.u32, ctx->r20.u32, ctx->r21.u32);
    DiagPrint("  r22=0x%08X r23=0x%08X r24=0x%08X r25=0x%08X r26=0x%08X r27=0x%08X\n", ctx->r22.u32, ctx->r23.u32, ctx->r24.u32, ctx->r25.u32,
        ctx->r26.u32, ctx->r27.u32);
    DiagPrint("  r28=0x%08X r29=0x%08X r30=0x%08X r31=0x%08X\n", ctx->r28.u32, ctx->r29.u32, ctx->r30.u32, ctx->r31.u32);
#ifdef _WIN32
    // XPORT_CRASH_WORDS=r27+0x260:0xE0,r19:0x100: guest memory to dump, by register.
    if (const char* spec = std::getenv("XPORT_CRASH_WORDS"))
    {
        const uint32_t* const regs[] = {&ctx->r0.u32, &ctx->r1.u32, &ctx->r2.u32, &ctx->r3.u32, &ctx->r4.u32, &ctx->r5.u32, &ctx->r6.u32, &ctx->r7.u32,
            &ctx->r8.u32, &ctx->r9.u32, &ctx->r10.u32, &ctx->r11.u32, &ctx->r12.u32, &ctx->r13.u32, &ctx->r14.u32, &ctx->r15.u32, &ctx->r16.u32,
            &ctx->r17.u32, &ctx->r18.u32, &ctx->r19.u32, &ctx->r20.u32, &ctx->r21.u32, &ctx->r22.u32, &ctx->r23.u32, &ctx->r24.u32, &ctx->r25.u32,
            &ctx->r26.u32, &ctx->r27.u32, &ctx->r28.u32, &ctx->r29.u32, &ctx->r30.u32, &ctx->r31.u32};
        for (const char* p = spec; *p == 'r' || *p == '[';)
        {
            // [r19+0xD0]+0x10:0x100 follows the pointer at r19+0xD0.
            const bool deref = *p == '[';
            p += deref ? 2 : 1;
            char* end = nullptr;
            const unsigned long index = strtoul(p, &end, 10);
            uint32_t address = index < 32 ? *regs[index] : 0;
            if (*end == '+') address += uint32_t(strtoul(end + 1, &end, 0));
            const auto readable = [](uint32_t guest, uint32_t size)
            {
                MEMORY_BASIC_INFORMATION info{};
                return guest >= 0x1000 && guest < PPC_MEMORY_SIZE - size &&
                    VirtualQuery(g_memory.base + guest, &info, sizeof(info)) && info.State == MEM_COMMIT &&
                    !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                    uint64_t(g_memory.base + guest) + size <= uint64_t(info.BaseAddress) + info.RegionSize;
            };
            if (deref)
            {
                if (*end == ']') end++;
                address = readable(address, 4) ? ByteSwap(*reinterpret_cast<uint32_t*>(g_memory.base + address)) : 0;
                if (*end == '+') address += uint32_t(strtoul(end + 1, &end, 0));
            }
            const uint32_t size = *end == ':' ? uint32_t(strtoul(end + 1, &end, 0)) : 0x40;
            DiagPrint("  dump %.*s = 0x%08X\n", int(end - p), p, address);
            for (uint32_t offset = 0; readable(address, size) && offset < size; offset += 16)
            {
                const uint32_t* w = reinterpret_cast<uint32_t*>(g_memory.base + address + offset);
                DiagPrint("    +%03X %08X %08X %08X %08X\n", offset, ByteSwap(w[0]), ByteSwap(w[1]), ByteSwap(w[2]), ByteSwap(w[3]));
            }
            p = *end == ',' ? end + 1 : end;
        }
    }
#endif
#endif

    // A call through a null pointer: the object and its table tell if it was freed or overwritten.
    if (!ctr && g_memory.base && ctx->r3.u32 >= 0x1000 && ctx->r3.u32 < PPC_MEMORY_SIZE - 0x80)
    {
        const auto word = [](uint32_t address) { return ByteSwap(*reinterpret_cast<uint32_t*>(g_memory.base + address)); };
        for (uint32_t offset = 0; offset < 0x80; offset += 0x20)
        {
            const uint32_t at = ctx->r3.u32 + offset;
            DiagPrint("  [r3+%02X] %08X %08X %08X %08X %08X %08X %08X %08X\n", offset, word(at), word(at + 4),
                word(at + 8), word(at + 12), word(at + 16), word(at + 20), word(at + 24), word(at + 28));
        }
    }

    PrintGuestBacktrace(ctx);
}

// The report is also written to crash.txt next to the save folder (on the phone:
// Android/data/com.xport/files/crash.txt), replaced by every new crash.
static void SetReportPath()
{
    DiagSetReportPath((os::paths::GetSavePath().parent_path() / "crash.txt").string());
}

#ifdef _WIN32
static void PrintHostBacktrace()
{
    // Host backtrace (offsets inside each module). Deep stacks (recursion, stack overflow) print
    // their top and bottom frames: the bottom shows how the thread got there.
    static void* frames[8192];
    uint32_t count = 0;
    while (count < std::size(frames))
    {
        const USHORT got = RtlCaptureStackBackTrace(count, USHORT(std::min<size_t>(62, std::size(frames) - count)), frames + count, nullptr);
        if (got == 0)
            break;
        count += got;
    }
    for (uint32_t i = 0; i < count; i++)
    {
        if (i == 24 && count > 48)
        {
            DiagPrint("  ... %u frames ...\n", count - 48);
            i = count - 24;
        }
        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m);
        DiagPrint("  #%02u +0x%llX\n", i, (unsigned long long)(uint64_t(frames[i]) - uint64_t(m)));
    }
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info)
{
    const auto* record = info->ExceptionRecord;
    // A write to a page Xenia's GPU watches (XPORT_GPU=xenia): it fires the watch, opens the page
    // and the write is retried.
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2 &&
        record->ExceptionInformation[0] == 1 && xportgpu::HandleWriteFault((void*)record->ExceptionInformation[1]))
        return EXCEPTION_CONTINUE_EXECUTION;
    // Ignore first-chance C++ exceptions and debugger notifications.
    if (record->ExceptionCode == 0xE06D7363 || record->ExceptionCode == 0x406D1388 || record->ExceptionCode < 0x80000000)
        return EXCEPTION_CONTINUE_SEARCH;

    DiagOpenReportFile();
    const uint64_t pc = uint64_t(record->ExceptionAddress);
    HMODULE module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &module);
    char moduleName[MAX_PATH] = "?";
    if (module)
        GetModuleFileNameA(module, moduleName, sizeof(moduleName));

    DiagPrint("\n*** crash: exception 0x%08lX at %s+0x%llX\n", record->ExceptionCode, strrchr(moduleName, '\\') ? strrchr(moduleName, '\\') + 1 : moduleName,
        (unsigned long long)(pc - uint64_t(module)));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
    {
        DiagPrint("  %s 0x%llX\n", record->ExceptionInformation[0] ? "write to" : "read from", (unsigned long long)record->ExceptionInformation[1]);
        DumpGuestState(record->ExceptionInformation[1]);
    }
    else
    {
        DumpGuestState(0);
    }

    PrintHostBacktrace();
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

// abort() (also reached from std::terminate and failed asserts) ends the process with a fast fail
// that no exception handler sees: report it here.
static void AbortHandler(int)
{
    DiagOpenReportFile();
    DiagPrint("\n*** crash: abort()\n");
    DumpGuestState(0);
    PrintHostBacktrace();
    fflush(stderr);
}

static void TerminateHandler()
{
    DiagOpenReportFile();
    try
    {
        if (std::current_exception())
            std::rethrow_exception(std::current_exception());
        DiagPrint("\n*** crash: std::terminate without an exception\n");
    }
    catch (const std::exception& e)
    {
        DiagPrint("\n*** crash: uncaught exception: %s\n", e.what());
    }
    catch (...)
    {
        DiagPrint("\n*** crash: uncaught non-standard exception\n");
    }
    std::abort();
}

void InstallCrashHandler()
{
    SetReportPath();
    AddVectoredExceptionHandler(1, CrashHandler);
    signal(SIGABRT, AbortHandler);
    std::set_terminate(TerminateHandler);
}

// Windows reserves stack for the handler itself.
void PrepareCrashHandlerForThread()
{
}
#else
static void PrintHostFrame(const char* label, uint64_t address)
{
    Dl_info dl{};
    if (dladdr((void*)address, &dl) && dl.dli_fname)
        DiagPrint("  %s %s+0x%llX\n", label, dl.dli_fname, (unsigned long long)(address - uint64_t(dl.dli_fbase)));
    else
        DiagPrint("  %s 0x%llX\n", label, (unsigned long long)address);
}

// Host frames: pc, lr and the frame-pointer chain (code built without frame pointers, such as a
// driver, can cut it short; lr still names the caller of the faulting function).
static void DumpHostStack(const ucontext_t* uc)
{
#if defined(__aarch64__)
    PrintHostFrame("host pc", uc->uc_mcontext.pc);
    PrintHostFrame("host lr", uc->uc_mcontext.regs[30]);
    auto* frame = reinterpret_cast<const uint64_t*>(uc->uc_mcontext.regs[29]);
    for (int i = 0; i < 24 && frame != nullptr && (uint64_t(frame) & 7) == 0; i++)
    {
        const uint64_t ret = frame[1];
        if (ret == 0)
            break;
        PrintHostFrame("host fp", ret);
        auto* next = reinterpret_cast<const uint64_t*>(frame[0]);
        if (next <= frame)
            break;
        frame = next;
    }
    // Unwind tables (they cross the signal frame on Android): reaches past driver code.
    struct Walk { int count; };
    Walk walk{};
    _Unwind_Backtrace([](_Unwind_Context* context, void* arg) -> _Unwind_Reason_Code
    {
        auto* walk = static_cast<Walk*>(arg);
        if (walk->count++ >= 32)
            return _URC_END_OF_STACK;
        PrintHostFrame("host unwind", _Unwind_GetIP(context));
        return _URC_NO_REASON;
    }, &walk);
#endif
}

static void CrashHandler(int sig, siginfo_t* info, void* ucontext)
{
    // A write to a page Xenia's GPU watches (XPORT_GPU=xenia): it fires the watch, opens the page
    // and the write is retried.
    if (sig == SIGSEGV && xportgpu::HandleWriteFault(info->si_addr))
        return;
    DiagOpenReportFile();
    Dl_info dl{};
    const auto* uc = static_cast<ucontext_t*>(ucontext);
#if defined(__aarch64__)
    const uint64_t pc = uc->uc_mcontext.pc;
#else
    const uint64_t pc = uc->uc_mcontext.gregs[REG_RIP];
#endif
    dladdr((void*)pc, &dl);
    DiagPrint("\n*** crash: signal %d at %s+0x%llX, fault address 0x%llX\n", sig, dl.dli_fname ? dl.dli_fname : "?",
        (unsigned long long)(pc - uint64_t(dl.dli_fbase)), (unsigned long long)uint64_t(info->si_addr));
    LOGFN_ERROR("crash: signal {} at {}+0x{:X}, fault 0x{:X}", sig, dl.dli_fname ? dl.dli_fname : "?", pc - uint64_t(dl.dli_fbase), uint64_t(info->si_addr));
    char threadName[16] = {};
    prctl(PR_GET_NAME, threadName);
    DiagPrint("  thread %d '%s'\n", int(gettid()), threadName);
    DumpHostStack(uc);
    DumpGuestState(uint64_t(info->si_addr));
    fflush(stderr);
    signal(sig, SIG_DFL);
    raise(sig);
}

// A stack overflow leaves no stack for the handler (SA_ONSTACK needs one per thread).
void PrepareCrashHandlerForThread()
{
    thread_local std::unique_ptr<uint8_t[]> s_altStack;
    if (s_altStack)
        return;

    constexpr size_t size = 64 * 1024;
    s_altStack = std::make_unique<uint8_t[]>(size);
    stack_t stack{};
    stack.ss_sp = s_altStack.get();
    stack.ss_size = size;
    sigaltstack(&stack, nullptr);
}

void InstallCrashHandler()
{
    SetReportPath();
    PrepareCrashHandlerForThread();

    struct sigaction action{};
    action.sa_sigaction = CrashHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int sig : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP })
        sigaction(sig, &action, nullptr);
}
#endif
