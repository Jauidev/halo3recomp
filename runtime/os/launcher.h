#pragma once
// the launcher (Android), the screen before the game. MainActivity gives it its own
// SurfaceView; launcher.cpp draws it with OpenGL ES 3 and Dear ImGui (Xenia's copy) on its own
// thread and passes the touches and pad keys on, and launcher_ui.cpp is the screen itself: a
// floating grid of tiles over the background of the GPU's colour (Mali blue, Adreno red), the
// graphics profiles, the saves and their backups, the Vulkan driver, the controls, the system
// options and the credits. "Jugar" turns the choices into the game's options (cvars, files/
// switches, XPORT_* variables), ends the launcher and lets MainActivity start the game with them.
// XPORT_LAUNCHER=0 in the intent's options skips it (measurement scripts).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ImDrawList;

namespace launcher
{
    struct Platform
    {
        std::string externalDir;  // getExternalFilesDir: game/, save/, switches, crash.txt, backups/
        std::string internalDir;  // getFilesDir: imported drivers (code may run only from there)
        std::string nativeLibDir; // nativeLibraryDir: libvulkan_turnip.so
        std::string version;      // versionName
        std::string device;       // Build.MANUFACTURER + MODEL
        std::string locale;       // the phone's language (Locale.getLanguage: "es", "en"...)
        float density = 2.75f;    // pixels per dp
        uint64_t availMem = 0;    // ActivityManager.MemoryInfo at start
        uint64_t totalMem = 0;
    };

    struct Texture
    {
        uint32_t id = 0;
        int width = 0, height = 0;
    };

    // launcher.cpp, for the screen (launcher thread).
    const Platform& GetPlatform();
    std::vector<uint8_t> ReadAsset(const char* name);
    // Ends the launcher after this frame; MainActivity starts the game with these options
    // ("NAME=value" pairs separated by spaces).
    void RequestPlay(const std::string& options);
    void RequestExit();
    // Blurs what is drawn before this point of list's commands (a popup's backdrop).
    void AddBackdropBlur(ImDrawList* list);
    // Opens the system file picker for a driver .zip; MainActivity unpacks it into
    // internalDir/drivers/<name>/ and answers with UiDriverImported.
    void RequestDriverImport();
    // Opens the system file picker for the game's disc image (.iso); MainActivity answers with
    // UiIsoPicked.
    void RequestIsoImport();

    // Quits and starts the app again.
    void RequestRestart();
    // Parches (MainActivity / Patches.java, files in internalDir/patches): the file picker for a
    // patch .zip, and "on:<id>", "off:<id>" or "del:<id>"; both answer with UiPatchesChanged.
    // PatchesAvailable: false in an APK whose Java side has no patches.
    void RequestPatchImport();
    void RequestPatchAction(const std::string& action);
    bool PatchesAvailable();
#if XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6
    // Puts text on the phone's clipboard (MainActivity.launcherCopyText): Copiar in Último error.
    void RequestCopyText(const std::string& text);
#endif

    // launcher_iso.cpp: Importar ISO. The disc image (fd, closed when it ends) is unpacked into its
    // game's folder (externalDir/game) on its own thread; check gets default.xex's first bytes, says
    // whether they are the port's game (1), another (0) or the game but not the disc the port was
    // recompiled from (-1), and refuses another before anything is written. One import at a time.
    using XexCheck = std::function<int(const std::string& head, std::string& found)>;
    struct IsoStatus
    {
        bool running = false, cancelling = false;
        uint64_t done = 0, total = 0; // bytes
        std::string error;            // after it ended: empty, or picker, open, disc, game, version, read, write, space, move, cancel
        uint64_t needed = 0;          // space: the bytes it needs free
        int game = 0;                 // after it ended well: the game unpacked (as XexCheck)
    };
    bool IsoImportStart(int fd, XexCheck check); // false: one is running (fd closed)
    IsoStatus IsoImportStatus();
    void IsoImportCancel();
    void IsoImportCleanup(); // removes what an import or a deletion cut short left (the app was killed)
    // The game's folder is gone when it returns (false: it could not be moved); the files go on their
    // own thread.
    bool GameDelete(int game);

    // launcher_ui.cpp, called on the launcher thread.
    void UiInit();                                 // style and fonts (before the first frame)
    void UiSetTextures(Texture background, Texture blurred);
    void UiFrame(float width, float height, float dt);
    void UiShutdown();
    void UiBack();                                 // Android back key / pad B
    void UiInputSource(bool touch);                // the last input was a touch (or a key)
    void UiGamepad(const std::string& name);       // empty: none connected
    void UiDriverImported(const std::string& message, bool ok);
    // Patches.java's answer: "import|on|off|del\t<name>", "error\t<code>", or "cancel".
    void UiPatchesChanged(const std::string& message, bool ok);
    // The picked disc image's descriptor, or -1 and why (picker, open).
    void UiIsoPicked(int fd, const std::string& message);
    // The display cutout's safe insets in pixels (the camera hole): the grid keeps clear of them.
    void UiInsets(float left, float top, float right, float bottom);
    bool UiAnimating();                            // keep drawing at full rate
    // The GPU's colour family: true for Adreno (red background), false otherwise (blue).
    bool UiAdreno();
}
