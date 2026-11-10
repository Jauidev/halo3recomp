// the launcher's screen (see launcher.h), drawn with Dear ImGui by launcher.cpp. A floating
// grid of liquid-glass tiles over the GPU's background (Mali blue, Adreno red): Jugar and six pages
// (Juego, Partidas, GPU y drivers, Controles, Sistema, Créditos), plus the last crash report.
// One game per APK: the one this library was recompiled from (runtime/game.h, games/<title>/).
// Every tile and card keeps clear of the screen's edges and the camera hole; a tap counts only when
// the finger did not scroll, and the pad moves between tiles (ImGui's gamepad navigation). The
// choices are kept in files/launcher.cfg; Jugar turns them into the game's files/ switches and
// options (Play). The text is written in Spanish, the user's language; Tr shows it in the language
// chosen in Sistema (the phone's by default), from launcher_text.inc, and the game gets the same.
#if defined(__ANDROID__) && defined(XPORT_SHARED)
#include <stdafx.h>
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/api-level.h>
#endif
#include <ctime>
#include <deque>
#include <unordered_map>
#include "launcher.h"
#include "gpu_probe.h"
#include <game.h>

namespace launcher
{
namespace
{
    namespace fs = std::filesystem;

    enum Font { kSmall, kBody, kTitle, kHuge, kFontCount };
    constexpr float kFontDp[kFontCount] = {14, 18, 23, 38};

    enum class Page { Home, Saves, Gpu, Controls, System, Credits, Crash, Games };
    enum class Icon
    {
        Play, Graphics, Saves, Gpu, Controls, System, Credits, Back, Chevron, Check, Cross, Lock, Pencil, Bell,
        Warning, Info, Bolt, Gauge, Dot, Sparkle, Console, Trash, Restore, Import, Copy
    };

    using Options = std::vector<const char*>;

    // The launcher's languages and the game's: the phone's language code (Platform::locale) and the
    // Xbox language id for XPORT_LANGUAGE (os/paths.cpp). The order is launcher_text.inc's columns.
    struct Language
    {
        const char* label;
        const char* code;
        int id;
    };
    const Language kLanguages[] = {
        {"Español", "es", 5}, {"English", "en", 1}, {"Français", "fr", 4}, {"Deutsch", "de", 3}, {"Italiano", "it", 6}};
    constexpr int kEnglish = 1;

#include "launcher_text.inc"

    // The Vulkan renderer's requirements (gpu_probe.cpp): label and the text the probe's
    // "missing" list names it by.
    const char* const kRequirements[][2] = {
        {"Vulkan 1.2", "Vulkan 1.2"},
        {"depthClamp", "depthClamp"},
        {"sampleRateShading", "sampleRateShading"},
        {"independentBlend", "independentBlend"},
        {"Descriptor indexing", "descriptor indexing"},
        {"Buffer device address", "bufferDeviceAddress"},
        {"Dynamic rendering", "dynamic rendering"},
    };

    // Everything libgame.so links (build.ninja) and the APK carries. The renderer is LibertyRecomp's
    // (GPL-3, third_party/rexglue/COPYING-LibertyRecomp): credited in every game. GTA IV's Android
    // reference only in its builds (XPORT_TITLE_ID 0x545407F2).
    const char* const kCredits[][2] = {
        {"Liberty Recompiled", "Renderer nativo xnative · GPL-3"},
#if XPORT_TITLE_ID == 0x545407F2
        {"LibertyRecompAndroid", "Referencia · vaduur"},
#endif
        {"ReXGlue SDK", "Runtime de recompilación"},
        {"XenonRecomp", "PowerPC a C++ · hedge-dev"},
        {"XenosRecomp", "Shaders de Xenos · hedge-dev"},
        {"Unleashed Recompiled", "Kernel y XAM · hedge-dev"},
        {"Xenia Canary", "GPU: Vulkan, EDRAM y shaders"},
        {"DirectX Shader Compiler", "Shaders · Microsoft"},
        {"glslang", "Compilador GLSL · Khronos"},
        {"smol-v", "Compresión SPIR-V · Aras Pranckevicius"},
        {"volk", "Carga de Vulkan · Arseny Kapoulkine"},
        {"SMAA", "Antialiasing · Jorge Jimenez"},
        {"Mesa Turnip", "Driver Adreno · builds de KIMCHI y StevenMXZ"},
        {"libadrenotools", "Carga de drivers · bylaws"},
        {"libGPUCounters", "Contadores de GPU · Arm"},
        {"FFmpeg", "Audio XMA · FFmpeg"},
        {"libmspack", "Descompresión · Stuart Caie"},
        {"Zstandard", "Compresión · Meta"},
        {"Snappy", "Compresión · Google"},
        {"xxHash", "Hash rápido · Yann Collet"},
        {"unordered_dense", "Tablas hash · martinus"},
        {"O(1) Heap", "Memoria · Pavel Kirienko"},
        {"spdlog", "Registro · Gabi Melman"},
        {"fmt", "Formato de texto · Victor Zverovich"},
        {"Dear ImGui", "Interfaz del launcher · Omar Cornut"},
        {"stb_image", "Imágenes · Sean Barrett"},
        {"Khronos · LunarG", "Validation layers y GFXReconstruct"},
    };

    constexpr const char* kTurnipName = "Turnip 27.0.0-R2";
    constexpr int kAutoBackupsKept = 10;

    struct Config
    {
        std::string driver = "auto"; // auto, system, turnip or an imported driver's folder
        int autoBackup = 1;          // Off, Al jugar, Diaria
        std::string lastBackupDay;
        int touch = 0;               // Auto (hidden with a pad), Siempre, Nunca
        int opacity = 2;             // 25, 50, 75, 100 %
        int size = 1;                // Pequeño, Normal, Grande
        int vibration = 0;           // On, Off
        int bgPause = 0;             // On, Off
        int fps = 1;                 // Off, Simple (the rate), Detallado (frame times, CPU, memory, battery)
        int language = 0;            // Automático (the phone's), then kLanguages
        bool welcomeShown = false;   // Importa tu ISO was shown (it shows once)
#if XPORT_TITLE_ID == 0x4D5308AB
        // Gears of War 3's Gráficos (games/gears_of_war_3/game.cpp applies them).
        int shadows = 0;             // On, Off: the real-time shadows
        int effects = 0;             // On, Off: the image effects and the particles' full detail
#endif
    };

    struct Notice
    {
        int level; // 0 info, 1 warning, 2 error
        std::string text;
        Page page;
        bool blocks; // Jugar cannot start
    };

    struct ToastItem
    {
        std::string text;
        int level;
        float start;
    };

    struct Save
    {
        std::string folder, episode, mission, date;
        uint64_t bytes = 0;
    };

    struct Backup
    {
        std::string folder, date, kind;
        int saves = 0;
        uint64_t bytes = 0;
    };

    struct Driver
    {
        std::string id, name, dir, library;
        bool usable = true;
        bool bundled = false; // came in the APK (MainActivity.installBundledDrivers): not deletable
        std::string problem;
    };

    // Comprobar (Sistema): walks the game folder on its own thread.
    struct GameCheck
    {
        std::atomic<bool> running{false};
        std::mutex mutex;
        std::vector<std::string> lines;
    };

    struct Scroller
    {
        bool armed = false, dragging = false, stopped = false;
        float velocity = 0;
        float target = -1, targetStart = 0; // a smooth scroll to this offset (-1: none)
    };

    struct State
    {
        Config config;
        int ui = 0; // the language shown and passed to the game (kLanguages)
        Page page = Page::Home;
        float pageStart = -1, clock = 0, dt = 1 / 60.f, fade = 1;
        float dp = 2.75f;
        float insets[4] = {};
        bool touch = true, adreno = false, playing = false;
        Texture background, blurred;
        ImVec2 coverOrigin, coverSize{1, 1};
        ImFont* fonts[kFontCount] = {};
        std::vector<uint8_t> fontData;
        ImDrawList* drawList = nullptr; // set while drawing outside a window (toasts)
        const GpuProbe* gpu = nullptr;
        bool turnipPresent = false;
        int game = 0; // 0 missing, 1 in game/
        bool xexOk = false; // default.xex is the build this library was recompiled from (XexMatches)
        const char* isoError = nullptr; // Importar ISO refused the file: the overlay's sentence
        float isoErrorStart = 0;
        std::string xex;    // what its header says (the log)
        bool importing = false; // Importar ISO is unpacking a disc (launcher_iso.cpp)
        std::vector<Save> saves;
        std::vector<Backup> backups;
        std::vector<Driver> drivers;
        std::vector<Notice> notices;
        bool noticesOpen = false;
        bool welcome = false; // the launcher opened without the game: Importa tu ISO over the home
        std::deque<ToastItem> toasts;
        std::string gamepad;
        std::string confirm;
        float confirmTime = 0;
        float backTime = -10;
        Scroller scroll;
        float viewHeight = 0;
        bool resetScroll = false;
        std::string crash, crashDate;
        GameCheck check;
    };
    State g;

    // ---------- language ----------

    // The text in the language shown: a Spanish text launcher_text.inc has, translated; any other
    // (names, numbers, text already translated) as it is.
    std::string_view Tr(std::string_view text)
    {
        if (g.ui <= 0 || g.ui >= int(std::size(kLanguages)) || text.size() > 512)
            return text;
        static const auto table = []
        {
            std::unordered_map<std::string_view, const char* const*> map;
            for (const auto& row : kTranslations)
                map.emplace(row[0], row);
            return map;
        }();
        const auto it = table.find(text);
        return it == table.end() ? text : std::string_view(it->second[g.ui]);
    }

    // A Spanish format string translated, then formatted.
    template <typename... Args>
    std::string Trf(std::string_view format, const Args&... args)
    {
        return fmt::vformat(Tr(format), fmt::make_format_args(args...));
    }

    // A number with the language's decimal mark (a dot in English, a comma in the others).
    std::string Decimal(double value, int digits)
    {
        std::string text = fmt::format("{:.{}f}", value, digits);
        if (g.ui != kEnglish)
            std::replace(text.begin(), text.end(), '.', ',');
        return text;
    }

    // Automático takes the phone's language, English when the game has no text in it.
    int LanguageIndex(int choice)
    {
        if (choice > 0 && choice <= int(std::size(kLanguages)))
            return choice - 1;
        const std::string& locale = GetPlatform().locale;
        for (int i = 0; i < int(std::size(kLanguages)); ++i)
        {
            if (locale.rfind(kLanguages[i].code, 0) == 0)
                return i;
        }
        return kEnglish;
    }

    // ---------- files ----------

    // One game per APK, in the app's external files folder.
    constexpr int kThisGame = 1;

    fs::path External() { return GetPlatform().externalDir; }
    fs::path SaveDir() { return External() / "save"; }
    fs::path BackupDir() { return External() / "backups"; }
    fs::path GameDir() { return External() / "game"; }
    fs::path DriverDir() { return fs::path(GetPlatform().internalDir) / "drivers"; }

    std::string ReadText(const fs::path& path, size_t limit = 1 << 20)
    {
        std::string text;
        if (FILE* file = fopen(path.c_str(), "rb"))
        {
            text.resize(limit);
            text.resize(fread(text.data(), 1, limit, file));
            fclose(file);
        }
        return text;
    }

    bool WriteText(const fs::path& path, const std::string& text)
    {
        FILE* file = fopen(path.c_str(), "wb");
        if (!file)
            return false;
        const bool ok = fwrite(text.data(), 1, text.size(), file) == text.size();
        return fclose(file) == 0 && ok;
    }

    uint64_t TreeSize(const fs::path& path)
    {
        std::error_code ec;
        if (fs::is_regular_file(path, ec))
            return fs::file_size(path, ec);
        uint64_t bytes = 0;
        for (auto it = fs::recursive_directory_iterator(path, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
        {
            if (it->is_regular_file(ec))
                bytes += it->file_size(ec);
        }
        return bytes;
    }

    std::string Size(uint64_t bytes)
    {
        if (bytes < 1024 * 1024)
            return fmt::format("{} KB", (bytes + 1023) / 1024);
        return Decimal(bytes / (1024.0 * 1024.0), 1) + " MB";
    }

    std::string Now(const char* format)
    {
        const time_t now = time(nullptr);
        tm local{};
        localtime_r(&now, &local);
        char text[64];
        strftime(text, sizeof(text), format, &local);
        return text;
    }

    // The value of "key": "..." in a meta.json (drivers); empty without one.
    std::string JsonString(const std::string& json, const char* key)
    {
        size_t at = json.find(std::string("\"") + key + "\"");
        if (at == std::string::npos)
            return {};
        at = json.find(':', at);
        at = at == std::string::npos ? at : json.find('"', at);
        if (at == std::string::npos)
            return {};
        std::string value;
        for (size_t i = at + 1; i < json.size() && json[i] != '"'; ++i)
            value += json[i] == '\\' && i + 1 < json.size() ? json[++i] : json[i];
        return value;
    }

    // A save is a folder in save/ and its <folder>.xcontent record; cache/ and hdd/ are not saves.
    bool IsSaveFolder(const fs::directory_entry& entry)
    {
        std::error_code ec;
        const std::string name = entry.path().filename().string();
        return entry.is_directory(ec) && !name.empty() && name[0] != '.' && name != "cache" && name != "hdd";
    }

    // The save's display name (XCONTENT_DATA::szDisplayName, UTF-16 big-endian at offset 8):
    // "<episode> - <mission> - MM/DD/YY HH:MM:SS" in some games, any name in the others.
    void ReadSaveName(Save& save, const fs::path& record)
    {
        const std::string data = ReadText(record, 8 + 256);
        std::string name;
        for (size_t i = 8; i + 1 < data.size(); i += 2)
        {
            const uint32_t c = uint32_t(uint8_t(data[i])) << 8 | uint8_t(data[i + 1]);
            if (c == 0)
                break;
            if (c < 0x80)
                name += char(c);
            else if (c < 0x800)
                name += {char(0xC0 | c >> 6), char(0x80 | (c & 0x3F))};
            else
                name += {char(0xE0 | c >> 12), char(0x80 | (c >> 6 & 0x3F)), char(0x80 | (c & 0x3F))};
        }
        const size_t first = name.find(" - "), last = name.rfind(" - ");
        if (first == std::string::npos || last == first)
        {
            save.mission = name.empty() ? save.folder : name;
            return;
        }
        save.episode = name.substr(0, first);
        save.mission = name.substr(first + 3, last - first - 3);
        int month, day, year, hour, minute;
        if (sscanf(name.c_str() + last + 3, "%d/%d/%d %d:%d", &month, &day, &year, &hour, &minute) == 5)
            save.date = fmt::format("{:02}/{:02}/{} {:02}:{:02}", day, month, 2000 + year % 100, hour, minute);
    }

    void ScanSaves()
    {
        g.saves.clear();
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(SaveDir(), ec))
        {
            if (!IsSaveFolder(entry))
                continue;
            Save save;
            save.folder = entry.path().filename().string();
            fs::path record = entry.path();
            record += ".xcontent";
            ReadSaveName(save, record);
            save.bytes = TreeSize(entry.path()) + TreeSize(record);
            g.saves.push_back(std::move(save));
        }
        std::sort(g.saves.begin(), g.saves.end(), [](const Save& a, const Save& b) { return a.folder < b.folder; });
    }

    void ScanBackups()
    {
        g.backups.clear();
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(BackupDir(), ec))
        {
            if (!entry.is_directory(ec))
                continue;
            Backup backup;
            backup.folder = entry.path().filename().string();
            int year, month, day, hour, minute, second;
            if (sscanf(backup.folder.c_str(), "%d-%d-%d_%d-%d-%d", &year, &month, &day, &hour, &minute, &second) != 6)
                continue;
            backup.date = fmt::format("{:02}/{:02}/{} {:02}:{:02}", day, month, year, hour, minute);
            backup.kind = backup.folder.ends_with("_auto") ? "Automática"
                : backup.folder.ends_with("_antes") ? "Antes de restaurar o borrar" : "Manual";
            for (const auto& item : fs::directory_iterator(entry.path(), ec))
                backup.saves += IsSaveFolder(item);
            backup.bytes = TreeSize(entry.path());
            g.backups.push_back(std::move(backup));
        }
        std::sort(g.backups.begin(), g.backups.end(), [](const Backup& a, const Backup& b) { return a.folder > b.folder; });
    }

