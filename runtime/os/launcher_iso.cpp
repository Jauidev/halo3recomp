// Importar ISO (launcher.h): the game's disc image, an Xbox 360 .iso the system file
// picker opened, unpacked into its game's folder on its own thread. The disc's game partition is XDVDFS, read
// the way tools/xdvdfs.py reads it: a volume descriptor naming the root directory table, each table
// a binary tree of entries (left and right child offsets, start sector, size, attributes, name).
// default.xex is checked first (the launcher's XexMatches): another game writes nothing. The files
// go to game.import/ beside game/, in the order they lie on the disc, and game/ is replaced only
// once they are all there, so a cancelled or failed import leaves the game that was there.
#if defined(__ANDROID__) && defined(XPORT_SHARED)
#include <stdafx.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <unordered_set>
#include "launcher.h"

namespace launcher
{
namespace
{
    namespace fs = std::filesystem;

    constexpr uint64_t kSector = 2048;
    constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
    // Where the game partition starts: XGD3, XGD2, XGD1, plain XISO.
    constexpr uint64_t kPartitions[] = {0x2080000, 0xFD90000, 0x18300000, 0};
    // Bounds for a damaged or hostile image.
    constexpr size_t kMaxEntries = 100000;
    constexpr uint32_t kMaxTable = 16u << 20;
    constexpr int kMaxDepth = 32;

    struct Entry
    {
        std::string path; // inside the disc, '/' between the names
        uint32_t sector = 0, size = 0;
        bool directory = false;
    };

    struct Import
    {
        std::atomic<bool> running{false}, cancel{false};
        std::atomic<uint64_t> done{0}, total{0};
        std::mutex mutex; // error, needed
        std::string error;
        uint64_t needed = 0;
        int game = 0;
        std::mutex folders; // <game>.import/ and <game>.old/: one thread changes them at a time
    };
    Import g_import;

    // game as XexCheck's answer.
    std::string Folder(int) { return "game"; }
    fs::path GameDir(int game) { return fs::path(GetPlatform().externalDir) / Folder(game); }
    fs::path StagingDir(int game) { return fs::path(GetPlatform().externalDir) / (Folder(game) + ".import"); }
    fs::path OldDir(int game) { return fs::path(GetPlatform().externalDir) / (Folder(game) + ".old"); }
    fs::path DeletedDir(int game) { return fs::path(GetPlatform().externalDir) / (Folder(game) + ".delete"); }

    uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
    uint32_t Le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

    bool ReadAt(int fd, uint64_t offset, void* out, size_t size)
    {
        auto* p = static_cast<uint8_t*>(out);
        while (size)
        {
            const ssize_t got = pread(fd, p, size, off_t(offset));
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0)
                return false;
            p += got;
            offset += uint64_t(got);
            size -= size_t(got);
        }
        return true;
    }

    bool WriteAll(int fd, const uint8_t* p, size_t size)
    {
        while (size)
        {
            const ssize_t put = write(fd, p, size);
            if (put < 0 && errno == EINTR)
                continue;
            if (put <= 0)
                return false;
            p += put;
            size -= size_t(put);
        }
        return true;
    }

    // A name a disc may hold: no separators, not . or .., no control characters.
    bool SafeName(const std::string& name)
    {
        if (name.empty() || name == "." || name == "..")
            return false;
        for (const unsigned char c : name)
        {
            if (c < 0x20 || c == '/' || c == '\\')
                return false;
        }
        return true;
    }

