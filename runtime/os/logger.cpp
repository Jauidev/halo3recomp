#include <stdafx.h>
#include <deque>
#ifdef __ANDROID__
#include <android/log.h>
#endif

// minimal logger (stderr on the PC, logcat tag "xport" on Android).
namespace os::logger
{
    static Mutex g_logMutex;
    static std::deque<std::string> g_recentLines;
    constexpr size_t RECENT_LINES = 64;

    void Init()
    {
    }

    void Log(const std::string_view str, ELogType type, const char* func)
    {
        static constexpr const char* s_prefixes[] = { "", "[stub] ", "[warn] ", "[error] " };
        const char* prefix = s_prefixes[size_t(type)];
        std::lock_guard lock(g_logMutex);
#ifdef __ANDROID__
        static constexpr int s_priorities[] = { ANDROID_LOG_INFO, ANDROID_LOG_DEBUG, ANDROID_LOG_WARN, ANDROID_LOG_ERROR };
        __android_log_print(s_priorities[size_t(type)], "xport", "%s%s%s%.*s", prefix, func ? func : "", func ? ": " : "", int(str.size()), str.data());
#endif
        // Also on Android: the phase 2 build runs from adb shell, where stderr is the quickest view.
        fprintf(stderr, "%s%s%s%.*s\n", prefix, func ? func : "", func ? ": " : "", int(str.size()), str.data());

        g_recentLines.push_back(fmt::format("{}{}{}{}", prefix, func ? func : "", func ? ": " : "", str));
        if (g_recentLines.size() > RECENT_LINES)
            g_recentLines.pop_front();
    }

    std::vector<std::string> GetRecentLines(size_t count)
    {
        std::lock_guard lock(g_logMutex);
        count = std::min(count, g_recentLines.size());
        return { g_recentLines.end() - count, g_recentLines.end() };
    }
}