    void ScanDrivers()
    {
        g.drivers.clear();
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(DriverDir(), ec))
        {
            const std::string meta = ReadText(entry.path() / "meta.json", 64 * 1024);
            if (meta.empty())
                continue;
            Driver driver;
            driver.id = entry.path().filename().string();
            driver.dir = entry.path().string() + "/";
            driver.library = JsonString(meta, "libraryName");
            driver.name = JsonString(meta, "name");
            if (driver.name.empty())
                driver.name = driver.id;
            const std::string version = JsonString(meta, "driverVersion");
            int major = 0, minor = 0;
            const size_t digits = version.find_first_of("0123456789");
            if (digits != std::string::npos)
                sscanf(version.c_str() + digits, "%d.%d", &major, &minor);
            driver.bundled = access((driver.dir + ".bundled").c_str(), F_OK) == 0;
            // "minApi": 36 (a number): the Android version the driver's libraries were built for.
            [[maybe_unused]] int minApi = 0;
            if (const size_t key = meta.find("\"minApi\""); key != std::string::npos)
            {
                if (const size_t colon = meta.find(':', key); colon != std::string::npos)
                    minApi = atoi(meta.c_str() + colon + 1);
            }
            if (driver.library.empty() || access((driver.dir + driver.library).c_str(), R_OK) != 0)
                driver.problem = Tr("Falta la librería del driver");
#ifdef __ANDROID__
            else if (minApi > android_get_device_api_level())
                driver.problem = Trf("Necesita Android con API {} (este tiene {})", minApi, android_get_device_api_level());
#endif
            else if (major == 1 && minor < 2)
                driver.problem = Trf("Vulkan {}.{}: necesita 1.2", major, minor);
            driver.usable = driver.problem.empty();
            if (!driver.usable)
                LOGFN_WARNING("launcher: driver {} not usable: {}", driver.id, driver.problem);
            g.drivers.push_back(std::move(driver));
        }
    }

    // The library was recompiled from one default.xex: another executable does not match the
    // recompiled code and crashes, so its header (plain, in front of the encrypted image) is checked:
    // the execution info record has the title, media ID and version the port was made from
    // (games/<title>/game.cmake; a media or version of 0 accepts any).
    // data: the file's first bytes (256 KB hold the header). 1 the port's game, 0 another, -1 the
    // game but not the disc this port was recompiled from.
    int XexHeadGame(const std::string& data, std::string& found)
    {
        const auto u32 = [&](size_t at)
        {
            return at + 4 <= data.size() ? uint32_t(uint8_t(data[at])) << 24 | uint32_t(uint8_t(data[at + 1])) << 16 |
                    uint32_t(uint8_t(data[at + 2])) << 8 | uint8_t(data[at + 3])
                                         : 0u;
        };
        if (data.size() < 0x18 || data.compare(0, 4, "XEX2") != 0)
        {
            found = "not a XEX2";
            return 0;
        }
        for (uint32_t i = 0, count = u32(0x14); i < count && 0x20 + i * 8 <= data.size(); ++i)
        {
            if (u32(0x18 + i * 8) != 0x00040006) // XEX_HEADER_EXECUTION_INFO
                continue;
            const uint32_t at = u32(0x1C + i * 8);
            const uint32_t media = u32(at), version = u32(at + 4), title = u32(at + 12);
            found = fmt::format("title {:08X} media {:08X} version {:08X}", title, media, version);
            if (title != uint32_t(XPORT_TITLE_ID))
                return 0;
            // A game has the same title ID in every region, so another disc of it gets here.
            const bool mediaOk = uint32_t(XPORT_XEX_MEDIA) == 0 || media == uint32_t(XPORT_XEX_MEDIA);
            const bool versionOk = uint32_t(XPORT_XEX_VERSION) == 0 || version == uint32_t(XPORT_XEX_VERSION);
            return mediaOk && versionOk ? 1 : -1;
        }
        found = "no execution info";
        return 0;
    }

    bool XexMatches(const fs::path& path, int game, std::string& found)
    {
        return XexHeadGame(ReadText(path, 256 * 1024), found) == game;
    }

    void ScanGame()
    {
        std::error_code ec;
        g.game = fs::is_regular_file(GameDir() / "default.xex", ec) ? kThisGame : 0;
        g.xex.clear();
        g.xexOk = g.game != 0 && XexMatches(GameDir() / "default.xex", kThisGame, g.xex);
    }

    const char* GameLabel()
    {
        return g.game == 0 ? Tr("Juego no encontrado").data() : game::Title();
    }

    // ---------- settings ----------

    fs::path ConfigPath() { return External() / "launcher.cfg"; }

    void SaveConfig()
    {
        const Config& c = g.config;
        std::string text = fmt::format("driver={}\nauto_backup={}\nlast_backup={}\ntouch={}\nopacity={}\n"
                            "size={}\nvibration={}\nbg_pause={}\nfps={}\nlang={}\nwelcome_shown={}\n",
            c.driver, c.autoBackup, c.lastBackupDay, c.touch, c.opacity, c.size, c.vibration,
            c.bgPause, c.fps, c.language == 0 ? "auto" : kLanguages[c.language - 1].code, int(c.welcomeShown));
#if XPORT_TITLE_ID == 0x4D5308AB
        text += fmt::format("shadows={}\neffects={}\n", c.shadows, c.effects);
#endif
        if (!WriteText(ConfigPath(), text))
            LOGFN_WARNING("launcher: {} could not be written", ConfigPath().string());
    }

    void LoadConfig()
    {
        Config& c = g.config;
        const std::string text = ReadText(ConfigPath(), 16 * 1024);
        bool lang = false;
        size_t start = 0;
        while (start < text.size())
        {
            size_t end = text.find('\n', start);
            if (end == std::string::npos)
                end = text.size();
            const std::string line = text.substr(start, end - start);
            start = end + 1;
            const size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;
            const std::string key = line.substr(0, equals), value = line.substr(equals + 1);
            auto number = [&](int& field, int count)
            {
                const int parsed = atoi(value.c_str());
                if (parsed >= 0 && parsed < count)
                    field = parsed;
            };
            if (key == "driver") c.driver = value.empty() ? "auto" : value;
            else if (key == "auto_backup") number(c.autoBackup, 3);
            else if (key == "last_backup") c.lastBackupDay = value;
            else if (key == "touch") number(c.touch, 3);
            else if (key == "opacity") number(c.opacity, 4);
            else if (key == "size") number(c.size, 3);
            else if (key == "vibration") number(c.vibration, 2);
            else if (key == "bg_pause") number(c.bgPause, 2);
            else if (key == "fps") number(c.fps, 3);
            else if (key == "welcome_shown") c.welcomeShown = value == "1";
#if XPORT_TITLE_ID == 0x4D5308AB
            else if (key == "shadows") number(c.shadows, 2);
            else if (key == "effects") number(c.effects, 2);
#endif
            else if (key == "lang")
            {
                lang = true;
                c.language = 0;
                for (int i = 0; i < int(std::size(kLanguages)); ++i)
                {
                    if (value == kLanguages[i].code)
                        c.language = i + 1;
                }
            }
            // Before 0.8.0D: an index into kLanguages, Español (0) the default and no Automático.
            else if (key == "language" && !lang)
            {
                const int old = atoi(value.c_str());
                c.language = old > 0 && old < int(std::size(kLanguages)) ? old + 1 : 0;
            }
        }
    }

    // ---------- GPU and drivers ----------

    bool SystemDriverOk() { return g.gpu && g.gpu->found && g.gpu->missing.empty(); }

    const Driver* ImportedDriver(const std::string& id)
    {
        for (const Driver& driver : g.drivers)
        {
            if (driver.id == id)
                return &driver;
        }
        return nullptr;
    }

    // The driver the game will load: "system", "turnip" or an imported driver's id.
    std::string ActiveDriver()
    {
        if (!g.adreno)
            return "system";
        const std::string& chosen = g.config.driver;
        if (chosen == "system" && SystemDriverOk())
            return chosen;
        if (chosen == "turnip" && g.turnipPresent)
            return chosen;
        if (const Driver* driver = ImportedDriver(chosen); driver && driver->usable)
            return chosen;
        return SystemDriverOk() || !g.turnipPresent ? "system" : "turnip";
    }

    std::string DriverName(const std::string& id)
    {
        if (id == "system")
            return std::string(Tr("Driver del sistema"));
        if (id == "turnip")
            return kTurnipName;
        const Driver* driver = ImportedDriver(id);
        return driver ? driver->name : id;
    }

    std::string GpuName()
    {
        if (!g.gpu || !g.gpu->found)
            return std::string(Tr("GPU sin Vulkan"));
        std::string name = g.gpu->device;
        for (const char* noise : {" (TM)", "(TM)"})
        {
            if (const size_t at = name.find(noise); at != std::string::npos)
                name.erase(at, strlen(noise));
        }
        return name;
    }

    // ---------- notices ----------

    void Toast(const std::string& text, int level = 0)
    {
        g.toasts.push_back({text, level, g.clock});
        while (g.toasts.size() > 3)
            g.toasts.pop_front();
    }

    void BuildNotices()
    {
        g.notices.clear();
        if (g.game == 0)
            g.notices.push_back({2, std::string(Tr("No se encuentra el juego: toca para importar la ISO")), Page::Games, true});
        if (g.game == kThisGame && !g.xexOk)
            g.notices.push_back({2, std::string(Tr("Versión del juego no compatible: usa el disco con el que se hizo el port, sin actualizaciones")),
                Page::Games, true});
        if (!g.gpu || !g.gpu->found)
        {
            g.notices.push_back({2, std::string(Tr("No se encontró una GPU con Vulkan")) +
                (g.gpu && !g.gpu->missing.empty() ? " (" + g.gpu->missing + ")" : std::string()), Page::Gpu, !g.adreno});
        }
        else if (!g.gpu->missing.empty())
        {
            if (!g.adreno)
                g.notices.push_back({2, Trf("Tu GPU no es compatible con el renderer: {}", g.gpu->missing), Page::Gpu, true});
            else if (!g.turnipPresent)
                g.notices.push_back({2, std::string(Tr("El driver del sistema no es compatible y falta Turnip en la APK")), Page::Gpu, true});
            else if (ActiveDriver() == "turnip")
                g.notices.push_back({0, std::string(Tr("Se usará Turnip: el driver del sistema no es compatible")), Page::Gpu, false});
        }
        if (g.adreno && g.config.driver != "auto" && ActiveDriver() != g.config.driver)
            g.notices.push_back({1, Trf("El driver elegido no está disponible: se usará {}", DriverName(ActiveDriver())), Page::Gpu, false});
        std::error_code ec;
        if (fs::is_regular_file(External() / "crash.txt", ec))
            g.notices.push_back({1, std::string(Tr("La última partida se cerró por un error: toca para ver el informe")), Page::Crash, false});
        const Platform& platform = GetPlatform();
        if (platform.availMem && platform.availMem < 1536ull * 1024 * 1024)
        {
            g.notices.push_back({1, Trf("Poca memoria libre ({} GB): cierra otras apps antes de jugar",
                Decimal(platform.availMem / (1024.0 * 1024 * 1024), 1)), Page::System, false});
        }
    }

    const Notice* Blocking()
    {
        for (const Notice& notice : g.notices)
        {
            if (notice.blocks)
                return &notice;
        }
        return nullptr;
    }

    int WorstNotice()
    {
        int level = -1;
        for (const Notice& notice : g.notices)
            level = std::max(level, notice.level);
        return level;
    }

    // After the language setting changed: the texts made in advance are made again.
    void ApplyLanguage()
    {
        g.ui = LanguageIndex(g.config.language);
        ScanDrivers();
        BuildNotices();
    }

    // ---------- backups ----------

    // Copies every save (folder and record) into backups/<date>[_<kind>]/; returns false with
    // nothing to copy or on a copy error (the half-made copy is removed).
    bool MakeBackup(const char* kind)
    {
        std::error_code ec;
        const std::string name = Now("%Y-%m-%d_%H-%M-%S") + (kind ? std::string("_") + kind : std::string());
        const fs::path target = BackupDir() / name;
        int copied = 0;
        bool ok = true;
        for (const auto& entry : fs::directory_iterator(SaveDir(), ec))
        {
            if (!IsSaveFolder(entry))
                continue;
            if (copied == 0)
                fs::create_directories(target, ec);
            fs::copy(entry.path(), target / entry.path().filename(), fs::copy_options::recursive, ec);
            ok = ok && !ec;
            fs::path record = entry.path();
            record += ".xcontent";
            if (fs::exists(record, ec))
            {
                fs::copy_file(record, target / record.filename(), fs::copy_options::overwrite_existing, ec);
                ok = ok && !ec;
            }
            ++copied;
        }
        if (copied && !ok)
        {
            fs::remove_all(target, ec);
            LOGFN_WARNING("launcher: backup {} failed", name);
        }
        else if (copied)
        {
            LOGFN("launcher: backup {} ({} saves)", name, copied);
        }
        // Only the newest automatic copies are kept; the manual ones stay until deleted.
        if (kind)
        {
            ScanBackups();
            int automatic = 0;
            for (const Backup& backup : g.backups)
            {
                if (backup.kind != "Manual" && ++automatic > kAutoBackupsKept)
                    fs::remove_all(BackupDir() / backup.folder, ec);
            }
        }
        ScanBackups();
        return copied && ok;
    }

    void RemoveSaves()
    {
        std::error_code ec;
        std::vector<fs::path> doomed;
        for (const auto& entry : fs::directory_iterator(SaveDir(), ec))
        {
            if (IsSaveFolder(entry))
            {
                doomed.push_back(entry.path());
                fs::path record = entry.path();
                record += ".xcontent";
                doomed.push_back(record);
            }
        }
        for (const fs::path& path : doomed)
            fs::remove_all(path, ec);
    }