    // One directory table and, depth first, the tables of its folders. False: the image is damaged.
    bool Walk(int fd, uint64_t base, uint32_t sector, uint32_t size, const std::string& prefix, int depth,
        std::vector<Entry>& out)
    {
        if (size == 0)
            return true;
        if (depth > kMaxDepth || size > kMaxTable)
            return false;
        std::vector<uint8_t> table(size);
        if (!ReadAt(fd, base + sector * kSector, table.data(), table.size()))
            return false;
        std::vector<uint32_t> pending{0};
        std::unordered_set<uint32_t> seen;
        while (!pending.empty())
        {
            const uint32_t position = pending.back();
            pending.pop_back();
            const size_t at = size_t(position) * 4;
            if (!seen.insert(position).second || at + 14 > table.size())
                continue;
            const uint8_t* e = &table[at];
            const uint16_t left = Le16(e), right = Le16(e + 2);
            if (left == 0xFFFF) // the padding after a sector's last entry
                continue;
            const uint8_t length = e[13];
            if (at + 14 + length > table.size() || out.size() >= kMaxEntries)
                return false;
            const std::string name(reinterpret_cast<const char*>(e + 14), length);
            if (!SafeName(name))
                return false;
            Entry entry{prefix.empty() ? name : prefix + "/" + name, Le32(e + 4), Le32(e + 8), (e[12] & 0x10) != 0};
            out.push_back(entry);
            if (entry.directory && !Walk(fd, base, entry.sector, entry.size, entry.path, depth + 1, out))
                return false;
            if (left)
                pending.push_back(left);
            if (right)
                pending.push_back(right);
        }
        return true;
    }

