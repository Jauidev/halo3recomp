// NT file layer (NtCreateFile, NtReadFile, ...). UnleashedRecomp hooked Sonic's XAPI file
// functions by address and left these as stubs; in the title the XAPI layer runs recompiled and calls
// them. Structures and status codes follow Xenia (xboxkrnl_io.cc, xboxkrnl_io_info.cc).
//
// Guest paths ("game:\common.rpf", "d:\...", "\Device\Cdrom0\...") resolve onto the extracted disc,
// case-insensitively because the phone's file systems are case-sensitive.
#include <stdafx.h>
#include <cpu/guest_thread.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <kernel/xam.h>
#include <kernel/xdm.h>
#include <os/paths.h>
#include <os/thread_dump.h>
#include <xport_gpu.h>

#include <deque>

struct Event;
uint32_t NtSetEvent(Event* handle, be<uint32_t>* previousState);

namespace
{
    constexpr uint32_t X_STATUS_SUCCESS = 0x00000000;
    constexpr uint32_t X_STATUS_NO_MORE_FILES = 0x80000006;
    constexpr uint32_t X_STATUS_INVALID_HANDLE = 0xC0000008;
    constexpr uint32_t X_STATUS_INVALID_PARAMETER = 0xC000000D;
    constexpr uint32_t X_STATUS_END_OF_FILE = 0xC0000011;
    constexpr uint32_t X_STATUS_ACCESS_DENIED = 0xC0000022;
    constexpr uint32_t X_STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
    constexpr uint32_t X_STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
    constexpr uint32_t X_STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
    constexpr uint32_t X_STATUS_FILE_IS_A_DIRECTORY = 0xC00000BA;
    constexpr uint32_t X_STATUS_NOT_A_DIRECTORY = 0xC0000103;
    constexpr uint32_t X_STATUS_INFO_LENGTH_MISMATCH = 0xC0000004;
    constexpr uint32_t X_STATUS_INVALID_INFO_CLASS = 0xC0000003;

    // CreateDisposition / IO_STATUS_BLOCK.Information
    constexpr uint32_t FILE_SUPERSEDE = 0, FILE_OPEN = 1, FILE_CREATE = 2, FILE_OPEN_IF = 3, FILE_OVERWRITE = 4, FILE_OVERWRITE_IF = 5;
    constexpr uint32_t FILE_SUPERSEDED = 0, FILE_OPENED = 1, FILE_CREATED = 2, FILE_OVERWRITTEN = 3, FILE_DOES_NOT_EXIST = 5;
    constexpr uint32_t FILE_DIRECTORY_FILE = 0x1, FILE_NON_DIRECTORY_FILE = 0x40;
    constexpr uint32_t X_FILE_ATTRIBUTE_READONLY = 0x01, X_FILE_ATTRIBUTE_DIRECTORY = 0x10, X_FILE_ATTRIBUTE_NORMAL = 0x80;

    // A fixed timestamp (2009-10-29, the title's release) for every file: the game only compares them.
    constexpr uint64_t FILE_TIME = 128999232000000000ull;

    std::string Lower(std::string_view s)
    {
        std::string out(s);
        for (char& c : out)
            c = char(tolower((unsigned char)c));
        return out;
    }

    Mutex g_linkMutex;
    std::unordered_map<std::string, std::string> g_symbolicLinks; // lower-case "cache:" -> target path

    // Host directory behind a guest device or root ("game:", "d:", "\device\cdrom0", ...).
    std::filesystem::path ResolveRoot(const std::string& root)
    {
        if (root == "\\device\\cdrom0" || root == "\\device\\cdrom" || root == "dvd:")
            return os::paths::GetGamePath();
        // Halo 3 keeps two cache partitions of its own, each a folder here: cache0: (map cache
        // cache000-014.map, fonts, preferences) and cache1: (saves, films, web cache). Its
        // \Device\Harddisk0\Cache%u names the same two; partition0 (the raw disk, only opened to
        // see that there is a hard disk) is the hdd folder.
        auto folder = [](const char* name)
        {
            std::filesystem::path path = os::paths::GetSavePath() / name;
            std::error_code ec;
            std::filesystem::create_directories(path, ec);
            return path;
        };
        if (root == "cache0:" || root == "\\device\\harddisk0\\cache0")
            return folder("cache0");
        if (root == "cache1:" || root == "\\device\\harddisk0\\cache1")
            return folder("cache1");
        if (root == "cache:")
            return os::paths::GetSavePath() / "cache";
        if (root == "\\device\\harddisk0\\partition1" || root == "\\device\\harddisk0\\partition0" || root == "hdd:")
            return folder("hdd");

        if (root.size() > 1 && root.back() == ':')
        {
            const std::string name = root.substr(0, root.size() - 1);
            std::string_view mapped = XamGetRootPath(name);
            if (mapped.empty())
                mapped = XamGetRootPath(Lower(name));
            if (mapped.empty() && name.size() == 1)
                mapped = XamGetRootPath(std::string(1, char(toupper((unsigned char)name[0]))));
            if (!mapped.empty())
                return std::filesystem::path(std::string(mapped));
        }
        return {};
    }

