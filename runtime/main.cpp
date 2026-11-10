#include <game.h>
// entry point of the runtime. Based on UnleashedRecomp's main.cpp (KiSystemStartup,
// LdrLoadModule, GuestThread::Start) without the installer, UI, mods or video device: phase 2
// runs headless until the game reaches its main loop.
#include <stdafx.h>
#include <apu/audio.h>
#include <cpu/guest_thread.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <kernel/xam.h>
#include <os/paths.h>
#include <os/thread_dump.h>
#include <os/thread_policy.h>
#include "loader.h"

void InstallCrashHandler();

Memory g_memory;
Heap g_userHeap;

static void KiSystemStartup()
{
    g_userHeap.Init();

    const std::string gamePath = os::paths::GetGamePath().string();
    const auto gameContent = XamMakeContent(XCONTENTTYPE_RESERVED, "Game");
    XamRegisterContent(gameContent, gamePath);

    std::error_code ec;
    std::filesystem::create_directories(os::paths::GetSavePath(), ec);
    XamRegisterSaves();

    // The game reads its data through game:\ and d:\ (the disc).
    XamContentCreateEx(0, "game", &gameContent, OPEN_EXISTING, nullptr, nullptr, 0, 0, nullptr);
    XamContentCreateEx(0, "D", &gameContent, OPEN_EXISTING, nullptr, nullptr, 0, 0, nullptr);

    // update:\ holds the disc's own data patch (update\data\effects\entityFx.dat, ...); the game
    // never mounts it itself.
    XamRootCreate("update", (os::paths::GetGamePath() / "update").string());

    XAudioInitializeSystem();
}

// the whole game run. main() calls it in the command-line build; the Android app's
// libgame.so (XPORT_SHARED) starts it on its own thread from runtime/os/android_app.cpp.
int XportMain()
{
    os::logger::Init();
    InstallCrashHandler();

    if (g_memory.base == nullptr)
    {
        LOGN_ERROR("cannot reserve the 4 GB guest address space");
        return 1;
    }

    const auto xexPath = os::paths::GetGamePath() / "default.xex";
    LOGFN("game data: {}", os::paths::GetGamePath().string());

    KiSystemStartup();
    StartThreadDumpWatchdog();

    const uint32_t entry = LdrLoadModule(xexPath);
    if (entry == 0)
        return 1;
    game::AfterImageLoad();

    NameCurrentThread("xport-main");
    try
    {
        GuestThread::Start({ entry, 0, 0 });
    }
    catch (const GuestThreadExit& exit)
    {
        LOGFN("main thread terminated with code {}", exit.exitCode);
    }

    return 0;
}

#ifndef XPORT_SHARED
int main(int argc, char* argv[])
{
    return XportMain();
}
#endif
