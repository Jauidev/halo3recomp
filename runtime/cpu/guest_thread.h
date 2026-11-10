#pragma once

#include <kernel/xdm.h>

// Use pthreads directly on macOS to be able to increase default stack size.
#ifdef __APPLE__
#define USE_PTHREAD 1
#endif

#ifdef USE_PTHREAD
#include <pthread.h>
#endif

#define CURRENT_THREAD_HANDLE uint32_t(-2)

struct GuestThreadContext
{
    PPCContext ppcContext{};
    uint8_t* thread = nullptr;

    // stackSize: what the title asked for (ExCreateThread); at least the default 256 KB.
    GuestThreadContext(uint32_t cpuNumber, uint32_t stackSize = 0);
    ~GuestThreadContext();
};

struct GuestThreadParams
{
    uint32_t function;
    uint32_t value;
    uint32_t flags;
    // XAPI thread startup routine from ExCreateThread; when set it is called as
    // startup(function, value) instead of function(value), as on the console.
    uint32_t startup = 0;
    // ExCreateThread's stack size (0: the default).
    uint32_t stackSize = 0;
};

// thrown by ExTerminateThread to unwind the host thread back to GuestThreadFunc.
struct GuestThreadExit
{
    uint32_t exitCode;
};

struct GuestThreadHandle : KernelObject
{
    GuestThreadParams params;
    std::atomic<bool> suspended;
    bool exited = false; // guarded by g_dispatcherMutex
    uint32_t exitCode = 0;
#ifdef USE_PTHREAD
    pthread_t thread;
#else
    std::thread thread;
#endif

    GuestThreadHandle(const GuestThreadParams& params);
    ~GuestThreadHandle() override;

    uint32_t GetThreadId() const;

    bool IsSignaled() override;
};

struct GuestThread
{
    // handle: the guest handle of the thread, registered for naming once its context exists.
    static uint32_t Start(const GuestThreadParams& params, uint32_t handle = 0);
    static GuestThreadHandle* Start(const GuestThreadParams& params, uint32_t* threadId);

    static uint32_t GetCurrentThreadId();
    static void SetLastError(uint32_t error);

#ifdef _WIN32
    static void SetThreadName(uint32_t threadId, const char* name);
#endif
};
