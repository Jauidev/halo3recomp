#pragma once

#include "heap.h"
#include "memory.h"

#define OBJECT_SIGNATURE           (uint32_t)'XBOX'
#define GUEST_INVALID_HANDLE_VALUE 0xFFFFFFFF

#ifndef _WIN32

#define S_OK                       0x00000000
#define FALSE                      0x00000000
#define TRUE                       0x00000001
#define STATUS_SUCCESS             0x00000000
#define STATUS_WAIT_0              0x00000000
#define STATUS_USER_APC            0x000000C0 
#define STATUS_TIMEOUT             0x00000102
#define STATUS_FAIL_CHECK          0xC0000229
#define STATUS_INVALID_HANDLE      0xC0000008
#define STATUS_INVALID_PARAMETER   0xC000000D
#define STATUS_MUTANT_NOT_OWNED    0xC0000046
#define STATUS_SEMAPHORE_LIMIT_EXCEEDED 0xC0000047
#define INFINITE                   0xFFFFFFFF
#define FILE_ATTRIBUTE_DIRECTORY   0x00000010  
#define FILE_ATTRIBUTE_NORMAL      0x00000080  
#define GENERIC_READ               0x80000000
#define GENERIC_WRITE              0x40000000
#define FILE_READ_DATA             0x0001
#define FILE_SHARE_READ            0x00000001  
#define FILE_SHARE_WRITE           0x00000002
#define CREATE_NEW                 1
#define CREATE_ALWAYS              2
#define OPEN_EXISTING              3
#define INVALID_FILE_SIZE          0xFFFFFFFF
#define INVALID_SET_FILE_POINTER   0xFFFFFFFF
#define INVALID_FILE_ATTRIBUTES    0xFFFFFFFF
#define FILE_BEGIN                 0
#define FILE_CURRENT               1
#define FILE_END                   2
#define ERROR_NO_MORE_FILES        0x12
#define ERROR_NO_SUCH_USER         0x525
#define ERROR_SUCCESS              0x0
#define ERROR_PATH_NOT_FOUND       0x3
#define ERROR_BAD_ARGUMENTS        0xA0
#define ERROR_DEVICE_NOT_CONNECTED 0x48F
#define PAGE_READWRITE             0x04

typedef union _LARGE_INTEGER {
    struct {
        uint32_t LowPart;
        int32_t HighPart;
    };
    struct {
        uint32_t LowPart;
        int32_t HighPart;
    } u;
    int64_t QuadPart;
} LARGE_INTEGER;

static_assert(sizeof(LARGE_INTEGER) == 8);

typedef struct _FILETIME
{
    uint32_t dwLowDateTime;
    uint32_t dwHighDateTime;
} FILETIME;

static_assert(sizeof(FILETIME) == 8);

typedef struct _WIN32_FIND_DATAA
{
    uint32_t dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    uint32_t nFileSizeHigh;
    uint32_t nFileSizeLow;
    uint32_t dwReserved0;
    uint32_t dwReserved1;
    char cFileName[260];
    char cAlternateFileName[14];
} WIN32_FIND_DATAA;

static_assert(sizeof(WIN32_FIND_DATAA) == 320);

#endif

// when a wait gives up; std::nullopt waits forever.
using GuestDeadline = std::optional<std::chrono::steady_clock::time_point>;

GuestDeadline GuestTimeoutToDeadline(const be<int64_t>* timeout);

// every wait goes through one dispatcher lock, the way the Xbox kernel serialises its
// dispatcher. Unleashed's per-object atomics only handled timeouts of 0 or infinite and wait-any
// over events; the title waits with real timeouts and on mixed objects.
// A blocked thread links one entry per object it waits on into that object's waiter list and
// sleeps on its own condition variable, so a signal wakes only the threads waiting on that object.
// With one shared condition variable, every signal woke all ~25 waiting game threads, which then
// queued on g_dispatcherMutex: the game's main thread spent ~14% of its time blocked in
// NtReleaseSemaphore while queueing jobs (simpleperf, 2026-10-26).
// IsSignaled, Acquire and DueTime are called with g_dispatcherMutex held.
struct KernelWaitEntry
{
    std::condition_variable* condition;
    KernelWaitEntry* next;
};

struct KernelObject
{
    explicit KernelObject(uint32_t initialReferences = 1)
        : references(initialReferences)
    {
    }

