#include <stdafx.h>
#include <cpu/ppc_context.h>
#include <cpu/guest_thread.h>
#include <kernel/memory.h>
#include "thread_dump.h"
#include <cstdarg>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif
#ifdef __ANDROID__
#include <android/log.h>
#endif
#ifndef _WIN32
#include <fcntl.h>
#endif

static std::mutex g_contextsMutex;
static std::vector<std::pair<PPCContext*, uint32_t>> g_contexts;
static std::unordered_map<PPCContext*, int> g_hostTids;

void RegisterGuestContext(PPCContext* ctx)
{
    std::lock_guard guard{ g_contextsMutex };
    g_contexts.emplace_back(ctx, GuestThread::GetCurrentThreadId());
#ifdef __ANDROID__
    g_hostTids[ctx] = gettid();
#endif
}

void UnregisterGuestContext(PPCContext* ctx)
{
    std::lock_guard guard{ g_contextsMutex };
    std::erase_if(g_contexts, [ctx](const auto& entry) { return entry.first == ctx; });
}

size_t GetGuestThreadCount()
{
    std::lock_guard guard{ g_contextsMutex };
    return g_contexts.size();
}

void PrintGuestBacktrace(const PPCContext* ctx)
{
    // Follow the r1 back chain; non-leaf functions save LR at -8 of the caller's frame. Keeps the
    // first 16 and the last 16 (a ring), so runaway recursion still shows its origin.
    uint32_t first[16], last[16];
    uint32_t count = 0;
    uint32_t sp = ctx->r1.u32;
    while (count < 1000000 && sp != 0 && sp < PPC_MEMORY_SIZE - 8)
    {
        const uint32_t next = ByteSwap(*reinterpret_cast<uint32_t*>(g_memory.base + sp));
        if (next <= sp || next - sp > 0x100000)
            break;
        const uint32_t lr = ByteSwap(*reinterpret_cast<uint32_t*>(g_memory.base + next - 8));
        if (count < 16)
            first[count] = lr;
        last[count % 16] = lr;
        count++;
        sp = next;
    }
    std::string line = fmt::format("  guest backtrace ({} frames):", count);
    for (uint32_t i = 0; i < std::min(count, 16u); i++)
        line += fmt::format(" {:08X}", first[i]);
    if (count > 16)
    {
        line += " ...";
        for (uint32_t i = std::max(count - 16, 16u); i < count; i++)
            line += fmt::format(" {:08X}", last[i % 16]);
    }
    DiagPrint("%s\n", line.c_str());
}

// Fixed storage: the crash handler must not allocate (the crash may be inside malloc).
static char g_reportPath[1024];
#ifdef _WIN32
static FILE* g_reportFile;
#else
static int g_reportFd = -1;
#endif

void DiagSetReportPath(const std::string& path)
{
    snprintf(g_reportPath, sizeof(g_reportPath), "%s", path.c_str());
}

void DiagOpenReportFile()
{
    if (g_reportPath[0] == '\0')
        return;
#ifdef _WIN32
    if (g_reportFile == nullptr)
        g_reportFile = fopen(g_reportPath, "w");
#else
    if (g_reportFd < 0)
        g_reportFd = open(g_reportPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
#endif
}

void DiagPrint(const char* format, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    fputs(buffer, stderr);
#ifdef _WIN32
    if (g_reportFile != nullptr)
    {
        fputs(buffer, g_reportFile);
        fflush(g_reportFile);
    }
#else
    if (g_reportFd >= 0)
        write(g_reportFd, buffer, strlen(buffer));
#endif
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_ERROR, "xport", buffer);
#endif
}

// XPORT_DUMP_MEM="addr,*addr,..." (hex): prints 8 words at each address; with '*' the word at the
// address is followed as a guest pointer first. Read while the game runs, like the registers.
static void DumpGuestMemory()
{
    const char* list = getenv("XPORT_DUMP_MEM");
    if (list == nullptr)
        return;

    auto readWord = [](uint32_t address) { return ByteSwap(*reinterpret_cast<uint32_t*>(g_memory.base + address)); };
    for (const char* item = list; *item != '\0';)
    {
        const bool follow = *item == '*';
        if (follow)
            item++;
        char* end;
        uint32_t address = uint32_t(strtoul(item, &end, 16));
        if (end == item)
            break;
        // "*addr+off": the offset is added to the followed pointer (fields of a heap object).
        uint32_t offset = 0;
        if (*end == '+')
        {
            const char* start = end + 1;
            offset = uint32_t(strtoul(start, &end, 16));
        }
        item = *end == ',' ? end + 1 : end;

        std::string line = fmt::format("mem {}{:08X}", follow ? "*" : "", address);
        if (offset != 0)
            line += fmt::format("+{:X}", offset);
        if (follow)
        {
            address = readWord(address & ~3u);
            line += fmt::format(" -> {:08X}", address);
            if (address == 0)
            {
                DiagPrint("%s (skipped)\n", line.c_str());
                continue;
            }
        }
        address += offset;
        // The low 64 KB of guest memory is no-access (null pointers fault): a followed null or a
        // small value would crash the dump thread.
        if (address < 0x10000 || uint64_t(address) + 32 > PPC_MEMORY_SIZE)
        {
            DiagPrint("%s (skipped)\n", line.c_str());
            continue;
        }
        address &= ~3u;
        line += ":";
        for (uint32_t i = 0; i < 8 && uint64_t(address) + i * 4 + 4 <= PPC_MEMORY_SIZE; i++)
            line += fmt::format(" {:08X}", readWord(address + i * 4));
        DiagPrint("%s\n", line.c_str());
    }
}

