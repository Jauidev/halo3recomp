#include <stdafx.h>
#include "guest_thread.h"
#include <kernel/memory.h>
#include <kernel/heap.h>
#include <kernel/function.h>
#include "ppc_context.h"
#include <os/thread_dump.h>
#include <os/thread_policy.h>
#include <loader.h>

constexpr size_t PCR_SIZE = 0xAB0;
// The TLS block 0(r13) points at starts with the title's __declspec(thread) data (copied from the
// XEX's TLS template, LdrGetTlsTemplate). Halo 3 has 0x2E8 bytes of it: in 0x100 bytes its
// variables ran into the TEB that follows, and SetLastError (TEB+0x160 = its TLS+0x260, a
// pointer the engine sets per thread) nulled one: a write to guest 0 at sub_82210598.
constexpr size_t TLS_SIZE = 0x1000;
constexpr size_t TEB_SIZE = 0x2E0;
// The default stack. A thread gets what ExCreateThread asked for when that is more: Halo 3's game
// thread asks for 0xA0000 and one of its functions (sub_8250CB50, loading a mission) puts a
// 0x50000-byte frame on it; with 256 KB the stack ran down through the thread's own PCR, zeroed the
// TLS pointer at 0(r13) and the next TLS read faulted at guest 0xA8.
constexpr size_t STACK_SIZE = 0x40000;

constexpr size_t TEB_OFFSET = PCR_SIZE + TLS_SIZE;

GuestThreadContext::GuestThreadContext(uint32_t cpuNumber, uint32_t stackSize)
{
    assert(thread == nullptr);

    const size_t stackBytes = std::max<size_t>(STACK_SIZE, (size_t(stackSize) + 0xFFF) & ~size_t(0xFFF));
    const size_t totalSize = PCR_SIZE + TLS_SIZE + TEB_SIZE + stackBytes;
    thread = (uint8_t*)g_userHeap.Alloc(totalSize);
    memset(thread, 0, totalSize);

    *(uint32_t*)thread = ByteSwap(g_memory.MapVirtual(thread + PCR_SIZE)); // tls pointer
    *(uint32_t*)(thread + 0x100) = ByteSwap(g_memory.MapVirtual(thread + PCR_SIZE + TLS_SIZE)); // teb pointer
    *(thread + 0x10C) = cpuNumber;

    uint32_t tlsRaw = 0, tlsRawSize = 0, tlsDataSize = 0;
    if (LdrGetTlsTemplate(tlsRaw, tlsRawSize, tlsDataSize) && tlsDataSize)
    {
        if (tlsDataSize > TLS_SIZE)
            LOGF_ERROR("static TLS of 0x{:X} bytes does not fit in 0x{:X}", tlsDataSize, TLS_SIZE);
        const size_t copy = std::min<size_t>(std::min(tlsRawSize, tlsDataSize), TLS_SIZE);
        if (tlsRaw && copy)
            memcpy(thread + PCR_SIZE, g_memory.Translate(tlsRaw), copy);
    }
    else
    {
        *(uint32_t*)(thread + PCR_SIZE + 0x10) = 0xFFFFFFFF; // that one TLS entry that felt quirky
    }
    *(uint32_t*)(thread + PCR_SIZE + TLS_SIZE + 0x14C) = ByteSwap(GuestThread::GetCurrentThreadId()); // thread id

    ppcContext.r1.u64 = g_memory.MapVirtual(thread + PCR_SIZE + TLS_SIZE + TEB_SIZE + stackBytes); // stack pointer
    ppcContext.r13.u64 = g_memory.MapVirtual(thread);
    ppcContext.fpscr.loadFromHost();

    assert(GetPPCContext() == nullptr);
    SetPPCContext(ppcContext);
    RegisterGuestContext(&ppcContext);
    RegisterHostThread(GuestThread::GetCurrentThreadId());
    PrepareCrashHandlerForThread();
}

GuestThreadContext::~GuestThreadContext()
{
    UnregisterGuestContext(&ppcContext);
    g_userHeap.Free(thread);
}

#ifdef USE_PTHREAD
static size_t GetStackSize()
{
    // Cache as this should not change.
    static size_t stackSize = 0;
    if (stackSize == 0)
    {
        // 8 MiB is a typical default.
        constexpr auto defaultSize = 8 * 1024 * 1024;
        struct rlimit lim;
        const auto ret = getrlimit(RLIMIT_STACK, &lim);
        if (ret == 0 && lim.rlim_cur < defaultSize)
        {
            // Use what the system allows.
            stackSize = lim.rlim_cur;
        }
        else
        {
            stackSize = defaultSize;
        }
    }
    return stackSize;
}