    // Case-insensitive lookup of one component inside a host directory.
    std::filesystem::path MatchComponent(const std::filesystem::path& dir, const std::string& component)
    {
        std::filesystem::path exact = dir / component;
        std::error_code ec;
        if (std::filesystem::exists(exact, ec))
            return exact;
        const std::string lower = Lower(component);
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (Lower(entry.path().filename().string()) == lower)
                return entry.path();
        }
        return exact;
    }

    // Appends the '\'-separated guest components of rest to a host directory, each matched
    // case-insensitively.
    std::filesystem::path AppendComponents(std::filesystem::path host, const std::string& rest)
    {
        size_t start = 0;
        while (start < rest.size())
        {
            size_t end = rest.find('\\', start);
            if (end == std::string::npos)
                end = rest.size();
            std::string component = rest.substr(start, end - start);
            if (!component.empty() && component != ".")
                host = MatchComponent(host, component);
            start = end + 1;
        }
        return host;
    }

    // Guest path -> host path. Empty when the device is unknown.
    std::filesystem::path ResolvePath(std::string_view guestPath)
    {
        std::string path(guestPath);
        std::replace(path.begin(), path.end(), '/', '\\');
        if (path.starts_with("\\??\\"))
            path = path.substr(4);

        std::string lower = Lower(path);
        {
            std::lock_guard lock(g_linkMutex);
            for (const auto& [link, target] : g_symbolicLinks)
            {
                if (lower.starts_with(link))
                {
                    path = target + path.substr(link.size());
                    lower = Lower(path);
                    break;
                }
            }
        }

        // Split "<root>\rest": root is "x:" or a "\device\..." prefix.
        std::string root, rest;
        size_t colon = path.find(':');
        if (colon != std::string::npos && path.find('\\') > colon)
        {
            root = lower.substr(0, colon + 1);
            rest = path.substr(colon + 1);
        }
        else if (lower.starts_with("\\device\\"))
        {
            size_t end = lower.find('\\', 8);
            if (lower.starts_with("\\device\\harddisk0\\"))
                end = lower.find('\\', 18);
            root = lower.substr(0, end);
            rest = end == std::string::npos ? "" : path.substr(end);
        }
        else
        {
            root = "game:"; // relative paths are relative to the title's directory
            rest = path;
        }

        std::filesystem::path host = ResolveRoot(root);
        if (host.empty())
        {
            LOGF_WARNING("unknown device in {}", guestPath);
            return {};
        }
        return AppendComponents(host, rest);
    }

    std::string GuestString(const XANSI_STRING* str)
    {
        if (!str || !str->Buffer.get())
            return {};
        return std::string(str->Buffer.get(), str->Length.get());
    }

    struct FileHandle final : KernelObject
    {
        std::filesystem::path path;
        std::fstream stream;
        std::ios::openmode streamMode{}; // how stream was opened, to reopen it after a rename
        bool isDirectory = false;
        uint64_t position = 0;
        // Directory enumeration state for NtQueryDirectoryFile.
        std::vector<std::filesystem::directory_entry> entries;
        size_t nextEntry = 0;
        bool enumerated = false;
        // NtSetInformationFile class 30 (FileCompletionInformation): the I/O completion port its
        // overlapped reads and writes report to, and the key they report with.
        uint32_t completionPort = 0;
        uint32_t completionKey = 0;
        // FileDispositionInformation (class 13) or FILE_DELETE_ON_CLOSE: removed when the handle
        // closes. Only files under the save path (Halo 3 deletes its per-session autosave queue
        // files, cache1:\autosave\asq_*.temp; ignored, one 64 KB file was left per launch).
        bool deleteOnClose = false;

        ~FileHandle() override
        {
            if (!deleteOnClose)
                return;
            if (stream.is_open())
                stream.close();
            std::error_code ec;
            std::filesystem::remove(path, ec); // a directory only when empty
            LOGF("deleted {}{}", path.string(), ec ? " (failed: " + ec.message() + ")" : "");
        }
    };

    // Deletion is allowed only inside the save folder: disc files and anything else stay.
    bool InSaveFolder(const std::filesystem::path& path)
    {
        const std::filesystem::path relative = path.lexically_normal().lexically_relative(
            os::paths::GetSavePath().lexically_normal());
        return !relative.empty() && *relative.begin() != "..";
    }

    // An I/O completion port (NtCreateIoCompletion). Halo 3's ASYNC_IO thread reads the maps with
    // overlapped reads on files bound to one and takes the results with NtRemoveIoCompletion; as
    // stubs the port handle was garbage and no read ever completed. The reads here finish at once,
    // so each one queues its packet before NtReadFile returns. Packets are guarded by
    // g_dispatcherMutex, so waits on the port (signaled while packets wait) work like on any object.
    struct IoCompletion final : KernelObject
    {
        struct Packet
        {
            uint32_t key;
            uint32_t apcContext;
            uint32_t status;
            uint32_t information;
        };
        std::deque<Packet> packets;

        bool IsSignaled() override
        {
            return !packets.empty();
        }
    };

    IoCompletion* GetCompletionPort(uint32_t handle)
    {
        if (handle == 0 || handle == GUEST_INVALID_HANDLE_VALUE || !IsKernelObject(handle) ||
            !IsRegisteredKernelObject(GetKernelObject(handle)))
            return nullptr;
        return dynamic_cast<IoCompletion*>(GetKernelObject(handle));
    }

    // Queues the packet of an overlapped request on a file bound to a port: no APC routine (that is
    // ReadFileEx's way) and a context (XAPI passes the OVERLAPPED; null when the caller set the low
    // bit of its event, which asks for no packet).
    void PostCompletion(const FileHandle* file, uint32_t apcRoutine, uint32_t apcContext, uint32_t status, uint32_t information)
    {
        if (!file->completionPort || apcRoutine || !apcContext)
            return;
        IoCompletion* port = GetCompletionPort(file->completionPort);
        if (!port)
            return;
        std::lock_guard lock(g_dispatcherMutex);
        port->packets.push_back({ file->completionKey, apcContext, status, information });
        WakeKernelWaiters(port);
    }

    FileHandle* GetFile(uint32_t handle)
    {
        // Special handles have the kernel-object bit set but no object behind them; 0xFFFFFFFD as a
        // RootDirectory means "no root" (Xenia ignores it too).
        if (handle == GUEST_INVALID_HANDLE_VALUE || !IsKernelObject(handle) || !IsRegisteredKernelObject(GetKernelObject(handle)))
        {
            if (handle != 0 && handle != GUEST_INVALID_HANDLE_VALUE && handle != 0xFFFFFFFD)
                LOGF_WARNING("not a file handle: 0x{:X}", handle);
            return nullptr;
        }
        return dynamic_cast<FileHandle*>(GetKernelObject(handle));
    }

    void Complete(XIO_STATUS_BLOCK* ioStatus, uint32_t status, uint32_t information, uint32_t eventHandle)
    {
        if (ioStatus)
        {
            ioStatus->Status = status;
            ioStatus->Information = information;
        }
        if (eventHandle && eventHandle != GUEST_INVALID_HANDLE_VALUE && IsKernelObject(eventHandle))
            NtSetEvent(GetKernelObject<Event>(eventHandle), nullptr);
    }

    // User-mode APCs of the calling guest thread. ReadFileEx/WriteFileEx pass NtReadFile/NtWriteFile
    // the XAPI completion wrapper as ApcRoutine (low bit set) and the title's completion routine as
    // ApcContext; the wrapper runs as ApcRoutine(ApcContext, IoStatusBlock, 0) and calls the routine.
    // Halo 3's tag resource loader reads with ReadFileEx and polls SleepEx(0, TRUE) until its routine
    // marks the read done: with the APC dropped, the loader task never finished, the async task queue
    // stayed blocked on it and the game froze at ~30 s. The I/O completes before NtReadFile returns,
    // so only the issuing thread queues here (thread-local); the APCs run at its next alertable wait.
    struct QueuedApc
    {
        uint32_t routine;
        uint32_t context;
        uint32_t argument1;
        uint32_t argument2;
    };
    thread_local std::deque<QueuedApc> t_queuedApcs;

    void QueueIoApc(uint32_t apcRoutine, uint32_t apcContext, XIO_STATUS_BLOCK* ioStatus)
    {
        // Same condition as Xenia: the low bit only tags the routine, and both must be set.
        const uint32_t routine = apcRoutine & ~1u;
        if (routine == 0 || apcContext == 0)
            return;
        t_queuedApcs.push_back({ routine, apcContext, ioStatus ? g_memory.MapVirtual(ioStatus) : 0u, 0 });
    }
}

