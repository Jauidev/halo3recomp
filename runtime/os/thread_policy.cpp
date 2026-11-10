// thread names and core placement (see thread_policy.h). The placement follows what
// the Modern Combat 4 port measured on this phone (mc4-arm64-port/src/hle/affinity.cpp):
// - HyperOS/MediaTek rewrite a thread's cpuset every few seconds, which resets its mask, so the
//   masks are re-applied: the main thread's every 100 ms, the others every 2 seconds;
// - with the SoC hot, core_ctl pauses the fastest core; sched_setaffinity still succeeds but the
//   thread runs elsewhere. Seen off it 3 checks in a row, the main thread moves to the big
//   cluster for 2 seconds and then tries again.
// Roles: the game's main thread gets the fastest core alone; the busiest other threads get the
// other big cores; everything else runs anywhere except the fastest core. The game names only
// one thread (PathServerThread), so "busy" is measured: every 2 seconds each thread's CPU time
// is read from /proc, and a thread above PROMOTE_SHARE of a core moves to the big cores (at most
// MAX_BUSY of them) until it drops below DEMOTE_SHARE. Measured in TLAD (2026-10-23): main ~46%,
// audio engine ~52%, render ~25%, one more game thread ~25%, all others under 20%.
// A runtime thread (xport-affinity) does all of this; the game's own loops are not hooked
// because the main loop changes between the menus and the game.
// The game's render and sound threads are known by their entry point (KnownGuestThreadName) and
// stay on the big cores: measured on 2026-10-26, the render thread dropped under DEMOTE_SHARE
// while it waited for the renderer and was sent to the little cores ~90 times in 6 minutes (one
// stutter each). A busy thread also needs QUIET_SWEEPS quiet sweeps in a row before it leaves the
// big cores. Audio and vblank threads wake often and do little work: they run anywhere but the
// fastest core at normal priority (on the little cores at nice +4 their wakeups came late).
// The game's render thread also gets a big core of its own (Role::Render, the last big core before
// the fastest): sharing cores 4-6, it lost ~20% of its time to the renderer threads it wakes for
// every batch of commands (wakeup preemption), while the game's main thread waited for it.
// The game's task and job workers (Role::Worker, named by entry point in ExCreateThread) are what
// the main thread waits for ~77% of its time, but each one is too quiet to count as busy, so they
// ran on the little cores. They now run on every big core, the main and render cores included
// (those threads wait 77% and 60% of the time), at nice 5: low enough that a waking worker never
// preempts the thread that owns the core (CFS wakeup granularity), so they fill idle big cores.
// XpRecorder (Role::Recorder) records each frame's Vulkan commands and caps the frame rate
// (~38 ms of a ~45 ms frame, 2026-10-26). It shares cores 4-5 (99% busy) with XpSoundThread,
// XpRenderWorker and busy game threads, all at nice -6, and lost time to each of their wakeups;
// it now runs on the same cores at nice -10, so it is not preempted by them and gets ~2.4x their
// share when the cores are contended. Only the recorder itself: the Vulkan driver's threads are
// created from it and inherit its name (4 of them, 2026-10-26), and at nice -10 they took the
// sound threads' time.
// XpSoundThread (the game's mixer) and xport-audio (the XAudio clock, apu/null_driver.cpp) are
// Role::Audio: big cores at nice -12, above the recorder. With the recorder at -10 and the sound
// thread at -6 on the same two 98%-busy cores, the sound came out distorted (2026-10-26).
// Layout mode C (2026-10-27, files/layout_mode): cores 4-5 were ~99% busy with the recorder (69%
// of a core), the sound threads (57%) and XpRenderWorker (34%), while the fastest core ran
// xport-main at 19% (it waits for the task workers 77% of its time). XpRenderWorker now shares the
// fastest core with xport-main at nice -6 (Role::PrimeShared; xport-main at -10 goes first), and the
// fake GPU's ring thread xport-gpu (1% of a core in many short wakeups, each a preemption on the
// recorder's cores) runs anywhere but the fastest core. Mode D puts the recorder alone on the
// fastest core (Role::RecorderPrime, with the main thread's core_ctl fallback) and xport-main on the
// big cores; mode A is the placement before, and the default again: hot, the fastest core is
// throttled below the others and C lost 4 fps to A (ReadLayoutMode).
// XPORT_AFFINITY=0 disables placement (names are still set).
#include <stdafx.h>
#include <os/logger.h>
#include <kernel/function.h>
#include <kernel/xdm.h>
#include <cpu/guest_thread.h>
#include "thread_policy.h"
#include "perf_hint.h"
#ifdef __ANDROID__
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <dirent.h>
#include <unistd.h>
#endif

namespace
{
    enum class Role
    {
        Main,   // the fastest core alone
        Render, // the game's render thread: one big core alone
        Worker, // the game's task and job workers: every big core, at low priority
        Recorder, // XpRecorder: the big cores beside main and render, above the other threads there
        Audio,  // the game's sound thread and the XAudio clock: big cores, above the recorder
        PrimeShared,   // the fastest core beside xport-main, below it (layout mode C: XpRenderWorker)
        RecorderPrime, // XpRecorder alone on the fastest core (layout mode D)
        Big,    // big cores except the fastest and the render core
        Little, // little/efficiency cores (Cortex-A510)
        Other,  // any core except the fastest
    };

    // Runtime threads that do heavy per-frame work. Matched as substrings of the names.
    constexpr const char* s_bigThreads[] = {
        "Render",
        "render",
        "XpRecorder",
        "gpu",
        "VkRenderThread",
        "Physics",
        "physics",
        "Sound",
    };