    void RestoreBackup(const Backup& backup)
    {
        std::error_code ec;
        const fs::path source = BackupDir() / backup.folder;
        if (!fs::is_directory(source, ec))
        {
            Toast("La copia ya no existe", 2);
            return;
        }
        if (!g.saves.empty() && !MakeBackup("antes"))
        {
            Toast("No se pudo hacer una copia de las partidas actuales: no se ha restaurado nada", 2);
            return;
        }
        RemoveSaves();
        fs::create_directories(SaveDir(), ec);
        bool ok = true;
        for (const auto& entry : fs::directory_iterator(source, ec))
        {
            fs::copy(entry.path(), SaveDir() / entry.path().filename(),
                fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            ok = ok && !ec;
        }
        ScanSaves();
        ScanBackups();
        Toast(ok ? Trf("Copia del {} restaurada", backup.date) : std::string(Tr("La copia se restauró con errores")), ok ? 0 : 2);
        LOGFN("launcher: restored {}{}", backup.folder, ok ? "" : " with errors");
    }

    void DeleteSave(const Save& save)
    {
        if (!MakeBackup("antes"))
        {
            Toast("No se pudo hacer la copia de seguridad: la partida no se ha borrado", 2);
            return;
        }
        std::error_code ec;
        fs::remove_all(SaveDir() / save.folder, ec);
        fs::remove(SaveDir() / (save.folder + ".xcontent"), ec);
        const std::string name = save.mission;
        ScanSaves();
        Toast(Trf("Partida borrada (hay una copia en Copias de seguridad): {}", name));
    }

    // ---------- the game check ----------

    void StartGameCheck()
    {
        if (g.check.running.exchange(true))
            return;
        {
            std::lock_guard lock(g.check.mutex);
            g.check.lines = {" Comprobando…"};
        }
        // Each line starts with '+' (found), '-' (missing) or ' ' (information).
        const std::string totals(Tr(" {} archivos · {} GB"));
        const std::string version = std::string(g.xexOk ? "+" : "-") + std::string(Tr("Versión compatible"));
        std::thread([totals, version]
        {
            std::vector<std::string> lines;
            std::error_code ec, entryError;
            const fs::path root = GameDir();
            const auto has = [&](const char* name) { return fs::exists(root / name, entryError) ? "+" : "-"; };
            lines.push_back(std::string(has("default.xex")) + "default.xex");
            lines.push_back(version);
            uint64_t files = 0, bytes = 0;
            for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
                 it.increment(ec))
            {
                if (it->is_regular_file(entryError))
                {
                    ++files;
                    bytes += it->file_size(entryError);
                }
            }
            const std::string size = Decimal(bytes / (1024.0 * 1024 * 1024), 2);
            lines.push_back(fmt::vformat(totals, fmt::make_format_args(files, size)));
            std::lock_guard lock(g.check.mutex);
            g.check.lines = std::move(lines);
            g.check.running = false;
        }).detach();
    }

    // ---------- Jugar ----------

    void WriteSwitch(const char* name, const std::string& value)
    {
        if (!WriteText(External() / name, value + "\n"))
            LOGFN_WARNING("launcher: files/{} could not be written", name);
    }

    // The files/ switches the game reads at its start (rexglue_glue/graphics.cpp,
    // os/background.cpp, os/vk_driver.cpp).
    void WriteSwitches()
    {
        WriteSwitch("bg_pause", g.config.bgPause == 0 ? "1" : "0");
        std::error_code ec;
        const std::string driver = ActiveDriver();
        if (!g.adreno || g.config.driver == "auto")
            fs::remove(External() / "vk_driver", ec);
        else if (driver == "system" || driver == "turnip")
            WriteSwitch("vk_driver", driver);
        else if (const Driver* imported = ImportedDriver(driver))
            WriteSwitch("vk_driver", "custom\n" + imported->dir + "\n" + imported->library);
    }

    // The game's options (MainActivity and android_app.cpp read them).
    std::string GameOptions()
    {
        static const char* const alpha[] = {"0.25", "0.5", "0.75", "1"};
        static const char* const scale[] = {"0.85", "1", "1.2"};
        static const int touch[] = {2, 1, 0};
        const Config& c = g.config;
        std::string options = fmt::format("XPORT_LANGUAGE={} XPORT_FPS={} XPORT_TOUCH={} XPORT_TOUCH_ALPHA={} XPORT_TOUCH_SCALE={} "
                           "XPORT_TOUCH_HAPTIC={}",
            kLanguages[g.ui].id, c.fps, touch[c.touch], alpha[c.opacity], scale[c.size], c.vibration == 0 ? 1 : 0);
#if XPORT_TITLE_ID == 0x4D5308AB
        // Gráficos: 1 on, 0 off (games/gears_of_war_3/game.cpp reads them).
        options += fmt::format(" XPORT_SHADOWS={} XPORT_EFFECTS={}", c.shadows == 0 ? 1 : 0, c.effects == 0 ? 1 : 0);
#endif
        return options;
    }

    void Play()
    {
        if (g.playing)
            return;
        if (g.importing)
        {
            Toast("Espera a que termine la importación", 1);
            return;
        }
        if (const Notice* block = Blocking())
        {
            Toast(block->text, 2);
            return;
        }
        const std::string today = Now("%Y-%m-%d");
        if (!g.saves.empty() && (g.config.autoBackup == 1 || (g.config.autoBackup == 2 && g.config.lastBackupDay != today)))
        {
            if (MakeBackup("auto"))
                g.config.lastBackupDay = today;
        }
        WriteSwitches();
        SaveConfig();
        const std::string options = GameOptions();
        LOGFN("launcher: play (driver {}): {}", ActiveDriver(), options);
        g.playing = true;
        RequestPlay(options);
    }

    void Go(Page page)
    {
        if (page == g.page)
            return;
        g.page = page;
        g.pageStart = g.clock;
        g.confirm.clear();
        g.resetScroll = true;
        g.scroll = {};
        if (page == Page::Crash)
        {
            const fs::path path = External() / "crash.txt";
            g.crash = ReadText(path, 24 * 1024);
            g.crashDate.clear();
            struct stat info{};
            if (stat(path.c_str(), &info) == 0)
            {
                tm local{};
                localtime_r(&info.st_mtime, &local);
                char text[32];
                strftime(text, sizeof(text), "%d/%m/%Y %H:%M", &local);
                g.crashDate = text;
            }
        }
        if (page == Page::Saves)
        {
            ScanSaves();
            ScanBackups();
        }
        if (page == Page::Games)
        {
            ScanGame();
            BuildNotices();
        }
    }

    // ---------- drawing ----------

    float Dp(float value) { return value * g.dp; }

    ImDrawList* DrawList() { return g.drawList ? g.drawList : ImGui::GetWindowDrawList(); }

    ImU32 Rgba(int r, int gr, int b, float a)
    {
        return IM_COL32(r, gr, b, int(std::clamp(a * g.fade, 0.f, 1.f) * 255 + 0.5f));
    }
    ImU32 White(float a) { return Rgba(255, 255, 255, a); }
#if defined(XPORT_LAUNCHER_ACCENT) && defined(XPORT_LAUNCHER_TINT)
    // The game's own colours (0xRRGGBB in its game.cmake, with its own background).
    ImU32 Hex(uint32_t rgb, float a) { return Rgba(int(rgb >> 16 & 255), int(rgb >> 8 & 255), int(rgb & 255), a); }
    ImU32 Accent(float a) { return Hex(XPORT_LAUNCHER_ACCENT, a); }
    ImU32 Tint(float a) { return Hex(XPORT_LAUNCHER_TINT, a); }
#else
    ImU32 Accent(float a) { return g.adreno ? Rgba(255, 84, 84, a) : Rgba(70, 150, 255, a); }
    ImU32 Tint(float a) { return g.adreno ? Rgba(42, 10, 14, a) : Rgba(8, 20, 44, a); }
#endif
    ImU32 Level(int level, float a)
    {
        // Information is grey: the Adreno accent is red, the colour of the errors.
        return level >= 2 ? Rgba(255, 82, 82, a) : level == 1 ? Rgba(255, 190, 60, a) : Rgba(120, 130, 148, a);
    }

    ImTextureID TextureOf(const Texture& texture) { return ImTextureID(texture.id); }

    ImVec2 CoverUv(ImVec2 p) { return (p - g.coverOrigin) / g.coverSize; }

    float FontSize(Font font) { return g.fonts[font] ? g.fonts[font]->FontSize : Dp(kFontDp[font]); }

    // The size of a text as shown (translated; MeasureShown: already translated).
    ImVec2 MeasureShown(Font font, std::string_view text, float wrap = 0)
    {
        ImFont* f = g.fonts[font];
        return f ? f->CalcTextSizeA(f->FontSize, FLT_MAX, wrap, text.data(), text.data() + text.size()) : ImVec2();
    }

    ImVec2 Measure(Font font, std::string_view text, float wrap = 0) { return MeasureShown(font, Tr(text), wrap); }

    // The translated text cut with an ellipsis to fit width (0: no limit).
    std::string Fit(Font font, std::string_view source, float width)
    {
        const std::string text(Tr(source));
        if (width <= 0 || MeasureShown(font, text).x <= width)
            return text;
        std::string cut = text;
        while (!cut.empty())
        {
            size_t size = cut.size() - 1;
            while (size > 0 && (uint8_t(cut[size]) & 0xC0) == 0x80)
                --size;
            cut.resize(size);
            while (!cut.empty() && cut.back() == ' ')
                cut.pop_back();
            if (MeasureShown(font, cut + "…").x <= width)
                return cut + "…";
        }
        return "…";
    }

    void TextShown(Font font, ImVec2 pos, ImU32 color, const std::string& text)
    {
        if (ImFont* f = g.fonts[font])
            DrawList()->AddText(f, f->FontSize, ImFloor(pos), color, text.c_str());
    }

    // The text translated (Tr) and cut to width.
    void Text(Font font, ImVec2 pos, ImU32 color, std::string_view text, float width = 0)
    {
        TextShown(font, pos, color, Fit(font, text, width));
    }

    void TextCentered(Font font, ImVec2 center, ImU32 color, std::string_view text, float width = 0)
    {
        const std::string shown = Fit(font, text, width);
        TextShown(font, center - MeasureShown(font, shown) * 0.5f, color, shown);
    }

    // Translated and wrapped to width; returns the height used.
    float TextWrapped(Font font, ImVec2 pos, ImU32 color, std::string_view source, float width)
    {
        ImFont* f = g.fonts[font];
        if (!f)
            return 0;
        const std::string_view text = Tr(source);
        DrawList()->AddText(f, f->FontSize, ImFloor(pos), color, text.data(), text.data() + text.size(), width);
        return MeasureShown(font, text, width).y;
    }

    // The path of a triangle whose corners are rounded with radius r (the play mark).
    void RoundedTrianglePath(ImDrawList* dl, const ImVec2 (&p)[3], float r)
    {
        dl->PathClear();
        for (int i = 0; i < 3; ++i)
        {
            const ImVec2 v = p[i], toPrevious = p[(i + 2) % 3] - v, toNext = p[(i + 1) % 3] - v;
            const ImVec2 a = toPrevious / ImSqrt(ImLengthSqr(toPrevious)), b = toNext / ImSqrt(ImLengthSqr(toNext));
            const float half = std::acos(std::clamp(ImDot(a, b), -1.f, 1.f)) / 2;
            // The corner's circle touches both edges; its arc runs between the two touching points.
            const ImVec2 bisector = (a + b) / ImSqrt(ImLengthSqr(a + b));
            const ImVec2 c = v + bisector * (r / std::sin(half));
            const ImVec2 t1 = v + a * (r / std::tan(half)), t2 = v + b * (r / std::tan(half));
            const float from = std::atan2(t1.y - c.y, t1.x - c.x);
            float sweep = std::atan2(t2.y - c.y, t2.x - c.x) - from;
            sweep = sweep > IM_PI ? sweep - 2 * IM_PI : sweep < -IM_PI ? sweep + 2 * IM_PI : sweep;
            dl->PathArcTo(c, r, from, from + sweep, 6);
        }
    }

    // The icons, drawn on a 24x24 grid scaled to size.
    void DrawIcon(Icon icon, ImVec2 center, float size, ImU32 color)
    {
        ImDrawList* dl = DrawList();
        const float s = size / 24, thickness = std::max(1.f, 1.9f * s);
        const auto P = [&](float x, float y) { return center + ImVec2((x - 12) * s, (y - 12) * s); };
        const auto Line = [&](std::initializer_list<ImVec2> points, bool closed = false)
        {
            dl->PathClear();
            for (const ImVec2& p : points)
                dl->PathLineTo(P(p.x, p.y));
            dl->PathStroke(color, closed ? ImDrawFlags_Closed : ImDrawFlags_None, thickness);
        };
        const auto Circle = [&](float x, float y, float r, bool filled = false)
        {
            if (filled)
                dl->AddCircleFilled(P(x, y), r * s, color);
            else
                dl->AddCircle(P(x, y), r * s, color, 0, thickness);
        };
        const auto Arc = [&](float x, float y, float r, float from, float to)
        {
            dl->PathClear();
            dl->PathArcTo(P(x, y), r * s, from, to);
            dl->PathStroke(color, ImDrawFlags_None, thickness);
        };
        const auto Box = [&](float x0, float y0, float x1, float y1, float r)
        {
            dl->AddRect(P(x0, y0), P(x1, y1), color, r * s, ImDrawFlags_None, thickness);
        };
        constexpr float pi = IM_PI;
        switch (icon)
        {
        case Icon::Play:
        {
            // A line icon like the others, with rounded corners; the centroid (x 12) on the centre.
            const ImVec2 points[3] = {P(8.25f, 5.5f), P(19.5f, 12), P(8.25f, 18.5f)};
            RoundedTrianglePath(dl, points, 1.5f * s);
            dl->PathStroke(color, ImDrawFlags_Closed, thickness);
            break;
        }
        case Icon::Graphics:
            Line({{4, 7}, {20, 7}});
            Line({{4, 12}, {20, 12}});
            Line({{4, 17}, {20, 17}});
            Circle(9, 7, 2.4f, true);
            Circle(15, 12, 2.4f, true);
            Circle(10, 17, 2.4f, true);
            break;
        case Icon::Saves:
            Box(4.5f, 4.5f, 19.5f, 19.5f, 2.5f);
            Line({{8, 4.5f}, {8, 9}, {15, 9}, {15, 4.5f}});
            Box(7.5f, 13.5f, 16.5f, 19.5f, 1);
            break;
        case Icon::Gpu:
            Box(6, 6, 18, 18, 2);
            Box(9.5f, 9.5f, 14.5f, 14.5f, 0.5f);
            for (const float v : {9.f, 15.f})
            {
                Line({{v, 3}, {v, 6}});
                Line({{v, 18}, {v, 21}});
                Line({{3, v}, {6, v}});
                Line({{18, v}, {21, v}});
            }
            break;
        case Icon::Controls:
            Box(2.5f, 7.5f, 21.5f, 17.5f, 5);
            Line({{8, 10.5f}, {8, 15}});
            Line({{5.75f, 12.75f}, {10.25f, 12.75f}});
            Circle(15.5f, 11.5f, 1.2f, true);
            Circle(17.8f, 14, 1.2f, true);
            break;
        case Icon::System:
            Circle(12, 12, 3.2f);
            for (int i = 0; i < 8; ++i)
            {
                const float angle = i * pi / 4;
                Line({{12 + std::cos(angle) * 6.5f, 12 + std::sin(angle) * 6.5f},
                    {12 + std::cos(angle) * 9.5f, 12 + std::sin(angle) * 9.5f}});
            }
            break;
        case Icon::Credits:
            Line({{12, 3.5f}, {14.6f, 8.8f}, {20.5f, 9.7f}, {16.2f, 13.8f}, {17.2f, 19.6f}, {12, 16.9f}, {6.8f, 19.6f},
                     {7.8f, 13.8f}, {3.5f, 9.7f}, {9.4f, 8.8f}},
                true);
            break;
        case Icon::Back:
            Line({{15, 5}, {8, 12}, {15, 19}});
            break;
        case Icon::Chevron:
            Line({{9, 5}, {16, 12}, {9, 19}});
            break;
        case Icon::Check:
            Line({{5, 12.5f}, {9.5f, 17}, {19, 7.5f}});
            break;
        case Icon::Cross:
            Line({{6.5f, 6.5f}, {17.5f, 17.5f}});
            Line({{17.5f, 6.5f}, {6.5f, 17.5f}});
            break;
        case Icon::Lock:
            Box(5, 11, 19, 20, 2);
            Line({{8, 11}, {8, 8}});
            Arc(12, 8, 4, pi, 2 * pi);
            Line({{16, 8}, {16, 11}});
            break;
        case Icon::Pencil:
            Line({{4, 20}, {8, 20}, {19, 9}, {15, 5}, {4, 16}}, true);
            break;
        case Icon::Bell:
            Line({{5.5f, 16.5f}, {18.5f, 16.5f}});
            Line({{7, 16.5f}, {7, 11}});
            Arc(12, 11, 5, pi, 2 * pi);
            Line({{17, 11}, {17, 16.5f}});
            Circle(12, 19.4f, 1.5f, true);
            break;
        case Icon::Warning:
            Line({{12, 3.5f}, {21.5f, 20}, {2.5f, 20}}, true);
            Line({{12, 9.5f}, {12, 14}});
            Circle(12, 17, 1.2f, true);
            break;
        case Icon::Info:
            Circle(12, 12, 9);
            Line({{12, 11}, {12, 16.5f}});
            Circle(12, 7.8f, 1.2f, true);
            break;
        case Icon::Bolt:
            Line({{13.5f, 2.5f}, {5, 13.5f}, {11.5f, 13.5f}, {10.5f, 21.5f}, {19, 10}, {12.5f, 10}}, true);
            break;
        case Icon::Gauge:
            Arc(12, 15, 8.5f, pi, 2 * pi);
            Line({{12, 15}, {16.5f, 9.5f}});
            Circle(12, 15, 1.6f, true);
            break;
        case Icon::Dot:
            Circle(12, 12, 8.5f);
            Circle(12, 12, 3.2f, true);
            break;
        case Icon::Sparkle:
            Line({{12, 3}, {14, 10}, {21, 12}, {14, 14}, {12, 21}, {10, 14}, {3, 12}, {10, 10}}, true);
            break;
        case Icon::Console:
            Box(2.5f, 7, 21.5f, 17, 3.5f);
            Circle(12, 12, 2.6f);
            Line({{5.5f, 10}, {7.5f, 10}});
            break;
        case Icon::Trash:
            Line({{4.5f, 7}, {19.5f, 7}});
            Line({{9.5f, 7}, {9.5f, 4.5f}, {14.5f, 4.5f}, {14.5f, 7}});
            Line({{6.5f, 7}, {7.5f, 20}, {16.5f, 20}, {17.5f, 7}});
            Line({{10.5f, 10.5f}, {10.5f, 16.5f}});
            Line({{13.5f, 10.5f}, {13.5f, 16.5f}});
            break;
        case Icon::Restore:
            Arc(12, 12, 7.5f, pi, 2.75f * pi);
            Line({{2, 9.5f}, {4.5f, 12.3f}, {7.3f, 10}});
            break;
        case Icon::Import:
            Line({{12, 4}, {12, 15}});
            Line({{7, 10.5f}, {12, 15.5f}, {17, 10.5f}});
            Line({{5, 19.5f}, {19, 19.5f}});
            break;
        case Icon::Copy:
            Box(8.5f, 8.5f, 19.5f, 19.5f, 2);
            Line({{15.5f, 8.5f}, {15.5f, 4.5f}, {4.5f, 4.5f}, {4.5f, 15.5f}, {8.5f, 15.5f}});
            break;
        }
    }