static void* GuestThreadFunc(void* arg)
{
    GuestThreadHandle* hThread = (GuestThreadHandle*)arg;
#else
static void GuestThreadFunc(GuestThreadHandle* hThread)
{
#endif
    hThread->suspended.wait(true);

    uint32_t exitCode;
    try
    {
        exitCode = GuestThread::Start(hThread->params, g_memory.MapVirtual(hThread));
    }
    catch (const GuestThreadExit& exit)
    {
        exitCode = exit.exitCode;
    }

    {
        std::lock_guard guard{ g_dispatcherMutex };
        hThread->exitCode = exitCode;
        hThread->exited = true;
        WakeKernelWaiters(hThread);
    }

    // Drop the reference the running thread held (see the constructor).
    ReleaseKernelObject(hThread);
#ifdef USE_PTHREAD
    return nullptr;
#endif
}

// Two references: the handle and the running thread, so closing the handle of a live thread
// neither frees it under the thread nor blocks the caller in a join.
GuestThreadHandle::GuestThreadHandle(const GuestThreadParams& params)
    : KernelObject(2), params(params), suspended((params.flags & 0x1) != 0)
#ifdef USE_PTHREAD
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, GetStackSize());
    const auto ret = pthread_create(&thread, &attr, GuestThreadFunc, this);
    if (ret != 0) {
        fprintf(stderr, "pthread_create failed with error code 0x%X.\n", ret);
        return;
    }
}
#else
      , thread(GuestThreadFunc, this)
{
}
#endif

GuestThreadHandle::~GuestThreadHandle()
{
    // The last reference is usually dropped by the thread itself on exit: it cannot join itself.
#ifdef USE_PTHREAD
    if (pthread_equal(thread, pthread_self()))
        pthread_detach(thread);
    else
        pthread_join(thread, nullptr);
#else
    if (thread.joinable())
    {
        if (thread.get_id() == std::this_thread::get_id())
            thread.detach();
        else
            thread.join();
    }
#endif
}

template <typename ThreadType>
static uint32_t CalcThreadId(const ThreadType& id)
{
    if constexpr (sizeof(id) == 4)
        return *reinterpret_cast<const uint32_t*>(&id);
    else
        return XXH32(&id, sizeof(id), 0);
}

uint32_t GuestThreadHandle::GetThreadId() const
{
#ifdef USE_PTHREAD
    return CalcThreadId(thread);
#else
    return CalcThreadId(thread.get_id());
#endif
}

bool GuestThreadHandle::IsSignaled()
{
    return exited;
}

uint32_t GuestThread::Start(const GuestThreadParams& params, uint32_t handle)
{
    const auto procMask = (uint8_t)(params.flags >> 24);
    const auto cpuNumber = procMask == 0 ? 0 : 7 - std::countl_zero(procMask);

    GuestThreadContext ctx(cpuNumber, params.stackSize);
    if (handle != 0)
        RegisterHostThreadHandle(handle);

    if (params.startup != 0)
    {
        ctx.ppcContext.r3.u64 = params.function;
        ctx.ppcContext.r4.u64 = params.value;
        g_memory.FindFunction(params.startup)(ctx.ppcContext, g_memory.base);
    }
    else
    {
        ctx.ppcContext.r3.u64 = params.value;
        g_memory.FindFunction(params.function)(ctx.ppcContext, g_memory.base);
    }

    return ctx.ppcContext.r3.u32;
}

GuestThreadHandle* GuestThread::Start(const GuestThreadParams& params, uint32_t* threadId)
{
    auto hThread = CreateKernelObject<GuestThreadHandle>(params);

    if (threadId != nullptr)
    {
        *threadId = hThread->GetThreadId();
    }

    return hThread;
}

uint32_t GuestThread::GetCurrentThreadId()
{
#ifdef USE_PTHREAD
    return CalcThreadId(pthread_self());
#else
    return CalcThreadId(std::this_thread::get_id());
#endif
}

void GuestThread::SetLastError(uint32_t error)
{
    auto* thread = (char*)g_memory.Translate(GetPPCContext()->r13.u32);
    if (*(uint32_t*)(thread + 0x150))
    {
        // Program doesn't want errors
        return;
    }

    // TEB + 0x160 : Win32LastError
    *(uint32_t*)(thread + TEB_OFFSET + 0x160) = ByteSwap(error);
}

#ifdef _WIN32
void GuestThread::SetThreadName(uint32_t threadId, const char* name)
{
#pragma pack(push,8)
    const DWORD MS_VC_EXCEPTION = 0x406D1388;

    typedef struct tagTHREADNAME_INFO
    {
        DWORD dwType; // Must be 0x1000.
        LPCSTR szName; // Pointer to name (in user addr space).
        DWORD dwThreadID; // Thread ID (-1=caller thread).
        DWORD dwFlags; // Reserved for future use, must be zero.
    } THREADNAME_INFO;
#pragma pack(pop)

    THREADNAME_INFO info;
    info.dwType = 0x1000;
    info.szName = name;
    info.dwThreadID = threadId;
    info.dwFlags = 0;

    __try
    {
        RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}
#endif

void SetThreadNameImpl(uint32_t a1, uint32_t threadId, uint32_t* name)
{
#ifdef _WIN32
    GuestThread::SetThreadName(threadId, (const char*)g_memory.Translate(ByteSwap(*name)));
#endif
}

int GetThreadPriorityImpl(GuestThreadHandle* hThread)
{
#ifdef _WIN32
    return GetThreadPriority(hThread == GetKernelObject(CURRENT_THREAD_HANDLE) ? GetCurrentThread() : hThread->thread.native_handle());
#else 
    return 0;
#endif
}

uint32_t SetThreadIdealProcessorImpl(GuestThreadHandle* hThread, uint32_t dwIdealProcessor)
{
    return 0;
}

// UnleashedRecomp hooked Sonic-specific XAPI thread helpers here; none are hooked for the title yet.