    // Background worker & audio threads assigned to the 4 little/efficiency cores.
    constexpr const char* s_littleThreads[] = {
        "tex-worker",
        "worker",
        "Worker",
        "affinity",
        "pool",
        "Pool",
        "async",
        "Async",
        "compiler",
        "Compile",
        "mali-",
        "hwc",
        "surfaceflinger",
        // XpRetirer only frees finished frames' commands. Busy (~18% of a core), it was promoted
        // to the big cores and took XpRecorder's core each time the recorder woke it: ~3 ms per
        // frame of the recorder (simpleperf, 2026-10-26).
        "Retirer",
        // The texture disk cache's file work (native_texture_disk_cache.cpp): busy while the cache
        // fills, and nothing waits for it.
        "XpTexCache",
        // The background shader translation (shader_translator.cpp) and pipeline compiles
        // (graphics_system.cpp, XpPipeCompile matches "Compile" above). Halo 3, 2026-11-10: with
        // no name here they counted as busy threads and were moved to the two big cores the
        // recorder, the sound and the render worker share, at nice -6, each time a new area
        // brought shaders and pipelines: the frame rate dropped until they were done.
        "ShaderXlate",
    };

    constexpr double PROMOTE_SHARE = 0.10; // of one core (10% promotes to Big)
    constexpr double DEMOTE_SHARE = 0.05;  // 5% demotes
    constexpr double INCUMBENT_BONUS = 0.10;
    constexpr size_t MAX_BUSY = 4;         // allow all active engine threads on big cluster
    constexpr int QUIET_SWEEPS = 5;        // seconds below the threshold before leaving the big cores

    struct HostThread
    {
        int tid = 0;
        std::string name;
        Role role = Role::Other;
    };

    std::mutex g_mutex;
    std::unordered_map<uint32_t, HostThread> g_threads; // by guest thread id (and handle)
    std::unordered_map<uint32_t, std::string> g_pendingNames; // named before the thread registered
    std::unordered_map<uint32_t, uint32_t> g_handleIds;       // handle -> id of a thread not registered yet
    char g_workerMode = 'A'; // worker_mode test switch (ReadWorkerMode), under g_mutex
    char g_recorderMode = 'A'; // recorder_mode test switch (ReadRecorderMode), under g_mutex
    // layout_mode test switch (ReadLayoutMode), written under g_mutex; the placement thread reads it.
    std::atomic<char> g_layoutMode{'A'};
    std::atomic<int> g_recorderTid{0}; // xport_thread_policy_set_recorder; 0 until the renderer starts

    // tid: the thread's, so that only the recorder itself among the threads named XpRecorder is
    // placed as the recorder.
    Role RoleForName(const std::string& name, int tid)
    {
        const char layout = g_layoutMode.load(std::memory_order_relaxed);
        if (name == "xport-main")
            return Role::Main; // layout mode D moves it in PlacementThread, it stays the main thread
        if (name == "XpRenderThread")
            return Role::Render;
#if XPORT_TITLE_ID == 0x4D5307E6
        // Halo 3 names its own threads (THREADNAME_INFO): RENDER submits every frame's D3D work
        // (VdSwap keeps its name, so it never becomes XpRenderThread), AUDIO mixes the sound, and
        // NET_DEBUG is the idle debug listener. ASYNC_IO (the disc reads) stays measured.
        if (name == "RENDER")
            return Role::Render;
        if (name == "AUDIO")
            return Role::Audio;
        if (name == "NET_DEBUG")
            return Role::Little;
#endif
        // Before the "Worker" substring sends them to the little cores.
        if (name == "XpTaskWorker" || name == "XpJobWorker")
            return g_workerMode == 'B' ? Role::Other : Role::Worker;
        // Before the "Render" substring sends it to cores 4-5 with the recorder.
        if (name == "XpRenderWorker" && layout == 'C')
            return Role::PrimeShared;
        // Before the "gpu" substring sends it to the big cores.
        if (name == "xport-gpu" && layout != 'A')
            return Role::Other;
        if (name == "XpRecorder")
        {
            const int recorder = g_recorderTid.load(std::memory_order_relaxed);
            if (recorder != 0 && tid != recorder)
                return Role::Other; // a driver thread created by the recorder
            if (layout == 'D' && recorder != 0)
                return Role::RecorderPrime;
            return g_recorderMode == 'B' ? Role::Big : Role::Recorder;
        }
        if (name == "XpSoundThread" || name == "xport-audio")
            return Role::Audio;
        for (const char* big : s_bigThreads)
        {
            if (name.find(big) != std::string::npos)
                return Role::Big;
        }
        for (const char* little : s_littleThreads)
        {
            if (name.find(little) != std::string::npos)
                return Role::Little;
        }
        return Role::Other;
    }

    const char* PlacementName(Role role)
    {
        switch (role)
        {
        case Role::Main: return "fastest core";
        case Role::Render: return "its own big core";
        case Role::Worker: return "every big core at low priority";
        case Role::Recorder: return "big cores, above the other threads there";
        case Role::Audio: return "big cores, above the recorder";
        case Role::PrimeShared: return "fastest core, beside xport-main";
        case Role::RecorderPrime: return "fastest core alone";
        case Role::Big: return "big cores";
        case Role::Little: return "little cores (Cortex-A510)";
        default: return "any core but the fastest";
        }
    }

#ifdef __ANDROID__
    struct Layout
    {
        bool enabled = false;
        int prime = -1;
        int render = -1;
        int primeCapacity = 0;
        int reference = -1;        // a big core beside the prime one (PrimeThrottled)
        int referenceCapacity = 0;
        cpu_set_t primeOnly{};
        cpu_set_t renderOnly{};
        cpu_set_t big{};        // big cores except the prime and render ones
        cpu_set_t allBig{};     // every big core (fallback for Main, and the workers)
        cpu_set_t others{};     // every core except the prime one
        cpu_set_t little{};     // little / efficiency cores (Cortex-A510)
    };