// Runs the calling thread's queued APCs (called by the alertable waits, which then return
// STATUS_USER_APC). Returns whether any ran.
bool DeliverQueuedApcs()
{
    if (t_queuedApcs.empty())
        return false;
    while (!t_queuedApcs.empty())
    {
        const QueuedApc apc = t_queuedApcs.front();
        t_queuedApcs.pop_front();
        if (g_memory.FindFunction(apc.routine) == nullptr)
        {
            LOGF_ERROR("APC routine 0x{:08X} is not a recompiled function, skipped", apc.routine);
            continue;
        }
        // Runs on the waiting thread's stack, as if the wait had called it.
        GuestToHostFunction<void>(apc.routine, apc.context, apc.argument1, apc.argument2);
    }
    return true;
}

namespace
{

    struct X_FILE_NETWORK_OPEN_INFORMATION
    {
        be<uint64_t> CreationTime;
        be<uint64_t> LastAccessTime;
        be<uint64_t> LastWriteTime;
        be<uint64_t> ChangeTime;
        be<uint64_t> AllocationSize;
        be<uint64_t> EndOfFile;
        be<uint32_t> FileAttributes;
        be<uint32_t> Pad;
    };
    static_assert(sizeof(X_FILE_NETWORK_OPEN_INFORMATION) == 56);

    void FillNetworkOpenInfo(const std::filesystem::path& path, X_FILE_NETWORK_OPEN_INFORMATION* info)
    {
        std::error_code ec;
        const bool directory = std::filesystem::is_directory(path, ec);
        const uint64_t size = directory ? 0 : std::filesystem::file_size(path, ec);
        info->CreationTime = FILE_TIME;
        info->LastAccessTime = FILE_TIME;
        info->LastWriteTime = FILE_TIME;
        info->ChangeTime = FILE_TIME;
        info->AllocationSize = (size + 0xFFF) & ~0xFFFull;
        info->EndOfFile = size;
        info->FileAttributes = directory ? X_FILE_ATTRIBUTE_DIRECTORY : X_FILE_ATTRIBUTE_NORMAL;
        info->Pad = 0;
    }

    // Case-insensitive wildcard match ('*' and '?').
    bool WildcardMatch(std::string_view pattern, std::string_view name)
    {
        if (pattern.empty())
            return name.empty();
        if (pattern[0] == '*')
            return WildcardMatch(pattern.substr(1), name) || (!name.empty() && WildcardMatch(pattern, name.substr(1)));
        if (name.empty())
            return false;
        if (pattern[0] == '?' || tolower((unsigned char)pattern[0]) == tolower((unsigned char)name[0]))
            return WildcardMatch(pattern.substr(1), name.substr(1));
        return false;
    }
}

// read statistics for headless runs (reported by the fake GPU every 2 seconds).
std::atomic<uint64_t> g_fileReads;
std::atomic<uint64_t> g_fileBytesRead;
static std::mutex g_lastOpenedMutex;
static std::string g_lastOpened;

std::string GetLastOpenedFile()
{
    std::lock_guard guard{ g_lastOpenedMutex };
    return g_lastOpened;
}

// XPORT_SCAN_FOR=<text>[;<text>...] with XPORT_BACKTRACE_OPEN (debugging, Windows only): on those
// opens, every place in guest memory holding one of the texts (ASCII or UTF-16BE, any case), with
// the text around it.
static void ScanGuestMemory()
{
#ifdef _WIN32
    const char* list = getenv("XPORT_SCAN_FOR");
    if (list == nullptr)
        return;
    std::vector<std::string> patterns;
    for (std::string_view rest = list; !rest.empty();)
    {
        const size_t end = rest.find(';');
        if (end != 0)
            patterns.emplace_back(rest.substr(0, end));
        if (end == std::string_view::npos)
            break;
        rest.remove_prefix(end + 1);
    }
    auto lower = [](uint8_t c) { return uint8_t(c >= 'A' && c <= 'Z' ? c + 32 : c); };
    int hits = 0;
    for (uint64_t address = 0x10000; address < PPC_MEMORY_SIZE && hits < 200;)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(g_memory.base + address, &info, sizeof(info)) == 0)
            break;
        const uint64_t regionEnd = uint64_t(reinterpret_cast<uint8_t*>(info.BaseAddress) - g_memory.base) + info.RegionSize;
        const bool readable = info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        if (readable)
        {
            const uint8_t* data = g_memory.base;
            for (uint64_t at = address; at < regionEnd && hits < 200; at++)
            {
                for (const std::string& pattern : patterns)
                {
                    const size_t n = pattern.size();
                    bool ascii = at + n <= regionEnd, wide = at + 2 * n <= regionEnd;
                    for (size_t i = 0; i < n && (ascii || wide); i++)
                    {
                        const uint8_t want = lower(uint8_t(pattern[i]));
                        if (ascii && lower(data[at + i]) != want)
                            ascii = false;
                        if (wide && (data[at + 2 * i] != 0 || lower(data[at + 2 * i + 1]) != want))
                            wide = false;
                    }
                    if (!ascii && !wide)
                        continue;
                    // The text around it, from the start of the string (up to 64 characters back).
                    const uint32_t step = wide ? 2 : 1;
                    uint64_t from = at;
                    auto printable = [&](uint64_t a) { return wide ? data[a] == 0 && data[a + 1] >= 0x20 && data[a + 1] < 0x7F : data[a] >= 0x20 && data[a] < 0x7F; };
                    while (from >= address + step && at - from < 64 * step && printable(from - step))
                        from -= step;
                    std::string text;
                    for (uint64_t a = from; a + step <= regionEnd && text.size() < 200 && printable(a); a += step)
                        text += char(data[a + step - 1]);
                    DiagPrint("scan: %08X %s \"%s\"\n", uint32_t(at), wide ? "utf16" : "ascii", text.c_str());
                    hits++;
                    at += (wide ? 2 * n : n) - 1;
                    break;
                }
            }
        }
        address = regionEnd;
    }
    DiagPrint("scan: %d hits\n", hits);
