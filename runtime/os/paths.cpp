#include <stdafx.h>
#include "paths.h"

namespace os::paths
{
    static std::filesystem::path FromEnv(const char* name, const char* fallback)
    {
        const char* value = getenv(name);
        return std::filesystem::absolute(value && *value ? value : fallback);
    }

    const std::filesystem::path& GetGamePath()
    {
        static const std::filesystem::path s_path = FromEnv("XPORT_GAME_DIR", "game");
        return s_path;
    }

    const std::filesystem::path& GetSavePath()
    {
        static const std::filesystem::path s_path = FromEnv("XPORT_SAVE_DIR", "save");
        return s_path;
    }
}

namespace os
{
    uint32_t GetLanguage()
    {
        static const uint32_t s_language = []
        {
            const char* value = getenv("XPORT_LANGUAGE");
            return value && *value ? uint32_t(atoi(value)) : 5u;
        }();
        return s_language;
    }
}