    int ReadCapacity(int cpu)
    {
        char path[96];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
        FILE* file = fopen(path, "r");
        if (!file)
            return -1;
        int value = -1;
        if (fscanf(file, "%d", &value) != 1)
            value = -1;
        fclose(file);
        return value;
    }

    const Layout& GetLayout()
    {
        static const Layout s_layout = []
        {
            Layout layout;
            const char* setting = getenv("XPORT_AFFINITY");
            if (setting && setting[0] == '0')
                return layout;

            const int count = int(sysconf(_SC_NPROCESSORS_CONF));
            int capacities[CPU_SETSIZE] = {};
            int best = -1;
            for (int cpu = 0; cpu < count && cpu < CPU_SETSIZE; cpu++)
            {
                capacities[cpu] = ReadCapacity(cpu);
                if (capacities[cpu] >= best) // ties: the higher index is usually the faster core
                {
                    best = capacities[cpu];
                    layout.prime = cpu;
                }
            }
            if (layout.prime < 0 || count < 2)
                return layout;

            CPU_ZERO(&layout.primeOnly);
            CPU_ZERO(&layout.big);
            CPU_ZERO(&layout.allBig);
            CPU_ZERO(&layout.others);
            CPU_ZERO(&layout.little);
            CPU_SET(layout.prime, &layout.primeOnly);
            for (int cpu = 0; cpu < count && cpu < CPU_SETSIZE; cpu++)
            {
                const bool isBig = capacities[cpu] * 2 > best;
                if (isBig)
                    CPU_SET(cpu, &layout.allBig);
                if (isBig && cpu != layout.prime)
                    CPU_SET(cpu, &layout.big);
                if (!isBig)
                    CPU_SET(cpu, &layout.little);
                if (cpu != layout.prime)
                    CPU_SET(cpu, &layout.others);
            }
            if (CPU_COUNT(&layout.little) == 0)
                layout.little = layout.others;
            // The render core: the highest big core but the prime one, if two others remain.
            CPU_ZERO(&layout.renderOnly);
            if (CPU_COUNT(&layout.big) >= 3)
            {
                for (int cpu = count - 1; cpu >= 0 && layout.render < 0; cpu--)
                {
                    if (CPU_ISSET(cpu, &layout.big))
                        layout.render = cpu;
                }
                CPU_CLR(layout.render, &layout.big);
                CPU_CLR(layout.render, &layout.others);
                CPU_SET(layout.render, &layout.renderOnly);
            }
            else
            {
                layout.renderOnly = layout.big;
            }
            layout.primeCapacity = best;
            for (int cpu = 0; cpu < count && cpu < CPU_SETSIZE && layout.reference < 0; cpu++)
            {
                if (cpu != layout.prime && CPU_ISSET(cpu, &layout.allBig))
                {
                    layout.reference = cpu;
                    layout.referenceCapacity = capacities[cpu];
                }
            }
            layout.enabled = true;
            LOGFN("affinity: main thread on core {} (capacity {}), render thread on core {}, {} big cores beside them, {} little cores",
                layout.prime, best, layout.render, CPU_COUNT(&layout.big), CPU_COUNT(&layout.little));
            return layout;
        }();
        return s_layout;
    }

    const cpu_set_t* MaskFor(Role role)
    {
        const Layout& layout = GetLayout();
        switch (role)
        {
        case Role::Main:
        case Role::PrimeShared:
        case Role::RecorderPrime: return &layout.primeOnly; // PlacementThread follows ApplyMain's fallback
        case Role::Render: return &layout.renderOnly;
        case Role::Worker: return &layout.allBig;
        case Role::Recorder:
        case Role::Audio:
        case Role::Big: return &layout.big;
        case Role::Little: return &layout.little;
        default: return &layout.others;
        }
    }

    void ApplyPriority(int tid, Role role)
    {
        int prio = 0;
        switch (role)
        {
        case Role::Main: prio = -10; break;
        case Role::Render: prio = -8; break;
        case Role::Worker: prio = 5; break;
        case Role::Recorder: prio = -10; break;
        case Role::Audio: prio = -12; break;
        case Role::PrimeShared: prio = -6; break; // under xport-main (-10)
        case Role::RecorderPrime: prio = -10; break;
        case Role::Big: prio = -6; break;
        case Role::Other: prio = 0; break;
        case Role::Little: prio = 4; break;
        }
        setpriority(PRIO_PROCESS, tid, prio);
    }

    void Apply(int tid, Role role)
    {
        if (GetLayout().enabled)
        {
            sched_setaffinity(tid, sizeof(cpu_set_t), MaskFor(role));
            ApplyPriority(tid, role);
        }
    }

    void SetOsName(int tid, const std::string& name)
    {
        // comm holds 15 characters.
        const std::string shortName = name.substr(0, 15);
        if (tid == gettid())
        {
            prctl(PR_SET_NAME, shortName.c_str());
            return;
        }
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
        if (FILE* file = fopen(path, "w"))
        {
            fputs(shortName.c_str(), file);
            fclose(file);
        }
    }

    int CurrentTid()
    {
        return gettid();
    }