#endif
}

// XPORT_LOG_FILES=1 (debugging): every open, and every path that is not found, in the log.
static bool LogFileOpens()
{
    static const bool s_enabled = getenv("XPORT_LOG_FILES") != nullptr;
    return s_enabled;
}

uint32_t NtCreateFile(be<uint32_t>* fileHandle, uint32_t desiredAccess, XOBJECT_ATTRIBUTES* attributes, XIO_STATUS_BLOCK* ioStatus,
    be<uint64_t>* allocationSize, uint32_t fileAttributes, uint32_t shareAccess, uint32_t disposition, uint32_t createOptions)
{
    std::string guestPath = GuestString(attributes ? attributes->Name.get() : nullptr);
    if (attributes && attributes->RootDirectory && attributes->RootDirectory != GUEST_INVALID_HANDLE_VALUE)
    {
        if (auto* parent = GetFile(attributes->RootDirectory))
            guestPath = parent->path.string() + "\\" + guestPath; // host path + relative guest path
    }

    std::filesystem::path path = attributes && attributes->RootDirectory && GetFile(attributes->RootDirectory)
        ? MatchComponent(GetFile(attributes->RootDirectory)->path, GuestString(attributes->Name.get()))
        : ResolvePath(guestPath);

    *fileHandle = GUEST_INVALID_HANDLE_VALUE;

    // XPORT_BACKTRACE_OPEN=<text>[;<text>...] (debugging): the guest backtrace of every open whose
    // path has one of them.
    static const char* s_backtraceOpen = getenv("XPORT_BACKTRACE_OPEN");
    auto pathWanted = [&guestPath]
    {
        std::string_view list = s_backtraceOpen;
        while (!list.empty())
        {
            const size_t end = list.find(';');
            const std::string_view item = list.substr(0, end);
            if (!item.empty() && guestPath.find(item) != std::string::npos)
                return true;
            if (end == std::string_view::npos)
                break;
            list.remove_prefix(end + 1);
        }
        return false;
    };
    if (s_backtraceOpen != nullptr && pathWanted())
    {
        DiagPrint("open of %s, regs r3-r10 %08X %08X %08X %08X %08X %08X %08X %08X\n", guestPath.c_str(),
            GetPPCContext()->r3.u32, GetPPCContext()->r4.u32, GetPPCContext()->r5.u32, GetPPCContext()->r6.u32,
            GetPPCContext()->r7.u32, GetPPCContext()->r8.u32, GetPPCContext()->r9.u32, GetPPCContext()->r10.u32);
        PrintGuestBacktrace(GetPPCContext());
        ScanGuestMemory();
    }

    if (path.empty())
    {
        Complete(ioStatus, X_STATUS_OBJECT_PATH_NOT_FOUND, FILE_DOES_NOT_EXIST, 0);
        return X_STATUS_OBJECT_PATH_NOT_FOUND;
    }

    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    const bool directory = exists && std::filesystem::is_directory(path, ec);
    uint32_t information = FILE_OPENED;

    if (!exists && (disposition == FILE_OPEN || disposition == FILE_OVERWRITE))
    {
        if (LogFileOpens())
            LOGF_WARNING("not found: {}", guestPath);
        else
            LOGF_UTILITY("not found: {}", guestPath);
        Complete(ioStatus, X_STATUS_OBJECT_NAME_NOT_FOUND, FILE_DOES_NOT_EXIST, 0);
        return X_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (exists && disposition == FILE_CREATE)
    {
        Complete(ioStatus, X_STATUS_OBJECT_NAME_COLLISION, FILE_OPENED, 0);
        return X_STATUS_OBJECT_NAME_COLLISION;
    }
    if (directory && (createOptions & FILE_NON_DIRECTORY_FILE))
    {
        Complete(ioStatus, X_STATUS_FILE_IS_A_DIRECTORY, FILE_OPENED, 0);
        return X_STATUS_FILE_IS_A_DIRECTORY;
    }
    if (exists && !directory && (createOptions & FILE_DIRECTORY_FILE))
    {
        Complete(ioStatus, X_STATUS_NOT_A_DIRECTORY, FILE_OPENED, 0);
        return X_STATUS_NOT_A_DIRECTORY;
    }

    auto* file = CreateKernelObject<FileHandle>();
    file->path = path;
    if ((createOptions & 0x00001000) && InSaveFolder(path)) // FILE_DELETE_ON_CLOSE
        file->deleteOnClose = true;

    if (createOptions & FILE_DIRECTORY_FILE || directory)
    {
        if (!exists)
        {
            std::filesystem::create_directories(path, ec);
            information = FILE_CREATED;
        }
        file->isDirectory = true;
    }
    else
    {
        // Disc data is read-only; writable files (saves, cache) live under the save path.
        const bool write = (desiredAccess & (0x40000000 | 0x2 | 0x4 | 0x10000000)) != 0; // GENERIC_WRITE, FILE_WRITE_DATA, FILE_APPEND_DATA, GENERIC_ALL
        const bool truncate = disposition == FILE_SUPERSEDE || disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF;
        if (!exists || truncate)
        {
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream(path, std::ios::binary | std::ios::trunc);
            information = !exists ? FILE_CREATED : (disposition == FILE_SUPERSEDE ? FILE_SUPERSEDED : FILE_OVERWRITTEN);
        }
        file->streamMode = std::ios::binary | std::ios::in | (write ? std::ios::out : std::ios::openmode{});
        file->stream.open(path, file->streamMode);
        if (!file->stream.is_open())
        {
            DestroyKernelObject(file);
            LOGF_WARNING("cannot open {} ({})", guestPath, path.string());
            Complete(ioStatus, X_STATUS_ACCESS_DENIED, 0, 0);
            return X_STATUS_ACCESS_DENIED;
        }
    }

    // XPORT_LOG_FILES=1 lists every open: in headless runs it shows how far loading got.
    static const bool s_logFiles = getenv("XPORT_LOG_FILES") != nullptr;
    if (s_logFiles)
        LOGF("{}", guestPath);
    {
        std::lock_guard guard{ g_lastOpenedMutex };
        g_lastOpened = guestPath;
    }

    *fileHandle = GetKernelHandle(file);
    Complete(ioStatus, X_STATUS_SUCCESS, information, 0);
    return X_STATUS_SUCCESS;
}

uint32_t NtOpenFile(be<uint32_t>* fileHandle, uint32_t desiredAccess, XOBJECT_ATTRIBUTES* attributes, XIO_STATUS_BLOCK* ioStatus, uint32_t openOptions)
{
    return NtCreateFile(fileHandle, desiredAccess, attributes, ioStatus, nullptr, 0, 0, FILE_OPEN, openOptions);
}

uint32_t NtReadFile(uint32_t fileHandle, uint32_t eventHandle, uint32_t apcRoutine, uint32_t apcContext, XIO_STATUS_BLOCK* ioStatus,
    void* buffer, uint32_t length, be<uint64_t>* byteOffset)
{
    FileHandle* file = GetFile(fileHandle);
    if (!file || file->isDirectory)
        return X_STATUS_INVALID_HANDLE;

    // An offset of -2 (FILE_USE_FILE_POINTER_POSITION) means "current position".
    uint64_t offset = byteOffset && byteOffset->get() != 0xFFFFFFFFFFFFFFFEull ? byteOffset->get() : file->position;
    file->stream.clear();
    file->stream.seekg(offset);
    // The read may go straight from the kernel into the buffer, and a syscall that meets a page
    // Xenia's GPU write-protects fails instead of faulting: fire those watches first.
    xportgpu::PrepareHostWrite(g_memory.MapVirtual(buffer), length);
    file->stream.read((char*)buffer, length);
    const uint32_t read = uint32_t(file->stream.gcount());
    file->position = offset + read;

    if (read == 0 && length != 0)
    {
        Complete(ioStatus, X_STATUS_END_OF_FILE, 0, eventHandle);
        PostCompletion(file, apcRoutine, apcContext, X_STATUS_END_OF_FILE, 0);
        return X_STATUS_END_OF_FILE;
    }
    g_fileReads++;
    g_fileBytesRead += read;
    // XPORT_LOG_READS=1 (debugging): every read, with where it lands in guest memory.
    // XPORT_LOG_READS=2: also the time (ms since the first read), the caller and its guest backtrace.
    static const char* s_logReadsValue = getenv("XPORT_LOG_READS");
    static const bool s_logReads = s_logReadsValue != nullptr;
    if (s_logReads)
    {
        static const auto s_firstRead = std::chrono::steady_clock::now();
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s_firstRead).count();
        LOGF("{} ms {} @0x{:X} +0x{:X} -> 0x{:08X} (0x{:X} read, port 0x{:X}, apc 0x{:X}/0x{:X}, thread {:X}, lr 0x{:08X})", ms, file->path.filename().string(),
            offset, length, g_memory.MapVirtual(buffer), read, file->completionPort, apcRoutine, apcContext,
            GuestThread::GetCurrentThreadId(), uint32_t(GetPPCContext()->lr));
        if (s_logReadsValue[0] == '2')
            PrintGuestBacktrace(GetPPCContext());
    }
    Complete(ioStatus, X_STATUS_SUCCESS, read, eventHandle);
    PostCompletion(file, apcRoutine, apcContext, X_STATUS_SUCCESS, read);
    QueueIoApc(apcRoutine, apcContext, ioStatus);
    return X_STATUS_SUCCESS;
}

