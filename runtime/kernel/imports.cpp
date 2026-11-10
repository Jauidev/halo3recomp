#include <game.h>
#include <stdafx.h>
#include <cpu/ppc_context.h>
#include <cpu/guest_thread.h>
#include <apu/audio.h>
#include <os/paths.h>
#include "function.h"
#include "xex.h"
#include "xbox.h"
#include "heap.h"
#include "memory.h"
#include <memory>
#include <mutex>
#include <random>
#include <unordered_set>
#include "xam.h"
#include "xdm.h"
#include <os/background.h>
#include <os/logger.h>
#include <os/thread_policy.h>

#ifdef _WIN32
#include <ntstatus.h>
#define gmtime_r(value, parts) gmtime_s(parts, value)
#define timegm _mkgmtime
#endif
#if defined(__ANDROID__) || defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

uint32_t CompleteOverlapped(XXOVERLAPPED* overlapped, uint32_t result, uint32_t length);

// kernel objects on top of the dispatcher in xdm.cpp. State is guarded by
// g_dispatcherMutex; every change that can satisfy a wait calls WakeKernelWaiters.
struct Event final : KernelObject, HostObject<XKEVENT>
{
    bool manualReset;
    bool signaled;

    Event(XKEVENT* header)
        : manualReset(!header->Type), signaled(!!header->SignalState)
    {
    }

    Event(bool manualReset, bool initialState)
        : manualReset(manualReset), signaled(initialState)
    {
    }

    bool IsSignaled() override
    {
        return signaled;
    }

    void Acquire() override
    {
        if (!manualReset)
            signaled = false;
    }

    // Both return the previous state, like KeSetEvent/KeResetEvent.
    bool Set()
    {
        std::lock_guard guard{ g_dispatcherMutex };
        const bool previous = signaled;
        signaled = true;
        WakeKernelWaiters(this);
        return previous;
    }

    bool Reset()
    {
        std::lock_guard guard{ g_dispatcherMutex };
        const bool previous = signaled;
        signaled = false;
        return previous;
    }

    // Wakes the waiters that are waiting now, then leaves the event non-signaled.
    bool Pulse()
    {
        bool previous;
        {
            std::lock_guard guard{ g_dispatcherMutex };
            previous = signaled;
            signaled = true;
            WakeKernelWaiters(this);
        }
        std::this_thread::yield();
        std::lock_guard guard{ g_dispatcherMutex };
        signaled = false;
        return previous;
    }
};

struct Semaphore final : KernelObject, HostObject<XKSEMAPHORE>
{
    uint32_t count;
    uint32_t maximumCount;

    Semaphore(XKSEMAPHORE* semaphore)
        : count(semaphore->Header.SignalState), maximumCount(semaphore->Limit)
    {
    }

    Semaphore(uint32_t count, uint32_t maximumCount)
        : count(count), maximumCount(maximumCount)
    {
    }

    bool IsSignaled() override
    {
        return count != 0;
    }

    void Acquire() override
    {
        --count;
    }

    uint32_t Release(uint32_t releaseCount, uint32_t* previousCount)
    {
        std::lock_guard guard{ g_dispatcherMutex };
        if (previousCount != nullptr)
            *previousCount = count;

        if (count + releaseCount > maximumCount)
            return STATUS_SEMAPHORE_LIMIT_EXCEEDED;

        count += releaseCount;
        WakeKernelWaiters(this);
        return STATUS_SUCCESS;
    }
};

struct Mutant final : KernelObject
{
    uint32_t owner = 0;
    uint32_t recursion = 0;

    Mutant(bool initialOwner)
    {
        if (initialOwner)
        {
            owner = GuestThread::GetCurrentThreadId();
            recursion = 1;
        }
    }

    bool IsSignaled() override
    {
        return recursion == 0 || owner == GuestThread::GetCurrentThreadId();
    }

    void Acquire() override
    {
        owner = GuestThread::GetCurrentThreadId();
        ++recursion;
    }

    uint32_t Release(int32_t* previousCount)
    {
        std::lock_guard guard{ g_dispatcherMutex };
        if (recursion == 0 || owner != GuestThread::GetCurrentThreadId())
            return STATUS_MUTANT_NOT_OWNED;

        if (previousCount != nullptr)
            *previousCount = 1 - int32_t(recursion);

        if (--recursion == 0)
            owner = 0;

        WakeKernelWaiters(this);
        return STATUS_SUCCESS;
    }
};

struct Timer final : KernelObject
{
    bool manualReset;
    bool signaled = false;
    GuestDeadline due;
    std::chrono::milliseconds period{};

    Timer(bool manualReset)
        : manualReset(manualReset)
    {
    }

    // Signals the timer once its due time has passed and schedules the next period.
    void Update()
    {
        const auto now = std::chrono::steady_clock::now();
        if (!due || now < *due)
            return;

        signaled = true;

        if (period.count() > 0)
        {
            while (*due <= now)
                *due += period;
        }
        else
        {
            due.reset();
        }
    }

    bool IsSignaled() override
    {
        Update();
        return signaled;
    }

    void Acquire() override
    {
        if (!manualReset)
            signaled = false;
    }

    GuestDeadline DueTime() override
    {
        return due;
    }

    bool Set(GuestDeadline dueTime, std::chrono::milliseconds periodTime)
    {
        std::lock_guard guard{ g_dispatcherMutex };
        const bool previous = signaled;
        signaled = false;
        due = dueTime;
        period = periodTime;
        // The waiters re-read DueTime for their wake-up deadline.
        WakeKernelWaiters(this);
        return previous;
    }

    bool Cancel()
    {
        std::lock_guard guard{ g_dispatcherMutex };
        Update();
        due.reset();
        return signaled;
    }
};

// The runtime object behind a dispatcher pointer: one of ours (handed out by
// ObReferenceObjectByHandle) or a guest-initialised header, created on first use.
static KernelObject* QueryDispatcherObject(XDISPATCHER_HEADER* header)
{
    if (IsRegisteredKernelObject(header))
        return reinterpret_cast<KernelObject*>(header);

    switch (header->Type)
    {
    case 0:
    case 1:
        return QueryKernelObject<Event>(*header);

    case 5:
        return QueryKernelObject<Semaphore>(*header);

    default:
        LOGF_ERROR("unsupported dispatcher object type {} at 0x{:X}", header->Type, g_memory.MapVirtual(header));
        return nullptr;
    }
}

inline void CloseKernelObject(XDISPATCHER_HEADER& header)
{
    if (header.WaitListHead.Flink != OBJECT_SIGNATURE)
    {
        return;
    }

    DestroyKernelObject(header.WaitListHead.Blink);
}

void VdHSIOCalibrationLock()
{
    LOG_UTILITY("!!! STUB !!!");
}

void KeCertMonitorData()
{
    LOG_UTILITY("!!! STUB !!!");
}

void XexExecutableModuleHandle()
{
    LOG_UTILITY("!!! STUB !!!");
}

void ExLoadedCommandLine()
{
    LOG_UTILITY("!!! STUB !!!");
}

void KeDebugMonitorData()
{
    LOG_UTILITY("!!! STUB !!!");
}

void ExThreadObjectType()
{
    LOG_UTILITY("!!! STUB !!!");
}

void KeTimeStampBundle()
{
    LOG_UTILITY("!!! STUB !!!");
}

void XboxHardwareInfo()
{
    LOG_UTILITY("!!! STUB !!!");
}

void XGetVideoMode()
{
    LOG_UTILITY("!!! STUB !!!");
}

uint32_t XGetGameRegion()
{
    return 0x03FF;
}

uint32_t XMsgStartIORequest(uint32_t App, uint32_t Message, XXOVERLAPPED* lpOverlapped, void* Buffer, uint32_t szBuffer)
{
    // system messages (presence, achievements...) are accepted and completed at once.
    if (lpOverlapped)
        return CompleteOverlapped(lpOverlapped, 0, 0);
    return STATUS_SUCCESS;
}

// one local profile, user 0 (Unleashed reported all four users as signed in).
constexpr uint64_t PROFILE_XUID = 0xB13EBABEBABEBABE;
constexpr const char* PROFILE_NAME = "Player";

void LogInputQuery(const char* function, uint32_t userIndex, uint32_t flags, uint32_t result);

uint32_t XamUserGetSigninState(uint32_t userIndex)
{
    LogInputQuery("XamUserGetSigninState", userIndex, 0, userIndex == 0 ? 1 : 0);
    return userIndex == 0 ? 1 : 0; // 1 = signed in locally
}

uint32_t XamUserGetXUID(uint32_t userIndex, uint32_t typeMask, be<uint64_t>* xuid)
{
    if (userIndex != 0)
        return 0x80070525; // X_E_NO_SUCH_USER (an HRESULT; titles test "< 0")

    *xuid = PROFILE_XUID;
    return 0;
}

uint32_t XamUserGetName(uint32_t userIndex, char* buffer, uint32_t bufferLength)
{
    if (userIndex != 0)
        return 0x00000525; // ERROR_NO_SUCH_USER

    if (bufferLength > 0)
    {
        strncpy(buffer, PROFILE_NAME, bufferLength - 1);
        buffer[bufferLength - 1] = '\0';
    }
    return 0;
}

uint32_t XamGetSystemVersion()
{
    return 0;
}

uint32_t XamContentGetCreator(uint32_t userIndex, const XCONTENT_DATA* contentData, be<uint32_t>* isCreator, be<uint64_t>* xuid, XXOVERLAPPED* overlapped)
{
    if (isCreator)
        *isCreator = true;

    if (xuid)
        *xuid = PROFILE_XUID;

    if (overlapped)
        return CompleteOverlapped(overlapped, 0, 0);
    return 0;
}

uint32_t XamContentGetDeviceState()
{
    return 0;
}

uint32_t XamUserGetSigninInfo(uint32_t userIndex, uint32_t flags, XUSER_SIGNIN_INFO* info)
{
    LogInputQuery("XamUserGetSigninInfo", userIndex, flags, userIndex == 0 ? 0 : 0x80070525);
    if (userIndex == 0)
    {
        memset(info, 0, sizeof(*info));
        info->xuid = PROFILE_XUID;
        info->SigninState = 1;
        strcpy(info->Name, PROFILE_NAME);
        return 0;
    }

    // An HRESULT (X_E_NO_SUCH_USER, as Xenia): titles test it with "< 0". Halo 3 took the plain
    // Win32 code 0x525 as success, signed in users 1-3 with random names and then refused to start
    // the campaign because their pads were disconnected.
    return 0x80070525;
}