    // CPU time (user + system, in clock ticks) and the CPU the thread last ran on.
    bool ReadTaskStat(int tid, uint64_t& ticks, int& cpu)
    {
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
        FILE* file = fopen(path, "r");
        if (!file)
            return false;
        char buffer[1024];
        const size_t size = fread(buffer, 1, sizeof(buffer) - 1, file);
        fclose(file);
        buffer[size] = '\0';

        // The command name (field 2) may hold spaces; field 3 starts after its ')'.
        char* cursor = strrchr(buffer, ')');
        if (cursor == nullptr)
            return false;
        cursor += 2;
        uint64_t utime = 0, stime = 0;
        cpu = -1;
        for (int field = 3; field <= 39 && *cursor != '\0'; field++)
        {
            char* end;
            const unsigned long long value = strtoull(cursor, &end, 10);
            if (field == 14)
                utime = value;
            else if (field == 15)
                stime = value;
            else if (field == 39)
                cpu = int(value);
            cursor = end;
            while (*cursor == ' ')
                cursor++;
            if (field == 3) // the state is a letter, not a number
                cursor = strchr(cursor, ' ') ? strchr(cursor, ' ') + 1 : cursor;
        }
        ticks = utime + stime;
        return cpu >= 0;
    }

    long ReadCpuFrequency(int cpu, const char* file)
    {
        char path[96];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/%s", cpu, file);
        FILE* handle = fopen(path, "r");
        if (!handle)
            return -1;
        long value = -1;
        if (fscanf(handle, "%ld", &value) != 1)
            value = -1;
        fclose(handle);
        return value;
    }

    // Hot, the thermal limit takes the fastest core lower than the other big ones (seen on this
    // phone: 0.9-1.2 GHz against 1.9, see layout mode C above), and core_ctl does not pause it, so
    // the main thread stayed alone on what had become the slowest big core. Each core's speed
    // under the limit is its capacity scaled by scaling_max_freq / cpuinfo_max_freq; the fastest
    // core counts as throttled under 80% of another big core's and as recovered over 90%. Read once
    // a second. files/thermal_main_mode starting with 'B' turns the check off; a file that cannot
    // be read leaves the placement as it was.
    bool PrimeThrottled(std::chrono::steady_clock::time_point now)
    {
        static bool s_throttled = false;
        static std::chrono::steady_clock::time_point s_next;
        if (now < s_next)
            return s_throttled;
        s_next = now + std::chrono::seconds(1);
        const Layout& layout = GetLayout();
        static const std::string path = []
        {
            const char* save = getenv("XPORT_SAVE_DIR");
            return std::string(save ? save : ".") + "/../thermal_main_mode";
        }();
        bool off = false;
        if (FILE* file = fopen(path.c_str(), "r"))
        {
            off = fgetc(file) == 'B';
            fclose(file);
        }
        if (off || layout.reference < 0)
            return s_throttled = false;
        const long primeLimit = ReadCpuFrequency(layout.prime, "scaling_max_freq");
        const long primeMax = ReadCpuFrequency(layout.prime, "cpuinfo_max_freq");
        const long referenceLimit = ReadCpuFrequency(layout.reference, "scaling_max_freq");
        const long referenceMax = ReadCpuFrequency(layout.reference, "cpuinfo_max_freq");
        if (primeLimit <= 0 || primeMax <= 0 || referenceLimit <= 0 || referenceMax <= 0)
            return s_throttled = false;
        const double prime = double(layout.primeCapacity) * double(primeLimit) / double(primeMax);
        const double reference = double(layout.referenceCapacity) * double(referenceLimit) / double(referenceMax);
        s_throttled = prime < reference * (s_throttled ? 0.90 : 0.80);
        return s_throttled;
    }

    // Main-thread placement with the paused-core fallback, every 100 ms. Returns the mask the
    // thread got, for the threads that follow it (Role::PrimeShared). In layout mode D the thread
    // placed here is the recorder.
    const cpu_set_t* ApplyMain(int tid)
    {
        const Layout& layout = GetLayout();
        using clock = std::chrono::steady_clock;
        enum State { Prime, BigPaused, BigCpuset, BigThrottled };
        static int s_state = -1;
        static int s_strays;
        static clock::time_point s_retry;
        static uint64_t s_lastTicks;
        const auto now = clock::now();

        // The last CPU only counts if the thread ran since the previous check: a sleeping
        // thread still shows the core it ran on before its mask changed.
        uint64_t ticks = 0;
        int cpu = -1;
        const bool ran = ReadTaskStat(tid, ticks, cpu) && ticks != s_lastTicks;
        s_lastTicks = ticks;

        int next = Prime;
        if ((s_state == BigPaused || s_state == BigCpuset) && now < s_retry)
            next = s_state;
        else if (PrimeThrottled(now))
            next = BigThrottled;
        else if (sched_setaffinity(tid, sizeof(cpu_set_t), &layout.primeOnly) != 0)
            next = BigCpuset;
        else if (s_state == Prime && ran && cpu != layout.prime)
            next = ++s_strays >= 3 ? BigPaused : Prime;
        else if (ran)
            s_strays = 0;

        if ((next == BigPaused || next == BigCpuset) && next != s_state)
        {
            s_strays = 0;
            s_retry = now + std::chrono::seconds(2);
            sched_setaffinity(tid, sizeof(cpu_set_t), &layout.allBig);
        }
        // Every call: the system resets the mask every few seconds (see the top of the file).
        if (next == BigThrottled)
        {
            s_strays = 0;
            sched_setaffinity(tid, sizeof(cpu_set_t), &layout.allBig);
        }

        // core_ctl pauses the fastest core on and off, so the paused fallback is reported as a
        // count every 30 seconds instead of at every switch.
        static int s_pauses;
        static clock::time_point s_nextReport = now + std::chrono::seconds(30);
        if (next != s_state)
        {
            if (next == BigThrottled)
                LOGN("affinity: fastest core limited below the other big cores (heat); main thread on the big cluster");
            else if (s_state == BigThrottled)
                LOGN("affinity: fastest core no longer limited; main thread back on it");
            if (next == BigThrottled || (s_state == BigThrottled && next == Prime))
                ;
            else if (next == BigPaused && s_state != -1)
                s_pauses++;
            else if (next == BigCpuset || s_state == -1)
                LOGFN("affinity: main thread on the {}", next == BigCpuset ? "big cluster (outside the cpuset)" : "fastest core");
            s_state = next;
        }
        if (now >= s_nextReport)
        {
            if (s_pauses != 0)
                LOGFN("affinity: fastest core paused {} times in 30 s; main thread on the big cluster meanwhile", s_pauses);
            s_pauses = 0;
            s_nextReport = now + std::chrono::seconds(30);
        }
        return s_state == Prime ? &layout.primeOnly : &layout.allBig;
    }