// Racy by design: the threads keep running while their contexts are read.
static void DumpGuestThreads(int elapsed)
{
    std::lock_guard guard{ g_contextsMutex };
    DiagPrint("\n*** thread dump after %d s: %zu guest threads\n", elapsed, g_contexts.size());
    for (const auto& [ctx, threadId] : g_contexts)
    {
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
        DiagPrint("thread %08X (tid %d): lr=0x%08X ctr=0x%08X r1=0x%08X r3=0x%08X r4=0x%08X\n",
            threadId, g_hostTids.count(ctx) ? g_hostTids[ctx] : 0, lr, ctr, ctx->r1.u32, ctx->r3.u32, ctx->r4.u32);
        DiagPrint("  r5-r12 %08X %08X %08X %08X %08X %08X %08X %08X  r28-r31 %08X %08X %08X %08X\n",
            ctx->r5.u32, ctx->r6.u32, ctx->r7.u32, ctx->r8.u32, ctx->r9.u32, ctx->r10.u32, r11, r12,
#ifndef PPC_CONFIG_NON_VOLATILE_AS_LOCAL
            ctx->r28.u32, ctx->r29.u32, ctx->r30.u32, ctx->r31.u32
#else
            0, 0, 0, 0
#endif
        );
        PrintGuestBacktrace(ctx);
    }
    DumpGuestMemory();
    fflush(stderr);
}

// XPORT_WATCH=<hex guest address> (debugging): a thread spins on the word there and, at each change
// (the first 40), prints old -> new and every guest thread's lr and back chain. The writer is the
// thread whose lr points just after its last call in the function that stored the value.
static void StartGuestWordWatch()
{
    const char* value = getenv("XPORT_WATCH");
    if (value == nullptr)
        return;
    const uint32_t address = uint32_t(strtoul(value, nullptr, 16)) & ~3u;
    if (address < 0x10000 || address >= PPC_MEMORY_SIZE - 4)
        return;
    // XPORT_WATCH_AFTER=<seconds>: start later, once the game has committed the memory.
    const char* after = getenv("XPORT_WATCH_AFTER");
    const int delay = after ? atoi(after) : 20;
    std::thread([address, delay]
    {
        std::this_thread::sleep_for(std::chrono::seconds(delay));
        auto word = [address] { return ByteSwap(*reinterpret_cast<volatile uint32_t*>(g_memory.base + address)); };
        uint32_t last = word();
        DiagPrint("*** watch %08X: %08X\n", address, last);
        for (int events = 0; events < 40;)
        {
            const uint32_t now = word();
            if (now == last)
            {
#if defined(_M_X64) || defined(__x86_64__)
                _mm_pause();
#endif
                continue;
            }
            events++;
            std::lock_guard guard{ g_contextsMutex };
            DiagPrint("*** watch %08X: %08X -> %08X\n", address, last, now);
            for (const auto& [ctx, threadId] : g_contexts)
            {
#ifndef PPC_CONFIG_SKIP_LR
                const uint32_t lr = uint32_t(ctx->lr);
#else
                const uint32_t lr = 0;
#endif
                DiagPrint("  thread %08X lr=0x%08X r1=0x%08X\n", threadId, lr, ctx->r1.u32);
                PrintGuestBacktrace(ctx);
            }
            last = now;
        }
    }).detach();
}

void StartThreadDumpWatchdog()
{
    StartGuestWordWatch();
    const char* value = getenv("XPORT_DUMP_AFTER");
    if (value == nullptr || atoi(value) <= 0)
        return;

    // XPORT_DUMP_EVERY=<seconds>: dump again at that interval (tells spinning threads from blocked ones).
    const char* every = getenv("XPORT_DUMP_EVERY");
    const int interval = every ? atoi(every) : 0;
    const int seconds = atoi(value);
    std::thread([seconds, interval]
    {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        for (int elapsed = seconds;; elapsed += interval)
        {
            DumpGuestThreads(elapsed);
            if (interval <= 0)
                break;
            std::this_thread::sleep_for(std::chrono::seconds(interval));
        }
    }).detach();
}