    // The import itself; nullptr when game/ holds the disc, else the error's code.
    const char* Unpack(int fd, const XexCheck& check, uint64_t& needed, int& game)
    {
        uint64_t base = 0;
        uint32_t rootSector = 0, rootSize = 0;
        bool found = false;
        for (const uint64_t offset : kPartitions)
        {
            uint8_t volume[28];
            if (ReadAt(fd, offset + 32 * kSector, volume, sizeof(volume)) && memcmp(volume, kMagic, 20) == 0)
            {
                base = offset;
                rootSector = Le32(volume + 20);
                rootSize = Le32(volume + 24);
                found = true;
                break;
            }
        }
        std::vector<Entry> entries;
        if (!found || !Walk(fd, base, rootSector, rootSize, "", 0, entries))
            return "disc";

        const auto xex = std::find_if(entries.begin(), entries.end(),
            [](const Entry& e) { return !e.directory && strcasecmp(e.path.c_str(), "default.xex") == 0; });
        if (xex == entries.end())
            return "game";
        std::string head(std::min<uint32_t>(xex->size, 256 * 1024), '\0');
        if (!ReadAt(fd, base + xex->sector * kSector, head.data(), head.size()))
            return "read";
        std::string identity;
        game = check(head, identity);
        uint64_t total = 0;
        for (const Entry& e : entries)
            total += e.directory ? 0 : e.size;
        LOGFN("launcher: iso: partition 0x{:X}, {} entries, {} bytes, default.xex {}", base, entries.size(), total, identity);
        if (game <= 0)
            return game < 0 ? "version" : "game";

        std::lock_guard folders(g_import.folders);
        std::error_code ec;
        const fs::path staging = StagingDir(game);
        fs::remove_all(staging, ec);
        struct statvfs disk{};
        if (statvfs(GetPlatform().externalDir.c_str(), &disk) == 0)
        {
            const uint64_t want = total + (64ull << 20);
            if (uint64_t(disk.f_bavail) * disk.f_frsize < want)
            {
                needed = want;
                return "space";
            }
        }
        g_import.total = total;
        fs::create_directories(staging, ec);
        for (const Entry& e : entries)
        {
            if (e.directory && !ec)
                fs::create_directories(staging / e.path, ec);
        }
        if (ec)
            return "write";

        std::vector<const Entry*> files;
        for (const Entry& e : entries)
        {
            if (!e.directory)
                files.push_back(&e);
        }
        std::sort(files.begin(), files.end(), [](const Entry* a, const Entry* b) { return a->sector < b->sector; });
        std::vector<uint8_t> buffer(4 << 20);
        for (const Entry* e : files)
        {
            const int out = open((staging / e->path).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
            if (out < 0)
                return "write";
            uint64_t offset = base + e->sector * kSector;
            uint32_t left = e->size;
            const char* failure = nullptr;
            while (left && !failure)
            {
                const size_t chunk = std::min<size_t>(left, buffer.size());
                if (g_import.cancel)
                    failure = "cancel";
                else if (!ReadAt(fd, offset, buffer.data(), chunk))
                    failure = "read";
                else if (!WriteAll(out, buffer.data(), chunk))
                    failure = "write";
                offset += chunk;
                left -= uint32_t(chunk);
                g_import.done += chunk;
            }
            if (close(out) != 0 && !failure)
                failure = "write";
            if (failure)
                return failure;
        }

        // game/ replaced: the old one moves aside until the new one is in its place.
        const fs::path target = GameDir(game), old = OldDir(game);
        fs::remove_all(old, ec);
        ec.clear();
        if (fs::exists(target, ec))
        {
            fs::rename(target, old, ec);
            if (ec)
                return "move";
        }
        fs::rename(staging, target, ec);
        if (ec)
        {
            std::error_code back;
            fs::rename(old, target, back);
            return "move";
        }
        fs::remove_all(old, ec);
        return nullptr;
    }

    void Run(int fd, XexCheck check)
    {
        const auto start = std::chrono::steady_clock::now();
        uint64_t needed = 0;
        int game = 0;
        const char* failure = Unpack(fd, check, needed, game);
        close(fd);
        if (failure && game > 0)
        {
            std::lock_guard folders(g_import.folders);
            std::error_code ec;
            fs::remove_all(StagingDir(game), ec);
        }
        const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        if (failure)
            LOGFN_WARNING("launcher: iso import failed ({}) after {:.1f} s", failure, seconds);
        else
            LOGFN("launcher: iso imported: {} bytes in {:.1f} s", g_import.total.load(), seconds);
        {
            std::lock_guard lock(g_import.mutex);
            g_import.error = failure ? failure : "";
            g_import.needed = needed;
            g_import.game = failure ? 0 : game;
        }
        g_import.running = false;
    }
}

bool IsoImportStart(int fd, XexCheck check)
{
    if (g_import.running.exchange(true))
    {
        close(fd);
        return false;
    }
    g_import.cancel = false;
    g_import.done = 0;
    g_import.total = 0;
    {
        std::lock_guard lock(g_import.mutex);
        g_import.error.clear();
        g_import.needed = 0;
        g_import.game = 0;
    }
    std::thread(Run, fd, std::move(check)).detach();
    return true;
}

IsoStatus IsoImportStatus()
{
    IsoStatus status;
    status.running = g_import.running;
    status.cancelling = g_import.cancel;
    status.done = g_import.done;
    status.total = g_import.total;
    std::lock_guard lock(g_import.mutex);
    status.error = g_import.error;
    status.needed = g_import.needed;
    status.game = g_import.game;
    return status;
}

void IsoImportCancel()
{
    g_import.cancel = true;
}

void IsoImportCleanup()
{
    for (const int game : {1})
    {
        std::error_code ec;
        if (!fs::exists(StagingDir(game), ec) && !fs::exists(OldDir(game), ec) && !fs::exists(DeletedDir(game), ec))
            continue;
        std::thread([game]
        {
            std::lock_guard folders(g_import.folders);
            std::error_code ec;
            // A <game>.old/ without <game>/ is the game a killed import was replacing: it goes back.
            if (fs::exists(OldDir(game), ec) && !fs::exists(GameDir(game), ec))
                fs::rename(OldDir(game), GameDir(game), ec);
            fs::remove_all(OldDir(game), ec);
            fs::remove_all(StagingDir(game), ec);
            fs::remove_all(DeletedDir(game), ec);
            LOGN("launcher: iso: removed what an interrupted import left");
        }).detach();
    }
}

bool GameDelete(int game)
{
    std::lock_guard folders(g_import.folders);
    std::error_code ec;
    const fs::path deleted = DeletedDir(game);
    fs::remove_all(deleted, ec);
    ec.clear();
    fs::rename(GameDir(game), deleted, ec);
    if (ec)
    {
        LOGFN_WARNING("launcher: could not delete game {}: {}", game, ec.message());
        return false;
    }
    std::thread([deleted, game]
    {
        std::error_code ec;
        fs::remove_all(deleted, ec);
        LOGFN("launcher: game {} deleted", game);
    }).detach();
    return true;
}
}
#endif