    // Test switch (2026-10-26): the first character of <XPORT_SAVE_DIR>/../affinity_mode picks where busy
    // unnamed threads go, read every sweep so modes can be compared in one session:
    // A = big cores at big priority (default), B = little and big cores at normal priority,
    // C = big cores at normal priority. Measured standing in the street, 3 rounds of 20 s each:
    // A 22.4 fps, C 20.6, B 17.3 (A won every round).
    char ReadAffinityMode()
    {
        static const std::string path = []
        {
            const char* save = getenv("XPORT_SAVE_DIR");
            return std::string(save ? save : ".") + "/../affinity_mode";
        }();
        char mode = 'A';
        if (FILE* file = fopen(path.c_str(), "r"))
        {
            const int c = fgetc(file);
            if (c == 'A' || c == 'B' || c == 'C')
                mode = char(c);
            fclose(file);
        }
        return mode;
    }

    // Test switch next to affinity_mode: files/dispatcher_mode starting with 'B' makes every kernel
    // signal wake every blocked game thread (the old shared condition variable, see xdm.h).
    void ReadDispatcherMode()
    {
        static const std::string path = []
        {
            const char* save = getenv("XPORT_SAVE_DIR");
            return std::string(save ? save : ".") + "/../dispatcher_mode";
        }();
        bool wakeAll = false;
        if (FILE* file = fopen(path.c_str(), "r"))
        {
            wakeAll = fgetc(file) == 'B';
            fclose(file);
        }
        if (g_dispatcherWakeAll.exchange(wakeAll) != wakeAll)
            LOGFN("dispatcher: {}", wakeAll ? "every signal wakes every blocked thread" : "signals wake their own waiters");
    }

    // Test switch next to affinity_mode: files/sleep_mode starting with 'B' makes every Sleep(0)
    // yield, as before back-to-back ones became short sleeps (see KeDelayExecutionThread).
    void ReadSleepMode()
    {
        static const std::string path = []
        {
            const char* save = getenv("XPORT_SAVE_DIR");
            return std::string(save ? save : ".") + "/../sleep_mode";
        }();
        bool alwaysYield = false;
        if (FILE* file = fopen(path.c_str(), "r"))
        {
            alwaysYield = fgetc(file) == 'B';
            fclose(file);
        }
        if (g_sleepZeroAlwaysYields.exchange(alwaysYield) != alwaysYield)
            LOGFN("sleep: {}", alwaysYield ? "Sleep(0) always yields" : "back-to-back Sleep(0) calls sleep 200 us");
    }

    // Test switch next to affinity_mode: the first character of files/worker_mode places the game's
    // task and job workers. A = every big core at nice 5 (default); B = as before 2026-10-26, like
    // any unnamed thread (big cores only while busy). Also measured on 2026-10-26, same session:
    // nice 0 instead of 5 made no difference, and the app may not raise uclamp.min (EPERM).
    void ReadWorkerMode()
    {
        static const std::string path = []
        {
            const char* save = getenv("XPORT_SAVE_DIR");
            return std::string(save ? save : ".") + "/../worker_mode";
        }();
        char mode = 'A';
        if (FILE* file = fopen(path.c_str(), "r"))
        {
            if (fgetc(file) == 'B')
                mode = 'B';
            fclose(file);
        }
        if (mode != g_workerMode)
            LOGFN("affinity: workers mode {}: {}", mode, mode == 'B' ? "placed as unnamed threads" : "every big core at low priority");
        g_workerMode = mode;
    }

    // Test switch next to affinity_mode: files/recorder_mode starting with 'B' places XpRecorder as
    // before 2026-10-26 (a big-core thread at nice -6 like the others on its cores).
    void ReadRecorderMode()
    {
        static const std::string path = []
        {
            const char* save = getenv("XPORT_SAVE_DIR");
            return std::string(save ? save : ".") + "/../recorder_mode";
        }();
        char mode = 'A';
        if (FILE* file = fopen(path.c_str(), "r"))
        {
            if (fgetc(file) == 'B')
                mode = 'B';
            fclose(file);
        }
        if (mode != g_recorderMode)
            LOGFN("affinity: recorder mode {}: {}", mode, mode == 'B' ? "nice -6 like the other big-core threads" : "nice -10 above the other big-core threads");
        g_recorderMode = mode;
    }

