#include <stdafx.h>
#include "xdm.h"
#include "freelist.h"

#ifdef _WIN32
#include <ntstatus.h>
#endif

Mutex g_kernelLock;
std::mutex g_dispatcherMutex;
std::atomic<bool> g_dispatcherWakeAll;
std::atomic<bool> g_sleepZeroAlwaysYields;
static KernelWaitEntry* g_blockedThreads; // one entry per blocked thread, for g_dispatcherWakeAll

static Mutex g_objectRegistryLock;
static ankerl::unordered_dense::set<const void*> g_objectRegistry;

void RegisterKernelObject(KernelObject* obj)
{
    std::lock_guard guard{ g_objectRegistryLock };
    g_objectRegistry.insert(obj);
}

bool IsRegisteredKernelObject(const void* obj)
{
    std::lock_guard guard{ g_objectRegistryLock };
    return g_objectRegistry.contains(obj);
}

void DestroyKernelObject(KernelObject* obj)
{
    {
        std::lock_guard guard{ g_objectRegistryLock };
        g_objectRegistry.erase(obj);
    }

    obj->~KernelObject();
    g_userHeap.Free(obj);
}

GuestDeadline GuestTimeoutToDeadline(const be<int64_t>* timeout)
{
    using Ticks = std::chrono::duration<int64_t, std::ratio<1, 10000000>>;

    if (timeout == nullptr)
        return std::nullopt;

    const int64_t value = *timeout;
    const auto now = std::chrono::steady_clock::now();

    // Negative values are relative, in 100 ns units.
    if (value <= 0)
        return now + Ticks(-value);

    // Positive values are absolute system time (100 ns since 1601).
    constexpr int64_t FILETIME_EPOCH_DIFFERENCE = 116444736000000000LL;
    const int64_t systemTime = std::chrono::duration_cast<Ticks>(std::chrono::system_clock::now().time_since_epoch()).count() + FILETIME_EPOCH_DIFFERENCE;
    return now + Ticks(std::max<int64_t>(value - systemTime, 0));
}

void WakeKernelWaiters(KernelObject* obj)
{
    // Under g_dispatcherMutex a linked entry's thread is still blocked in WaitForKernelObjects,
    // so its condition variable is alive.
    KernelWaitEntry* entry = g_dispatcherWakeAll.load(std::memory_order_relaxed) ? g_blockedThreads : obj->waiters;
    for (; entry != nullptr; entry = entry->next)
        entry->condition->notify_one();
}

void RetainKernelObject(KernelObject* obj)
{
    obj->references.fetch_add(1);
}

void ReleaseKernelObject(KernelObject* obj)
{
    if (obj->references.fetch_sub(1) == 1)
        DestroyKernelObject(obj);
}

uint32_t WaitForKernelObjects(KernelObject* const* objects, uint32_t count, bool waitAll, GuestDeadline deadline)
{
    // Keep the objects alive even if another thread closes their handles during the wait.
    struct References
    {
        KernelObject* const* objects;
        uint32_t count;
        References(KernelObject* const* objects, uint32_t count) : objects(objects), count(count)
        {
            for (uint32_t i = 0; i < count; i++)
                RetainKernelObject(objects[i]);
        }
        ~References()
        {
            for (uint32_t i = 0; i < count; i++)
                ReleaseKernelObject(objects[i]);
        }
    } references{ objects, count };

    std::unique_lock lock{ g_dispatcherMutex };

    // Linked into the objects' waiter lists only while blocked; unlinked (lock held) on return.
    thread_local std::condition_variable t_condition;
    struct WaitEntries
    {
        KernelObject* const* objects;
        uint32_t count;
        bool linked = false;
        KernelWaitEntry blocked;
        KernelWaitEntry entries[64];

        WaitEntries(KernelObject* const* objects, uint32_t count) : objects(objects), count(count)
        {
        }

        static void Unlink(KernelWaitEntry** list, KernelWaitEntry* entry)
        {
            for (KernelWaitEntry** link = list; *link != nullptr; link = &(*link)->next)
            {
                if (*link == entry)
                {
                    *link = entry->next;
                    return;
                }
            }
        }

        void Link()
        {
            for (uint32_t i = 0; i < count; i++)
            {
                entries[i] = { &t_condition, objects[i]->waiters };
                objects[i]->waiters = &entries[i];
            }
            blocked = { &t_condition, g_blockedThreads };
            g_blockedThreads = &blocked;
            linked = true;
        }

        ~WaitEntries()
        {
            if (!linked)
                return;

            for (uint32_t i = 0; i < count; i++)
                Unlink(&objects[i]->waiters, &entries[i]);
            Unlink(&g_blockedThreads, &blocked);
        }
    } waitEntries{ objects, count };
    assert(count <= std::size(waitEntries.entries));

    while (true)
    {
        if (waitAll)
        {
            bool all = true;
            for (uint32_t i = 0; i < count && all; i++)
                all = objects[i]->IsSignaled();

            if (all)
            {
                for (uint32_t i = 0; i < count; i++)
                    objects[i]->Acquire();

                return STATUS_SUCCESS;
            }
        }
        else
        {
            for (uint32_t i = 0; i < count; i++)
            {
                if (objects[i]->IsSignaled())
                {
                    objects[i]->Acquire();
                    return STATUS_WAIT_0 + i;
                }
            }
        }

        if (deadline && std::chrono::steady_clock::now() >= *deadline)
            return STATUS_TIMEOUT;

        GuestDeadline wake = deadline;
        for (uint32_t i = 0; i < count; i++)
        {
            const GuestDeadline due = objects[i]->DueTime();
            if (due && (!wake || *due < *wake))
                wake = due;
        }

        if (!waitEntries.linked)
            waitEntries.Link();

        if (wake)
            t_condition.wait_until(lock, *wake);
        else
            t_condition.wait(lock);
    }
}

uint32_t KernelObject::Wait(GuestDeadline deadline)
{
    KernelObject* self = this;
    return WaitForKernelObjects(&self, 1, false, deadline);
}

uint32_t GetKernelHandle(KernelObject* obj)
{
    assert(obj != GetInvalidKernelObject());
    return g_memory.MapVirtual(obj);
}

void DestroyKernelObject(uint32_t handle)
{
    DestroyKernelObject(GetKernelObject(handle));
}

bool IsKernelObject(uint32_t handle)
{
    return (handle & 0x80000000) != 0;
}

bool IsKernelObject(void* obj)
{
    return IsKernelObject(g_memory.MapVirtual(obj));
}

bool IsInvalidKernelObject(void* obj)
{
    return obj == GetInvalidKernelObject();
}