// Reads into a list of 4 KB pages (FILE_SEGMENT_ELEMENT array of guest page addresses).
uint32_t NtReadFileScatter(uint32_t fileHandle, uint32_t eventHandle, uint32_t apcRoutine, uint32_t apcContext, XIO_STATUS_BLOCK* ioStatus,
    be<uint32_t>* segments, uint32_t length, be<uint64_t>* byteOffset)
{
    FileHandle* file = GetFile(fileHandle);
    if (!file || file->isDirectory)
        return X_STATUS_INVALID_HANDLE;

    uint64_t offset = byteOffset && byteOffset->get() != 0xFFFFFFFFFFFFFFFEull ? byteOffset->get() : file->position;
    file->stream.clear();
    file->stream.seekg(offset);

    uint32_t total = 0;
    for (uint32_t i = 0; total < length; i++)
    {
        const uint32_t chunk = std::min<uint32_t>(0x1000, length - total);
        xportgpu::PrepareHostWrite(segments[i], chunk);
        file->stream.read((char*)g_memory.Translate(segments[i]), chunk);
        const uint32_t read = uint32_t(file->stream.gcount());
        total += read;
        if (read < chunk)
            break;
    }
    file->position = offset + total;

    if (total == 0 && length != 0)
    {
        Complete(ioStatus, X_STATUS_END_OF_FILE, 0, eventHandle);
        PostCompletion(file, apcRoutine, apcContext, X_STATUS_END_OF_FILE, 0);
        return X_STATUS_END_OF_FILE;
    }
    g_fileReads++;
    g_fileBytesRead += total;
    static const bool s_logScatter = getenv("XPORT_LOG_READS") != nullptr;
    if (s_logScatter)
        LOGF("{} @0x{:X} +0x{:X} -> scatter 0x{:08X}... (0x{:X} read)", file->path.filename().string(), offset, length,
            uint32_t(segments[0]), total);
    Complete(ioStatus, X_STATUS_SUCCESS, total, eventHandle);
    PostCompletion(file, apcRoutine, apcContext, X_STATUS_SUCCESS, total);
    QueueIoApc(apcRoutine, apcContext, ioStatus);
    return X_STATUS_SUCCESS;
}