    // Jugar's icon, the same as the pages' tiles (Tile): a white line icon on a disc of the GPU's
    // colour; a lock on red when something blocks the game (blocked), on glass when it cannot start.
    void PlayButton(ImVec2 center, float radius, bool ready, bool blocked)
    {
        DrawList()->AddCircleFilled(center, radius, ready ? Accent(0.30f) : blocked ? Rgba(255, 82, 82, 0.30f) : White(0.10f));
        DrawIcon(ready ? Icon::Play : Icon::Lock, center, radius * 1.1f, White(ready || blocked ? 1 : 0.8f));
    }

    // A liquid-glass panel: the blurred background under it, the GPU's tint, a rim of light.
    void Glass(ImVec2 a, ImVec2 b, float rounding, bool selected = false, bool held = false, ImU32 wash = 0)
    {
        ImDrawList* dl = DrawList();
        if (g.blurred.id)
            dl->AddImageRounded(TextureOf(g.blurred), a, b, CoverUv(a), CoverUv(b), White(1), rounding);
        dl->AddRectFilled(a, b, Tint(0.46f), rounding);
        if (wash)
            dl->AddRectFilled(a, b, wash, rounding);
        if (selected)
            dl->AddRectFilled(a, b, Accent(0.30f), rounding);
        if (held)
            dl->AddRectFilled(a, b, White(0.10f), rounding);
        dl->AddRect(a, b, selected ? Accent(0.95f) : White(0.17f), rounding, ImDrawFlags_None, selected ? Dp(2) : Dp(1.2f));
        dl->AddLine(ImVec2(a.x + rounding, a.y + Dp(1.5f)), ImVec2(b.x - rounding, a.y + Dp(1.5f)), White(0.20f), Dp(1));
    }

    // An invisible button over [a, b]: true when tapped (not when the finger scrolled, nor when it
    // only stopped a fling) or activated with the pad. held: the finger is on it.
    bool Hit(const char* id, ImVec2 a, ImVec2 b, bool* held = nullptr, bool enabled = true)
    {
        ImGui::SetCursorScreenPos(a);
        if (!enabled)
            ImGui::BeginDisabled();
        const bool pressed = ImGui::InvisibleButton(id, ImMax(b - a, ImVec2(1, 1)));
        if (!enabled)
            ImGui::EndDisabled();
        if (held)
            *held = enabled && ImGui::IsItemActive() && !g.scroll.dragging;
        if (!pressed || !enabled)
            return false;
        const ImGuiIO& io = ImGui::GetIO();
        return !io.MouseReleased[0] || (io.MouseDragMaxDistanceSqr[0] <= Dp(12) * Dp(12) && !g.scroll.stopped);
    }

    // The pad's focus around the item just submitted.
    void FocusRing(ImVec2 a, ImVec2 b, float rounding)
    {
        if (!g.touch && ImGui::IsItemFocused())
        {
            const ImVec2 out(Dp(3), Dp(3));
            DrawList()->AddRect(a - out, b + out, Rgba(255, 255, 255, 0.95f), rounding + Dp(3), ImDrawFlags_None, Dp(2.5f));
        }
    }

    float PillHeight() { return Dp(48); }

    // A row of choices; returns the chosen index.
    int Segmented(const char* id, ImVec2 a, float width, const Options& options, int value, bool enabled = true)
    {
        const int count = int(options.size());
        const float gap = Dp(6), height = PillHeight();
        const float pill = (width - gap * (count - 1)) / count;
        Font font = kBody;
        for (const char* option : options)
        {
            if (Measure(kBody, option).x > pill - Dp(14))
                font = kSmall;
        }
        ImGui::PushID(id);
        int chosen = value;
        for (int i = 0; i < count; ++i)
        {
            const ImVec2 pa = a + ImVec2(i * (pill + gap), 0), pb = pa + ImVec2(pill, height);
            ImGui::PushID(i);
            bool held = false;
            const bool tapped = Hit("##option", pa, pb, &held, enabled);
            ImGui::PopID();
            const bool selected = i == value;
            ImDrawList* dl = DrawList();
            dl->AddRectFilled(pa, pb, selected ? Accent(enabled ? 0.92f : 0.45f) : White(held ? 0.18f : 0.07f), height / 2);
            if (!selected)
                dl->AddRect(pa, pb, White(0.13f), height / 2, ImDrawFlags_None, Dp(1));
            TextCentered(font, (pa + pb) * 0.5f, White(selected ? 1 : enabled ? 0.82f : 0.4f), options[i], pill - Dp(10));
            FocusRing(pa, pb, height / 2);
            if (tapped)
                chosen = i;
        }
        ImGui::PopID();
        return chosen;
    }

    // A pill-shaped button; icon is optional.
    float ButtonWidth(const char* label, bool icon) { return Measure(kBody, label).x + Dp(36) + (icon ? Dp(28) : 0); }

    bool Button(const char* id, ImVec2 a, float width, const char* label, const Icon* icon, ImU32 fill, bool enabled = true)
    {
        const ImVec2 b = a + ImVec2(width, Dp(46));
        bool held = false;
        const bool tapped = Hit(id, a, b, &held, enabled);
        ImDrawList* dl = DrawList();
        const float rounding = Dp(23);
        dl->AddRectFilled(a, b, fill, rounding);
        if (held)
            dl->AddRectFilled(a, b, White(0.14f), rounding);
        dl->AddRect(a, b, White(0.16f), rounding, ImDrawFlags_None, Dp(1));
        const ImVec2 size = Measure(kBody, label);
        float x = a.x + (width - size.x - (icon ? Dp(28) : 0)) / 2;
        const float center = (a.y + b.y) / 2;
        const ImU32 color = White(enabled ? 1 : 0.4f);
        if (icon)
        {
            DrawIcon(*icon, ImVec2(x + Dp(10), center), Dp(20), color);
            x += Dp(28);
        }
        Text(kBody, ImVec2(x, center - size.y / 2), color, label);
        FocusRing(a, b, rounding);
        return tapped;
    }

    // A button that asks again: the first tap turns it into "¿Seguro?" for 4 s, the second acts.
    bool ConfirmButton(const std::string& key, ImVec2 a, float width, const char* label, Icon icon, bool danger)
    {
        const bool asking = g.confirm == key && g.clock - g.confirmTime < 4;
        const ImU32 fill = asking ? Rgba(230, 60, 60, 0.9f) : danger ? Rgba(255, 80, 80, 0.20f) : White(0.10f);
        if (!Button(key.c_str(), a, width, asking ? "¿Seguro?" : label, &icon, fill))
            return false;
        if (asking)
        {
            g.confirm.clear();
            return true;
        }
        g.confirm = key;
        g.confirmTime = g.clock;
        return false;
    }

    float ConfirmWidth(const char* label) { return std::max(ButtonWidth(label, true), ButtonWidth("¿Seguro?", true)); }

    // Cards in columns inside a scrolling page.
    struct Grid
    {
        ImVec2 origin;
        float width = 0, gap = 0, y = 0, rowHeight = 0;
        int columns = 1, column = 0;

        float CellWidth(int span = 1) const
        {
            span = std::min(span, columns);
            const float cell = (width - gap * (columns - 1)) / columns;
            return cell * span + gap * (span - 1);
        }
        ImVec2 Place(float height, int span = 1)
        {
            span = std::min(span, columns);
            if (column + span > columns)
                NewRow();
            const ImVec2 at = origin + ImVec2(column * (CellWidth() + gap), y);
            column += span;
            rowHeight = std::max(rowHeight, height);
            return at;
        }
        void NewRow()
        {
            if (column > 0)
                y += rowHeight + gap;
            column = 0;
            rowHeight = 0;
        }
        float End()
        {
            NewRow();
            return y;
        }
    };

    void Section(Grid& grid, const char* title)
    {
        grid.NewRow();
        grid.y += Dp(6);
        Text(kTitle, grid.origin + ImVec2(Dp(6), grid.y), White(0.95f), title);
        grid.y += FontSize(kTitle) + Dp(12);
    }

    void Card(ImVec2 a, ImVec2 b, ImU32 wash = 0) { Glass(a, b, Dp(20), false, false, wash); }

    float SettingHeight() { return Dp(16) + FontSize(kBody) + Dp(12) + PillHeight() + Dp(16); }

    // A card with a label and its choices; true when the value changed.
    bool SettingCard(Grid& grid, const char* id, const char* label, const Options& options, int& value, int span = 1)
    {
        const float height = SettingHeight(), width = grid.CellWidth(span);
        const ImVec2 a = grid.Place(height, span);
        Card(a, a + ImVec2(width, height));
        Text(kBody, a + ImVec2(Dp(18), Dp(16)), White(0.92f), label, width - Dp(36));
        const int chosen = Segmented(id, a + ImVec2(Dp(14), Dp(16) + FontSize(kBody) + Dp(12)), width - Dp(28), options, value);
        if (chosen == value)
            return false;
        value = chosen;
        SaveConfig();
        return true;
    }

    void Tag(ImVec2 a, const std::string& text, ImU32 fill)
    {
        const ImVec2 size = Measure(kSmall, text);
        const ImVec2 b = a + ImVec2(size.x + Dp(18), size.y + Dp(8));
        DrawList()->AddRectFilled(a, b, fill, (b.y - a.y) / 2);
        Text(kSmall, a + ImVec2(Dp(9), Dp(4)), White(1), text);
    }

    float TagWidth(const std::string& text) { return Measure(kSmall, text).x + Dp(18); }

    // ---------- pages ----------

