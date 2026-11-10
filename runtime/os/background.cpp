// holds the game's threads while the Android app is in the background (see background.h).
#include <stdafx.h>
#include <condition_variable>
#include <mutex>
#include <os/logger.h>
#include "background.h"

namespace
{
    std::mutex g_mutex;
    std::condition_variable g_released;
    // Read without the lock by the threads' check; written under it.
    std::atomic<bool> g_held;
    // Every request and release moves it on, so a late engage cannot hold a game in the foreground.
    uint32_t g_ticket;
    std::chrono::steady_clock::time_point g_heldSince;
    // Waits during the current hold (guarded by g_mutex), for the release's log line.
    uint32_t g_waits;

    bool Enabled()
    {
        const char* save = getenv("XPORT_SAVE_DIR");
        const std::string path = std::string(save ? save : ".") + "/../bg_pause";
        bool enabled = true;
        if (FILE* file = fopen(path.c_str(), "r"))
        {
            enabled = fgetc(file) != '0';
            fclose(file);
        }
        return enabled;
    }
}

uint32_t RequestBackgroundHold()
{
    std::lock_guard lock(g_mutex);
    return ++g_ticket;
}

bool EngageBackgroundHold(uint32_t ticket)
{
    if (!Enabled())
    {
        LOGN("background: hold off (files/bg_pause = 0)");
        return false;
    }
    std::lock_guard lock(g_mutex);
    if (ticket != g_ticket || g_held)
        return false;
    g_held = true;
    g_heldSince = std::chrono::steady_clock::now();
    g_waits = 0;
    LOGN("background: game held");
    return true;
}

void ReleaseBackgroundHold()
{
    {
        std::lock_guard lock(g_mutex);
        ++g_ticket;
        if (!g_held)
            return;
        g_held = false;
        LOGFN("background: game released after {:.1f} s ({} threads waited)",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - g_heldSince).count(), g_waits);
    }
    g_released.notify_all();
}

bool BackgroundHeld()
{
    return g_held.load(std::memory_order_acquire);
}

bool WaitWhileBackgroundHold(const char* who)
{
    if (!g_held.load(std::memory_order_acquire))
        return false;
    std::unique_lock lock(g_mutex);
    if (!g_held)
        return false;
    ++g_waits;
    if (who)
        LOGFN("background: {} thread waits", who);
    g_released.wait(lock, [] { return !g_held.load(std::memory_order_relaxed); });
    return true;
}