uint32_t NtWriteFile(uint32_t fileHandle, uint32_t eventHandle, uint32_t apcRoutine, uint32_t apcContext, XIO_STATUS_BLOCK* ioStatus,
    const void* buffer, uint32_t length, be<uint64_t>* byteOffset)
{
    FileHandle* file = GetFile(fileHandle);
    if (!file || file->isDirectory)
        return X_STATUS_INVALID_HANDLE;

    uint64_t offset = byteOffset && byteOffset->get() != 0xFFFFFFFFFFFFFFFEull ? byteOffset->get() : file->position;
    file->stream.clear();
    file->stream.seekp(offset);
    file->stream.write((const char*)buffer, length);
    file->stream.flush();
    file->position = offset + length;

    const uint32_t status = file->stream.good() ? X_STATUS_SUCCESS : X_STATUS_ACCESS_DENIED;
    Complete(ioStatus, status, length, eventHandle);
    PostCompletion(file, apcRoutine, apcContext, status, length);
    // A request that fails at once runs no completion routine (ReadFileEx/WriteFileEx return FALSE).
    if (status == X_STATUS_SUCCESS)
        QueueIoApc(apcRoutine, apcContext, ioStatus);
    return status;
}

uint32_t NtFlushBuffersFile(uint32_t fileHandle, XIO_STATUS_BLOCK* ioStatus)
{
    if (FileHandle* file = GetFile(fileHandle); file && !file->isDirectory)
        file->stream.flush();
    Complete(ioStatus, X_STATUS_SUCCESS, 0, 0);
    return X_STATUS_SUCCESS;
}

uint32_t NtQueryInformationFile(uint32_t fileHandle, XIO_STATUS_BLOCK* ioStatus, void* info, uint32_t length, uint32_t infoClass)
{
    FileHandle* file = GetFile(fileHandle);
    if (!file)
        return X_STATUS_INVALID_HANDLE;

    std::error_code ec;
    const uint64_t size = file->isDirectory ? 0 : std::filesystem::file_size(file->path, ec);
    auto* out = (uint8_t*)info;
    uint32_t written = 0;

    switch (infoClass)
    {
    case 4: // FileBasicInformation: 4 times, attributes
        if (length < 40) return X_STATUS_INFO_LENGTH_MISMATCH;
        for (int i = 0; i < 4; i++)
            *(be<uint64_t>*)(out + i * 8) = FILE_TIME;
        *(be<uint32_t>*)(out + 32) = file->isDirectory ? X_FILE_ATTRIBUTE_DIRECTORY : X_FILE_ATTRIBUTE_NORMAL;
        *(be<uint32_t>*)(out + 36) = 0;
        written = 40;
        break;
    case 5: // FileStandardInformation: allocation size, end of file, links, delete pending, directory
        if (length < 24) return X_STATUS_INFO_LENGTH_MISMATCH;
        *(be<uint64_t>*)(out + 0) = (size + 0xFFF) & ~0xFFFull;
        *(be<uint64_t>*)(out + 8) = size;
        *(be<uint32_t>*)(out + 16) = 1;
        out[20] = 0;
        out[21] = file->isDirectory;
        written = 24;
        break;
    case 6: // FileInternalInformation: index number
        if (length < 8) return X_STATUS_INFO_LENGTH_MISMATCH;
        *(be<uint64_t>*)out = std::hash<std::string>{}(file->path.string());
        written = 8;
        break;
    case 14: // FilePositionInformation
        if (length < 8) return X_STATUS_INFO_LENGTH_MISMATCH;
        *(be<uint64_t>*)out = file->position;
        written = 8;
        break;
    case 20: // FileEndOfFileInformation
        if (length < 8) return X_STATUS_INFO_LENGTH_MISMATCH;
        *(be<uint64_t>*)out = size;
        written = 8;
        break;
    case 26: // FileSectorInformation: first disc sector of the file, used to order streaming reads.
        // There is no disc layout on the host, so every file starts at sector 0.
        if (length < 4) return X_STATUS_INFO_LENGTH_MISMATCH;
        *(be<uint32_t>*)out = 0;
        written = 4;
        break;
    case 34: // FileNetworkOpenInformation
        if (length < sizeof(X_FILE_NETWORK_OPEN_INFORMATION)) return X_STATUS_INFO_LENGTH_MISMATCH;
        FillNetworkOpenInfo(file->path, (X_FILE_NETWORK_OPEN_INFORMATION*)info);
        written = sizeof(X_FILE_NETWORK_OPEN_INFORMATION);
        break;
    default:
        LOGF_WARNING("unhandled class {} (length {}) for {}", infoClass, length, file->path.string());
        Complete(ioStatus, X_STATUS_INVALID_INFO_CLASS, 0, 0);
        return X_STATUS_INVALID_INFO_CLASS;
    }

    Complete(ioStatus, X_STATUS_SUCCESS, written, 0);
    return X_STATUS_SUCCESS;
}

// FileRenameInformation (class 10, what MoveFile sends): ReplaceIfExists, RootDirectory, then the
// new name as an ANSI_STRING. It was unhandled, so a title that writes a file under a temporary
// name and then renames it into place (the usual safe save) got an error and kept nothing.
// The name is a full guest path ("cache1:\x", "\Device\Harddisk0\Cache1\x"), relative to
// RootDirectory, or a bare name in the file's own folder. Like Xenia, an existing file of that
// name is replaced. Only files inside the save folder can move (disc files stay put).
static uint32_t RenameFile(FileHandle* file, const void* info, uint32_t length)
{
    struct RenameInformation
    {
        be<uint32_t> replaceIfExists;
        be<uint32_t> rootDirectory;
        XANSI_STRING name;
    };
    static_assert(sizeof(RenameInformation) == 16);
    if (length < sizeof(RenameInformation))
        return X_STATUS_INFO_LENGTH_MISMATCH;

    const auto* rename = static_cast<const RenameInformation*>(info);
    std::string name = GuestString(&rename->name);
    std::replace(name.begin(), name.end(), '/', '\\');
    if (name.empty())
        return X_STATUS_INVALID_PARAMETER;

    std::filesystem::path target;
    if (FileHandle* root = rename->rootDirectory != 0 ? GetFile(rename->rootDirectory) : nullptr)
        target = AppendComponents(root->path, name);
    else if (name.find(':') != std::string::npos || name[0] == '\\')
        target = ResolvePath(name);
    else
        target = AppendComponents(file->path.parent_path(), name);

    if (target.empty() || !InSaveFolder(file->path) || !InSaveFolder(target))
    {
        LOGF_WARNING("refused: {} -> {}", file->path.string(), name);
        return X_STATUS_ACCESS_DENIED;
    }
    if (Lower(target.lexically_normal().string()) == Lower(file->path.lexically_normal().string()))
        return X_STATUS_SUCCESS;

    // The host file cannot move while it is open (Windows), so the stream is closed and reopened.
    const bool reopen = file->stream.is_open();
    if (reopen)
        file->stream.close();

    const std::filesystem::path from = file->path;
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    ec.clear();
    std::filesystem::rename(from, target, ec);
    if (!ec)
        file->path = target;

    if (reopen)
        file->stream.open(file->path, file->streamMode);
    LOGF("{} -> {}{}", from.string(), target.string(), ec ? " (failed: " + ec.message() + ")" : "");
    return ec ? X_STATUS_ACCESS_DENIED : X_STATUS_SUCCESS;
}