    virtual ~KernelObject()
    {
    }

    virtual bool IsSignaled()
    {
        assert(false && "Wait not implemented for this kernel object.");
        return false;
    }

    // Consumes the signal a satisfied wait took: auto-reset events, semaphore counts, mutant ownership.
    virtual void Acquire()
    {
    }

    // When the object becomes signaled by itself (timers), so waiters wake up in time.
    virtual GuestDeadline DueTime()
    {
        return std::nullopt;
    }

    uint32_t Wait(GuestDeadline deadline);

    // like the Xbox object manager, an object lives until its handle is closed AND no
    // wait (or running thread) still uses it; Unleashed destroyed it on NtClose.
    std::atomic<uint32_t> references;

    // Threads blocked on this object (guarded by g_dispatcherMutex).
    KernelWaitEntry* waiters = nullptr;
};

void RetainKernelObject(KernelObject* obj);
void ReleaseKernelObject(KernelObject* obj);

extern std::mutex g_dispatcherMutex;

// Wakes the threads waiting on obj so they re-check their objects. Call with g_dispatcherMutex
// held, right after changing obj's signal state.
void WakeKernelWaiters(KernelObject* obj);

// Test switch (files/dispatcher_mode, read by thread_policy.cpp): every signal wakes every blocked
// thread, as the shared condition variable did, to compare both in one game session.
extern std::atomic<bool> g_dispatcherWakeAll;

// Test switch (files/sleep_mode, read by thread_policy.cpp): Sleep(0) always yields, as before
// KeDelayExecutionThread turned back-to-back ones into short sleeps (2026-10-26).
extern std::atomic<bool> g_sleepZeroAlwaysYields;

// Returns STATUS_WAIT_0 + index (wait-any), STATUS_SUCCESS (wait-all) or STATUS_TIMEOUT.
uint32_t WaitForKernelObjects(KernelObject* const* objects, uint32_t count, bool waitAll, GuestDeadline deadline);

// Objects created by the runtime, so a wait on a pointer can tell them from guest dispatcher
// headers (ObReferenceObjectByHandle hands out our object address as the "object").
void RegisterKernelObject(KernelObject* obj);
bool IsRegisteredKernelObject(const void* obj);

template<typename T, typename... Args>
inline T* CreateKernelObject(Args&&... args)
{
    static_assert(std::is_base_of_v<KernelObject, T>);
    T* obj = g_userHeap.AllocPhysical<T>(std::forward<Args>(args)...);
    RegisterKernelObject(obj);
    return obj;
}

template<typename T = KernelObject>
inline T* GetKernelObject(uint32_t handle)
{
    assert(handle != GUEST_INVALID_HANDLE_VALUE);
    return reinterpret_cast<T*>(g_memory.Translate(handle));
}

uint32_t GetKernelHandle(KernelObject* obj);

void DestroyKernelObject(KernelObject* obj);
void DestroyKernelObject(uint32_t handle);

bool IsKernelObject(uint32_t handle);
bool IsKernelObject(void* obj);

bool IsInvalidKernelObject(void* obj);

template<typename T = void>
inline T* GetInvalidKernelObject()
{
    return reinterpret_cast<T*>(g_memory.Translate(GUEST_INVALID_HANDLE_VALUE));
}

extern Mutex g_kernelLock;

template<typename T>
inline T* QueryKernelObject(XDISPATCHER_HEADER& header)
{
    std::lock_guard guard{ g_kernelLock };
    if (header.WaitListHead.Flink != OBJECT_SIGNATURE)
    {
        header.WaitListHead.Flink = OBJECT_SIGNATURE;
        auto* obj = CreateKernelObject<T>(reinterpret_cast<typename T::guest_type*>(&header));
        header.WaitListHead.Blink = g_memory.MapVirtual(obj);

        return obj;
    }

    return static_cast<T*>(g_memory.Translate(header.WaitListHead.Blink.get()));
}

// Get object without initialisation
template<typename T>
inline T* TryQueryKernelObject(XDISPATCHER_HEADER& header)
{
    if (header.WaitListHead.Flink != OBJECT_SIGNATURE)
        return nullptr;

    return static_cast<T*>(g_memory.Translate(header.WaitListHead.Blink.get()));
}
