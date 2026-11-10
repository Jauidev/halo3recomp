// swap read-ahead for the game's memory (see swap_in.h).
#include <stdafx.h>
#include <kernel/memory.h>
#include <os/logger.h>
#include "swap_in.h"
#include "thread_policy.h"

#ifdef __ANDROID__
#include <sys/resource.h>

namespace
{
    // madvise holds the address space lock for reading through each call; short calls keep the
    // runtime's mprotect (write watches), which needs it for writing, from waiting behind one.
    constexpr uintptr_t CHUNK = 2u << 20;

    struct Range
    {
        uintptr_t begin;
        uintptr_t end;
    };

    // The native heap (scudo's regions) from /proc/self/maps. The recompiled code is left out: it
    // comes back from the file without swap, and reading all of it ahead would push other pages out.
    std::vector<Range> HeapRanges()
    {
        std::vector<Range> ranges;
        FILE* maps = fopen("/proc/self/maps", "r");
        if (!maps)
            return ranges;
        char line[512];
        while (fgets(line, sizeof(line), maps))
        {
            unsigned long begin, end;
            if (strstr(line, "[anon:scudo:") && sscanf(line, "%lx-%lx", &begin, &end) == 2)
                ranges.push_back({ begin, end });
        }
        fclose(maps);
        return ranges;
    }

    bool Enabled()
    {
        static const std::string path = []
        {
            const char* save = getenv("XPORT_SAVE_DIR");
            return std::string(save ? save : ".") + "/../swap_in";
        }();
        bool enabled = false;
        if (FILE* file = fopen(path.c_str(), "r"))
        {
            enabled = fgetc(file) == '1';
            fclose(file);
        }
        return enabled;
    }

    // Anonymous memory: reads the swapped-out pages into the swap cache. The physical memory views
    // (shared memory): the same for the pages of the memory object. Unmapped gaps are skipped.
    void WillNeed(uintptr_t begin, uintptr_t end)
    {
        for (uintptr_t at = begin; at < end; at += CHUNK)
            madvise(reinterpret_cast<void*>(at), std::min(CHUNK, end - at), MADV_WILLNEED);
    }

    long MajorFaults()
    {
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        return usage.ru_majflt;
    }

    void SwapInThread()
    {
        NameCurrentThread("XpSwapInWorker"); // "Worker": the little cores, nice 4
        std::vector<Range> heap;
        int lastEnabled = -1;
        double passMs = 0;
        uint32_t passes = 0;
        long faults = MajorFaults();
        for (uint32_t second = 0;; ++second)
        {
            const bool enabled = Enabled();
            if (int(enabled) != lastEnabled)
            {
                LOGFN("swap-in: read-ahead {}", enabled ? "on (files/swap_in = 1)" : "off");
                lastEnabled = enabled;
            }
            if (enabled)
            {
                if (second % 10 == 0)
                    heap = HeapRanges();
                const auto start = std::chrono::steady_clock::now();
                const uintptr_t guest = reinterpret_cast<uintptr_t>(g_memory.base);
                WillNeed(guest, guest + PPC_MEMORY_SIZE);
                for (const Range& range : heap)
                    WillNeed(range.begin, range.end);
                passMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                ++passes;
            }
            if (second % 10 == 9)
            {
                const long now = MajorFaults();
                LOGFN("swap-in: {} major faults in 10 s, read-ahead {:.1f} ms per pass ({} passes)",
                    now - faults, passes ? passMs / passes : 0.0, passes);
                faults = now;
                passMs = 0;
                passes = 0;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

void StartSwapInThread()
{
    if (g_memory.base)
        std::thread(SwapInThread).detach();
}
#else
void StartSwapInThread() {}
#endif