    // Test switch next to recorder_mode: the first character of files/layout_mode. 'A' (default) =
    // the placement before 2026-10-27; 'C' = XpRenderWorker beside xport-main on the fastest core
    // and xport-gpu off the big cores; 'D' = XpRecorder alone on the fastest core and xport-main on
    // the big cores. Same session on the phone, hot (thermal forecast ~1.0, big cores held at
    // 1.9 GHz), 2026-10-27: C 25.2 fps with 11.4% of frames over 50 ms, A 29.2 fps with 1.9%. The
    // thermal limit takes the fastest core lower than the others (0.9-1.2 GHz against 1.9), and C
    // had stacked the game's main thread and the render worker on it.
    void ReadLayoutMode()
    {
        static const std::string path = []
        {
            const char* save = getenv("XPORT_SAVE_DIR");
            return std::string(save ? save : ".") + "/../layout_mode";
        }();
        char mode = 'A';
        if (FILE* file = fopen(path.c_str(), "r"))
        {
            const int c = fgetc(file);
            if (c == 'C' || c == 'D')
                mode = char(c);
            fclose(file);
        }
        if (g_layoutMode.exchange(mode, std::memory_order_relaxed) != mode)
        {
            LOGFN("affinity: layout mode {}: {}", mode,
                mode == 'A' ? "as before 2026-10-27"
                : mode == 'D' ? "recorder alone on the fastest core, main thread on the big cores"
                : "render worker beside the main thread on the fastest core");
        }
    }

    // A role the sweep re-applies every second whatever the thread's load.
    bool IsFixedRole(Role role)
    {
        return role == Role::Render || role == Role::Worker || role == Role::Recorder || role == Role::Audio ||
            role == Role::PrimeShared || role == Role::RecorderPrime;
    }

    // Test switch next to affinity_mode: files/adpf_mode starting with 'A' opens the performance
    // hint session (perf_hint.h), 'B' closes it; without the file XPORT_ADPF=1 decides.
    void ReadAdpfMode()
    {
        static const std::string path = []
        {
            const char* save = getenv("XPORT_SAVE_DIR");
            return std::string(save ? save : ".") + "/../adpf_mode";
        }();
        bool enabled = PerfHintDefaultEnabled();
        if (FILE* file = fopen(path.c_str(), "r"))
        {
            const int c = fgetc(file);
            if (c == 'A' || c == 'B')
                enabled = c == 'A';
            fclose(file);
        }
        PerfHintSetEnabled(enabled);
    }

    // The threads each frame goes through: the game's main and render threads, the task and job
    // workers the main thread waits for, and the renderer's assembler and recorder.
    bool InFrameChain(const std::string& name)
    {
        return name == "xport-main" || name == "XpRenderThread" || name == "XpTaskWorker" ||
            name == "XpJobWorker" || name == "XpRenderWorker";
    }

    struct Usage
    {
        uint64_t ticks = 0;
        bool busy = false;
        int quiet = 0; // sweeps in a row out of the candidates while busy
    };

