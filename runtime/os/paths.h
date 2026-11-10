#pragma once
// where the extracted disc and the saves live on the host.
namespace os::paths
{
    // XPORT_GAME_DIR, or "game" next to the working directory.
    const std::filesystem::path& GetGamePath();
    // XPORT_SAVE_DIR, or "save" next to the working directory.
    const std::filesystem::path& GetSavePath();
}

namespace os
{
    // Xbox language id: 1 English, 3 German, 4 French, 5 Spanish (default), 6 Italian. Env XPORT_LANGUAGE.
    uint32_t GetLanguage();
}