uint32_t NtSetInformationFile(uint32_t fileHandle, XIO_STATUS_BLOCK* ioStatus, void* info, uint32_t length, uint32_t infoClass)
{
    FileHandle* file = GetFile(fileHandle);
    if (!file)
        return X_STATUS_INVALID_HANDLE;

    std::error_code ec;
    switch (infoClass)
    {
    case 4:  // FileBasicInformation: times/attributes are not kept
        break;
    case 10: // FileRenameInformation
        if (const uint32_t status = RenameFile(file, info, length); status != X_STATUS_SUCCESS)
        {
            Complete(ioStatus, status, 0, 0);
            return status;
        }
        break;
    case 13: // FileDispositionInformation: BOOLEAN DeleteFile
        if (length < 1)
            return X_STATUS_INFO_LENGTH_MISMATCH;
        file->deleteOnClose = *static_cast<const uint8_t*>(info) != 0 && InSaveFolder(file->path);
        break;
    case 14: // FilePositionInformation
        file->position = *(be<uint64_t>*)info;
        break;
    case 19: // FileAllocationInformation
    case 20: // FileEndOfFileInformation
        if (!file->isDirectory)
        {
            file->stream.flush();
            std::filesystem::resize_file(file->path, *(be<uint64_t>*)info, ec);
        }
        break;
    case 30: // FileCompletionInformation: port handle, key (Halo 3 binds its map files)
        if (length < 8)
            return X_STATUS_INFO_LENGTH_MISMATCH;
        file->completionPort = reinterpret_cast<be<uint32_t>*>(info)[0];
        file->completionKey = reinterpret_cast<be<uint32_t>*>(info)[1];
        break;
    default:
        LOGF_WARNING("unhandled class {} (length {}) for {}", infoClass, length, file->path.string());
        Complete(ioStatus, X_STATUS_INVALID_INFO_CLASS, 0, 0);
        return X_STATUS_INVALID_INFO_CLASS;
    }

    Complete(ioStatus, X_STATUS_SUCCESS, length, 0);
    return X_STATUS_SUCCESS;
}

uint32_t NtQueryFullAttributesFile(XOBJECT_ATTRIBUTES* attributes, X_FILE_NETWORK_OPEN_INFORMATION* info)
{
    const std::string guestPath = GuestString(attributes ? attributes->Name.get() : nullptr);
    std::filesystem::path path = ResolvePath(guestPath);
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec))
    {
        if (LogFileOpens())
            LOGF_WARNING("not found: {}", guestPath);
        else
            LOGF_UTILITY("not found: {}", guestPath);
        return X_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    FillNetworkOpenInfo(path, info);
    return X_STATUS_SUCCESS;
}

uint32_t NtQueryDirectoryFile(uint32_t fileHandle, uint32_t eventHandle, uint32_t apcRoutine, uint32_t apcContext, XIO_STATUS_BLOCK* ioStatus,
    uint8_t* info, uint32_t length, XANSI_STRING* fileName, uint32_t restartScan)
{
    FileHandle* file = GetFile(fileHandle);
    if (!file || !file->isDirectory)
        return X_STATUS_INVALID_HANDLE;

    if (!file->enumerated || restartScan)
    {
        const std::string pattern = fileName ? GuestString(fileName) : "*";
        file->entries.clear();
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(file->path, ec))
        {
            if (WildcardMatch(pattern.empty() ? "*" : pattern, entry.path().filename().string()))
                file->entries.push_back(entry);
        }
        file->nextEntry = 0;
        file->enumerated = true;
    }

    if (file->nextEntry >= file->entries.size())
    {
        Complete(ioStatus, X_STATUS_NO_MORE_FILES, 0, eventHandle);
        return X_STATUS_NO_MORE_FILES;
    }

    // X_FILE_DIRECTORY_INFORMATION: one entry per call, like Xenia.
    const auto& entry = file->entries[file->nextEntry++];
    const std::string name = entry.path().filename().string();
    const uint32_t needed = 0x40 + uint32_t(name.size());
    if (length < needed)
        return X_STATUS_INFO_LENGTH_MISMATCH;

    std::error_code ec;
    const bool directory = entry.is_directory(ec);
    const uint64_t size = directory ? 0 : entry.file_size(ec);
    memset(info, 0, needed);
    *(be<uint32_t>*)(info + 0x00) = 0; // NextEntryOffset
    *(be<uint32_t>*)(info + 0x04) = uint32_t(file->nextEntry - 1);
    for (int i = 0; i < 4; i++)
        *(be<uint64_t>*)(info + 0x08 + i * 8) = FILE_TIME;
    *(be<uint64_t>*)(info + 0x28) = size;
    *(be<uint64_t>*)(info + 0x30) = (size + 0xFFF) & ~0xFFFull;
    *(be<uint32_t>*)(info + 0x38) = directory ? X_FILE_ATTRIBUTE_DIRECTORY : X_FILE_ATTRIBUTE_NORMAL;
    *(be<uint32_t>*)(info + 0x3C) = uint32_t(name.size());
    memcpy(info + 0x40, name.data(), name.size());

    Complete(ioStatus, X_STATUS_SUCCESS, needed, eventHandle);
    return X_STATUS_SUCCESS;
}