    void SavesPage(Grid& grid)
    {
        // Copies: automatic ones and a copy now.
        {
            const float height = SettingHeight(), width = grid.CellWidth(2);
            const ImVec2 a = grid.Place(height, 2);
            Card(a, a + ImVec2(width, height));
            Text(kBody, a + ImVec2(Dp(18), Dp(16)), White(0.92f), "Copias automáticas", width - Dp(36));
            const float button = ButtonWidth("Copiar ahora", true);
            const ImVec2 row = a + ImVec2(Dp(14), Dp(16) + FontSize(kBody) + Dp(12));
            const int chosen = Segmented("##auto", row, width - Dp(28) - button - Dp(12), {"Off", "Al jugar", "Diaria"}, g.config.autoBackup);
            if (chosen != g.config.autoBackup)
            {
                g.config.autoBackup = chosen;
                SaveConfig();
            }
            const Icon copy = Icon::Copy;
            if (Button("##backup", ImVec2(a.x + width - Dp(14) - button, row.y + Dp(1)), button, "Copiar ahora", &copy, Accent(0.85f), !g.saves.empty()))
            {
                ScanSaves();
                if (MakeBackup(nullptr))
                    Toast("Copia de seguridad creada");
                else
                    Toast(g.saves.empty() ? "No hay partidas que copiar" : "No se pudo crear la copia", g.saves.empty() ? 1 : 2);
            }
        }

        Section(grid, g.saves.empty() ? "Partidas" : Trf("Partidas ({})", g.saves.size()).c_str());
        const float rowHeight = Dp(16) + FontSize(kBody) + Dp(6) + FontSize(kSmall) + Dp(16) + Dp(46) + Dp(14);
        if (g.saves.empty())
        {
            const ImVec2 a = grid.Place(Dp(72), 2);
            Card(a, a + ImVec2(grid.CellWidth(2), Dp(72)));
            Text(kBody, a + ImVec2(Dp(18), Dp(36) - FontSize(kBody) / 2), White(0.75f), "Todavía no hay partidas guardadas");
        }
        for (size_t i = 0; i < g.saves.size(); ++i)
        {
            const Save& save = g.saves[i];
            const float width = grid.CellWidth();
            const ImVec2 a = grid.Place(rowHeight);
            Card(a, a + ImVec2(width, rowHeight));
            float x = a.x + Dp(18);
            if (!save.episode.empty())
            {
                Tag(ImVec2(x, a.y + Dp(16)), save.episode, Accent(0.7f));
                x += TagWidth(save.episode) + Dp(10);
            }
            Text(kBody, ImVec2(x, a.y + Dp(16)), White(1), save.mission, a.x + width - Dp(18) - x);
            Text(kSmall, a + ImVec2(Dp(18), Dp(16) + FontSize(kBody) + Dp(6)), White(0.65f),
                (save.date.empty() ? save.folder : save.date) + " · " + Size(save.bytes), width - Dp(36));
            const float button = ConfirmWidth("Borrar");
            ImGui::PushID(int(i));
            if (ConfirmButton("delete:" + save.folder, ImVec2(a.x + width - Dp(14) - button, a.y + rowHeight - Dp(14) - Dp(46)),
                    button, "Borrar", Icon::Trash, true))
            {
                const Save copy = save;
                ImGui::PopID();
                DeleteSave(copy);
                return;
            }
            ImGui::PopID();
        }

        Section(grid, g.backups.empty() ? "Copias de seguridad" : Trf("Copias de seguridad ({})", g.backups.size()).c_str());
        if (g.backups.empty())
        {
            const ImVec2 a = grid.Place(Dp(72), 2);
            Card(a, a + ImVec2(grid.CellWidth(2), Dp(72)));
            Text(kBody, a + ImVec2(Dp(18), Dp(36) - FontSize(kBody) / 2), White(0.75f), "Sin copias de seguridad");
        }
        for (size_t i = 0; i < g.backups.size(); ++i)
        {
            const Backup& backup = g.backups[i];
            const float width = grid.CellWidth();
            const ImVec2 a = grid.Place(rowHeight);
            Card(a, a + ImVec2(width, rowHeight));
            Text(kBody, a + ImVec2(Dp(18), Dp(16)), White(1), backup.date, width - Dp(36));
            Text(kSmall, a + ImVec2(Dp(18), Dp(16) + FontSize(kBody) + Dp(6)), White(0.65f),
                Trf("{} · {} {} · {}", Tr(backup.kind), backup.saves, Tr(backup.saves == 1 ? "partida" : "partidas"), Size(backup.bytes)),
                width - Dp(36));
            const float y = a.y + rowHeight - Dp(14) - Dp(46);
            const float remove = ConfirmWidth("Borrar"), restore = ConfirmWidth("Restaurar");
            ImGui::PushID(int(i));
            if (ConfirmButton("remove:" + backup.folder, ImVec2(a.x + width - Dp(14) - remove, y), remove, "Borrar", Icon::Trash, true))
            {
                std::error_code ec;
                fs::remove_all(BackupDir() / backup.folder, ec);
                ImGui::PopID();
                ScanBackups();
                Toast("Copia borrada");
                return;
            }
            if (ConfirmButton("restore:" + backup.folder, ImVec2(a.x + width - Dp(14) - remove - Dp(10) - restore, y), restore,
                    "Restaurar", Icon::Restore, false))
            {
                const Backup copy = backup;
                ImGui::PopID();
                RestoreBackup(copy);
                return;
            }
            ImGui::PopID();
        }
    }

    float ChoiceHeight() { return Dp(16) + FontSize(kBody) + Dp(6) + FontSize(kSmall) + Dp(16); }

    // A selectable row (drivers): its name and an optional tag, no secondary line.
    bool ChoiceRow(Grid& grid, const char* id, const std::string& title, bool selected, bool enabled, const std::string& tag,
        ImU32 tagFill, float reserve = 0)
    {
        const float height = ChoiceHeight();
        const float width = grid.CellWidth();
        const ImVec2 a = grid.Place(height), b = a + ImVec2(width, height);
        bool held = false;
        const bool tapped = Hit(id, a, ImVec2(b.x - reserve, b.y), &held, enabled);
        Glass(a, b, Dp(20), selected, held);
        const float radius = Dp(11), cx = a.x + Dp(18) + radius, cy = (a.y + b.y) / 2;
        ImDrawList* dl = DrawList();
        dl->AddCircle(ImVec2(cx, cy), radius, White(enabled ? 0.7f : 0.3f), 0, Dp(2));
        if (selected)
            dl->AddCircleFilled(ImVec2(cx, cy), radius - Dp(5), White(1));
        const float x = cx + radius + Dp(14);
        const float tagWidth = tag.empty() ? 0 : TagWidth(tag) + Dp(12);
        Text(kBody, ImVec2(x, cy - FontSize(kBody) / 2), White(enabled ? 1 : 0.5f), title, b.x - Dp(18) - reserve - tagWidth - x);
        if (!tag.empty())
            Tag(ImVec2(b.x - Dp(18) - reserve - TagWidth(tag), cy - (FontSize(kSmall) + Dp(8)) / 2), tag, tagFill);
        FocusRing(a, ImVec2(b.x - reserve, b.y), Dp(20));
        return tapped;
    }

    void ChooseDriver(const std::string& id)
    {
        g.config.driver = id;
        SaveConfig();
        BuildNotices();
        Toast(Trf("Driver: {}", id == "auto" ? Trf("automático ({})", DriverName(ActiveDriver())) : DriverName(id)));
    }

    void GpuPage(Grid& grid)
    {
#if XPORT_TITLE_ID == 0x4D5308AB
        // Gears of War 3: what can be dropped for speed, applied when the game starts
        // (games/gears_of_war_3/game.cpp).
        Section(grid, "Gráficos");
        SettingCard(grid, "##shadows", "Sombras", {"On", "Off"}, g.config.shadows);
        SettingCard(grid, "##effects", "Efectos", {"On", "Off"}, g.config.effects);
        Section(grid, "GPU");
#endif
        const bool ok = SystemDriverOk();
        const float rowHeight = Dp(30);
        const float height = Dp(16) + FontSize(kBody) + Dp(10) + std::size(kRequirements) * rowHeight + Dp(10);
        // The GPU.
        {
            const float width = grid.CellWidth();
            const ImVec2 a = grid.Place(height);
            Card(a, a + ImVec2(width, height));
            DrawList()->AddCircleFilled(a + ImVec2(Dp(40), Dp(40)), Dp(22), Accent(0.3f));
            DrawIcon(Icon::Gpu, a + ImVec2(Dp(40), Dp(40)), Dp(26), White(1));
            Text(kTitle, a + ImVec2(Dp(74), Dp(22)), White(1), GpuName(), width - Dp(92));
            float y = a.y + Dp(76);
            const auto line = [&](const std::string& label, const std::string& value)
            {
                Text(kSmall, ImVec2(a.x + Dp(18), y), White(0.6f), label);
                Text(kBody, ImVec2(a.x + Dp(18), y + FontSize(kSmall) + Dp(2)), White(0.95f), value, width - Dp(36));
                y += FontSize(kSmall) + FontSize(kBody) + Dp(12);
            };
            if (g.gpu && g.gpu->found)
            {
                line("Fabricante", GpuVendorName(g.gpu->vendor));
                line("Vulkan del sistema", fmt::format("{}.{}.{}", g.gpu->api >> 22, (g.gpu->api >> 12) & 0x3FF, g.gpu->api & 0xFFF));
                if (!g.gpu->driver.empty())
                    line("Driver del sistema", g.gpu->driver);
            }
            line("Driver que usará el juego", DriverName(ActiveDriver()));
        }
        // What the renderer needs from the phone's driver.
        {
            const float width = grid.CellWidth();
            const ImVec2 a = grid.Place(height);
            Card(a, a + ImVec2(width, height));
            Text(kBody, a + ImVec2(Dp(18), Dp(16)), White(0.92f), "Requisitos (driver del sistema)", width - Dp(36) - TagWidth("No compatible"));
            const char* status = ok ? "Compatible" : "No compatible";
            Tag(ImVec2(a.x + width - Dp(18) - TagWidth(status), a.y + Dp(14)), status, ok ? Rgba(40, 170, 90, 0.9f) : Rgba(220, 60, 60, 0.9f));
            float y = a.y + Dp(16) + FontSize(kBody) + Dp(10);
            for (const auto& requirement : kRequirements)
            {
                const bool met = g.gpu && g.gpu->found && g.gpu->missing.find(requirement[1]) == std::string::npos;
                DrawIcon(met ? Icon::Check : Icon::Cross, ImVec2(a.x + Dp(30), y + rowHeight / 2), Dp(18),
                    met ? Rgba(90, 220, 130, 1) : Rgba(255, 100, 100, 1));
                Text(kBody, ImVec2(a.x + Dp(50), y + (rowHeight - FontSize(kBody)) / 2), White(met ? 0.9f : 0.75f), requirement[0], width - Dp(68));
                y += rowHeight;
            }
        }

        Section(grid, "Driver Vulkan");
        if (!g.adreno)
        {
            ChoiceRow(grid, "##system", "Driver del sistema", true, true, ok ? "Compatible" : "No compatible",
                ok ? Rgba(40, 170, 90, 0.9f) : Rgba(220, 60, 60, 0.9f));
            return;
        }
        if (ChoiceRow(grid, "##auto", "Automático", g.config.driver == "auto", true, "", 0))
            ChooseDriver("auto");
        if (ChoiceRow(grid, "##system", "Driver del sistema", g.config.driver == "system", ok, ok ? "Compatible" : "No compatible",
                ok ? Rgba(40, 170, 90, 0.9f) : Rgba(220, 60, 60, 0.9f)))
            ChooseDriver("system");
        if (ChoiceRow(grid, "##turnip", kTurnipName, g.config.driver == "turnip", g.turnipPresent, "Incluido", Accent(0.6f)))
            ChooseDriver("turnip");
        for (size_t i = 0; i < g.drivers.size(); ++i)
        {
            const Driver& driver = g.drivers[i];
            ImGui::PushID(int(i));
            const float remove = driver.bundled ? 0.0f : ConfirmWidth("Borrar") + Dp(14);
            // An unusable one says so in its tag; its problem goes to the log.
            if (ChoiceRow(grid, "##driver", driver.name, g.config.driver == driver.id, driver.usable,
                    !driver.usable ? "No compatible" : driver.bundled ? "Incluido" : "Importado",
                    !driver.usable ? Rgba(220, 60, 60, 0.9f) : driver.bundled ? Accent(0.6f) : White(0.2f), remove))
                ChooseDriver(driver.id);
            // One the APK carries would be unpacked again at the next start: no Borrar.
            if (driver.bundled)
            {
                ImGui::PopID();
                continue;
            }
            // The row just placed: Place has moved past it in its row.
            const ImVec2 at = grid.origin + ImVec2((grid.column - 1) * (grid.CellWidth() + grid.gap), grid.y);
            const float height = ChoiceHeight();
            if (ConfirmButton("driver:" + driver.id, ImVec2(at.x + grid.CellWidth() - remove, at.y + (height - Dp(46)) / 2), remove - Dp(14),
                    "Borrar", Icon::Trash, true))
            {
                std::error_code ec;
                fs::remove_all(DriverDir() / driver.id, ec);
                const std::string name = driver.name;
                ImGui::PopID();
                if (g.config.driver == driver.id)
                    g.config.driver = "auto";
                ScanDrivers();
                SaveConfig();
                BuildNotices();
                Toast(Trf("Driver borrado: {}", name));
                return;
            }
            ImGui::PopID();
        }
        {
            const float height = Dp(16) + Dp(46) + Dp(16), width = grid.CellWidth(2);
            const ImVec2 a = grid.Place(height, 2);
            Card(a, a + ImVec2(width, height));
            const Icon importIcon = Icon::Import;
            const float button = ButtonWidth("Importar driver (.zip)", true);
            Text(kBody, ImVec2(a.x + Dp(18), a.y + (height - FontSize(kBody)) / 2), White(0.85f), "Drivers Turnip para Adreno",
                width - button - Dp(48));
            if (Button("##import", ImVec2(a.x + width - Dp(16) - button, a.y + Dp(16)), button, "Importar driver (.zip)", &importIcon, Accent(0.85f)))
                RequestDriverImport();
        }
    }

    void ControlsPage(Grid& grid)
    {
        SettingCard(grid, "##touch", "Controles táctiles", {"Auto", "Siempre", "Nunca"}, g.config.touch);
        SettingCard(grid, "##opacity", "Opacidad de botones", {"25 %", "50 %", "75 %", "100 %"}, g.config.opacity);
        SettingCard(grid, "##size", "Tamaño de botones", {"Pequeño", "Normal", "Grande"}, g.config.size);
        SettingCard(grid, "##vibration", "Vibración", {"On", "Off"}, g.config.vibration);
        const float height = SettingHeight(), width = grid.CellWidth(2);
        const ImVec2 a = grid.Place(height, 2);
        Card(a, a + ImVec2(width, height));
        const ImVec2 center = a + ImVec2(Dp(44), height / 2);
        DrawList()->AddCircleFilled(center, Dp(26), g.gamepad.empty() ? White(0.10f) : Accent(0.85f));
        DrawIcon(Icon::Controls, center, Dp(30), White(1));
        Text(kSmall, ImVec2(a.x + Dp(84), a.y + height / 2 - FontSize(kSmall) - Dp(2)), White(0.6f), "Mando");
        Text(kBody, ImVec2(a.x + Dp(84), a.y + height / 2 + Dp(2)), White(1),
            g.gamepad.empty() ? "Ningún mando conectado" : g.gamepad, width - Dp(102));
    }

#if XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6
    // Copiar (Último error): the report on the phone's clipboard, after a line with what it ran on,
    // to send it as it is.
    void CopyCrashReport()
    {
        const fs::path path = External() / "crash.txt";
        const std::string report = ReadText(path, 24 * 1024);
        if (report.empty())
        {
            Toast("El informe está vacío.", 1);
            return;
        }
        std::string date;
        struct stat info{};
        if (stat(path.c_str(), &info) == 0)
        {
            tm local{};
            localtime_r(&info.st_mtime, &local);
            char text[32];
            strftime(text, sizeof(text), "%d/%m/%Y %H:%M", &local);
            date = text;
        }
        RequestCopyText(fmt::format("{} v{} · {} · {} · {}\n\n{}", game::Title(), GetPlatform().version, GpuName(),
            GetPlatform().device, date, report));
        Toast("Informe copiado");
    }
#endif