    void DiscoverThreads()
    {
        DIR* dir = opendir("/proc/self/task");
        if (!dir)
            return;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr)
        {
            if (entry->d_name[0] == '.')
                continue;
            const int tid = atoi(entry->d_name);
            if (tid <= 0)
                continue;
            char path[64];
            snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
            char nameBuf[64] = {};
            if (FILE* f = fopen(path, "r"))
            {
                if (fgets(nameBuf, sizeof(nameBuf), f))
                {
                    size_t len = strlen(nameBuf);
                    while (len > 0 && (nameBuf[len - 1] == '\n' || nameBuf[len - 1] == '\r'))
                        nameBuf[--len] = '\0';
                }
                fclose(f);
            }
            const std::string name = nameBuf[0] ? nameBuf : "thread";

            HostThread* existing = nullptr;
            for (auto& [id, thread] : g_threads)
            {
                if (thread.tid == tid)
                {
                    existing = &thread;
                    break;
                }
            }
            if (!existing)
            {
                const uint32_t pseudoId = 0x80000000u | uint32_t(tid);
                HostThread& thread = g_threads[pseudoId];
                thread.tid = tid;
                thread.name = name;
                thread.role = RoleForName(thread.name, thread.tid);
                // Threads inherit their creator's name: only the thread NameCurrentThread named
                // is the game's main thread.
                if (thread.role == Role::Main)
                    thread.role = Role::Other;
                Apply(thread.tid, thread.role);
            }
            else if (existing->name == "xport-guest" || existing->name == "thread" || existing->name.rfind("com.xport", 0) == 0 ||
                (existing->name == "xport-main" && existing->role != Role::Main))
            {
                if (name != "xport-guest" && name != "thread" && name.rfind("com.xport", 0) != 0)
                {
                    existing->name = name;
                    existing->role = RoleForName(existing->name, existing->tid);
                    if (existing->role == Role::Main)
                        existing->role = Role::Other;
                    Apply(existing->tid, existing->role);
                }
            }
        }
        closedir(dir);
    }

    // Every 1 second: discover threads, measure CPU usage, pick busy threads, and re-apply masks.
    void Sweep(double elapsedSeconds)
    {
        static std::unordered_map<int, Usage> s_usage;
        static const double s_ticksPerSecond = double(sysconf(_SC_CLK_TCK));

        std::lock_guard guard{ g_mutex };
        DiscoverThreads();
        ReadDispatcherMode();
        ReadSleepMode();
        ReadWorkerMode();
        ReadRecorderMode();
        ReadLayoutMode();
        const char mode = ReadAffinityMode();
        static char s_lastMode = 0;
        if (mode != s_lastMode)
        {
            LOGFN("affinity: mode {}", mode);
            s_lastMode = mode;
        }

        std::unordered_map<int, HostThread*> byTid; // ids and handles share host threads
        for (auto& [id, thread] : g_threads)
            byTid[thread.tid] = &thread;

        ReadAdpfMode();
        std::vector<int32_t> frameChain;
        for (const auto& [tid, thread] : byTid)
        {
            if (InFrameChain(thread->name) || thread->role == Role::Main || thread->role == Role::Recorder ||
                thread->role == Role::RecorderPrime)
                frameChain.push_back(tid);
        }
        PerfHintSetThreads(std::move(frameChain));

        std::vector<std::pair<double, int>> candidates; // share, tid
        for (auto& [tid, thread] : byTid)
        {
            uint64_t ticks;
            int cpu;
            if (!ReadTaskStat(tid, ticks, cpu))
                continue;
            Usage& usage = s_usage[tid];
            const bool first = usage.ticks == 0;
            const double share = first ? 0.0 : double(ticks - usage.ticks) / (elapsedSeconds * s_ticksPerSecond);
            usage.ticks = ticks;
            const Role fixedRole = RoleForName(thread->name, tid);
            if (thread->role != Role::Main && fixedRole != Role::Little && !IsFixedRole(fixedRole))
            {
                // Threads already on the big cores rank higher, so a burst elsewhere (loading)
                // does not swap them out and back every sweep.
                if (share >= (usage.busy ? DEMOTE_SHARE : PROMOTE_SHARE))
                    candidates.emplace_back(usage.busy ? share + INCUMBENT_BONUS : share, tid);
            }
        }

        std::sort(candidates.begin(), candidates.end(), std::greater<>());
        if (candidates.size() > MAX_BUSY)
            candidates.resize(MAX_BUSY);

        for (auto& [tid, thread] : byTid)
        {
            if (thread->role == Role::Main)
                continue;
            if (const Role fixedRole = RoleForName(thread->name, tid); IsFixedRole(fixedRole))
            {
                thread->role = fixedRole;
                // The fastest-core roles get their mask from PlacementThread every 100 ms (with
                // the main thread's core_ctl fallback); here only their priority.
                if (fixedRole == Role::PrimeShared || fixedRole == Role::RecorderPrime)
                    ApplyPriority(tid, fixedRole);
                else
                    Apply(tid, thread->role);
                continue;
            }
            Usage& usage = s_usage[tid];
            const auto found = std::find_if(candidates.begin(), candidates.end(), [tid](const auto& c) { return c.second == tid; });
            bool busy = found != candidates.end();
            if (busy)
                usage.quiet = 0;
            else if (usage.busy && ++usage.quiet < QUIET_SWEEPS)
                busy = true;
            const Role nameRole = RoleForName(thread->name, tid);
            if (busy != usage.busy)
            {
                // Threads big by name stay on the big cores either way.
                if (nameRole != Role::Big)
                    LOGFN("affinity: thread {} '{}' {}", tid, thread->name, busy ? "busy -> big cores" : "quiet -> any/little core");
                usage.busy = busy;
            }
            // If explicitly Little (audio, vblank, background workers), keep it Little.
            // Explicitly Big threads (renderer, sound) and, in mode A, busy unnamed threads share
            // the remaining big cores: that oversubscribes them (~270% of work on two cores), but the
            // game's main thread waits for those workers and on the little cores they are slower.
            if (nameRole == Role::Little)
                thread->role = Role::Little;
            else if (nameRole == Role::Big || (busy && mode == 'A'))
                thread->role = Role::Big;
            else
                thread->role = Role::Other;

            Apply(tid, thread->role);
            if (busy && mode == 'C' && nameRole == Role::Other && GetLayout().enabled)
            {
                sched_setaffinity(tid, sizeof(cpu_set_t), &GetLayout().big);
                setpriority(PRIO_PROCESS, tid, 0);
            }
        }
    }

    void PlacementThread()
    {
        SetOsName(gettid(), "xport-affinity");
        using clock = std::chrono::steady_clock;
        auto lastSweep = clock::now();
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            int mainTid = 0;
            int recorderTid = 0;
            std::vector<int> primeShared;
            {
                std::lock_guard guard{ g_mutex };
                for (const auto& [id, thread] : g_threads)
                {
                    if (thread.role == Role::Main)
                        mainTid = thread.tid;
                    else if (thread.role == Role::PrimeShared)
                        primeShared.push_back(thread.tid);
                    else if (thread.role == Role::RecorderPrime)
                        recorderTid = thread.tid;
                }
            }
            // Layout mode D: the recorder takes the fastest core (and its core_ctl fallback), the
            // main thread the big cores beside the render core.
            const bool recorderPrime = g_layoutMode.load(std::memory_order_relaxed) == 'D' && recorderTid != 0;
            if (const int primeTid = recorderPrime ? recorderTid : mainTid; primeTid != 0)
            {
                const cpu_set_t* mask = ApplyMain(primeTid);
                for (const int tid : primeShared)
                    sched_setaffinity(tid, sizeof(cpu_set_t), mask);
            }
            if (recorderPrime && mainTid != 0)
                sched_setaffinity(mainTid, sizeof(cpu_set_t), &GetLayout().big);

            const auto now = clock::now();
            if (now - lastSweep >= std::chrono::milliseconds(1000))
            {
                Sweep(std::chrono::duration<double>(now - lastSweep).count());
                lastSweep = now;
                PerfHintTick(); // outside g_mutex: it asks the thermal service every 10 s
            }
        }
    }

    void StartPlacementThread()
    {
        static std::once_flag s_once;
        std::call_once(s_once, []
        {
            if (GetLayout().enabled)
                std::thread(PlacementThread).detach();
        });
    }
#else
    void Apply(int, Role) {}
    void SetOsName(int, const std::string&) {}
    int CurrentTid() { return 0; }
    void StartPlacementThread() {}
#endif
}