void XamShowSigninUI()
{
    LOG("profile 0 is always signed in");
}

uint32_t XamShowDeviceSelectorUI
(
    uint32_t userIndex,
    uint32_t contentType,
    uint32_t contentFlags,
    uint64_t totalRequested,
    be<uint32_t>* deviceId,
    XXOVERLAPPED* overlapped
)
{
    // picks the hard drive at once. With an XOVERLAPPED the result must also arrive
    // through it (Unleashed left it untouched, so a game polling it waited forever).
    LOGF("user {} content type 0x{:X} overlapped {}", userIndex, contentType, overlapped != nullptr);
    XamNotifyEnqueueEvent(9, true);
    *deviceId = 1;
    XamNotifyEnqueueEvent(9, false);

    if (overlapped)
        return CompleteOverlapped(overlapped, 0, 0);
    return 0;
}

void XamShowDirtyDiscErrorUI()
{
    LOG_UTILITY("!!! STUB !!!");
}

void XamEnableInactivityProcessing()
{
    LOG_UTILITY("!!! STUB !!!");
}

void XamResetInactivity()
{
    LOG_UTILITY("!!! STUB !!!");
}

void XamShowMessageBoxUIEx()
{
    LOG_UTILITY("!!! STUB !!!");
}

uint32_t XGetLanguage()
{
    return os::GetLanguage();
}

uint32_t XGetAVPack()
{
    return 0;
}

void XamLoaderTerminateTitle()
{
    LOG_UTILITY("!!! STUB !!!");
}

// No other title launched this one: no launch data (ERROR_NOT_FOUND, as on the console).
constexpr uint32_t X_ERROR_NOT_FOUND = 0x490;

uint32_t XamLoaderGetLaunchDataSize(be<uint32_t>* size)
{
    if (size != nullptr)
        *size = 0;
    return X_ERROR_NOT_FOUND;
}

uint32_t XamLoaderGetLaunchData(uint32_t buffer, uint32_t size)
{
    return X_ERROR_NOT_FOUND;
}

#if (XPORT_TITLE_ID != 0x4D5308AB && XPORT_TITLE_ID != 0x4D5307E6) // Gears of War 3 has the real one (loader.cpp)
void XamGetExecutionId()
{
    LOG_UTILITY("!!! STUB !!!");
}
#endif

void XamLoaderLaunchTitle()
{
    LOG_UTILITY("!!! STUB !!!");
}

void RtlInitAnsiString(XANSI_STRING* destination, char* source)
{
    const uint16_t length = source ? (uint16_t)strlen(source) : 0;
    destination->Length = length;
    destination->MaximumLength = length + 1;
    destination->Buffer = source;
}

uint32_t NtClose(uint32_t handle)
{
    if (handle == GUEST_INVALID_HANDLE_VALUE)
        return 0xFFFFFFFF;

    if (IsKernelObject(handle))
    {
        KernelObject* object = GetKernelObject(handle);
        if (!IsRegisteredKernelObject(object))
        {
            LOGF_WARNING("close of unknown handle 0x{:X}", handle);
            return STATUS_INVALID_HANDLE;
        }

        ReleaseKernelObject(object);
        return 0;
    }
    else
    {
        assert(false && "Unrecognized kernel object.");
        return 0xFFFFFFFF;
    }
}

uint32_t FscSetCacheElementCount()
{
    return 0;
}

// while the app is in the background (os/background.h), a guest thread that sleeps or
// waits with a timeout stops here: polling loops kept running with the render, audio and vblank
// threads held (an unnamed thread next to XpSoundThread, ~20% of a core, 2026-10-28). Waits
// without a timeout block anyway.
static void HoldTimedWait(const be<int64_t>* Timeout)
{
    if (Timeout)
        WaitWhileBackgroundHold(nullptr);
}

// I/O completion routines queued on the calling thread (kernel/io/nt_file.cpp). An alertable wait
// runs them first and returns STATUS_USER_APC instead of waiting, like the console kernel: titles
// poll their ReadFileEx reads with SleepEx(0, TRUE) / Wait*Ex(.., TRUE).
bool DeliverQueuedApcs();

uint32_t NtWaitForSingleObjectEx(uint32_t Handle, uint32_t WaitMode, uint32_t Alertable, be<int64_t>* Timeout)
{
    if (Alertable && DeliverQueuedApcs())
        return STATUS_USER_APC;
    HoldTimedWait(Timeout);
    if (!IsKernelObject(Handle))
    {
        LOGF_ERROR("wait on unrecognized handle 0x{:X}", Handle);
        return STATUS_INVALID_HANDLE;
    }

    return GetKernelObject(Handle)->Wait(GuestTimeoutToDeadline(Timeout));
}

void vsprintf_x()
{
    LOG_UTILITY("!!! STUB !!!");
}

uint32_t ExGetXConfigSetting(uint16_t Category, uint16_t Setting, void* Buffer, uint16_t SizeOfBuffer, be<uint32_t>* RequiredSize)
{
    uint32_t data[4]{};

    switch (Category)
    {
        // XCONFIG_SECURED_CATEGORY
        case 0x0002:
        {
            switch (Setting)
            {
                // XCONFIG_SECURED_AV_REGION
                case 0x0002:
                    data[0] = ByteSwap(0x00001000); // USA/Canada
                    break;

                default:
                    return 1;
            }
        }

        case 0x0003:
        {
            switch (Setting)
            {
                case 0x0001: // XCONFIG_USER_TIME_ZONE_BIAS
                case 0x0002: // XCONFIG_USER_TIME_ZONE_STD_NAME
                case 0x0003: // XCONFIG_USER_TIME_ZONE_DLT_NAME
                case 0x0004: // XCONFIG_USER_TIME_ZONE_STD_DATE
                case 0x0005: // XCONFIG_USER_TIME_ZONE_DLT_DATE
                case 0x0006: // XCONFIG_USER_TIME_ZONE_STD_BIAS
                case 0x0007: // XCONFIG_USER_TIME_ZONE_DLT_BIAS
                    data[0] = 0;
                    break;

                // XCONFIG_USER_LANGUAGE
                case 0x0009:
                    data[0] = ByteSwap(os::GetLanguage());
                    break;

                // XCONFIG_USER_VIDEO_FLAGS
                case 0x000A:
                    data[0] = ByteSwap(0x00040000);
                    break;

                // XCONFIG_USER_RETAIL_FLAGS
                case 0x000C:
                    data[0] = ByteSwap(1);
                    break;

                // XCONFIG_USER_COUNTRY
                case 0x000E:
                    data[0] = ByteSwap(103);
                    break;

                default:
                    return 1;
            }
        }
    }

    *RequiredSize = 4;
    memcpy(Buffer, data, std::min((size_t)SizeOfBuffer, sizeof(data)));

    return 0;
}

void MmQueryStatistics()
{
    LOG_UTILITY("!!! STUB !!!");
}

uint32_t NtCreateEvent(be<uint32_t>* handle, void* objAttributes, uint32_t eventType, uint32_t initialState)
{
    *handle = GetKernelHandle(CreateKernelObject<Event>(!eventType, !!initialState));
    return 0;
}

uint32_t XexCheckExecutablePrivilege()
{
    return 0;
}

void DbgPrint()
{
    LOG_UTILITY("!!! STUB !!!");
}

void __C_specific_handler_x()
{
    LOG_UTILITY("!!! STUB !!!");
}

#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
// Gears of War 3: XAPI's CreateFile sets the last error through this. As a stub the NTSTATUS came back
// unchanged (r3 untouched), so a missing file (Movies\Open_Logos.txt) was not ERROR_FILE_NOT_FOUND and
// UE3 took its I/O failure path. Same table as Windows/Xenia for the statuses the XAPI produces.
uint32_t RtlNtStatusToDosError(uint32_t status)
{
    switch (status)
    {
    case 0x00000000: return 0;    // SUCCESS
    case 0x00000102: return 258;  // TIMEOUT -> WAIT_TIMEOUT
    case 0x00000103: return 997;  // PENDING -> ERROR_IO_PENDING
    case 0x80000005: return 234;  // BUFFER_OVERFLOW -> ERROR_MORE_DATA
    case 0x80000006: return 18;   // NO_MORE_FILES
    case 0x80000011: return 170;  // DEVICE_BUSY -> ERROR_BUSY
    case 0x8000001A: return 259;  // NO_MORE_ENTRIES -> ERROR_NO_MORE_ITEMS
    case 0xC0000001: return 31;   // UNSUCCESSFUL -> ERROR_GEN_FAILURE
    case 0xC0000002: return 1;    // NOT_IMPLEMENTED -> ERROR_INVALID_FUNCTION
    case 0xC0000005: return 998;  // ACCESS_VIOLATION -> ERROR_NOACCESS
    case 0xC0000008: return 6;    // INVALID_HANDLE
    case 0xC000000D: return 87;   // INVALID_PARAMETER
    case 0xC000000E: return 2;    // NO_SUCH_DEVICE -> ERROR_FILE_NOT_FOUND
    case 0xC000000F: return 2;    // NO_SUCH_FILE -> ERROR_FILE_NOT_FOUND
    case 0xC0000010: return 1;    // INVALID_DEVICE_REQUEST -> ERROR_INVALID_FUNCTION
    case 0xC0000011: return 38;   // END_OF_FILE -> ERROR_HANDLE_EOF
    case 0xC0000017: return 8;    // NO_MEMORY -> ERROR_NOT_ENOUGH_MEMORY
    case 0xC0000022: return 5;    // ACCESS_DENIED
    case 0xC0000023: return 122;  // BUFFER_TOO_SMALL -> ERROR_INSUFFICIENT_BUFFER
    case 0xC0000033: return 123;  // OBJECT_NAME_INVALID -> ERROR_INVALID_NAME
    case 0xC0000034: return 2;    // OBJECT_NAME_NOT_FOUND -> ERROR_FILE_NOT_FOUND
    case 0xC0000035: return 183;  // OBJECT_NAME_COLLISION -> ERROR_ALREADY_EXISTS
    case 0xC000003A: return 3;    // OBJECT_PATH_NOT_FOUND -> ERROR_PATH_NOT_FOUND
    case 0xC000003B: return 161;  // OBJECT_PATH_SYNTAX_BAD -> ERROR_BAD_PATHNAME
    case 0xC0000043: return 32;   // SHARING_VIOLATION
    case 0xC000007F: return 112;  // DISK_FULL
    case 0xC000009A: return 1450; // INSUFFICIENT_RESOURCES -> ERROR_NO_SYSTEM_RESOURCES
    case 0xC00000BA: return 5;    // FILE_IS_A_DIRECTORY -> ERROR_ACCESS_DENIED
    case 0xC00000BB: return 50;   // NOT_SUPPORTED
    case 0xC0000101: return 145;  // DIRECTORY_NOT_EMPTY -> ERROR_DIR_NOT_EMPTY
    case 0xC0000102: return 1392; // FILE_CORRUPT_ERROR -> ERROR_FILE_CORRUPT
    case 0xC0000103: return 267;  // NOT_A_DIRECTORY -> ERROR_DIRECTORY
    case 0xC0000120: return 995;  // CANCELLED -> ERROR_OPERATION_ABORTED
    }
    if ((status & 0xFFFF0000) == 0x80070000 || (status & 0xFFFF0000) == 0xC0070000)
        return status & 0xFFFF; // FACILITY_WIN32 carries the error itself
    LOGF_WARNING("unmapped NTSTATUS 0x{:08X}", status);
    return 317; // ERROR_MR_MID_NOT_FOUND
}
#else
void RtlNtStatusToDosError()
{
    LOG_UTILITY("!!! STUB !!!");
}
#endif