    void SystemPage(Grid& grid)
    {
        SettingCard(grid, "##bgpause", "Pausar en segundo plano", {"On", "Off"}, g.config.bgPause);
        SettingCard(grid, "##fps", "Contador de FPS", {"Off", "Simple", "Detallado"}, g.config.fps);
        {
            Options names = {"Automático"};
            for (const Language& language : kLanguages)
                names.push_back(language.label);
            if (SettingCard(grid, "##language", "Idioma", names, g.config.language, 2))
                ApplyLanguage();
        }
        // The game files.
        {
            std::vector<std::string> lines;
            {
                std::lock_guard lock(g.check.mutex);
                lines = g.check.lines;
            }
            const float width = grid.CellWidth(2);
            const float lineHeight = FontSize(kBody) + Dp(8);
            const float header = Dp(16) + Dp(46) + Dp(12); // the title beside the button
            const float height = header + lines.size() * lineHeight + Dp(10);
            const ImVec2 a = grid.Place(std::max(height, Dp(96)), 2);
            Card(a, a + ImVec2(width, std::max(height, Dp(96))));
            const float button = ButtonWidth("Comprobar", true);
            Text(kBody, a + ImVec2(Dp(18), Dp(16) + (Dp(46) - FontSize(kBody)) / 2), White(0.92f), "Archivos del juego", width - button - Dp(52));
            const Icon check = Icon::Check;
            if (Button("##check", ImVec2(a.x + width - Dp(16) - button, a.y + Dp(16)), button, "Comprobar", &check, White(0.12f), !g.check.running))
            {
                ScanGame();
                BuildNotices();
                StartGameCheck();
            }
            float y = a.y + header;
            for (const std::string& line : lines)
            {
                if (line.empty())
                    continue;
                const bool found = line[0] == '+', missing = line[0] == '-';
                float x = a.x + Dp(18);
                if (found || missing)
                {
                    DrawIcon(found ? Icon::Check : Icon::Cross, ImVec2(x + Dp(9), y + FontSize(kBody) / 2), Dp(18),
                        found ? Rgba(90, 220, 130, 1) : Rgba(255, 100, 100, 1));
                    x += Dp(28);
                }
                Text(kBody, ImVec2(x, y), missing ? Rgba(255, 140, 140, 1) : White(0.85f), line.substr(1), a.x + width - Dp(18) - x);
                y += lineHeight;
            }
        }
        // The last error.
        std::error_code ec;
        const bool crashed = fs::is_regular_file(External() / "crash.txt", ec);
        {
            const float height = SettingHeight(), width = grid.CellWidth();
            const ImVec2 a = grid.Place(height);
            Card(a, a + ImVec2(width, height), crashed ? Rgba(255, 170, 40, 0.12f) : 0);
            Text(kBody, a + ImVec2(Dp(18), Dp(16)), White(0.92f), "Último error", width - Dp(36));
            if (crashed)
            {
                const Icon warning = Icon::Warning;
#if XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6
                // Copiar beside it: the report on the clipboard, to send it.
                const Icon copy = Icon::Copy;
                const float copyWidth = ButtonWidth("Copiar", true);
                const ImVec2 row = a + ImVec2(Dp(14), Dp(16) + FontSize(kBody) + Dp(12));
                if (Button("##crash", row, width - Dp(28) - copyWidth - Dp(10), "Ver informe", &warning, Rgba(255, 170, 40, 0.5f)))
                    Go(Page::Crash);
                if (Button("##crashcopy", ImVec2(a.x + width - Dp(14) - copyWidth, row.y), copyWidth, "Copiar", &copy, White(0.12f)))
                    CopyCrashReport();
#else
                if (Button("##crash", a + ImVec2(Dp(14), Dp(16) + FontSize(kBody) + Dp(12)), width - Dp(28), "Ver informe", &warning,
                        Rgba(255, 170, 40, 0.5f)))
                    Go(Page::Crash);
#endif
            }
            else
            {
                Text(kBody, a + ImVec2(Dp(18), Dp(16) + FontSize(kBody) + Dp(24)), White(0.6f), "Sin errores registrados", width - Dp(36));
            }
        }
        // Version and reset.
        {
            const float height = SettingHeight(), width = grid.CellWidth();
            const ImVec2 a = grid.Place(height);
            Card(a, a + ImVec2(width, height));
            Text(kBody, a + ImVec2(Dp(18), Dp(16)), White(0.92f), Trf("Versión {}", GetPlatform().version), width - Dp(36));
            const float button = ConfirmWidth("Restablecer ajustes");
            if (ConfirmButton("reset", ImVec2(a.x + width - Dp(14) - button, a.y + height - Dp(14) - Dp(46)), button,
                    "Restablecer ajustes", Icon::Restore, true))
            {
                const bool welcomeShown = g.config.welcomeShown; // not a setting: it stays shown
                g.config = Config{};
                g.config.welcomeShown = welcomeShown;
                SaveConfig();
                ApplyLanguage();
                Toast("Ajustes restablecidos");
            }
        }
    }

    void CreditsPage(Grid& grid)
    {
        {
            const float height = Dp(20) + FontSize(kSmall) + Dp(4) + FontSize(kHuge) + Dp(20), width = grid.CellWidth(grid.columns);
            const ImVec2 a = grid.Place(height, grid.columns);
            Glass(a, a + ImVec2(width, height), Dp(22), true);
            DrawList()->AddCircleFilled(a + ImVec2(Dp(52), height / 2), Dp(30), Accent(0.95f));
            DrawIcon(Icon::Credits, a + ImVec2(Dp(52), height / 2), Dp(32), White(1));
            Text(kSmall, a + ImVec2(Dp(98), Dp(20)), White(0.75f), "Port Android");
            Text(kHuge, a + ImVec2(Dp(98), Dp(20) + FontSize(kSmall) + Dp(4)), White(1), "Jauidev", width - Dp(116));
        }
        const float height = Dp(14) + FontSize(kBody) + Dp(4) + FontSize(kSmall) + Dp(14);
        for (const auto& credit : kCredits)
        {
            const float width = grid.CellWidth();
            const ImVec2 a = grid.Place(height);
            Card(a, a + ImVec2(width, height));
            Text(kBody, a + ImVec2(Dp(16), Dp(14)), White(1), credit[0], width - Dp(32));
            Text(kSmall, a + ImVec2(Dp(16), Dp(14) + FontSize(kBody) + Dp(4)), White(0.65f), credit[1], width - Dp(32));
        }
        {
            const std::string legal = Trf("Port no oficial de {}: necesitas tu propia copia del juego. El juego y sus marcas pertenecen a sus propietarios.",
                game::Title());
            const float width = grid.CellWidth(grid.columns);
            const float height = Measure(kSmall, legal, width - Dp(36)).y + Dp(28);
            const ImVec2 a = grid.Place(height, grid.columns);
            Card(a, a + ImVec2(width, height));
            TextWrapped(kSmall, a + ImVec2(Dp(18), Dp(14)), White(0.7f), legal, width - Dp(36));
        }
    }

    void CrashPage(Grid& grid)
    {
        const float width = grid.CellWidth(grid.columns);
        const std::string text = g.crash.empty() ? "El informe está vacío." : g.crash;
        {
            const float height = Dp(16) + Dp(46) + Dp(16);
            const ImVec2 a = grid.Place(height, grid.columns);
            Card(a, a + ImVec2(width, height), Rgba(255, 170, 40, 0.10f));
#if XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6
            // Copiar beside Borrar informe: the report on the clipboard, to send it.
            const float copyWidth = ButtonWidth("Copiar", true) + Dp(10);
#else
            const float copyWidth = 0;
#endif
            Text(kBody, ImVec2(a.x + Dp(18), a.y + (height - FontSize(kBody)) / 2), White(0.92f), g.crashDate.empty() ? "Último error"
                : Trf("Último error · {}", g.crashDate), width - ConfirmWidth("Borrar informe") - copyWidth - Dp(48));
            const float button = ConfirmWidth("Borrar informe");
#if XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6
            const Icon copy = Icon::Copy;
            if (Button("##crashcopy", ImVec2(a.x + width - Dp(16) - button - copyWidth, a.y + Dp(16)), copyWidth - Dp(10), "Copiar",
                    &copy, White(0.12f)))
                CopyCrashReport();
#endif
            if (ConfirmButton("crash", ImVec2(a.x + width - Dp(16) - button, a.y + Dp(16)), button, "Borrar informe", Icon::Trash, true))
            {
                std::error_code ec;
                fs::remove(External() / "crash.txt", ec);
                BuildNotices();
                Toast("Informe borrado");
                Go(Page::System);
                return;
            }
        }
        const float height = Measure(kSmall, text, width - Dp(36)).y + Dp(32);
        const ImVec2 a = grid.Place(height, grid.columns);
        Card(a, a + ImVec2(width, height));
        TextWrapped(kSmall, a + ImVec2(Dp(18), Dp(16)), White(0.85f), text, width - Dp(36));
    }

    // The game in Juego: tapped when it can start, Jugar;
    // otherwise the reason (note) as a toast, or, without one, true for the caller to act.
    bool GameTile(const char* id, ImVec2 a, ImVec2 b, const char* title, Font font, int game, bool ready, bool found, const std::string& status,
        const std::string& note)
    {
        bool held = false;
        const bool tapped = Hit(id, a, b, &held);
        const float rounding = Dp(26), pad = Dp(22), width = b.x - a.x;
        Glass(a, b, rounding, ready, held);
        const float radius = std::clamp((b.y - a.y) * 0.13f, Dp(24), Dp(36));
        const ImVec2 center = a + ImVec2(pad + radius, pad + radius);
        PlayButton(center, radius, ready, false);
        Tag(ImVec2(b.x - pad - TagWidth(status), a.y + pad), status,
            ready ? Rgba(40, 170, 90, 0.9f) : found ? Rgba(255, 170, 40, 0.75f) : White(0.18f));
        // The name and its tag only; note (why it cannot start) is the toast of a tap.
        Text(font, ImVec2(a.x + pad, b.y - pad - FontSize(font)), White(ready ? 1 : 0.75f), title, width - 2 * pad);
        FocusRing(a, b, rounding);
        if (!tapped)
            return false;
        if (ready)
            Play();
        else if (!note.empty())
            Toast(note, 1);
        return !ready && note.empty();
    }

    // Importar ISO, under the games: the disc image unpacked into its game's folder (launcher_iso.cpp),
    // its progress while it runs. The same game already there is replaced once the new one is whole.
    float IsoCardHeight() { return Dp(16) + Dp(46) + Dp(16); }

    void IsoCard(Grid& grid)
    {
        const float height = IsoCardHeight(), width = grid.CellWidth(grid.columns);
        const ImVec2 a = grid.Place(height, grid.columns);
        Card(a, a + ImVec2(width, height));
        if (g.importing)
        {
            const IsoStatus status = IsoImportStatus();
            const float share = status.total ? std::clamp(float(double(status.done) / double(status.total)), 0.f, 1.f) : 0;
            const char* label = "Cancelar";
            const float button = ButtonWidth(label, true);
            const float left = a.x + Dp(18), right = a.x + width - Dp(16) - button - Dp(18);
            const float top = a.y + (height - FontSize(kBody) - Dp(18)) / 2;
            Text(kBody, ImVec2(left, top), White(0.92f), Trf("Importando… {} %", int(share * 100)), right - left);
            const ImVec2 pa(left, top + FontSize(kBody) + Dp(12)), pb(right, top + FontSize(kBody) + Dp(18));
            DrawList()->AddRectFilled(pa, pb, White(0.14f), Dp(3));
            if (share > 0)
                DrawList()->AddRectFilled(pa, ImVec2(pa.x + (pb.x - pa.x) * share, pb.y), Accent(0.95f), Dp(3));
            const Icon cross = Icon::Cross;
            if (Button("##isocancel", ImVec2(a.x + width - Dp(16) - button, a.y + Dp(16)), button, label, &cross, White(0.12f),
                    !status.cancelling))
                IsoImportCancel();
            return;
        }
        const char* label = "Importar ISO";
        const float button = ButtonWidth(label, true);
        Text(kBody, ImVec2(a.x + Dp(18), a.y + (height - FontSize(kBody)) / 2), White(0.85f), "Disco del juego", width - button - Dp(48));
        const ImVec2 at(a.x + width - Dp(16) - button, a.y + Dp(16));
        const Icon import = Icon::Import;
        if (Button("##iso", at, button, label, &import, Accent(0.85f)))
            RequestIsoImport();
    }

    bool GameInFolder(int) { return g.game != 0; }

    // The game's files, not its saves.
    void DeleteGame(int game)
    {
        if (!GameDelete(game))
        {
            Toast("No se pudo borrar el juego", 2);
            return;
        }
        ScanGame();
        BuildNotices();
        Toast("Juego borrado");
    }

    // Borrar juego: a button for each game that is there, each asking again.
    void DeleteCard(Grid& grid)
    {
        const float height = IsoCardHeight(), width = grid.CellWidth(grid.columns);
        const ImVec2 a = grid.Place(height, grid.columns);
        Card(a, a + ImVec2(width, height));
        float right = a.x + width - Dp(16);
        const struct { int game; const char* title; } games[] = {{kThisGame, game::Title()}};
        for (const auto& game : games)
        {
            if (!GameInFolder(game.game))
                continue;
            const float button = ConfirmWidth(game.title);
            if (ConfirmButton(game.game == 1 ? "deletegame1" : "deletegame2", ImVec2(right - button, a.y + Dp(16)), button, game.title, Icon::Trash, true))
                DeleteGame(game.game);
            right -= button + Dp(10);
        }
        Text(kBody, ImVec2(a.x + Dp(18), a.y + (height - FontSize(kBody)) / 2), White(0.85f), "Borrar juego", right - a.x - Dp(18));
    }

    // Each frame while Importar ISO runs: once it ends, the game is looked for again and the result told.
    void PollImport()
    {
        if (!g.importing)
            return;
        const IsoStatus status = IsoImportStatus();
        if (status.running)
            return;
        g.importing = false;
        ScanGame();
        BuildNotices();
        if (status.error.empty())
        {
            Toast("Juego importado");
            return;
        }
        if (status.error == "cancel")
        {
            Toast("Importación cancelada", 1);
            return;
        }
        static const char* const reasons[][2] = {
            {"open", "no se pudo abrir el archivo"},
            {"read", "error al leer la ISO"},
            {"write", "no se pudo escribir en el almacenamiento"},
            {"move", "no se pudo colocar la carpeta del juego"},
        };
        // The wrong file gets the panel with what the ISO must be, not a toast.
        g.isoError = status.error == "version" ? "Esta ISO es de otra versión del juego: importa el disco con el que se hizo el port, sin actualizaciones"
            : status.error == "game" ? "Esta ISO no es del juego de este port"
            : status.error == "disc" ? "Este archivo no es una ISO de Xbox 360" : nullptr;
        if (g.isoError)
        {
            g.isoErrorStart = g.clock;
            return;
        }
        std::string reason = status.error;
        for (const auto& known : reasons)
        {
            if (status.error == known[0])
                reason = Tr(known[1]);
        }
        if (status.error == "space")
            reason = Trf("hacen falta {} GB libres", Decimal(status.needed / (1024.0 * 1024 * 1024), 1));
        Toast(Trf("No se pudo importar la ISO: {}", reason), 2);
    }

    // The game's tile.
    void GameCell(Grid& grid, float height, const char* id, const char* title, Font font, int game)
    {
        const bool found = g.game == game;
        const bool missing = g.game == 0;
        const bool ok = g.xexOk;
        const Notice* block = Blocking();
        const std::string status(Tr(g.importing ? "Importando…" : !found ? "No encontrado" : !ok ? "No compatible"
            : block ? "No disponible" : "Listo"));
        // Without the game, a tap opens the file picker for its ISO.
        const std::string note = g.importing ? std::string(Tr("Espera a que termine la importación"))
            : missing ? std::string() : block ? block->text : status;
        const ImVec2 a = grid.Place(height, grid.columns);
        if (GameTile(id, a, a + ImVec2(grid.CellWidth(grid.columns), height), title, font, game, found && ok && !block && !g.importing, found, status, note))
            RequestIsoImport();
    }