uint32_t NtQueryVolumeInformationFile(uint32_t fileHandle, XIO_STATUS_BLOCK* ioStatus, uint8_t* info, uint32_t length, uint32_t infoClass)
{
    switch (infoClass)
    {
    case 1: // FileFsVolumeInformation: creation time, serial, label length, supports objects, label
        if (length < 24) return X_STATUS_INFO_LENGTH_MISMATCH;
        memset(info, 0, 24);
        *(be<uint64_t>*)(info + 0) = FILE_TIME;
        *(be<uint32_t>*)(info + 8) = 0x12345678;
        Complete(ioStatus, X_STATUS_SUCCESS, 24, 0);
        return X_STATUS_SUCCESS;
    case 3: // FileFsSizeInformation: total units, available units, sectors per unit, bytes per sector
        if (length < 24) return X_STATUS_INFO_LENGTH_MISMATCH;
        *(be<uint64_t>*)(info + 0) = 0x100000;
        *(be<uint64_t>*)(info + 8) = 0x80000;
        *(be<uint32_t>*)(info + 16) = 1;
        *(be<uint32_t>*)(info + 20) = 0x4000;
        Complete(ioStatus, X_STATUS_SUCCESS, 24, 0);
        return X_STATUS_SUCCESS;
    case 5: // FileFsAttributeInformation: attributes, max name length, name length, name
        if (length < 16) return X_STATUS_INFO_LENGTH_MISMATCH;
        *(be<uint32_t>*)(info + 0) = 0;
        *(be<uint32_t>*)(info + 4) = 255;
        *(be<uint32_t>*)(info + 8) = 4;
        memcpy(info + 12, "FATX", 4);
        Complete(ioStatus, X_STATUS_SUCCESS, 16, 0);
        return X_STATUS_SUCCESS;
    default:
        LOGF_WARNING("unhandled class {}", infoClass);
        return X_STATUS_INVALID_INFO_CLASS;
    }
}

uint32_t ObCreateSymbolicLink(XANSI_STRING* link, XANSI_STRING* target)
{
    std::string name = Lower(GuestString(link));
    if (name.starts_with("\\??\\"))
        name = name.substr(4);
    std::string destination = GuestString(target);
    LOGF("{} -> {}", name, destination);
    std::lock_guard lock(g_linkMutex);
    g_symbolicLinks[name] = destination;
    return X_STATUS_SUCCESS;
}

uint32_t ObDeleteSymbolicLink(XANSI_STRING* link)
{
    std::string name = Lower(GuestString(link));
    if (name.starts_with("\\??\\"))
        name = name.substr(4);
    std::lock_guard lock(g_linkMutex);
    g_symbolicLinks.erase(name);
    return X_STATUS_SUCCESS;
}

uint32_t NtCreateIoCompletion(be<uint32_t>* handle, uint32_t desiredAccess, void* objectAttributes, uint32_t concurrentThreads)
{
    if (!handle)
        return X_STATUS_INVALID_PARAMETER;
    *handle = GetKernelHandle(CreateKernelObject<IoCompletion>());
    return X_STATUS_SUCCESS;
}

// Waits for a packet (the timeout like any wait's: none = forever) and hands out its key, context
// and status. STATUS_TIMEOUT when none came in time.
uint32_t NtRemoveIoCompletion(uint32_t handle, be<uint32_t>* key, be<uint32_t>* apcContext, XIO_STATUS_BLOCK* ioStatus, be<int64_t>* timeout)
{
    IoCompletion* port = GetCompletionPort(handle);
    if (!port)
        return X_STATUS_INVALID_HANDLE;
    const GuestDeadline deadline = GuestTimeoutToDeadline(timeout);
    for (;;)
    {
        {
            std::lock_guard lock(g_dispatcherMutex);
            if (!port->packets.empty())
            {
                const IoCompletion::Packet packet = port->packets.front();
                port->packets.pop_front();
                if (key)
                    *key = packet.key;
                if (apcContext)
                    *apcContext = packet.apcContext;
                if (ioStatus)
                {
                    ioStatus->Status = packet.status;
                    ioStatus->Information = packet.information;
                }
                static const bool s_logRemovals = getenv("XPORT_LOG_READS") != nullptr;
                if (s_logRemovals)
                    LOGF("port 0x{:X} key {} apc 0x{:08X} status 0x{:X} 0x{:X} bytes, thread {:X}", handle, packet.key,
                        packet.apcContext, packet.status, packet.information, GuestThread::GetCurrentThreadId());
                return X_STATUS_SUCCESS;
            }
        }
        if (port->Wait(deadline) == 0x00000102) // STATUS_TIMEOUT
            return 0x00000102;
    }
}

GUEST_FUNCTION_HOOK(__imp__NtCreateIoCompletion, NtCreateIoCompletion);
GUEST_FUNCTION_HOOK(__imp__NtRemoveIoCompletion, NtRemoveIoCompletion);
GUEST_FUNCTION_HOOK(__imp__NtCreateFile, NtCreateFile);
GUEST_FUNCTION_HOOK(__imp__NtOpenFile, NtOpenFile);
GUEST_FUNCTION_HOOK(__imp__NtReadFile, NtReadFile);
GUEST_FUNCTION_HOOK(__imp__NtReadFileScatter, NtReadFileScatter);
GUEST_FUNCTION_HOOK(__imp__NtWriteFile, NtWriteFile);
GUEST_FUNCTION_HOOK(__imp__NtFlushBuffersFile, NtFlushBuffersFile);
GUEST_FUNCTION_HOOK(__imp__NtQueryInformationFile, NtQueryInformationFile);
GUEST_FUNCTION_HOOK(__imp__NtSetInformationFile, NtSetInformationFile);
GUEST_FUNCTION_HOOK(__imp__NtQueryFullAttributesFile, NtQueryFullAttributesFile);
GUEST_FUNCTION_HOOK(__imp__NtQueryDirectoryFile, NtQueryDirectoryFile);
GUEST_FUNCTION_HOOK(__imp__NtQueryVolumeInformationFile, NtQueryVolumeInformationFile);
GUEST_FUNCTION_HOOK(__imp__ObCreateSymbolicLink, ObCreateSymbolicLink);
GUEST_FUNCTION_HOOK(__imp__ObDeleteSymbolicLink, ObDeleteSymbolicLink);