void RegisterHostThread(uint32_t guestThreadId)
{
    StartPlacementThread();
    std::lock_guard guard{ g_mutex };
    const int tid = CurrentTid();
    HostThread& thread = g_threads[guestThreadId];
    thread.tid = tid;
    thread.name = "xport-guest";
    // A runtime name given earlier on this host thread (xport-main) carries over.
    auto runtime = g_threads.find(0x80000000u | uint32_t(tid));
    if (runtime != g_threads.end())
    {
        thread.name = runtime->second.name;
        thread.role = runtime->second.role;
        g_threads.erase(runtime);
    }
    auto pending = g_pendingNames.find(guestThreadId);
    if (pending != g_pendingNames.end())
    {
        thread.name = pending->second;
        thread.role = RoleForName(thread.name, thread.tid);
        g_pendingNames.erase(pending);
        LOGFN("thread {} '{}' -> {}", tid, thread.name, PlacementName(thread.role));
    }
    SetOsName(tid, thread.name);
    Apply(thread.tid, thread.role);
}

void NameGuestThreadAtStart(uint32_t guestThreadId, const char* name)
{
    {
        std::lock_guard guard{ g_mutex };
        auto it = g_threads.find(guestThreadId);
        if (it == g_threads.end() || it->second.tid == 0)
        {
            g_pendingNames[guestThreadId] = name;
            return;
        }
    }
    SetGuestThreadName(guestThreadId, name);
}

void RegisterHostThreadHandle(uint32_t handle)
{
    std::lock_guard guard{ g_mutex };
    const int tid = CurrentTid();
    for (auto& [id, thread] : g_threads)
    {
        if (thread.tid == tid)
        {
            g_threads[handle] = thread;
            return;
        }
    }
    g_threads[handle].tid = tid;
}

void RegisterHostThreadHandle(uint32_t handle, uint32_t guestThreadId)
{
    std::lock_guard guard{ g_mutex };
    auto it = g_threads.find(guestThreadId);
    if (it != g_threads.end())
    {
        g_threads[handle] = it->second;
        return;
    }
    for (auto& [id, thread] : g_threads)
    {
        if (id == guestThreadId)
        {
            g_threads[handle] = thread;
            return;
        }
    }
    // The new thread registers when it first runs. Recording the calling thread here (as before
    // 2026-10-26) gave the game's main thread the name of the PathServer thread it creates, and
    // with it a big-core role instead of the fastest core.
    g_handleIds[handle] = guestThreadId;
}

void SetGuestThreadName(uint32_t threadId, const char* name)
{
    std::lock_guard guard{ g_mutex };
    if (auto handle = g_handleIds.find(threadId); handle != g_handleIds.end())
    {
        const uint32_t id = handle->second;
        if (g_threads.find(id) == g_threads.end())
        {
            g_pendingNames[id] = name;
            return;
        }
        threadId = id;
    }

    HostThread* thread = nullptr;
    uint32_t pendingId = threadId; // the thread id a not-yet-started thread registers with
    if (threadId == 0xFFFFFFFF)
    {
        const int tid = CurrentTid();
        for (auto& [id, candidate] : g_threads)
        {
            if (candidate.tid == tid)
                thread = &candidate;
        }
    }
    else
    {
        auto it = g_threads.find(threadId);
        if (it != g_threads.end())
        {
            thread = &it->second;
        }
        else if (IsKernelObject(threadId))
        {
            auto* obj = GetKernelObject(threadId);
            if (IsRegisteredKernelObject(obj))
            {
                auto* hThread = static_cast<GuestThreadHandle*>(obj);
                const uint32_t realId = hThread->GetThreadId();
                pendingId = realId;
                auto it2 = g_threads.find(realId);
                if (it2 != g_threads.end())
                    thread = &it2->second;
            }
        }
    }

    if (thread == nullptr)
    {
        // A thread its creator names right after ExCreateThread (Halo 3: RENDER, AUDIO...) may not
        // have run yet: RegisterHostThread gives it the name when it starts. Android thread ids
        // (from pthread_self) often have the high bit that handles have, so ids are not told
        // from handles by that bit: a registered thread handle gave its thread id above.
        if (threadId != 0xFFFFFFFF)
        {
            g_pendingNames[pendingId] = name;
            LOGF("thread 0x{:X} named '{}' before it started", pendingId, name);
            return;
        }
        LOGF("unknown thread 0x{:X} named '{}'", threadId, name);
        return;
    }

    thread->name = name;
    if (thread->role != Role::Main)
        thread->role = RoleForName(thread->name, thread->tid);
    // The same host thread can be registered by id and by handle.
    for (auto& [id, other] : g_threads)
    {
        if (other.tid == thread->tid)
        {
            other.name = thread->name;
            other.role = thread->role;
        }
    }
    SetOsName(thread->tid, thread->name);
    Apply(thread->tid, thread->role);
    LOGF("thread {} '{}' -> {}", thread->tid, thread->name, PlacementName(thread->role));
}

void NameUnnamedCurrentThread(const char* name)
{
    thread_local bool done = false;
    if (done)
        return;
    done = true;
    {
        std::lock_guard guard{ g_mutex };
        const int tid = CurrentTid();
        for (auto& [id, thread] : g_threads)
        {
            if (thread.tid == tid && thread.name != "xport-guest" && thread.name != "thread")
                return;
        }
    }
    SetGuestThreadName(0xFFFFFFFF, name);
}

void NameCurrentThread(const char* name)
{
    StartPlacementThread();
    const int tid = CurrentTid();
    {
        std::lock_guard guard{ g_mutex };
        // Runtime threads have no guest id; key them by a value no guest id uses.
        HostThread& thread = g_threads[0x80000000u | uint32_t(tid)];
        thread.tid = tid;
        thread.name = name;
        thread.role = RoleForName(thread.name, tid);
        SetOsName(tid, thread.name);
        Apply(tid, thread.role);
    }
}

// From the renderer's frame recorder (graphics_system.cpp, FrameRecorderMain), on its own thread.
extern "C" void xport_thread_policy_set_recorder(int tid)
{
    g_recorderTid.store(tid, std::memory_order_relaxed);
}