    // Juego: the game's tile, Importar ISO and Borrar juego.
    void GamesPage(Grid& grid)
    {
        const float width = grid.CellWidth(grid.columns);
        // The tile fills the page above Importar ISO.
        const bool deletable = !g.importing && GameInFolder(kThisGame);
        const float cards = IsoCardHeight() + (deletable ? IsoCardHeight() + grid.gap : 0);
        const float height = std::max(Dp(180), g.viewHeight - cards - grid.gap - Dp(4));
        const char* const title = game::Title();
        const Font font = Measure(kHuge, title).x <= width - 2 * Dp(22) ? kHuge : kTitle;
        GameCell(grid, height, "##game", title, font, kThisGame);
        IsoCard(grid);
        if (deletable)
            DeleteCard(grid);
    }

    const char* PageTitle(Page page)
    {
        switch (page)
        {
        case Page::Saves: return "Partidas";
#if XPORT_TITLE_ID == 0x4D5308AB
        case Page::Gpu: return "Gráficos y GPU";
#else
        case Page::Gpu: return "GPU y drivers";
#endif
        case Page::Controls: return "Controles";
        case Page::System: return "Sistema";
        case Page::Credits: return "Créditos";
        case Page::Crash: return "Informe de error";
        case Page::Games: return "Juego";
        default: return "";
        }
    }

    // Touch scrolling of the page (inside its child window): drag, fling, and smooth scrolls.
    void Scroll(ImVec2 a, ImVec2 b)
    {
        Scroller& s = g.scroll;
        const ImGuiIO& io = ImGui::GetIO();
        const bool inside = ImRect(a, b).Contains(io.MousePos) && !g.noticesOpen;
        if (io.MouseClicked[0])
        {
            s.armed = inside;
            s.dragging = false;
            s.stopped = inside && std::fabs(s.velocity) > Dp(150);
            s.velocity = 0;
            s.target = -1;
        }
        const float max = ImGui::GetScrollMaxY();
        float y = ImGui::GetScrollY();
        if (s.armed && io.MouseDown[0])
        {
            if (!s.dragging && io.MouseDragMaxDistanceSqr[0] > Dp(10) * Dp(10))
                s.dragging = true;
            if (s.dragging)
            {
                y -= io.MouseDelta.y;
                s.velocity = ImLerp(s.velocity, -io.MouseDelta.y / std::max(g.dt, 1e-3f), 0.35f);
            }
        }
        else
        {
            s.armed = false;
            if (s.target >= 0)
            {
                y = ImLerp(y, std::min(s.target, max), 1 - std::exp(-12 * g.dt));
                if (std::fabs(y - std::min(s.target, max)) < 0.5f || g.clock - s.targetStart > 0.8f)
                    s.target = -1;
            }
            else if (std::fabs(s.velocity) > Dp(20))
            {
                y += s.velocity * g.dt;
                s.velocity *= std::exp(-4.f * g.dt);
                if (y <= 0 || y >= max)
                    s.velocity = 0;
            }
            else
            {
                s.velocity = 0;
            }
        }
        y = std::clamp(y, 0.f, std::max(max, 0.f));
        if (y != ImGui::GetScrollY())
            ImGui::SetScrollY(y);
    }

    void PageBody(ImVec2 a, ImVec2 b, float slide)
    {
        if (g.resetScroll)
        {
            ImGui::SetNextWindowScroll(ImVec2(0, 0));
            g.resetScroll = false;
        }
        ImGui::SetCursorScreenPos(a);
        ImGui::BeginChild("##page", b - a, ImGuiChildFlags_None,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
        Scroll(a, b);
        g.viewHeight = b.y - a.y;
        Grid grid;
        grid.origin = ImGui::GetCursorScreenPos() + ImVec2(0, slide);
        grid.width = b.x - a.x;
        grid.gap = Dp(12);
        grid.columns = grid.width >= Dp(620) ? 2 : 1;
        if (g.page == Page::Credits)
            grid.columns = std::clamp(int(grid.width / Dp(250)), 1, 4);
        switch (g.page)
        {
        case Page::Saves: SavesPage(grid); break;
        case Page::Gpu: GpuPage(grid); break;
        case Page::Controls: ControlsPage(grid); break;
        case Page::System: SystemPage(grid); break;
        case Page::Credits: CreditsPage(grid); break;
        case Page::Crash: CrashPage(grid); break;
        case Page::Games: GamesPage(grid); break;
        default: break;
        }
        const float height = grid.End();
        ImGui::SetCursorScreenPos(grid.origin + ImVec2(0, std::max(0.f, height - grid.gap)));
        ImGui::Dummy(ImVec2(1, Dp(4)));
        ImGui::EndChild();
    }

    // ---------- home ----------

    // Jugar, always as it is when the game can start (the user's choice, 0.11.3D: no lock, no "No
    // disponible"); what blocks it is in Avisos and on the game's tile in Elegir juego.
    void PlayTile(ImVec2 a, ImVec2 b)
    {
        bool held = false;
        const bool tapped = Hit("##play", a, b, &held);
        const float rounding = Dp(26);
        Glass(a, b, rounding, true, held);
        const float pad = Dp(22), width = b.x - a.x;
        const float radius = std::clamp((b.y - a.y) * 0.17f, Dp(24), Dp(40));
        const ImVec2 center = a + ImVec2(pad + radius, pad + radius);
        PlayButton(center, radius, true, false);
        // Only its name, no lines under it (the user's choice).
        Text(kHuge, ImVec2(a.x + pad, b.y - pad - FontSize(kHuge)), White(1), "Jugar", width - 2 * pad);
        FocusRing(a, b, rounding);
        if (tapped)
            Go(Page::Games);
    }

    void Tile(const char* id, ImVec2 a, ImVec2 b, Icon icon, const char* title, Page page, int alert)
    {
        bool held = false;
        const bool tapped = Hit(id, a, b, &held);
        const float rounding = Dp(22), pad = Dp(16), width = b.x - a.x;
        Glass(a, b, rounding, false, held);
        const float radius = std::clamp((b.y - a.y) * 0.16f, Dp(18), Dp(24));
        const ImVec2 center = a + ImVec2(pad + radius, pad + radius);
        DrawList()->AddCircleFilled(center, radius, Accent(0.30f));
        DrawIcon(icon, center, radius * 1.1f, White(1));
        if (alert >= 1)
            DrawList()->AddCircleFilled(ImVec2(b.x - pad - Dp(5), a.y + pad + Dp(5)), Dp(6), Level(alert, 1));
        // Only the title, at the bottom (no secondary line: the user's choice). One too long for a
        // narrow tile (GPU y drivers on the Redmi) takes the body font.
        const Font font = Measure(kTitle, title).x <= width - 2 * pad ? kTitle : kBody;
        Text(font, ImVec2(a.x + pad, b.y - pad - FontSize(font)), White(1), title, width - 2 * pad);
        FocusRing(a, b, rounding);
        if (tapped)
            Go(page);
    }

    void Home(ImVec2 a, ImVec2 b)
    {
        const float gap = Dp(12), width = b.x - a.x, height = b.y - a.y;
        struct Entry
        {
            const char* id;
            Icon icon;
            const char* title;
            Page page;
            int alert;
        };
        int gpuAlert = -1, systemAlert = -1;
        for (const Notice& notice : g.notices)
        {
            int& alert = notice.page == Page::Gpu ? gpuAlert : systemAlert;
            alert = std::max(alert, notice.level);
        }
        const Entry entries[] = {
            {"##game", Icon::Console, "Juego", Page::Games, -1},
            {"##saves", Icon::Saves, "Partidas", Page::Saves, -1},
#if XPORT_TITLE_ID == 0x4D5308AB
            {"##gpu", Icon::Graphics, "Gráficos y GPU", Page::Gpu, gpuAlert},
#else
            {"##gpu", Icon::Gpu, "GPU y drivers", Page::Gpu, gpuAlert},
#endif
            {"##controls", Icon::Controls, "Controles", Page::Controls, -1},
            {"##system", Icon::System, "Sistema", Page::System, systemAlert},
            {"##credits", Icon::Credits, "Créditos", Page::Credits, -1},
        };
        if (width >= height)
        {
            // Jugar takes two of five columns, the pages a 3x2 grid beside it.
            const float cell = (width - 4 * gap) / 5, tile = (height - gap) / 2;
            PlayTile(a, ImVec2(a.x + 2 * cell + gap, b.y));
            for (int i = 0; i < 6; ++i)
            {
                const ImVec2 at(a.x + (2 + i % 3) * (cell + gap), a.y + (i / 3) * (tile + gap));
                const Entry& e = entries[i];
                Tile(e.id, at, at + ImVec2(cell, tile), e.icon, e.title, e.page, e.alert);
            }
        }
        else
        {
            const float play = std::min(height * 0.3f, Dp(230));
            PlayTile(a, ImVec2(b.x, a.y + play));
            const float cell = (width - gap) / 2, tile = (height - play - 3 * gap) / 3;
            for (int i = 0; i < 6; ++i)
            {
                const ImVec2 at(a.x + (i % 2) * (cell + gap), a.y + play + gap + (i / 2) * (tile + gap));
                const Entry& e = entries[i];
                Tile(e.id, at, at + ImVec2(cell, tile), e.icon, e.title, e.page, e.alert);
            }
        }
    }

    // ---------- the bar, notices and toasts ----------

    void TopBar(ImVec2 a, float width)
    {
        const float height = Dp(46);
        float left = a.x, right = a.x + width;
        // The bell: the notices.
        {
            const ImVec2 ba(right - height, a.y), bb(right, a.y + height);
            bool held = false;
            const bool tapped = Hit("##bell", ba, bb, &held);
            Glass(ba, bb, height / 2, false, held);
            DrawIcon(Icon::Bell, (ba + bb) * 0.5f, Dp(22), White(1));
            if (!g.notices.empty())
            {
                const ImVec2 c(bb.x - Dp(9), ba.y + Dp(9));
                DrawList()->AddCircleFilled(c, Dp(10), Level(WorstNotice(), 1));
                TextCentered(kSmall, c, White(1), std::to_string(g.notices.size()));
            }
            FocusRing(ba, bb, height / 2);
            if (tapped)
                g.noticesOpen = true;
            right -= height + Dp(10);
        }
        // The version and the GPU, when there is room.
        const auto chip = [&](const char* id, const std::string& text, const Icon* icon, Page page)
        {
            const float w = Measure(kSmall, text).x + Dp(32) + (icon ? Dp(26) : 0);
            if (right - w < left + Dp(260))
                return;
            const ImVec2 ca(right - w, a.y), cb(right, a.y + height);
            bool held = false;
            const bool tapped = Hit(id, ca, cb, &held);
            Glass(ca, cb, height / 2, false, held);
            float x = ca.x + Dp(16);
            if (icon)
            {
                DrawIcon(*icon, ImVec2(x + Dp(10), a.y + height / 2), Dp(20), White(0.95f));
                x += Dp(26);
            }
            Text(kSmall, ImVec2(x, a.y + (height - FontSize(kSmall)) / 2), White(0.92f), text);
            FocusRing(ca, cb, height / 2);
            if (tapped)
                Go(page);
            right -= w + Dp(10);
        };
        chip("##version", "v" + GetPlatform().version, nullptr, Page::System);
        const Icon gpu = Icon::Gpu;
        chip("##gpuchip", GpuName(), &gpu, Page::Gpu);
        // Back and the title.
        if (g.page != Page::Home)
        {
            const ImVec2 ba(left, a.y), bb(left + height, a.y + height);
            bool held = false;
            const bool tapped = Hit("##back", ba, bb, &held);
            Glass(ba, bb, height / 2, false, held);
            DrawIcon(Icon::Back, (ba + bb) * 0.5f, Dp(22), White(1));
            FocusRing(ba, bb, height / 2);
            if (tapped)
                Go(Page::Home);
            left += height + Dp(10);
        }
        // The page's title; the home page has none (the user's choice: the game's name was there).
        if (g.page == Page::Home)
            return;
        const char* title = PageTitle(g.page);
        const float w = std::min(Measure(kTitle, title).x + Dp(40), right - left);
        Glass(ImVec2(left, a.y), ImVec2(left + w, a.y + height), height / 2);
        Text(kTitle, ImVec2(left + Dp(20), a.y + (height - FontSize(kTitle)) / 2), White(1), title, w - Dp(40));
    }

    void NoticesOverlay(float screenWidth, float screenHeight, float margin, float top)
    {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(screenWidth, screenHeight));
        ImGui::SetNextWindowFocus();
        ImGui::Begin("##notices", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
        ImDrawList* dl = DrawList();
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(screenWidth, screenHeight), Rgba(0, 0, 0, 0.5f));
        const float width = std::min(Dp(580), screenWidth - 2 * margin);
        const ImVec2 a((screenWidth - width) / 2, top);
        const float textWidth = width - Dp(18) - Dp(36) - Dp(18);
        float height = Dp(18) + FontSize(kTitle) + Dp(16);
        std::vector<float> rows;
        for (const Notice& notice : g.notices)
        {
            rows.push_back(std::max(Dp(60), Measure(kBody, notice.text, textWidth).y + Dp(28)));
            height += rows.back() + Dp(8);
        }
        if (g.notices.empty())
            height += Dp(56);
        height = std::min(height + Dp(10), screenHeight - top - margin);
        const ImVec2 b = a + ImVec2(width, height);
        Glass(a, b, Dp(24));
        Text(kTitle, a + ImVec2(Dp(20), Dp(18)), White(1), "Avisos");
        bool close = false;
        {
            const float size = Dp(40);
            const ImVec2 ca(b.x - Dp(14) - size, a.y + Dp(14)), cb = ca + ImVec2(size, size);
            bool held = false;
            close |= Hit("##close", ca, cb, &held);
            dl->AddCircleFilled((ca + cb) * 0.5f, size / 2, White(held ? 0.22f : 0.10f));
            DrawIcon(Icon::Cross, (ca + cb) * 0.5f, Dp(20), White(1));
            FocusRing(ca, cb, size / 2);
        }
        float y = a.y + Dp(18) + FontSize(kTitle) + Dp(16);
        ImGui::PushClipRect(a, b, true);
        if (g.notices.empty())
            Text(kBody, ImVec2(a.x + Dp(20), y + Dp(14)), White(0.7f), "No hay avisos");
        for (size_t i = 0; i < g.notices.size(); ++i)
        {
            const Notice& notice = g.notices[i];
            const ImVec2 ra(a.x + Dp(12), y), rb(b.x - Dp(12), y + rows[i]);
            ImGui::PushID(int(i));
            bool held = false;
            const bool tapped = Hit("##notice", ra, rb, &held);
            ImGui::PopID();
            dl->AddRectFilled(ra, rb, Level(notice.level, held ? 0.32f : 0.18f), Dp(16));
            DrawIcon(notice.level == 0 ? Icon::Info : Icon::Warning, ImVec2(ra.x + Dp(24), (ra.y + rb.y) / 2), Dp(22), Level(notice.level, 1));
            const float textHeight = Measure(kBody, notice.text, textWidth).y;
            TextWrapped(kBody, ImVec2(ra.x + Dp(48), (ra.y + rb.y - textHeight) / 2), White(0.95f), notice.text, textWidth);
            FocusRing(ra, rb, Dp(16));
            if (tapped)
            {
                close = true;
                if (notice.page != Page::Home)
                    Go(notice.page);
            }
            y += rows[i] + Dp(8);
        }
        ImGui::PopClipRect();
        // The panel itself takes taps; one outside it closes.
        Hit("##panel", a, b);
        if (Hit("##outside", ImVec2(0, 0), ImVec2(screenWidth, screenHeight)))
            close = true;
        ImGui::End();
        if (close)
            g.noticesOpen = false;
    }