#if (XPORT_TITLE_ID != 0x4D5308AB && XPORT_TITLE_ID != 0x4D5307E6) // Gears of War 3 has the real one (loader.cpp)
void XexGetModuleSection()
{
    LOG_UTILITY("!!! STUB !!!");
}
#endif

uint32_t RtlUnicodeToMultiByteN(char* MultiByteString, uint32_t MaxBytesInMultiByteString, be<uint32_t>* BytesInMultiByteString, const be<uint16_t>* UnicodeString, uint32_t BytesInUnicodeString)
{
    const auto reqSize = BytesInUnicodeString / sizeof(uint16_t);

    if (BytesInMultiByteString)
        *BytesInMultiByteString = reqSize;

    if (reqSize > MaxBytesInMultiByteString)
        return STATUS_FAIL_CHECK;

    for (size_t i = 0; i < reqSize; i++)
    {
        const auto c = UnicodeString[i].get();

        MultiByteString[i] = c < 256 ? c : '?';
    }

    return STATUS_SUCCESS;
}

uint32_t KeDelayExecutionThread(uint32_t WaitMode, bool Alertable, be<int64_t>* Timeout)
{
    // Pending I/O completion routines run here; without any, an alertable delay just sleeps
    // (Unleashed returned STATUS_USER_APC at once, which turns the title's sleep loops into busy loops).
    if (Alertable && DeliverQueuedApcs())
        return STATUS_USER_APC;
    HoldTimedWait(Timeout);
    const GuestDeadline deadline = GuestTimeoutToDeadline(Timeout);
    const auto now = std::chrono::steady_clock::now();

    if (deadline && *deadline > now)
    {
        std::this_thread::sleep_until(*deadline);
        return STATUS_SUCCESS;
    }

    // Sleep(0) yields. On the Xbox a loop of them spins on its own hardware thread; here
    // the thread of 0x82902050, polling a flag that way, kept a big core ~30% busy. Once a thread
    // calls it back to back (little guest work in between), each call sleeps briefly instead.
    constexpr uint32_t kYieldsBeforeSleeping = 64;
    thread_local uint32_t s_backToBack = 0;
    thread_local std::chrono::steady_clock::time_point s_lastReturn;
    thread_local bool s_reported = false;
    if (now - s_lastReturn > std::chrono::microseconds(100))
        s_backToBack = 0;
    if (++s_backToBack <= kYieldsBeforeSleeping || g_sleepZeroAlwaysYields.load(std::memory_order_relaxed))
    {
        std::this_thread::yield();
    }
    else
    {
        if (!s_reported)
        {
            s_reported = true;
            LOGF_WARNING("guest thread {:X} spins on Sleep(0); its yields now sleep 200 us",
                GuestThread::GetCurrentThreadId());
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    s_lastReturn = std::chrono::steady_clock::now();

    return STATUS_SUCCESS;
}

// ExAllocatePool*: blocks of the user heap. ExFreePool frees only those (the pool calls were stubs
// before, so anything else a title passes in is left alone).
static std::mutex g_poolMutex;
static std::unordered_set<uint32_t> g_poolBlocks;

static uint32_t AllocatePool(uint32_t size)
{
    void* block = g_userHeap.Alloc(std::max<uint32_t>(size, 1));
    if (!block)
        return 0;
    const uint32_t address = g_memory.MapVirtual(block);
    std::lock_guard lock(g_poolMutex);
    g_poolBlocks.insert(address);
    return address;
}

uint32_t ExAllocatePool(uint32_t size)
{
    return AllocatePool(size);
}

void ExFreePool(uint32_t address)
{
    {
        std::lock_guard lock(g_poolMutex);
        if (!g_poolBlocks.erase(address))
            return;
    }
    g_userHeap.Free(g_memory.Translate(address));
}

// SLIST_HEADER: the first entry, Depth and Sequence in 8 big-endian bytes, which the title also
// changes with ldarx/stdcx; a compare-exchange on the same 8 bytes keeps both atomic.
uint32_t InterlockedPopEntrySList_x(uint32_t list)
{
    auto* header = reinterpret_cast<uint64_t*>(g_memory.Translate(list));
    uint64_t old = __atomic_load_n(header, __ATOMIC_SEQ_CST);
    for (;;)
    {
        const uint64_t value = __builtin_bswap64(old);
        const uint32_t first = uint32_t(value >> 32);
        if (first == 0)
            return 0;
        const uint32_t next = __builtin_bswap32(*reinterpret_cast<uint32_t*>(g_memory.Translate(first)));
        const uint64_t popped = (uint64_t(next) << 32) | ((((value >> 16) - 1) & 0xFFFF) << 16) | (value & 0xFFFF);
        if (__atomic_compare_exchange_n(header, &old, __builtin_bswap64(popped), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return first;
    }
}

// Empties the list (depth 0, the same sequence) and returns its first entry.
uint32_t InterlockedFlushSList_x(uint32_t list)
{
    auto* header = reinterpret_cast<uint64_t*>(g_memory.Translate(list));
    uint64_t old = __atomic_load_n(header, __ATOMIC_SEQ_CST);
    for (;;)
    {
        const uint64_t value = __builtin_bswap64(old);
        if (__atomic_compare_exchange_n(header, &old, __builtin_bswap64(value & 0xFFFF), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return uint32_t(value >> 32);
    }
}

void NtDuplicateObject()
{
    LOG_UTILITY("!!! STUB !!!");
}

void ObDereferenceObject()
{
    LOG_UTILITY("!!! STUB !!!");
}

void KeSetBasePriorityThread(GuestThreadHandle* hThread, int priority)
{
#ifdef _WIN32
    if (priority == 16)
    {
        priority = 15;
    }
    else if (priority == -16)
    {
        priority = -15;
    }

    SetThreadPriority(hThread == GetKernelObject(CURRENT_THREAD_HANDLE) ? GetCurrentThread() : hThread->thread.native_handle(), priority);
#endif
}

uint32_t ObReferenceObjectByHandle(uint32_t handle, uint32_t objectType, be<uint32_t>* object)
{
    *object = handle;
    return 0;
}

void KeQueryBasePriorityThread()
{
    LOG_UTILITY("!!! STUB !!!");
}

uint32_t NtSuspendThread(GuestThreadHandle* hThread, uint32_t* suspendCount)
{
    assert(hThread != GetKernelObject(CURRENT_THREAD_HANDLE) && hThread->GetThreadId() == GuestThread::GetCurrentThreadId());

    hThread->suspended = true;
    hThread->suspended.wait(true);

    return S_OK;
}

uint32_t KeSetAffinityThread(uint32_t Thread, uint32_t Affinity, be<uint32_t>* lpPreviousAffinity)
{
    if (lpPreviousAffinity)
        *lpPreviousAffinity = 2;

    return 0;
}

// Unleashed kept RecursionCount and OwningThread little-endian, assuming the game never
// reads them, but the title does: sub_82191C04 reads RecursionCount to release a critical section fully
// and take it back as many times. A count of 1 read as 0x01000000 corrupted the lock and loading
// hung now and then. Both fields are big-endian now, as on the console. The lock itself is
// LockCount, which the game leaves alone: the owner's r13, with 0 and -1 (the kernel's initial
// value, also found in critical sections the game embeds pre-initialized) both meaning free.
struct GuestCriticalSection
{
    XDISPATCHER_HEADER Header;
    uint32_t LockCount; // host order: 0 or 0xFFFFFFFF free, else the owner's r13
    be<int32_t> RecursionCount;
    be<uint32_t> OwningThread;
};
static_assert(sizeof(GuestCriticalSection) == sizeof(XRTL_CRITICAL_SECTION));

static bool TryLockCriticalSection(GuestCriticalSection* cs, uint32_t thisThread, uint32_t& owner)
{
    auto& lock = AtomicRef(cs->LockCount);
    owner = lock.load();
    while (owner == 0 || owner == 0xFFFFFFFF)
    {
        if (lock.compare_exchange_weak(owner, thisThread))
        {
            cs->OwningThread = thisThread;
            cs->RecursionCount = 1;
            return true;
        }
    }
    if (owner == thisThread)
    {
        cs->RecursionCount = cs->RecursionCount + 1;
        return true;
    }
    return false;
}

void RtlLeaveCriticalSection(XRTL_CRITICAL_SECTION* section)
{
    auto* cs = reinterpret_cast<GuestCriticalSection*>(section);
    cs->RecursionCount = cs->RecursionCount - 1;
    if (cs->RecursionCount != 0)
        return;

    cs->OwningThread = 0;
    auto& lock = AtomicRef(cs->LockCount);
    lock.store(0);
    lock.notify_all();
}

void RtlEnterCriticalSection(XRTL_CRITICAL_SECTION* section)
{
    auto* cs = reinterpret_cast<GuestCriticalSection*>(section);
    const uint32_t thisThread = g_ppcContext->r13.u32;
    assert(thisThread != NULL);

    uint32_t owner;
    while (!TryLockCriticalSection(cs, thisThread, owner))
        AtomicRef(cs->LockCount).wait(owner);
}

void HalReturnToFirmware()
{
    LOG_UTILITY("!!! STUB !!!");
}

#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
// length in bytes, pattern stored big-endian (Xenia's RtlFillMemoryUlong). Gears of War 3's D3D
// (82B01118) fills an 800-byte command buffer on the stack with PM4 NOPs (0x80000000) with it;
// the stub left stack bytes there. Only for this title: the finished ports keep their stub.
void RtlFillMemoryUlong(be<uint32_t>* destination, uint32_t length, uint32_t pattern)
{
    for (uint32_t n = 0; n < length / 4; n++)
        destination[n] = pattern;
}
#else
void RtlFillMemoryUlong()
{
    LOG_UTILITY("!!! STUB !!!");
}
#endif

void KeBugCheckEx()
{
    __builtin_debugtrap();
}

uint32_t KeGetCurrentProcessType()
{
    return 1;
}

void RtlCompareMemoryUlong()
{
    LOG_UTILITY("!!! STUB !!!");
}

// the header must look like the kernel's (Type 1, a synchronization event; Xenia's
// xeRtlInitializeCriticalSection). Unleashed left it zeroed, and the engine's lock guard (sub_8285F920)
// skips locking a critical section whose first word is 0: the game's heap then ran unlocked and
// its free lists got corrupted when several threads allocated at once.
static void InitializeCriticalSectionHeader(XRTL_CRITICAL_SECTION* cs, uint8_t spinCountDiv256)
{
    cs->Header.Type = 1;
    cs->Header.Absolute = spinCountDiv256;
    cs->Header.Size = 0;
    cs->Header.Inserted = 0;
    cs->Header.SignalState = 0;
    cs->LockCount = -1;
    cs->RecursionCount = 0;
    cs->OwningThread = 0;
}

uint32_t RtlInitializeCriticalSection(XRTL_CRITICAL_SECTION* cs)
{
    InitializeCriticalSectionHeader(cs, 0);
    return 0;
}

// logs what the game raises; no SEH dispatch, execution just continues. 0x406D1388 is
// the debugger "set thread name" exception and is expected.
void RtlRaiseException_x(be<uint32_t>* record)
{
    const uint32_t code = record[0];
    if (code == 0x406D1388)
    {
        // THREADNAME_INFO in ExceptionInformation: type 0x1000, name, thread id, flags.
        if (record[5] == 0x1000 && record[6] != 0)
            SetGuestThreadName(record[7], static_cast<const char*>(g_memory.Translate(record[6])));
        return;
    }

    LOGF_WARNING("exception 0x{:08X} flags 0x{:X} address 0x{:08X} params {} [0x{:X}, 0x{:X}]",
        code, uint32_t(record[1]), uint32_t(record[3]), uint32_t(record[4]), uint32_t(record[5]), uint32_t(record[6]));
}

// Guest spinlocks hold the owner's r13 (0 = free). xport: on Android a waiter spins briefly,
// then sleeps on the lock word (futex) instead of yielding without end: when the holder lost its
// core on the busy big cores, every waiter gave its time slice away again and again. The sleep is
// bounded (200 us) because guest code may store 0 itself without a release call that wakes.
#if defined(__ANDROID__) || defined(__linux__)
static std::atomic<uint32_t> g_spinLockSleepers{ 0 }; // waiters asleep on any spinlock word

static void AcquireGuestSpinLock(uint32_t* spinLock)
{
    auto& spinLockRef = AtomicRef(*spinLock);
    const uint32_t owner = g_ppcContext->r13.u32;
    for (int spin = 0; spin < 100; spin++)
    {
        uint32_t expected = 0;
        if (spinLockRef.compare_exchange_weak(expected, owner, std::memory_order_acquire))
            return;
#if defined(__aarch64__)
        __asm__ volatile("yield");
#endif
    }
    // The sleeper count is raised before the last look at the word: a release that stores 0 after
    // it sees the count and wakes, one that stored 0 before it leaves the word free to take.
    g_spinLockSleepers.fetch_add(1, std::memory_order_seq_cst);
    while (true)
    {
        uint32_t expected = 0;
        if (spinLockRef.compare_exchange_strong(expected, owner, std::memory_order_seq_cst))
            break;
        const timespec timeout{ 0, 200'000 };
        syscall(SYS_futex, spinLock, FUTEX_WAIT_PRIVATE, expected, &timeout, nullptr, 0);
    }
    g_spinLockSleepers.fetch_sub(1, std::memory_order_relaxed);
}

static void ReleaseGuestSpinLock(uint32_t* spinLock)
{
    AtomicRef(*spinLock).store(0, std::memory_order_seq_cst);
    if (g_spinLockSleepers.load(std::memory_order_seq_cst) != 0)
        syscall(SYS_futex, spinLock, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
}
#else
static void AcquireGuestSpinLock(uint32_t* spinLock)
{
    auto& spinLockRef = AtomicRef(*spinLock);

    while (true)
    {
        uint32_t expected = 0;
        if (spinLockRef.compare_exchange_weak(expected, g_ppcContext->r13.u32))
            break;

        std::this_thread::yield();
    }
}

static void ReleaseGuestSpinLock(uint32_t* spinLock)
{
    AtomicRef(*spinLock) = 0;
}
#endif

void KfReleaseSpinLock(uint32_t* spinLock)
{
    ReleaseGuestSpinLock(spinLock);
}

void KfAcquireSpinLock(uint32_t* spinLock)
{
    AcquireGuestSpinLock(spinLock);
}

uint64_t KeQueryPerformanceFrequency()
{
    return 49875000;
}

bool VdPersistDisplay(uint32_t a1, uint32_t* a2)
{
    *a2 = NULL;
    return false;
}



void KeReleaseSpinLockFromRaisedIrql(uint32_t* spinLock)
{
    ReleaseGuestSpinLock(spinLock);
}

void KeAcquireSpinLockAtRaisedIrql(uint32_t* spinLock)
{
    AcquireGuestSpinLock(spinLock);
}

uint32_t KiApcNormalRoutineNop()
{
    return 0;
}



void VdSetSystemCommandBufferGpuIdentifierAddress()
{
    LOG_UTILITY("!!! STUB !!!");
}

void _vsnprintf_x()
{
    LOG_UTILITY("!!! STUB !!!");
}

void sprintf_x()
{
    LOG_UTILITY("!!! STUB !!!");
}

void ExRegisterTitleTerminateNotification()
{
    LOG_UTILITY("!!! STUB !!!");
}

void VdShutdownEngines()
{
    LOG_UTILITY("!!! STUB !!!");
}

void VdQueryVideoMode(XVIDEO_MODE* vm)
{
    memset(vm, 0, sizeof(XVIDEO_MODE));
    vm->DisplayWidth = 1280;
    vm->DisplayHeight = 720;
    vm->IsInterlaced = false;
    vm->IsWidescreen = true;
    vm->IsHighDefinition = true;
    vm->RefreshRate = 0x42700000;
    vm->VideoStandard = 1;
    vm->Unknown4A = 0x4A;
    vm->Unknown01 = 0x01;
}

// Layout from Xenia's X_DISPLAY_INFO. With the buffer left unfilled D3D keeps a null scaler rect and
// divides by its width (sub_82A52260), which traps on x86.
void VdGetCurrentDisplayInformation(uint8_t* info)
{
    constexpr uint16_t width = 1280, height = 720;
    constexpr auto store16 = [](uint8_t* p, uint16_t v) { *(be<uint16_t>*)p = v; };
    constexpr auto store32 = [](uint8_t* p, uint32_t v) { *(be<uint32_t>*)p = v; };

    memset(info, 0, 0x58);
    store16(info + 0x00, width);       // front buffer
    store16(info + 0x02, height);
    store32(info + 0x08 + 0x08, width); // scaler source rect x2, y2
    store32(info + 0x08 + 0x0C, height);
    store32(info + 0x08 + 0x10, width); // scaled output
    store32(info + 0x08 + 0x14, height);
    store32(info + 0x08 + 0x18, 1);     // vertical, horizontal filter
    store32(info + 0x08 + 0x28, 1);
    store16(info + 0x40, 320);          // display window overscan
    store16(info + 0x42, 180);
    store16(info + 0x44, 320);
    store16(info + 0x46, 180);
    store16(info + 0x48, width);        // display
    store16(info + 0x4A, height);
    store32(info + 0x4C, 0x42700000);   // 60.0f
    store16(info + 0x56, width);        // actual display width
}

void VdSetDisplayMode()
{
    LOG_UTILITY("!!! STUB !!!");
}






void VdCallGraphicsNotificationRoutines()
{
    LOG_UTILITY("!!! STUB !!!");
}

void VdInitializeScalerCommandBuffer()
{
    LOG_UTILITY("!!! STUB !!!");
}

void KeLeaveCriticalRegion()
{
    LOG_UTILITY("!!! STUB !!!");
}

uint32_t VdRetrainEDRAM()
{
    return 0;
}

void VdRetrainEDRAMWorker()
{
    LOG_UTILITY("!!! STUB !!!");
}

void KeEnterCriticalRegion()
{
    LOG_UTILITY("!!! STUB !!!");
}

void VdEnableDisableClockGating()
{
    LOG_UTILITY("!!! STUB !!!");
}

void KeBugCheck()
{
    __builtin_debugtrap();
}

void KeLockL2()
{
    LOG_UTILITY("!!! STUB !!!");
}

void KeUnlockL2()
{
    LOG_UTILITY("!!! STUB !!!");
}

uint32_t KeSetEvent(XKEVENT* pEvent, uint32_t Increment, bool Wait)
{
    return QueryKernelObject<Event>(*pEvent)->Set();
}

uint32_t KeResetEvent(XKEVENT* pEvent)
{
    return QueryKernelObject<Event>(*pEvent)->Reset();
}

uint32_t KeWaitForSingleObject(XDISPATCHER_HEADER* Object, uint32_t WaitReason, uint32_t WaitMode, bool Alertable, be<int64_t>* Timeout)
{
    if (Alertable && DeliverQueuedApcs())
        return STATUS_USER_APC;
    HoldTimedWait(Timeout);
    KernelObject* object = QueryDispatcherObject(Object);
    if (object == nullptr)
        return STATUS_INVALID_HANDLE;

    return object->Wait(GuestTimeoutToDeadline(Timeout));
}

static std::vector<size_t> g_tlsFreeIndices;
static size_t g_tlsNextIndex = 0;
static Mutex g_tlsAllocationMutex;

static uint32_t& KeTlsGetValueRef(size_t index)
{
    // Having this a global thread_local variable
    // for some reason crashes on boot in debug builds.
    thread_local std::vector<uint32_t> s_tlsValues;

    if (s_tlsValues.size() <= index)
    {
        s_tlsValues.resize(index + 1, 0);
    }

    return s_tlsValues[index];
}

uint32_t KeTlsGetValue(uint32_t dwTlsIndex)
{
    return KeTlsGetValueRef(dwTlsIndex);
}

uint32_t KeTlsSetValue(uint32_t dwTlsIndex, uint32_t lpTlsValue)
{
    KeTlsGetValueRef(dwTlsIndex) = lpTlsValue;
    return TRUE;
}

uint32_t KeTlsAlloc()
{
    std::lock_guard<Mutex> lock(g_tlsAllocationMutex);
    if (!g_tlsFreeIndices.empty())
    {
        size_t index = g_tlsFreeIndices.back();
        g_tlsFreeIndices.pop_back();
        return index;
    }

    return g_tlsNextIndex++;
}

uint32_t KeTlsFree(uint32_t dwTlsIndex)
{
    std::lock_guard<Mutex> lock(g_tlsAllocationMutex);
    g_tlsFreeIndices.push_back(dwTlsIndex);
    return TRUE;
}

uint32_t XMsgInProcessCall(uint32_t app, uint32_t message, be<uint32_t>* param1, be<uint32_t>* param2)
{
    if (message == 0x7001B)
    {
        uint32_t* ptr = (uint32_t*)g_memory.Translate(param1[1]);
        ptr[0] = 0;
        ptr[1] = 0;
    }

    return 0;
}


void NetDll_WSAStartup()
{
    LOG_UTILITY("!!! STUB !!!");
}

void NetDll_WSACleanup()
{
    LOG_UTILITY("!!! STUB !!!");
}

void NetDll_socket()
{
    LOG_UTILITY("!!! STUB !!!");
}

void NetDll_closesocket()
{
    LOG_UTILITY("!!! STUB !!!");
}

void NetDll_setsockopt()
{
    LOG_UTILITY("!!! STUB !!!");
}

void NetDll_bind()
{
    LOG_UTILITY("!!! STUB !!!");
}

void NetDll_connect()
{
    LOG_UTILITY("!!! STUB !!!");
}

void NetDll_listen()
{
    LOG_UTILITY("!!! STUB !!!");
}

void NetDll_accept()
{
    LOG_UTILITY("!!! STUB !!!");
}

void NetDll_select()
{
    LOG_UTILITY("!!! STUB !!!");
}

void NetDll_recv()
{
    LOG_UTILITY("!!! STUB !!!");
}

void NetDll_send()
{
    LOG_UTILITY("!!! STUB !!!");
}

void NetDll_inet_addr()
{
    LOG_UTILITY("!!! STUB !!!");
}

void NetDll___WSAFDIsSet()
{
    LOG_UTILITY("!!! STUB !!!");
}

#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
// Gears of War 3 sends its session messages (XGI app) through it and waits on the overlapped: left
// pending, "New campaign" stayed in the transition map (GearGame_P.xxx) and never opened the
// chapter's map, a black world where the player fell and died. Accepted and completed at once, like
// XMsgStartIORequest (Xenia's XGI app answers success as well).
uint32_t XMsgStartIORequestEx(uint32_t app, uint32_t message, XXOVERLAPPED* overlapped, void* buffer, uint32_t bufferLength, uint32_t extra)
{
    static std::atomic<int> s_logged{ 0 };
    if (s_logged.fetch_add(1) < 32)
        LOGF("app 0x{:X} message 0x{:08X} buffer {} bytes, overlapped {}", app, message, bufferLength, overlapped != nullptr);
    if (overlapped)
        return CompleteOverlapped(overlapped, 0, 0);
    return STATUS_SUCCESS;
}
#else
void XMsgStartIORequestEx()
{
    LOG_UTILITY("!!! STUB !!!");
}
#endif

bool RtlTryEnterCriticalSection(XRTL_CRITICAL_SECTION* section)
{
    auto* cs = reinterpret_cast<GuestCriticalSection*>(section);
    const uint32_t thisThread = g_ppcContext->r13.u32;
    assert(thisThread != NULL);

    uint32_t owner;
    return TryLockCriticalSection(cs, thisThread, owner);
}

uint32_t RtlInitializeCriticalSectionAndSpinCount(XRTL_CRITICAL_SECTION* cs, uint32_t spinCount)
{
    InitializeCriticalSectionHeader(cs, uint8_t(std::min<uint32_t>((spinCount + 255) >> 8, 255)));
    return 0;
}

void _vswprintf_x()
{
    LOG_UTILITY("!!! STUB !!!");
}

void _vscwprintf_x()
{
    LOG_UTILITY("!!! STUB !!!");
}

void _swprintf_x()
{
    LOG_UTILITY("!!! STUB !!!");
}

void _snwprintf_x()
{
    LOG_UTILITY("!!! STUB !!!");
}

void XeCryptBnQwBeSigVerify()
{
    LOG_UTILITY("!!! STUB !!!");
}

void XeKeysGetKey()
{
    LOG_UTILITY("!!! STUB !!!");
}

void XeCryptRotSumSha()
{
    LOG_UTILITY("!!! STUB !!!");
}


void KeEnableFpuExceptions()
{
    LOG_UTILITY("!!! STUB !!!");
}

void RtlUnwind_x()
{
    LOG_UTILITY("!!! STUB !!!");
}

void RtlCaptureContext_x()
{
    LOG_UTILITY("!!! STUB !!!");
}

uint32_t RtlMultiByteToUnicodeN(be<uint16_t>* UnicodeString, uint32_t MaxBytesInUnicodeString, be<uint32_t>* BytesInUnicodeString, const char* MultiByteString, uint32_t BytesInMultiByteString)
{
    uint32_t length = std::min(MaxBytesInUnicodeString / 2, BytesInMultiByteString);

    for (size_t i = 0; i < length; i++)
        UnicodeString[i] = MultiByteString[i];

    if (BytesInUnicodeString != nullptr)
        *BytesInUnicodeString = length * 2;

    return STATUS_SUCCESS;
}

void DbgBreakPoint()
{
    LOG_UTILITY("!!! STUB !!!");
}

// NtClearEvent takes the handle alone (NtResetEvent is the one that returns the previous state):
// r4 holds whatever the caller left there, which Gears of War has as 1.
uint32_t NtClearEvent(Event* handle)
{
    handle->Reset();
    return 0;
}

uint32_t NtResumeThread(GuestThreadHandle* hThread, uint32_t* suspendCount)
{
    assert(hThread != GetKernelObject(CURRENT_THREAD_HANDLE));

    hThread->suspended = false;
    hThread->suspended.notify_all();

    return S_OK;
}

uint32_t NtSetEvent(Event* handle, be<uint32_t>* previousState)
{
    const bool previous = handle->Set();
    if (previousState != nullptr)
        *previousState = previous;

    return 0;
}

uint32_t NtPulseEvent(Event* handle, be<uint32_t>* previousState)
{
    const bool previous = handle->Pulse();
    if (previousState != nullptr)
        *previousState = previous;

    return 0;
}

uint32_t NtCreateSemaphore(be<uint32_t>* Handle, XOBJECT_ATTRIBUTES* ObjectAttributes, uint32_t InitialCount, uint32_t MaximumCount)
{
    *Handle = GetKernelHandle(CreateKernelObject<Semaphore>(InitialCount, MaximumCount));
    return STATUS_SUCCESS;
}

uint32_t NtReleaseSemaphore(Semaphore* Handle, uint32_t ReleaseCount, int32_t* PreviousCount)
{
    uint32_t previousCount;
    const uint32_t status = Handle->Release(ReleaseCount, &previousCount);

    if (PreviousCount != nullptr)
        *PreviousCount = ByteSwap(previousCount);

    return status;
}

uint32_t NtWaitForMultipleObjectsEx(uint32_t Count, be<uint32_t>* Handles, uint32_t WaitType, uint32_t WaitMode, uint32_t Alertable, be<int64_t>* Timeout)
{
    if (Alertable && DeliverQueuedApcs())
        return STATUS_USER_APC;
    HoldTimedWait(Timeout);
    KernelObject* objects[64];
    if (Count > std::size(objects))
        return STATUS_INVALID_PARAMETER;

    for (uint32_t i = 0; i < Count; i++)
    {
        const uint32_t handle = Handles[i];
        if (!IsKernelObject(handle))
        {
            LOGF_ERROR("wait on unrecognized handle 0x{:X}", handle);
            return STATUS_INVALID_HANDLE;
        }

        objects[i] = GetKernelObject(handle);
    }

    // WaitType: 0 = wait all, 1 = wait any.
    return WaitForKernelObjects(objects, Count, WaitType == 0, GuestTimeoutToDeadline(Timeout));
}

uint32_t NtCreateMutant(be<uint32_t>* Handle, XOBJECT_ATTRIBUTES* ObjectAttributes, uint32_t InitialOwner)
{
    *Handle = GetKernelHandle(CreateKernelObject<Mutant>(InitialOwner != 0));
    return STATUS_SUCCESS;
}

uint32_t NtReleaseMutant(Mutant* Handle, be<int32_t>* PreviousCount)
{
    int32_t previousCount;
    const uint32_t status = Handle->Release(&previousCount);

    if (PreviousCount != nullptr && status == STATUS_SUCCESS)
        *PreviousCount = previousCount;

    return status;
}

uint32_t NtCreateTimer(be<uint32_t>* Handle, XOBJECT_ATTRIBUTES* ObjectAttributes, uint32_t TimerType)
{
    // TimerType: 0 = notification (manual reset), 1 = synchronization (auto reset).
    *Handle = GetKernelHandle(CreateKernelObject<Timer>(TimerType == 0));
    return STATUS_SUCCESS;
}

uint32_t NtSetTimerEx(Timer* Handle, be<int64_t>* DueTime, uint32_t ApcRoutine, uint32_t ApcMode, uint32_t ApcContext, uint32_t Resume, int32_t Period, be<uint32_t>* PreviousState)
{
    // Completion APCs need alertable waits to deliver them; none of those exist yet.
    if (ApcRoutine != 0)
        LOGF_WARNING("timer APC 0x{:X} ignored", ApcRoutine);

    const bool previous = Handle->Set(GuestTimeoutToDeadline(DueTime), std::chrono::milliseconds(std::max(Period, 0)));
    if (PreviousState != nullptr)
        *PreviousState = previous;

    return STATUS_SUCCESS;
}

uint32_t NtCancelTimer(Timer* Handle, be<uint32_t>* CurrentState)
{
    const bool state = Handle->Cancel();
    if (CurrentState != nullptr)
        *CurrentState = state;

    return STATUS_SUCCESS;
}

void RtlCompareStringN()
{
    LOG_UTILITY("!!! STUB !!!");
}

void _snprintf_x()
{
    LOG_UTILITY("!!! STUB !!!");
}

void StfsControlDevice()
{
    LOG_UTILITY("!!! STUB !!!");
}

void StfsCreateDevice()
{
    LOG_UTILITY("!!! STUB !!!");
}

void KeQuerySystemTime(be<uint64_t>* time)
{
    constexpr int64_t FILETIME_EPOCH_DIFFERENCE = 116444736000000000LL;

    auto now = std::chrono::system_clock::now();
    auto timeSinceEpoch = now.time_since_epoch();

    int64_t currentTime100ns = std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(timeSinceEpoch).count();
    currentTime100ns += FILETIME_EPOCH_DIFFERENCE;

    *time = currentTime100ns;
}

// the save's date (the game calls it as it autosaves). The stub left the fields as they
// were. 100 ns units since 1601, as KeQuerySystemTime returns them; Weekday 0 is Sunday.
void RtlTimeToTimeFields(const be<uint64_t>* time, XTIME_FIELDS* fields)
{
    constexpr int64_t FILETIME_EPOCH_DIFFERENCE = 116444736000000000LL;
    const int64_t units = int64_t(uint64_t(*time)) - FILETIME_EPOCH_DIFFERENCE;
    int64_t seconds = units / 10000000;
    int64_t remainder = units % 10000000;
    if (remainder < 0)
    {
        remainder += 10000000;
        --seconds;
    }
    const time_t value = time_t(seconds);
    tm parts{};
    gmtime_r(&value, &parts);
    fields->Year = uint16_t(parts.tm_year + 1900);
    fields->Month = uint16_t(parts.tm_mon + 1);
    fields->Day = uint16_t(parts.tm_mday);
    fields->Hour = uint16_t(parts.tm_hour);
    fields->Minute = uint16_t(parts.tm_min);
    fields->Second = uint16_t(parts.tm_sec);
    fields->Milliseconds = uint16_t(remainder / 10000);
    fields->Weekday = uint16_t(parts.tm_wday);
}

void RtlFreeAnsiString()
{
    LOG_UTILITY("!!! STUB !!!");
}

void RtlUnicodeStringToAnsiString()
{
    LOG_UTILITY("!!! STUB !!!");
}

void RtlInitUnicodeString()
{
    LOG_UTILITY("!!! STUB !!!");
}

void ExTerminateThread(uint32_t ExitCode)
{
    throw GuestThreadExit{ ExitCode };
}

uint32_t ExCreateThread(be<uint32_t>* handle, uint32_t stackSize, be<uint32_t>* threadId, uint32_t xApiThreadStartup, uint32_t startAddress, uint32_t startContext, uint32_t creationFlags)
{
    LOGF("startup 0x{:X} start 0x{:X} context 0x{:X} flags 0x{:X} stack 0x{:X}",
        xApiThreadStartup, startAddress, startContext, creationFlags, stackSize);

    uint32_t hostThreadId;

    auto* hThread = GuestThread::Start({ startAddress, startContext, creationFlags, xApiThreadStartup, stackSize }, &hostThreadId);
    uint32_t entry = startAddress;
    if (const char* name = game::ThreadName(startAddress, startContext, entry))
        NameGuestThreadAtStart(hostThreadId, name);
    if (entry != startAddress)
        LOGF("thread {} runs 0x{:X}", hostThreadId, entry);
    const uint32_t kHandle = GetKernelHandle(hThread);
    *handle = kHandle;
    RegisterHostThreadHandle(kHandle, hostThreadId);

    if (threadId != nullptr)
        *threadId = hostThreadId;

    return 0;
}

void IoInvalidDeviceRequest()
{
    LOG_UTILITY("!!! STUB !!!");
}

void ObReferenceObject()
{
    LOG_UTILITY("!!! STUB !!!");
}

void IoCreateDevice()
{
    LOG_UTILITY("!!! STUB !!!");
}

void IoDeleteDevice()
{
    LOG_UTILITY("!!! STUB !!!");
}

uint32_t ExAllocatePoolTypeWithTag(uint32_t size, uint32_t tag, uint32_t type)
{
    return AllocatePool(size);
}

// the inverse of RtlTimeToTimeFields. FALSE (0) for fields out of range.
uint32_t RtlTimeFieldsToTime(const XTIME_FIELDS* fields, be<uint64_t>* time)
{
    constexpr int64_t FILETIME_EPOCH_DIFFERENCE = 116444736000000000LL;
    const uint32_t month = fields->Month, day = fields->Day, hour = fields->Hour;
    const uint32_t minute = fields->Minute, second = fields->Second, milliseconds = fields->Milliseconds;
    if (fields->Year < 1601 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 ||
        second > 59 || milliseconds > 999)
    {
        return 0;
    }
    tm parts{};
    parts.tm_year = int(uint32_t(fields->Year)) - 1900;
    parts.tm_mon = int(month) - 1;
    parts.tm_mday = int(day);
    parts.tm_hour = int(hour);
    parts.tm_min = int(minute);
    parts.tm_sec = int(second);
    const int64_t seconds = int64_t(timegm(&parts));
    *time = uint64_t(seconds * 10000000 + int64_t(milliseconds) * 10000 + FILETIME_EPOCH_DIFFERENCE);
    return 1;
}

void IoCompleteRequest()
{
    LOG_UTILITY("!!! STUB !!!");
}

void RtlUpcaseUnicodeChar()
{
    LOG_UTILITY("!!! STUB !!!");
}

void ObIsTitleObject()
{
    LOG_UTILITY("!!! STUB !!!");
}

void IoCheckShareAccess()
{
    LOG_UTILITY("!!! STUB !!!");
}

void IoSetShareAccess()
{
    LOG_UTILITY("!!! STUB !!!");
}

void IoRemoveShareAccess()
{
    LOG_UTILITY("!!! STUB !!!");
}

void NetDll_XNetStartup()
{
    LOG_UTILITY("!!! STUB !!!");
}

void NetDll_XNetGetTitleXnAddr()
{
    LOG_UTILITY("!!! STUB !!!");
}

uint32_t NetDll_XNetRandom(uint32_t caller, uint8_t* buffer, uint32_t length)
{
    thread_local std::mt19937 random{ std::random_device{}() };
    for (uint32_t i = 0; i < length; i++)
        buffer[i] = uint8_t(random());
    return 0;
}

// No network: every lookup ends at once, WSAHOST_NOT_FOUND. XNDNS: iStatus, cina, aina[8].
uint32_t NetDll_XNetDnsLookup(uint32_t caller, const char* host, uint32_t event, be<uint32_t>* dns)
{
    // (Windows' winsock.h already defines WSAHOST_NOT_FOUND as a macro)
    constexpr uint32_t kHostNotFound = 11001, kXndnsSize = 4 + 4 + 8 * 4;
    void* block = g_userHeap.Alloc(kXndnsSize);
    memset(block, 0, kXndnsSize);
    *reinterpret_cast<be<uint32_t>*>(block) = kHostNotFound;
    *dns = g_memory.MapVirtual(block);
    if (event != 0)
        reinterpret_cast<Event*>(g_memory.Translate(event))->Set();
    return 0;
}

uint32_t NetDll_XNetDnsRelease(uint32_t caller, uint32_t dns)
{
    if (dns != 0)
        g_userHeap.Free(g_memory.Translate(dns));
    return 0;
}

uint32_t KeWaitForMultipleObjects(uint32_t Count, xpointer<XDISPATCHER_HEADER>* Objects, uint32_t WaitType, uint32_t WaitReason, uint32_t WaitMode, uint32_t Alertable, be<int64_t>* Timeout)
{
    if (Alertable && DeliverQueuedApcs())
        return STATUS_USER_APC;
    HoldTimedWait(Timeout);
    KernelObject* objects[64];
    if (Count > std::size(objects))
        return STATUS_INVALID_PARAMETER;

    for (uint32_t i = 0; i < Count; i++)
    {
        objects[i] = QueryDispatcherObject(Objects[i].get());
        if (objects[i] == nullptr)
            return STATUS_INVALID_HANDLE;
    }

    // WaitType: 0 = wait all, 1 = wait any.
    return WaitForKernelObjects(objects, Count, WaitType == 0, GuestTimeoutToDeadline(Timeout));
}

uint32_t KeRaiseIrqlToDpcLevel()
{
    return 0;
}

void KfLowerIrql() { }

uint32_t KeReleaseSemaphore(XKSEMAPHORE* semaphore, uint32_t increment, uint32_t adjustment, uint32_t wait)
{
    auto* object = QueryKernelObject<Semaphore>(semaphore->Header);
    uint32_t previousCount = 0;
    object->Release(adjustment, &previousCount);
    return previousCount;
}

void XAudioGetVoiceCategoryVolume()
{
    LOG_UTILITY("!!! STUB !!!");
}

uint32_t XAudioGetVoiceCategoryVolumeChangeMask(uint32_t Driver, be<uint32_t>* Mask)
{
    *Mask = 0;
    return 0;
}

uint32_t KeResumeThread(GuestThreadHandle* object)
{
    assert(object != GetKernelObject(CURRENT_THREAD_HANDLE));

    object->suspended = false;
    object->suspended.notify_all();
    return 0;
}

void KeInitializeSemaphore(XKSEMAPHORE* semaphore, uint32_t count, uint32_t limit)
{
    semaphore->Header.Type = 5;
    semaphore->Header.SignalState = count;
    semaphore->Limit = limit;

    auto* object = QueryKernelObject<Semaphore>(semaphore->Header);
}



// uint32_t XAudioRegisterRenderDriverClient(be<uint32_t>* callback, be<uint32_t>* driver)
// {
//     //printf("XAudioRegisterRenderDriverClient(): %x %x\n");
// 
//     *driver = apu::RegisterClient(callback[0], callback[1]);
//     return 0;
// }

// void XAudioUnregisterRenderDriverClient()
// {
//     printf("!!! STUB !!! XAudioUnregisterRenderDriverClient\n");
// }

// uint32_t XAudioSubmitRenderDriverFrame(uint32_t driver, void* samples)
// {
//     // printf("!!! STUB !!! XAudioSubmitRenderDriverFrame\n");
//     apu::SubmitFrames(samples);
// 
//     return 0;
// }

GUEST_FUNCTION_HOOK(__imp__XGetVideoMode, VdQueryVideoMode); // XGetVideoMode
GUEST_FUNCTION_HOOK(__imp__XNotifyGetNext, XNotifyGetNext);
GUEST_FUNCTION_HOOK(__imp__XGetGameRegion, XGetGameRegion);
GUEST_FUNCTION_HOOK(__imp__XMsgStartIORequest, XMsgStartIORequest);
GUEST_FUNCTION_HOOK(__imp__XamUserGetSigninState, XamUserGetSigninState);
GUEST_FUNCTION_HOOK(__imp__XamUserGetXUID, XamUserGetXUID);
GUEST_FUNCTION_HOOK(__imp__XamUserGetName, XamUserGetName);
GUEST_FUNCTION_HOOK(__imp__XamGetSystemVersion, XamGetSystemVersion);
GUEST_FUNCTION_HOOK(__imp__XamContentCreateEx, XamContentCreateEx);
GUEST_FUNCTION_HOOK(__imp__XamContentDelete, XamContentDelete);
GUEST_FUNCTION_HOOK(__imp__XamContentFlush, XamContentFlush);
GUEST_FUNCTION_HOOK(__imp__XamContentClose, XamContentClose);
GUEST_FUNCTION_HOOK(__imp__XamContentSetThumbnail, XamContentSetThumbnail);
GUEST_FUNCTION_HOOK(__imp__XamContentGetCreator, XamContentGetCreator);
GUEST_FUNCTION_HOOK(__imp__XamContentCreateEnumerator, XamContentCreateEnumerator);
GUEST_FUNCTION_HOOK(__imp__XamContentGetDeviceState, XamContentGetDeviceState);
GUEST_FUNCTION_HOOK(__imp__XamContentGetDeviceData, XamContentGetDeviceData);
GUEST_FUNCTION_HOOK(__imp__XamEnumerate, XamEnumerate);
GUEST_FUNCTION_HOOK(__imp__XamUserCreateAchievementEnumerator, XamUserCreateAchievementEnumerator);
GUEST_FUNCTION_HOOK(__imp__XamCreateEnumeratorHandle, XamCreateEnumeratorHandle);
GUEST_FUNCTION_HOOK(__imp__XamAlloc, XamAlloc);
GUEST_FUNCTION_HOOK(__imp__XamFree, XamFree);
GUEST_FUNCTION_HOOK(__imp__XamGetPrivateEnumStructureFromHandle, XamGetPrivateEnumStructureFromHandle);
GUEST_FUNCTION_HOOK(__imp__XamNotifyCreateListener, XamNotifyCreateListener);
GUEST_FUNCTION_HOOK(__imp__XamUserGetSigninInfo, XamUserGetSigninInfo);
GUEST_FUNCTION_HOOK(__imp__XamShowSigninUI, XamShowSigninUI);
GUEST_FUNCTION_HOOK(__imp__XamShowDeviceSelectorUI, XamShowDeviceSelectorUI);
GUEST_FUNCTION_HOOK(__imp__XamShowMessageBoxUI, XamShowMessageBoxUI);
GUEST_FUNCTION_HOOK(__imp__XamShowDirtyDiscErrorUI, XamShowDirtyDiscErrorUI);
GUEST_FUNCTION_HOOK(__imp__XamEnableInactivityProcessing, XamEnableInactivityProcessing);
GUEST_FUNCTION_HOOK(__imp__XamResetInactivity, XamResetInactivity);
GUEST_FUNCTION_HOOK(__imp__XamShowMessageBoxUIEx, XamShowMessageBoxUIEx);
GUEST_FUNCTION_HOOK(__imp__XGetLanguage, XGetLanguage);
GUEST_FUNCTION_HOOK(__imp__XGetAVPack, XGetAVPack);
GUEST_FUNCTION_HOOK(__imp__XamLoaderTerminateTitle, XamLoaderTerminateTitle);
#if (XPORT_TITLE_ID != 0x4D5308AB && XPORT_TITLE_ID != 0x4D5307E6)
GUEST_FUNCTION_HOOK(__imp__XamGetExecutionId, XamGetExecutionId);
#endif
GUEST_FUNCTION_HOOK(__imp__XamLoaderLaunchTitle, XamLoaderLaunchTitle);
GUEST_FUNCTION_HOOK(__imp__RtlInitAnsiString, RtlInitAnsiString);
GUEST_FUNCTION_HOOK(__imp__NtClose, NtClose);
GUEST_FUNCTION_HOOK(__imp__FscSetCacheElementCount, FscSetCacheElementCount);
GUEST_FUNCTION_HOOK(__imp__NtWaitForSingleObjectEx, NtWaitForSingleObjectEx);
GUEST_FUNCTION_HOOK(__imp__ExGetXConfigSetting, ExGetXConfigSetting);
GUEST_FUNCTION_HOOK(__imp__MmQueryStatistics, MmQueryStatistics);
GUEST_FUNCTION_HOOK(__imp__NtCreateEvent, NtCreateEvent);
GUEST_FUNCTION_HOOK(__imp__XexCheckExecutablePrivilege, XexCheckExecutablePrivilege);
GUEST_FUNCTION_HOOK(__imp__DbgPrint, DbgPrint);
GUEST_FUNCTION_HOOK(__imp____C_specific_handler, __C_specific_handler_x);
GUEST_FUNCTION_HOOK(__imp__RtlNtStatusToDosError, RtlNtStatusToDosError);
#if (XPORT_TITLE_ID != 0x4D5308AB && XPORT_TITLE_ID != 0x4D5307E6)
GUEST_FUNCTION_HOOK(__imp__XexGetModuleSection, XexGetModuleSection);
#endif
GUEST_FUNCTION_HOOK(__imp__RtlUnicodeToMultiByteN, RtlUnicodeToMultiByteN);
GUEST_FUNCTION_HOOK(__imp__KeDelayExecutionThread, KeDelayExecutionThread);
GUEST_FUNCTION_HOOK(__imp__ExFreePool, ExFreePool);
GUEST_FUNCTION_HOOK(__imp__ExAllocatePool, ExAllocatePool);
GUEST_FUNCTION_HOOK(__imp__InterlockedPopEntrySList, InterlockedPopEntrySList_x);
GUEST_FUNCTION_HOOK(__imp__InterlockedFlushSList, InterlockedFlushSList_x);
GUEST_FUNCTION_HOOK(__imp__NtPulseEvent, NtPulseEvent);
GUEST_FUNCTION_HOOK(__imp__XamLoaderGetLaunchDataSize, XamLoaderGetLaunchDataSize);
GUEST_FUNCTION_HOOK(__imp__XamLoaderGetLaunchData, XamLoaderGetLaunchData);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetRandom, NetDll_XNetRandom);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetDnsLookup, NetDll_XNetDnsLookup);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetDnsRelease, NetDll_XNetDnsRelease);
GUEST_FUNCTION_HOOK(__imp__NtDuplicateObject, NtDuplicateObject);
GUEST_FUNCTION_HOOK(__imp__ObDereferenceObject, ObDereferenceObject);
GUEST_FUNCTION_HOOK(__imp__KeSetBasePriorityThread, KeSetBasePriorityThread);
GUEST_FUNCTION_HOOK(__imp__ObReferenceObjectByHandle, ObReferenceObjectByHandle);
GUEST_FUNCTION_HOOK(__imp__KeQueryBasePriorityThread, KeQueryBasePriorityThread);
GUEST_FUNCTION_HOOK(__imp__NtSuspendThread, NtSuspendThread);
GUEST_FUNCTION_HOOK(__imp__KeSetAffinityThread, KeSetAffinityThread);
GUEST_FUNCTION_HOOK(__imp__RtlLeaveCriticalSection, RtlLeaveCriticalSection);
GUEST_FUNCTION_HOOK(__imp__RtlEnterCriticalSection, RtlEnterCriticalSection);
GUEST_FUNCTION_HOOK(__imp__HalReturnToFirmware, HalReturnToFirmware);
GUEST_FUNCTION_HOOK(__imp__RtlFillMemoryUlong, RtlFillMemoryUlong);
GUEST_FUNCTION_HOOK(__imp__KeBugCheckEx, KeBugCheckEx);
GUEST_FUNCTION_HOOK(__imp__KeGetCurrentProcessType, KeGetCurrentProcessType);
GUEST_FUNCTION_HOOK(__imp__RtlCompareMemoryUlong, RtlCompareMemoryUlong);
GUEST_FUNCTION_HOOK(__imp__RtlInitializeCriticalSection, RtlInitializeCriticalSection);
GUEST_FUNCTION_HOOK(__imp__RtlRaiseException, RtlRaiseException_x);
GUEST_FUNCTION_HOOK(__imp__KfReleaseSpinLock, KfReleaseSpinLock);
GUEST_FUNCTION_HOOK(__imp__KfAcquireSpinLock, KfAcquireSpinLock);
GUEST_FUNCTION_HOOK(__imp__KeQueryPerformanceFrequency, KeQueryPerformanceFrequency);
GUEST_FUNCTION_HOOK(__imp__VdPersistDisplay, VdPersistDisplay);
GUEST_FUNCTION_HOOK(__imp__KeReleaseSpinLockFromRaisedIrql, KeReleaseSpinLockFromRaisedIrql);
GUEST_FUNCTION_HOOK(__imp__KeAcquireSpinLockAtRaisedIrql, KeAcquireSpinLockAtRaisedIrql);
GUEST_FUNCTION_HOOK(__imp__KiApcNormalRoutineNop, KiApcNormalRoutineNop);
GUEST_FUNCTION_HOOK(__imp__VdSetSystemCommandBufferGpuIdentifierAddress, VdSetSystemCommandBufferGpuIdentifierAddress);
GUEST_FUNCTION_HOOK(__imp__ExRegisterTitleTerminateNotification, ExRegisterTitleTerminateNotification);
GUEST_FUNCTION_HOOK(__imp__VdShutdownEngines, VdShutdownEngines);
GUEST_FUNCTION_HOOK(__imp__VdQueryVideoMode, VdQueryVideoMode);
GUEST_FUNCTION_HOOK(__imp__VdGetCurrentDisplayInformation, VdGetCurrentDisplayInformation);
GUEST_FUNCTION_HOOK(__imp__VdSetDisplayMode, VdSetDisplayMode);
GUEST_FUNCTION_HOOK(__imp__VdCallGraphicsNotificationRoutines, VdCallGraphicsNotificationRoutines);
GUEST_FUNCTION_HOOK(__imp__VdInitializeScalerCommandBuffer, VdInitializeScalerCommandBuffer);
GUEST_FUNCTION_HOOK(__imp__KeLeaveCriticalRegion, KeLeaveCriticalRegion);
GUEST_FUNCTION_HOOK(__imp__VdRetrainEDRAM, VdRetrainEDRAM);
GUEST_FUNCTION_HOOK(__imp__VdRetrainEDRAMWorker, VdRetrainEDRAMWorker);
GUEST_FUNCTION_HOOK(__imp__KeEnterCriticalRegion, KeEnterCriticalRegion);
GUEST_FUNCTION_HOOK(__imp__VdEnableDisableClockGating, VdEnableDisableClockGating);
GUEST_FUNCTION_HOOK(__imp__KeBugCheck, KeBugCheck);
GUEST_FUNCTION_HOOK(__imp__KeLockL2, KeLockL2);
GUEST_FUNCTION_HOOK(__imp__KeUnlockL2, KeUnlockL2);
GUEST_FUNCTION_HOOK(__imp__KeSetEvent, KeSetEvent);
GUEST_FUNCTION_HOOK(__imp__KeResetEvent, KeResetEvent);
GUEST_FUNCTION_HOOK(__imp__KeWaitForSingleObject, KeWaitForSingleObject);
GUEST_FUNCTION_HOOK(__imp__KeTlsGetValue, KeTlsGetValue);
GUEST_FUNCTION_HOOK(__imp__KeTlsSetValue, KeTlsSetValue);
GUEST_FUNCTION_HOOK(__imp__KeTlsAlloc, KeTlsAlloc);
GUEST_FUNCTION_HOOK(__imp__KeTlsFree, KeTlsFree);
GUEST_FUNCTION_HOOK(__imp__XMsgInProcessCall, XMsgInProcessCall);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSAStartup, NetDll_WSAStartup);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSACleanup, NetDll_WSACleanup);
GUEST_FUNCTION_HOOK(__imp__NetDll_socket, NetDll_socket);
GUEST_FUNCTION_HOOK(__imp__NetDll_closesocket, NetDll_closesocket);
GUEST_FUNCTION_HOOK(__imp__NetDll_setsockopt, NetDll_setsockopt);
GUEST_FUNCTION_HOOK(__imp__NetDll_bind, NetDll_bind);
GUEST_FUNCTION_HOOK(__imp__NetDll_connect, NetDll_connect);
GUEST_FUNCTION_HOOK(__imp__NetDll_listen, NetDll_listen);
GUEST_FUNCTION_HOOK(__imp__NetDll_accept, NetDll_accept);
GUEST_FUNCTION_HOOK(__imp__NetDll_select, NetDll_select);
GUEST_FUNCTION_HOOK(__imp__NetDll_recv, NetDll_recv);
GUEST_FUNCTION_HOOK(__imp__NetDll_send, NetDll_send);
GUEST_FUNCTION_HOOK(__imp__NetDll_inet_addr, NetDll_inet_addr);
GUEST_FUNCTION_HOOK(__imp__NetDll___WSAFDIsSet, NetDll___WSAFDIsSet);
GUEST_FUNCTION_HOOK(__imp__XMsgStartIORequestEx, XMsgStartIORequestEx);
GUEST_FUNCTION_HOOK(__imp__XamInputGetCapabilities, XamInputGetCapabilities);
GUEST_FUNCTION_HOOK(__imp__XamInputGetState, XamInputGetState);
GUEST_FUNCTION_HOOK(__imp__XamInputSetState, XamInputSetState);
GUEST_FUNCTION_HOOK(__imp__XamInputGetKeystrokeEx, XamInputGetKeystrokeEx);
GUEST_FUNCTION_HOOK(__imp__RtlTryEnterCriticalSection, RtlTryEnterCriticalSection);
GUEST_FUNCTION_HOOK(__imp__RtlInitializeCriticalSectionAndSpinCount, RtlInitializeCriticalSectionAndSpinCount);
GUEST_FUNCTION_HOOK(__imp__XeCryptBnQwBeSigVerify, XeCryptBnQwBeSigVerify);
GUEST_FUNCTION_HOOK(__imp__XeKeysGetKey, XeKeysGetKey);
GUEST_FUNCTION_HOOK(__imp__XeCryptRotSumSha, XeCryptRotSumSha);
GUEST_FUNCTION_HOOK(__imp__KeEnableFpuExceptions, KeEnableFpuExceptions);
GUEST_FUNCTION_HOOK(__imp__RtlUnwind, RtlUnwind_x);
GUEST_FUNCTION_HOOK(__imp__RtlCaptureContext, RtlCaptureContext_x);
GUEST_FUNCTION_HOOK(__imp__RtlMultiByteToUnicodeN, RtlMultiByteToUnicodeN);
GUEST_FUNCTION_HOOK(__imp__DbgBreakPoint, DbgBreakPoint);
GUEST_FUNCTION_HOOK(__imp__NtClearEvent, NtClearEvent);
GUEST_FUNCTION_HOOK(__imp__NtResumeThread, NtResumeThread);
GUEST_FUNCTION_HOOK(__imp__NtSetEvent, NtSetEvent);
GUEST_FUNCTION_HOOK(__imp__NtCreateSemaphore, NtCreateSemaphore);
GUEST_FUNCTION_HOOK(__imp__NtReleaseSemaphore, NtReleaseSemaphore);
GUEST_FUNCTION_HOOK(__imp__NtWaitForMultipleObjectsEx, NtWaitForMultipleObjectsEx);
GUEST_FUNCTION_HOOK(__imp__NtCreateMutant, NtCreateMutant);
GUEST_FUNCTION_HOOK(__imp__NtReleaseMutant, NtReleaseMutant);
GUEST_FUNCTION_HOOK(__imp__NtCreateTimer, NtCreateTimer);
GUEST_FUNCTION_HOOK(__imp__NtSetTimerEx, NtSetTimerEx);
GUEST_FUNCTION_HOOK(__imp__NtCancelTimer, NtCancelTimer);
GUEST_FUNCTION_HOOK(__imp__RtlCompareStringN, RtlCompareStringN);
GUEST_FUNCTION_HOOK(__imp__StfsControlDevice, StfsControlDevice);
GUEST_FUNCTION_HOOK(__imp__StfsCreateDevice, StfsCreateDevice);
GUEST_FUNCTION_HOOK(__imp__KeQuerySystemTime, KeQuerySystemTime);
GUEST_FUNCTION_HOOK(__imp__RtlTimeToTimeFields, RtlTimeToTimeFields);
GUEST_FUNCTION_HOOK(__imp__RtlFreeAnsiString, RtlFreeAnsiString);
GUEST_FUNCTION_HOOK(__imp__RtlUnicodeStringToAnsiString, RtlUnicodeStringToAnsiString);
GUEST_FUNCTION_HOOK(__imp__RtlInitUnicodeString, RtlInitUnicodeString);
GUEST_FUNCTION_HOOK(__imp__ExTerminateThread, ExTerminateThread);
GUEST_FUNCTION_HOOK(__imp__ExCreateThread, ExCreateThread);
GUEST_FUNCTION_HOOK(__imp__IoInvalidDeviceRequest, IoInvalidDeviceRequest);
GUEST_FUNCTION_HOOK(__imp__ObReferenceObject, ObReferenceObject);
GUEST_FUNCTION_HOOK(__imp__IoCreateDevice, IoCreateDevice);
GUEST_FUNCTION_HOOK(__imp__IoDeleteDevice, IoDeleteDevice);
GUEST_FUNCTION_HOOK(__imp__ExAllocatePoolTypeWithTag, ExAllocatePoolTypeWithTag);
GUEST_FUNCTION_HOOK(__imp__RtlTimeFieldsToTime, RtlTimeFieldsToTime);
GUEST_FUNCTION_HOOK(__imp__IoCompleteRequest, IoCompleteRequest);
GUEST_FUNCTION_HOOK(__imp__RtlUpcaseUnicodeChar, RtlUpcaseUnicodeChar);
GUEST_FUNCTION_HOOK(__imp__ObIsTitleObject, ObIsTitleObject);
GUEST_FUNCTION_HOOK(__imp__IoCheckShareAccess, IoCheckShareAccess);
GUEST_FUNCTION_HOOK(__imp__IoSetShareAccess, IoSetShareAccess);
GUEST_FUNCTION_HOOK(__imp__IoRemoveShareAccess, IoRemoveShareAccess);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetStartup, NetDll_XNetStartup);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetGetTitleXnAddr, NetDll_XNetGetTitleXnAddr);
GUEST_FUNCTION_HOOK(__imp__KeWaitForMultipleObjects, KeWaitForMultipleObjects);
GUEST_FUNCTION_HOOK(__imp__KeRaiseIrqlToDpcLevel, KeRaiseIrqlToDpcLevel);
GUEST_FUNCTION_HOOK(__imp__KfLowerIrql, KfLowerIrql);
GUEST_FUNCTION_HOOK(__imp__KeReleaseSemaphore, KeReleaseSemaphore);
GUEST_FUNCTION_HOOK(__imp__XAudioGetVoiceCategoryVolume, XAudioGetVoiceCategoryVolume);
GUEST_FUNCTION_HOOK(__imp__XAudioGetVoiceCategoryVolumeChangeMask, XAudioGetVoiceCategoryVolumeChangeMask);
GUEST_FUNCTION_HOOK(__imp__KeResumeThread, KeResumeThread);
GUEST_FUNCTION_HOOK(__imp__KeInitializeSemaphore, KeInitializeSemaphore);
GUEST_FUNCTION_HOOK(__imp__XAudioRegisterRenderDriverClient, XAudioRegisterRenderDriverClient);
GUEST_FUNCTION_HOOK(__imp__XAudioUnregisterRenderDriverClient, XAudioUnregisterRenderDriverClient);
GUEST_FUNCTION_HOOK(__imp__XAudioSubmitRenderDriverFrame, XAudioSubmitRenderDriverFrame);