    // The first open without the game (once: launcher.cfg's welcome_shown): a glass panel over the
    // blurred home (AddBackdropBlur) with what the ISO must be, in tiles, and Importar ISO. Ahora no,
    // back or a tap outside close it.
    // What the ISO must be, over the blurred page: Importa tu ISO on the first open without a game, or
    // title and sentence of a file Importar ISO refused.
    void IsoOverlay(float screenWidth, float screenHeight, float margin, const char* title, const char* requirement, float start, bool error)
    {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(screenWidth, screenHeight));
        ImGui::SetNextWindowFocus();
        ImGui::Begin("##welcome", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
        ImDrawList* dl = DrawList();
        AddBackdropBlur(dl);
        const float appear = std::clamp((g.clock - start) / 0.3f, 0.f, 1.f), ease = 1 - (1 - appear) * (1 - appear);
        g.fade = ease;
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(screenWidth, screenHeight), Rgba(0, 0, 0, 0.30f));

        const float pad = Dp(22), gap = Dp(10), icon = Dp(26);
        const float width = std::min(Dp(640), screenWidth - 2 * margin), inner = width - 2 * pad;
        const float sentence = Measure(kBody, requirement, inner).y;
        const float fixed = pad + 2 * icon + Dp(14) + sentence + Dp(16) + Dp(18) + Dp(46) + pad;
        const float cell = std::clamp(screenHeight - 2 * margin - fixed, Dp(64), Dp(104));
        const float height = fixed + cell;
        const ImVec2 a((screenWidth - width) / 2, std::max(margin, (screenHeight - height) / 2) + Dp(14) * (1 - ease)), b = a + ImVec2(width, height);
        Glass(a, b, Dp(26));

        // The title beside its icon, the requirement under them.
        const ImVec2 center = a + ImVec2(pad + icon, pad + icon);
        dl->AddCircleFilled(center, icon, error ? Rgba(220, 60, 60, 0.9f) : Accent(0.85f));
        DrawIcon(error ? Icon::Warning : Icon::Import, center, Dp(28), White(1));
        Text(kTitle, ImVec2(center.x + icon + Dp(14), center.y - FontSize(kTitle) / 2), White(1), title, b.x - pad - center.x - icon - Dp(14));
        float y = a.y + pad + 2 * icon + Dp(14);
        TextWrapped(kBody, ImVec2(a.x + pad, y), White(0.85f), requirement, inner);
        y += sentence + Dp(16);

        // The requirements as tiles: the game, the console, the region.
        struct Need
        {
            Icon icon;
            const char* label;
        };
        const Need needs[] = {{Icon::Play, game::Title()}, {Icon::Console, "Xbox 360"}, {Icon::Check, "Versión compatible"}};
        const float tile = (inner - gap * (std::size(needs) - 1)) / std::size(needs);
        for (size_t i = 0; i < std::size(needs); ++i)
        {
            const ImVec2 ta(a.x + pad + i * (tile + gap), y), tb = ta + ImVec2(tile, cell);
            Glass(ta, tb, Dp(18));
            const float radius = std::clamp(cell * 0.2f, Dp(14), Dp(18));
            const ImVec2 c = ta + ImVec2(Dp(14) + radius, Dp(14) + radius);
            dl->AddCircleFilled(c, radius, Accent(0.30f));
            DrawIcon(needs[i].icon, c, radius * 1.1f, White(1));
            const Font font = Measure(kBody, needs[i].label).x <= tile - Dp(28) ? kBody : kSmall;
            Text(font, ImVec2(ta.x + Dp(14), tb.y - Dp(12) - FontSize(font)), White(1), needs[i].label, tile - Dp(28));
        }
        y += cell + Dp(18);

        // Importar ISO, and Ahora no (Cerrar after an error) to its left.
        bool close = false;
        const Icon import = Icon::Import;
        const char* const later = error ? "Cerrar" : "Ahora no";
        const float importWidth = ButtonWidth("Importar ISO", true), laterWidth = ButtonWidth(later, false);
        if (Button("##welcomeimport", ImVec2(b.x - pad - importWidth, y), importWidth, "Importar ISO", &import, Accent(0.85f)))
        {
            close = true;
            RequestIsoImport();
        }
        close |= Button("##welcomelater", ImVec2(b.x - pad - importWidth - gap - laterWidth, y), laterWidth, later, nullptr, White(0.12f));
        // The panel itself takes taps; one outside it closes.
        Hit("##welcomepanel", a, b);
        if (Hit("##welcomeoutside", ImVec2(0, 0), ImVec2(screenWidth, screenHeight)))
            close = true;
        g.fade = 1;
        ImGui::End();
        if (close)
        {
            g.welcome = false;
            g.isoError = nullptr;
        }
    }

    void DrawToasts(float screenWidth, float top)
    {
        g.drawList = ImGui::GetForegroundDrawList();
        float y = top;
        for (auto it = g.toasts.begin(); it != g.toasts.end();)
        {
            const float life = it->level >= 2 ? 5.f : 3.2f, age = g.clock - it->start;
            if (age > life)
            {
                it = g.toasts.erase(it);
                continue;
            }
            g.fade = std::min({1.f, age / 0.15f, (life - age) / 0.3f});
            const float maxWidth = std::min(Dp(640), screenWidth - Dp(48));
            const float textWidth = std::min(Measure(kBody, it->text).x, maxWidth - Dp(68));
            const float textHeight = Measure(kBody, it->text, textWidth + 1).y;
            const float width = textWidth + Dp(68), height = std::max(Dp(50), textHeight + Dp(24));
            const ImVec2 a((screenWidth - width) / 2, y), b = a + ImVec2(width, height);
            Glass(a, b, std::min(height / 2, Dp(25)), false, false, Level(it->level, 0.28f));
            DrawIcon(it->level == 0 ? Icon::Check : it->level == 1 ? Icon::Info : Icon::Warning, ImVec2(a.x + Dp(28), (a.y + b.y) / 2), Dp(20),
                it->level == 0 ? White(1) : Level(it->level, 1));
            TextWrapped(kBody, ImVec2(a.x + Dp(48), (a.y + b.y - textHeight) / 2), White(1), it->text, textWidth + 1);
            y += height + Dp(8);
            ++it;
        }
        g.fade = 1;
        g.drawList = nullptr;
    }

    void DrawBackground(float width, float height)
    {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        if (g.background.id)
        {
            const float scale = std::max(width / g.background.width, height / g.background.height);
            g.coverSize = ImVec2(g.background.width * scale, g.background.height * scale);
            g.coverOrigin = (ImVec2(width, height) - g.coverSize) * 0.5f;
            dl->AddImage(TextureOf(g.background), g.coverOrigin, g.coverOrigin + g.coverSize);
        }
        else
        {
            g.coverOrigin = ImVec2(0, 0);
            g.coverSize = ImVec2(width, height);
            dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(width, height), Accent(0.5f), Tint(1), Tint(1), Accent(0.25f));
        }
        // Darker toward the bottom, where the text sits.
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(width, height), Rgba(0, 0, 0, 0.10f), Rgba(0, 0, 0, 0.10f),
            Rgba(0, 0, 0, 0.42f), Rgba(0, 0, 0, 0.42f));
    }
}

void UiInit()
{
    g.dp = std::max(1.f, GetPlatform().density);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigDebugHighlightIdConflicts = false; // its tooltip would be ImGui's default window
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(0, 0);
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 0;
    style.ItemSpacing = ImVec2(0, 0);
    style.ScrollbarSize = 0;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);

    // Roboto at four sizes: Latin-1 (ñ, á, ·, ×, ©) and the punctuation block (…, —).
    g.fontData = ReadAsset("launcher/Roboto-Medium.ttf");
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0};
    if (g.fontData.empty())
    {
        LOGN_ERROR("launcher: launcher/Roboto-Medium.ttf is missing from the APK; no text");
    }
    else
    {
        for (int i = 0; i < kFontCount; ++i)
        {
            ImFontConfig config;
            config.FontDataOwnedByAtlas = false;
            config.OversampleH = 1;
            config.OversampleV = 1;
            config.PixelSnapH = true;
            g.fonts[i] = io.Fonts->AddFontFromMemoryTTF(g.fontData.data(), int(g.fontData.size()), std::round(Dp(kFontDp[i])), &config, ranges);
        }
        io.FontDefault = g.fonts[kBody];
    }

    g.gpu = &ProbeSystemGpu();
    g.adreno = UiAdreno();
    g.turnipPresent = access((GetPlatform().nativeLibDir + "/libvulkan_turnip.so").c_str(), R_OK) == 0;
    LoadConfig();
    g.ui = LanguageIndex(g.config.language);
    std::error_code ec;
    fs::create_directories(BackupDir(), ec);
    IsoImportCleanup();
    ScanGame();
    ScanSaves();
    ScanBackups();
    ScanDrivers();
    BuildNotices();
    // Importa tu ISO, once: the first open without the game.
    g.welcome = g.game == 0 && !g.config.welcomeShown;
    if (g.welcome)
    {
        g.config.welcomeShown = true;
        SaveConfig();
    }
    LOGFN("launcher: {} GPU {}, game {} (default.xex {}{}), {} saves, {} backups, {} imported drivers", g.adreno ? "Adreno" : "Mali/other",
        GpuName(), GameLabel(), g.xex.empty() ? "missing" : g.xex, g.xexOk ? ", the port's build" : "", g.saves.size(), g.backups.size(),
        g.drivers.size());
}

void UiSetTextures(Texture background, Texture blurred)
{
    g.background = background;
    g.blurred = blurred;
}

void UiFrame(float width, float height, float dt)
{
    g.clock += dt;
    g.dt = dt;
    PollImport();
    DrawBackground(width, height);

    // Clear of the edges and of the camera hole (the same on both sides: the screen may turn).
    const float side = std::max({Dp(18), g.insets[0] + Dp(6), g.insets[2] + Dp(6)});
    const float top = std::max(Dp(16), g.insets[1] + Dp(6)), bottom = std::max(Dp(16), g.insets[3] + Dp(6));
    const ImVec2 a(side, top), b(width - side, height - bottom);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::Begin("##launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    TopBar(a, b.x - a.x);
    const float bar = Dp(46) + Dp(12);
    const float appear = g.pageStart < 0 ? 1 : std::clamp((g.clock - g.pageStart) / 0.2f, 0.f, 1.f);
    const float ease = 1 - (1 - appear) * (1 - appear);
    g.fade = ease;
    if (g.page == Page::Home)
        Home(a + ImVec2(0, bar + Dp(14) * (1 - ease)), b + ImVec2(0, Dp(14) * (1 - ease)));
    else
        PageBody(a + ImVec2(0, bar), b, Dp(14) * (1 - ease));
    g.fade = 1;
    ImGui::End();

    if (g.isoError)
        IsoOverlay(width, height, std::max(side, top), "ISO no compatible", g.isoError, g.isoErrorStart, true);
    else if (g.welcome)
        IsoOverlay(width, height, std::max(side, top), "Importa tu ISO", "La ISO debe ser de Xbox 360, versión USA", 0, false);
    else if (g.noticesOpen)
        NoticesOverlay(width, height, side, a.y + bar);
    DrawToasts(width, a.y + bar);
}

void UiShutdown()
{
    SaveConfig();
    // The atlas points into fontData; ImGui::DestroyContext frees it after this.
    ImGui::GetIO().Fonts->Clear();
    g.fontData.clear();
    g.fontData.shrink_to_fit();
}

void UiBack()
{
    if (g.welcome || g.isoError)
    {
        g.welcome = false;
        g.isoError = nullptr;
        return;
    }
    if (g.noticesOpen)
    {
        g.noticesOpen = false;
        return;
    }
    if (!g.confirm.empty())
    {
        g.confirm.clear();
        return;
    }
    if (g.page == Page::Crash)
    {
        Go(Page::System);
        return;
    }
    if (g.page != Page::Home)
    {
        Go(Page::Home);
        return;
    }
    if (g.importing)
    {
        Toast("Espera a que termine la importación", 1);
        return;
    }
    if (g.clock - g.backTime < 2)
    {
        SaveConfig();
        RequestExit();
        return;
    }
    g.backTime = g.clock;
    Toast("Pulsa atrás otra vez para salir");
}

void UiInputSource(bool touch)
{
    g.touch = touch;
}

void UiGamepad(const std::string& name)
{
    if (name == g.gamepad)
        return;
    if (!name.empty())
        Toast(Trf("Mando conectado: {}", name));
    else if (!g.gamepad.empty())
        Toast("Mando desconectado", 1);
    g.gamepad = name;
}

// MainActivity's answer: the driver's name, or why it failed (one of the codes below, else the
// system's message).
void UiDriverImported(const std::string& message, bool ok)
{
    ScanDrivers();
    BuildNotices();
    if (ok)
    {
        Toast(Trf("Driver importado: {}", message));
        return;
    }
    if (message == "picker")
    {
        Toast("No hay un selector de archivos en este móvil", 2);
        return;
    }
    static const char* const reasons[][2] = {
        {"path", "ruta no válida en el zip"},
        {"size", "el zip es demasiado grande"},
        {"meta", "no tiene meta.json (no es un driver de AdrenoTools)"},
        {"library", "falta la librería del driver"},
        {"folder", "no se pudo crear la carpeta"},
        {"move", "no se pudo mover el driver"},
    };
    std::string reason = message;
    for (const auto& known : reasons)
    {
        if (message == known[0])
            reason = Tr(known[1]);
    }
    Toast(Trf("No se pudo importar el driver: {}", reason), 2);
}

// MainActivity's answer to Importar ISO: the disc image's descriptor, or -1 and why.
void UiIsoPicked(int fd, const std::string& message)
{
    if (fd < 0)
    {
        Toast(message == "picker" ? std::string(Tr("No hay un selector de archivos en este móvil"))
            : Trf("No se pudo importar la ISO: {}", Tr("no se pudo abrir el archivo")), 2);
        return;
    }
    LOGFN("launcher: importing the disc image {}", message);
    if (!IsoImportStart(fd, XexHeadGame))
    {
        Toast("Ya hay una importación en curso", 1);
        return;
    }
    g.importing = true;
    if (g.page != Page::Games)
        Go(Page::Games);
}

void UiInsets(float left, float top, float right, float bottom)
{
    g.insets[0] = left;
    g.insets[1] = top;
    g.insets[2] = right;
    g.insets[3] = bottom;
}

bool UiAnimating()
{
    const bool appearing = (g.pageStart >= 0 && g.clock - g.pageStart < 0.25f) || (g.welcome && g.clock < 0.35f) ||
        (g.isoError && g.clock - g.isoErrorStart < 0.35f);
    return appearing || !g.toasts.empty() || g.scroll.dragging || g.scroll.velocity != 0 || g.scroll.target >= 0 ||
        g.check.running;
}

bool UiAdreno()
{
    // Qualcomm's kernel driver (KGSL) or a Qualcomm GPU behind Vulkan.
    static const bool adreno = access("/dev/kgsl-3d0", F_OK) == 0 || ProbeSystemGpu().vendor == 0x5143;
    return adreno;
}
}
#endif
