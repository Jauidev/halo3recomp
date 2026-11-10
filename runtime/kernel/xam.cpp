#include <stdafx.h>
#include "xam.h"
#include "xdm.h"
#include <cpu/guest_thread.h>
#include <ranges>
#include <unordered_set>
#include <set>
#include "xxHashMap.h"
#include <os/paths.h>
#include <os/touch_pad.h>

struct XamListener : KernelObject
{
    uint32_t id{};
    uint64_t areas{};
    std::vector<std::tuple<uint32_t, uint32_t>> notifications;

    XamListener(const XamListener&) = delete;
    XamListener& operator=(const XamListener&) = delete;

    XamListener();
    ~XamListener();
};

struct XamEnumeratorBase : KernelObject
{
    virtual uint32_t Next(void* buffer)
    {
        return -1;
    }
};

template<typename TIterator = std::vector<XHOSTCONTENT_DATA>::iterator>
struct XamEnumerator : XamEnumeratorBase
{
    uint32_t fetch;
    size_t size;
    TIterator position;
    TIterator begin;
    TIterator end;

    XamEnumerator() = default;
    XamEnumerator(uint32_t fetch, size_t size, TIterator begin, TIterator end) : fetch(fetch), size(size), position(begin), begin(begin), end(end)
    {

    }

    uint32_t Next(void* buffer) override
    {
        if (position == end)
        {
            return -1;
        }

        if (buffer == nullptr)
        {
            for (size_t i = 0; i < fetch; i++)
            {
                if (position == end)
                {
                    return i == 0 ? -1 : i;
                }

                ++position;
            }
        }

        for (size_t i = 0; i < fetch; i++)
        {
            if (position == end)
            {
                return i == 0 ? -1 : i;
            }

            memcpy(buffer, &*position, size);

            ++position;
            buffer = (void*)((size_t)buffer + size);
        }

        return fetch;
    }
};

std::array<xxHashMap<XHOSTCONTENT_DATA>, 3> gContentRegistry{};
std::unordered_set<XamListener*> gListeners{};
xxHashMap<std::string> gRootMap;

std::string_view XamGetRootPath(const std::string_view& root)
{
    const auto result = gRootMap.find(StringHash(root));

    if (result == gRootMap.end())
        return "";

    return result->second;
}

void XamRootCreate(const std::string_view& root, const std::string_view& path)
{
    gRootMap.emplace(StringHash(root), path);
#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
    // ResolvePath looks devices up in lower case: Gears of War 3's checkpoint root "SG0_0" was an
    // unknown device. Its lower-case name is registered too (only this title).
    std::string lower(root);
    for (char& c : lower)
        c = char(tolower((unsigned char)c));
    if (lower != root)
        gRootMap.emplace(StringHash(lower), path);
#endif
}

XamListener::XamListener()
{
    gListeners.insert(this);
}

XamListener::~XamListener()
{
    gListeners.erase(this);
}

XCONTENT_DATA XamMakeContent(uint32_t type, const std::string_view& name)
{
    XCONTENT_DATA data{ 1, type };

    strncpy(data.szFileName, name.data(), sizeof(data.szFileName));

    return data;
}

void XamRegisterContent(const XCONTENT_DATA& data, const std::string_view& root)
{
    const auto idx = data.dwContentType - 1;

    gContentRegistry[idx].emplace(StringHash(data.szFileName), XHOSTCONTENT_DATA{ data }).first->second.szRoot = root;
}

void XamRegisterContent(uint32_t type, const std::string_view name, const std::string_view& root)
{
    XCONTENT_DATA data{ 1, type, {}, "" };

    strncpy(data.szFileName, name.data(), sizeof(data.szFileName));

    XamRegisterContent(data, root);
}

uint32_t XamNotifyCreateListener(uint64_t qwAreas)
{
    auto* listener = CreateKernelObject<XamListener>();

    listener->areas = qwAreas;

    // like Xenia's KernelState::RegisterNotifyListener, the first listener of system
    // notifications gets the ones a console sends at title start: the system UI opening and
    // closing, and the sign-in change of the already signed-in profile. Titles wait for them.
    static bool s_notifiedStartup;
    if (!s_notifiedStartup && (qwAreas & 1))
    {
        s_notifiedStartup = true;
        listener->notifications.emplace_back(9, 1);   // XN_SYS_UI: shown
        listener->notifications.emplace_back(9, 0);   // XN_SYS_UI: hidden
        listener->notifications.emplace_back(10, 1);  // XN_SYS_SIGNINCHANGED: user 0
        listener->notifications.emplace_back(10, 1);
    }

    return GetKernelHandle(listener);
}

void XamNotifyEnqueueEvent(uint32_t dwId, uint32_t dwParam)
{
    for (const auto& listener : gListeners)
    {
        if (((1 << MSG_AREA(dwId)) & listener->areas) == 0)
            continue;

        listener->notifications.emplace_back(dwId, dwParam);
    }
}

bool XNotifyGetNext(uint32_t hNotification, uint32_t dwMsgFilter, be<uint32_t>* pdwId, be<uint32_t>* pParam)
{
    auto& listener = *GetKernelObject<XamListener>(hNotification);

    if (dwMsgFilter)
    {
        for (size_t i = 0; i < listener.notifications.size(); i++)
        {
            if (std::get<0>(listener.notifications[i]) == dwMsgFilter)
            {
                if (pdwId)
                    *pdwId = std::get<0>(listener.notifications[i]);

                if (pParam)
                    *pParam = std::get<1>(listener.notifications[i]);

                listener.notifications.erase(listener.notifications.begin() + i);

                return true;
            }
        }
    }
    else
    {
        if (listener.notifications.empty())
            return false;

        if (pdwId)
            *pdwId = std::get<0>(listener.notifications[0]);

        if (pParam)
            *pParam = std::get<1>(listener.notifications[0]);

        listener.notifications.erase(listener.notifications.begin());

        return true;
    }

    return false;
}

uint32_t CompleteOverlapped(XXOVERLAPPED* overlapped, uint32_t result, uint32_t length);

uint32_t XamShowMessageBoxUI(uint32_t dwUserIndex, be<uint16_t>* wszTitle, be<uint16_t>* wszText, uint32_t cButtons,
    xpointer<be<uint16_t>>* pwszButtons, uint32_t dwFocusButton, uint32_t dwFlags, be<uint32_t>* pResult, XXOVERLAPPED* pOverlapped)
{
    *pResult = cButtons ? cButtons - 1 : 0;

#if _DEBUG
    assert("XamShowMessageBoxUI encountered!" && false);
#elif _WIN32
    // This code is Win32-only as it'll most likely crash, misbehave or
    // cause corruption due to using a different type of memory than what
    // wchar_t is on Linux. Windows uses 2 bytes while Linux uses 4 bytes.
    std::vector<std::wstring> texts{};

    texts.emplace_back(reinterpret_cast<wchar_t*>(wszTitle));
    texts.emplace_back(reinterpret_cast<wchar_t*>(wszText));

    for (size_t i = 0; i < cButtons; i++)
        texts.emplace_back(reinterpret_cast<wchar_t*>(pwszButtons[i].get()));

    for (auto& text : texts)
    {
        for (size_t i = 0; i < text.size(); i++)
            ByteSwapInplace(text[i]);
    }

    wprintf(L"[XamShowMessageBoxUI] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    wprintf(L"[XamShowMessageBoxUI] If you are encountering this message and the game has ceased functioning,\n");
    wprintf(L"[XamShowMessageBoxUI] please create an issue at https://github.com/hedge-dev/UnleashedRecomp/issues.\n");
    wprintf(L"[XamShowMessageBoxUI] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    wprintf(L"[XamShowMessageBoxUI] %ls\n", texts[0].c_str());
    wprintf(L"[XamShowMessageBoxUI] %ls\n", texts[1].c_str());
    wprintf(L"[XamShowMessageBoxUI] ");

    for (size_t i = 0; i < cButtons; i++)
    {
        wprintf(L"%ls", texts[2 + i].c_str());

        if (i != cButtons - 1)
            wprintf(L" | ");
    }

    wprintf(L"\n");
    wprintf(L"[XamShowMessageBoxUI] Defaulted to button: %d\n", pResult->get());
    wprintf(L"[XamShowMessageBoxUI] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
#endif

    LOGF("{} buttons, answered {}", cButtons, pResult->get());
    XamNotifyEnqueueEvent(9, 0);

    // complete the request through the XOVERLAPPED and signal its event.
    if (pOverlapped)
        return CompleteOverlapped(pOverlapped, 0, 0);

    return 0;
}

uint32_t XamContentCreateEnumerator(uint32_t dwUserIndex, uint32_t DeviceID, uint32_t dwContentType,
    uint32_t dwContentFlags, uint32_t cItem, be<uint32_t>* pcbBuffer, be<uint32_t>* phEnum)
{
    if (dwUserIndex != 0)
    {
        GuestThread::SetLastError(ERROR_NO_SUCH_USER);
        return 0xFFFFFFFF;
    }

    // The registry knows saves, downloads and publisher content (types 1-3); any other type
    // read past it. It has no items here.
    if (dwContentType - 1 >= gContentRegistry.size())
    {
        LOGF_WARNING("content type 0x{:X}: no items", dwContentType);
        if (pcbBuffer)
            *pcbBuffer = sizeof(_XCONTENT_DATA) * cItem;
        *phEnum = GetKernelHandle(CreateKernelObject<XamEnumeratorBase>());
        return 0;
    }

    const auto& registry = gContentRegistry[dwContentType - 1];
    const auto& values = registry | std::views::values;
    auto* enumerator = CreateKernelObject<XamEnumerator<decltype(values.begin())>>(cItem, sizeof(_XCONTENT_DATA), values.begin(), values.end());

    if (pcbBuffer)
        *pcbBuffer = sizeof(_XCONTENT_DATA) * cItem;

    *phEnum = GetKernelHandle(enumerator);

    return 0;
}

struct Event;
uint32_t NtSetEvent(Event* handle, be<uint32_t>* previousState);

// finishes an asynchronous XAM request at once (Xenia's CompleteOverlappedImmediateEx):
// result, extended error (what XGetOverlappedExtendedError returns) and length go into the
// XOVERLAPPED and its event is signaled. Returns what the caller must return, ERROR_IO_PENDING.
static uint32_t CompleteOverlappedEx(XXOVERLAPPED* overlapped, uint32_t result, uint32_t extendedError, uint32_t length)
{
    overlapped->Error = result;
    overlapped->Length = length;
    overlapped->dwExtendedError = extendedError;

    if (overlapped->hEvent != 0)
        NtSetEvent(GetKernelObject<Event>(overlapped->hEvent), nullptr);

    // Completion routines are APCs, which this runtime does not deliver yet.
    if (overlapped->pCompletionRoutine != 0)
        LOGF_WARNING("completion routine 0x{:X} ignored", uint32_t(overlapped->pCompletionRoutine));

    return 997; // ERROR_IO_PENDING
}

uint32_t CompleteOverlapped(XXOVERLAPPED* overlapped, uint32_t result, uint32_t length)
{
    // HRESULT_FROM_WIN32(result), as Xenia stores it. The title tells an empty save list (0x80070012,
    // ERROR_NO_MORE_FILES) from a failed enumeration by this field; with 0 it set the save count
    // to -1 and its save scan never finished.
    return CompleteOverlappedEx(overlapped, result, result ? 0x80070000 | (result & 0xFFFF) : 0, length);
}

uint32_t XamEnumerate(uint32_t hEnum, uint32_t dwFlags, void* pvBuffer, uint32_t cbBuffer, be<uint32_t>* pcItemsReturned, XXOVERLAPPED* pOverlapped)
{
    auto* enumerator = GetKernelObject<XamEnumeratorBase>(hEnum);
    const auto count = enumerator->Next(pvBuffer);

    const uint32_t result = count == -1 ? ERROR_NO_MORE_FILES : ERROR_SUCCESS;
    const uint32_t items = count == -1 ? 0 : count;

    // asynchronous enumerations complete through the XOVERLAPPED (Unleashed ignored it,
    // so a game waiting on its event waited forever). An asynchronous request that fails ends with
    // ERROR_FUNCTION_FAILED, the reason in the extended error (XAM, and Xenia's xeXamEnumerate):
    // Halo 3's enumeration task (sub_82580B30) takes 0/0 or 1627/0x80070012 as done, and with a
    // plain 18 its content catalogue dropped the empty download list, never became valid and
    // refused to create the "save and quit" copy (2026-11-09).
    if (pOverlapped)
    {
        if (result != ERROR_SUCCESS)
            return CompleteOverlappedEx(pOverlapped, 1627, 0x80070000 | result, 0); // ERROR_FUNCTION_FAILED
        return CompleteOverlapped(pOverlapped, result, items);
    }

    if (pcItemsReturned)
        *pcItemsReturned = items;

    return result;
}

// XAM enumerators created on behalf of the title (Xenia's XStaticUntypedEnumerator):
// an empty enumerator plus a zeroed block of extraSize bytes that the caller keeps its own
// state in, returned by XamGetPrivateEnumStructureFromHandle.
struct PrivateEnumerator final : XamEnumeratorBase
{
    uint32_t extra = 0; // guest address

    ~PrivateEnumerator() override
    {
        if (extra != 0)
            g_userHeap.Free(g_memory.Translate(extra));
    }
};

uint32_t XamCreateEnumeratorHandle(uint32_t userIndex, uint32_t appId, uint32_t openMessage, uint32_t closeMessage,
    uint32_t extraSize, uint32_t itemCount, uint32_t flags, be<uint32_t>* handle)
{
    auto* enumerator = CreateKernelObject<PrivateEnumerator>();
    if (extraSize != 0)
    {
        void* extra = g_userHeap.Alloc(extraSize);
        memset(extra, 0, extraSize);
        enumerator->extra = g_memory.MapVirtual(extra);
    }

    LOGF("app 0x{:X} messages 0x{:X}/0x{:X} extra {} items {}", appId, openMessage, closeMessage, extraSize, itemCount);
    *handle = GetKernelHandle(enumerator);
    return ERROR_SUCCESS;
}

uint32_t XamGetPrivateEnumStructureFromHandle(uint32_t handle, be<uint32_t>* structure)
{
    auto* object = GetKernelObject(handle);
    auto* enumerator = IsRegisteredKernelObject(object) ? dynamic_cast<PrivateEnumerator*>(object) : nullptr;
    if (enumerator == nullptr)
        return 0xC0000008; // STATUS_INVALID_HANDLE

    if (structure)
        *structure = enumerator->extra;
    return 0;
}

// no achievements are stored, so the enumerator is empty. The buffer size follows
// Xenia: X_ACHIEVEMENT_DETAILS (0x24 bytes) plus 464 bytes of strings per achievement.
uint32_t XamUserCreateAchievementEnumerator(uint32_t titleId, uint32_t userIndex, uint64_t xuid, uint32_t flags,
    uint32_t offset, uint32_t count, be<uint32_t>* bufferSize, be<uint32_t>* handle)
{
    if (bufferSize)
        *bufferSize = count * (0x24 + 464);

    *handle = GetKernelHandle(CreateKernelObject<XamEnumeratorBase>());
    return ERROR_SUCCESS;
}

// rewritten. Each save content gets its own folder (save path / content name) so the title's
// slots do not overwrite each other, contents found on disk count as existing, all creation
// modes are handled, and an XOVERLAPPED is always completed (the game polls it while checking
// its save slots). Content flags, low 4 bits: 1 create new, 2 create always, 3 open existing,
// 4 open always, 5 truncate existing.
static std::filesystem::path ContentRoot(const XCONTENT_DATA& content)
{
    if (content.dwContentType == XCONTENTTYPE_SAVEDATA)
        return os::paths::GetSavePath() / std::string(content.szFileName, strnlen(content.szFileName, sizeof(content.szFileName)));

    return os::paths::GetGamePath();
}

// each save's XCONTENT_DATA (display name included) as the game created it, beside
// its folder: save/<name>.xcontent. Only the registry knew it, and the registry starts empty, so
// after a restart the saves on disk were not enumerated.
static std::filesystem::path ContentRecordPath(const std::filesystem::path& root)
{
    std::filesystem::path record = root;
    record += ".xcontent";
    return record;
}

void XamRegisterSaves()
{
    std::error_code ec;
    uint32_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(os::paths::GetSavePath(), ec))
    {
        if (!entry.is_directory(ec))
            continue;
        const std::string name = entry.path().filename().string();
        if (name.empty() || name.size() >= XCONTENT_MAX_FILENAME)
            continue;
        // The hard disk's own areas (kernel/io/nt_file.cpp: the utility drive's cache0: and cache1:,
        // and the partitions in hdd) are no saves: listed as saved games, Halo 3's content
        // catalogue counted them as its items, and it may open or delete the ones it does not know.
        if (name == "cache0" || name == "cache1" || name == "hdd")
            continue;
        XCONTENT_DATA data = XamMakeContent(XCONTENTTYPE_SAVEDATA, name);
        if (FILE* file = fopen(ContentRecordPath(entry.path()).string().c_str(), "rb"))
        {
            XCONTENT_DATA stored{};
            if (fread(&stored, sizeof(stored), 1, file) == 1 && stored.dwContentType == XCONTENTTYPE_SAVEDATA &&
                strncmp(stored.szFileName, data.szFileName, sizeof(data.szFileName)) == 0)
            {
                data = stored;
            }
            fclose(file);
        }
        XamRegisterContent(data, (const char*)entry.path().u8string().c_str());
        ++count;
    }
    LOGFN("saves: {} found in {}", count, os::paths::GetSavePath().string());
}

static uint32_t FinishContentRequest(XXOVERLAPPED* overlapped, uint32_t result)
{
    return overlapped ? CompleteOverlapped(overlapped, result, 0) : result;
}

uint32_t XamContentCreateEx(uint32_t dwUserIndex, const char* szRootName, const XCONTENT_DATA* pContentData,
    uint32_t dwContentFlags, be<uint32_t>* pdwDisposition, be<uint32_t>* pdwLicenseMask,
    uint32_t dwFileCacheSize, uint64_t uliContentSize, PXXOVERLAPPED pOverlapped)
{
    constexpr uint32_t ERROR_ALREADY_EXISTS_X = 183;
    constexpr uint32_t CREATE_NEW_X = 1, CREATE_ALWAYS_X = 2, OPEN_ALWAYS_X = 4, TRUNCATE_EXISTING_X = 5;

    if (pContentData->dwContentType - 1 >= gContentRegistry.size())
    {
        LOGF_WARNING("{}: content type 0x{:X} is not supported", szRootName, uint32_t(pContentData->dwContentType));
        return FinishContentRequest(pOverlapped, ERROR_PATH_NOT_FOUND);
    }

    auto& registry = gContentRegistry[pContentData->dwContentType - 1];
    const auto hash = StringHash(pContentData->szFileName);
    const std::filesystem::path root = ContentRoot(*pContentData);

    std::error_code ec;
    const bool onDisk = pContentData->dwContentType == XCONTENTTYPE_SAVEDATA && std::filesystem::is_directory(root, ec);
    const bool exists = registry.contains(hash) || onDisk;
    const uint32_t mode = dwContentFlags & 0xF;

    LOGF("{} '{}' type 0x{:X} mode {} -> {}", szRootName, std::string(pContentData->szFileName, strnlen(pContentData->szFileName, sizeof(pContentData->szFileName))),
        uint32_t(pContentData->dwContentType), mode, exists ? "exists" : "new");

    if (pdwLicenseMask)
        *pdwLicenseMask = 0xFFFFFFFF;

    const bool create = mode == CREATE_NEW_X || mode == CREATE_ALWAYS_X || mode == OPEN_ALWAYS_X;
    if (mode == CREATE_NEW_X && exists)
        return FinishContentRequest(pOverlapped, ERROR_ALREADY_EXISTS_X);
    if (!exists && !create)
    {
        if (pdwDisposition)
            *pdwDisposition = XCONTENT_NEW;
        return FinishContentRequest(pOverlapped, ERROR_PATH_NOT_FOUND);
    }

    if (!exists)
        std::filesystem::create_directories(root, ec);
    // The title opens a save with an XCONTENT_DATA that has no display name, and tells the episode of
    // a save ("TLAD - <mission> - MM/DD/YY HH:MM:SS", parsed by sub_821CD930) and the newest one
    // from that name: a nameless save belongs to no episode and is never loaded. Recording every
    // open erased the names, one save per launch, until a new game started (2026-10-27). As on
    // the console, the name is set only when the content is created or replaced: loading from the
    // pause menu opens (mode 3) with a fresh "<episode> - <mission> - <now>" name, which would
    // have stamped the loaded save as new. A replaced save also refreshes the registry, so a
    // save made this session lists its new name and date.
    const bool named = pContentData->szDisplayName[0] != 0;
    const bool rename = named && (!exists || mode == CREATE_ALWAYS_X || mode == TRUNCATE_EXISTING_X);
    const auto registered = registry.find(hash);
    if (registered == registry.end())
        XamRegisterContent(*pContentData, (const char*)root.u8string().c_str());
    else if (rename)
        static_cast<XCONTENT_DATA&>(registered->second) = *pContentData;
    if (pContentData->dwContentType == XCONTENTTYPE_SAVEDATA && (rename || !exists))
    {
        if (FILE* file = fopen(ContentRecordPath(root).string().c_str(), "wb"))
        {
            fwrite(pContentData, sizeof(XCONTENT_DATA), 1, file);
            fclose(file);
        }
    }

    if ((mode == CREATE_ALWAYS_X || mode == TRUNCATE_EXISTING_X) && exists && pContentData->dwContentType == XCONTENTTYPE_SAVEDATA)
    {
        for (const auto& entry : std::filesystem::directory_iterator(root, ec))
            std::filesystem::remove_all(entry.path(), ec);
    }

    if (pdwDisposition)
        *pdwDisposition = exists ? XCONTENT_EXISTING : XCONTENT_NEW;

    XamRootCreate(szRootName, registry.find(hash)->second.szRoot);
    return FinishContentRequest(pOverlapped, ERROR_SUCCESS);
}

uint32_t XamContentClose(const char* szRootName, XXOVERLAPPED* pOverlapped)
{
    gRootMap.erase(StringHash(szRootName));
#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
    std::string lower(szRootName);
    for (char& c : lower)
        c = char(tolower((unsigned char)c));
    gRootMap.erase(StringHash(lower));
#endif
    return FinishContentRequest(pOverlapped, ERROR_SUCCESS);
}

// XamContentDelete (user index, XCONTENT_DATA, XOVERLAPPED, as Xenia): removes a save's folder and
// the record and thumbnail beside it. It was a stub that returned nothing and never completed the
// XOVERLAPPED, so a title deleting a content item (Halo 3's content catalogue has a delete task
// for its films, clips, screenshots and variants) waited on it forever. Only a plain folder name
// is accepted: an empty one would name the whole save folder.
uint32_t XamContentDelete(uint32_t dwUserIndex, const XCONTENT_DATA* pContentData, XXOVERLAPPED* pOverlapped)
{
    constexpr uint32_t ERROR_FILE_NOT_FOUND_X = 2, ERROR_ACCESS_DENIED_X = 5;
    if (pContentData == nullptr || pContentData->dwContentType != XCONTENTTYPE_SAVEDATA)
        return FinishContentRequest(pOverlapped, ERROR_ACCESS_DENIED_X);

    const std::string name(pContentData->szFileName, strnlen(pContentData->szFileName, sizeof(pContentData->szFileName)));
    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos)
        return FinishContentRequest(pOverlapped, ERROR_ACCESS_DENIED_X);

    const std::filesystem::path root = ContentRoot(*pContentData);
    std::error_code ec;
    const bool existed = std::filesystem::is_directory(root, ec);
    if (existed)
    {
        std::filesystem::remove_all(root, ec);
        std::filesystem::remove(ContentRecordPath(root), ec);
        std::filesystem::path thumbnail = root;
        thumbnail += ".thumbnail";
        std::filesystem::remove(thumbnail, ec);
    }
    gContentRegistry[XCONTENTTYPE_SAVEDATA - 1].erase(StringHash(pContentData->szFileName));
    LOGF("'{}' {}", name, existed ? "deleted" : "not found");
    return FinishContentRequest(pOverlapped, existed ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND_X);
}

// XamContentFlush (root name, XOVERLAPPED): commits an open content's writes. Every write here is
// already on disk, so it only has to finish the request; the generated stub returned 0 and left
// the XOVERLAPPED pending, so Halo 3's flush task never ended.
uint32_t XamContentFlush(const char* szRootName, XXOVERLAPPED* pOverlapped)
{
    LOGF("{}", szRootName ? szRootName : "(null)");
    return FinishContentRequest(pOverlapped, ERROR_SUCCESS);
}

// the game sets the save's thumbnail as it saves, through an XOVERLAPPED, and waits
// for it: the generated stub never completed it, and every save ended in "Error al autoguardar"
// although the save file had been written (2026-10-27). The arguments are those of xenia's
// XamContentSetThumbnail (the call at 0x8284A274 passes the user index in r3 and the overlapped
// in r7). The picture is kept beside the content folder (save/<name>.thumbnail), outside the
// files the game reads.
uint32_t XamContentSetThumbnail(uint32_t dwUserIndex, const XCONTENT_DATA* pContentData,
    const uint8_t* pbThumbnail, uint32_t cbThumbnail, XXOVERLAPPED* pOverlapped)
{
    // Xbox thumbnails are under 16 KB.
    if (pContentData && pContentData->dwContentType == XCONTENTTYPE_SAVEDATA && pbThumbnail &&
        cbThumbnail && cbThumbnail <= 256 * 1024)
    {
        std::filesystem::path path = ContentRoot(*pContentData);
        path += ".thumbnail";
        if (FILE* file = fopen(path.string().c_str(), "wb"))
        {
            fwrite(pbThumbnail, 1, cbThumbnail, file);
            fclose(file);
        }
    }
    LOGF("{} ({} bytes)", pContentData ? std::string(pContentData->szFileName,
        strnlen(pContentData->szFileName, sizeof(pContentData->szFileName))) : std::string(), cbThumbnail);
    return FinishContentRequest(pOverlapped, ERROR_SUCCESS);
}

uint32_t XamContentGetDeviceData(uint32_t DeviceID, XDEVICE_DATA* pDeviceData)
{
    static constexpr char name[] = "Hard Drive";

    pDeviceData->DeviceID = DeviceID;
    pDeviceData->DeviceType = XCONTENTDEVICETYPE_HDD;
    pDeviceData->ulDeviceBytes = 0x10000000;
    pDeviceData->ulDeviceFreeBytes = 0x10000000;
    for (size_t i = 0; i < sizeof(name); i++)
        pDeviceData->wszName[i] = name[i];

    return ERROR_SUCCESS;
}

// XPORT_LOG_INPUT=1: logs each distinct (function, user index, flags, result) of the pad and
// sign-in queries once (why a title counts more local players than one).
void LogInputQuery(const char* function, uint32_t userIndex, uint32_t flags, uint32_t result)
{
    static const bool s_enabled = getenv("XPORT_LOG_INPUT") != nullptr;
    if (!s_enabled)
        return;
    static std::mutex s_mutex;
    static std::set<std::string> s_seen;
    std::string key = fmt::format("{} user={:X} flags={:X} -> {:X}", function, userIndex, flags, result);
    std::lock_guard lock(s_mutex);
    if (s_seen.size() < 256 && s_seen.insert(key).second)
        LOGF_WARNING("input query: {}", key);
}

// XamInputGetCapabilities takes (userIndex, flags, caps); the extra leading argument in
// UnleashedRecomp belongs to XamInputGetCapabilitiesEx. One wired pad, always connected. Real input (Bluetooth pad, touch) comes in phase 6;
// until then the game sees a pad with nothing pressed.
uint32_t XamInputGetCapabilities(uint32_t userIndex, uint32_t flags, XAMINPUT_CAPABILITIES* caps)
{
    LogInputQuery("XamInputGetCapabilities", userIndex, flags, userIndex != 0 ? ERROR_DEVICE_NOT_CONNECTED : 0);
    if (userIndex != 0)
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(caps, 0, sizeof(*caps));
    caps->Type = XAMINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XAMINPUT_DEVSUBTYPE_GAMEPAD;
    return ERROR_SUCCESS;
}

// XPORT_AUTOPRESS=1 (headless runs only): after 15 s of boot, taps START, then A, every 3 seconds
// for 200 ms, to get past "press START" screens and menus without a pad. Stops for good at the
// first touch on the on-screen controls.
static uint16_t AutoPressButtons()
{
    static const bool s_enabled = getenv("XPORT_AUTOPRESS") != nullptr;
    if (!s_enabled)
        return 0;

    // A person is playing: stop pressing (it would open the pause menu every 6 seconds).
    static std::atomic<bool> s_touched;
    const os::touch_pad::State pad = os::touch_pad::Get();
    if (s_touched || pad.buttons != 0 || pad.leftTrigger != 0 || pad.rightTrigger != 0 ||
        pad.thumbLX != 0 || pad.thumbLY != 0 || pad.thumbRX != 0 || pad.thumbRY != 0)
    {
        s_touched = true;
        return 0;
    }

    using namespace std::chrono;
    static const auto s_start = steady_clock::now();
    const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - s_start).count();
    // XPORT_AUTOPRESS_UNTIL=<seconds>: stop there (measurements in the game, not the pause menu).
    static const long long s_until = [] {
        const char* until = getenv("XPORT_AUTOPRESS_UNTIL");
        return until ? atoll(until) * 1000 : LLONG_MAX;
    }();
    if (elapsed < 15000 || elapsed >= s_until || elapsed % 3000 >= 200)
        return 0;

    // XPORT_AUTOPRESS_START_UNTIL=<seconds>: only A after that, so a measurement run never opens
    // the pause menu however long the loading takes.
    static const long long s_start_until = [] {
        const char* until = getenv("XPORT_AUTOPRESS_START_UNTIL");
        return until ? atoll(until) * 1000 : LLONG_MAX;
    }();
    if (elapsed >= s_start_until)
        return XAMINPUT_GAMEPAD_A;
    return (elapsed / 3000) % 2 ? XAMINPUT_GAMEPAD_A : XAMINPUT_GAMEPAD_START;
}

// XPORT_AUTOPRESS_LOOK=1 (headless runs only): after 15 s also pushes the right stick up, then
// down, every 4 seconds, for titles that wait for the player to look around (Halo 3's Arrival:
// the visor shot waits for the look calibration). Stops at the first touch, like AutoPressButtons.
static int16_t AutoLookY()
{
    static const bool s_enabled = getenv("XPORT_AUTOPRESS_LOOK") != nullptr;
    if (!s_enabled)
        return 0;
    static std::atomic<bool> s_touched;
    const os::touch_pad::State pad = os::touch_pad::Get();
    if (pad.buttons || pad.thumbLX || pad.thumbLY || pad.thumbRX || pad.thumbRY)
        s_touched = true;
    if (s_touched)
        return 0;
    using namespace std::chrono;
    static const auto s_start = steady_clock::now();
    const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - s_start).count();
    if (elapsed < 15000)
        return 0;
    // XPORT_AUTOPRESS_LOOK_WORLD_AT=<seconds>: from then on, the view is levelled (down to the
    // floor limit for 2.5 s, then up for XPORT_AUTOPRESS_LOOK_LEVEL_MS, default 900) and stays
    // level while AutoLookX turns it and AutoMoveY walks: captures of the world, not the ground.
    static const long long s_world_at = [] {
        const char* at = getenv("XPORT_AUTOPRESS_LOOK_WORLD_AT");
        return at ? atoll(at) * 1000 : LLONG_MAX;
    }();
    static const long long s_level_ms = [] {
        const char* level = getenv("XPORT_AUTOPRESS_LOOK_LEVEL_MS");
        return level ? atoll(level) : 900;
    }();
    if (elapsed >= s_world_at)
    {
        const long long world = elapsed - s_world_at;
        if (world < 2500)
            return -32767;
        if (world < 2500 + s_level_ms)
            return 32767;
        return 0;
    }
    const auto phase = elapsed % 4000;
    if (phase < 600)
        return 32767;
    if (phase >= 2000 && phase < 2600)
        return -32767;
    return 0;
}

// With XPORT_AUTOPRESS_LOOK_WORLD_AT, once the view is level: every 6 s, turn right for 700 ms,
// then walk forward for 1.5 s.
static long long AutoWorldPhase()
{
    static const bool s_enabled = getenv("XPORT_AUTOPRESS_LOOK") != nullptr;
    static const long long s_world_at = [] {
        const char* at = getenv("XPORT_AUTOPRESS_LOOK_WORLD_AT");
        return at ? atoll(at) * 1000 : LLONG_MAX;
    }();
    static const long long s_level_ms = [] {
        const char* level = getenv("XPORT_AUTOPRESS_LOOK_LEVEL_MS");
        return level ? atoll(level) : 900;
    }();
    if (!s_enabled || s_world_at == LLONG_MAX)
        return -1;
    const os::touch_pad::State pad = os::touch_pad::Get();
    if (pad.buttons || pad.thumbLX || pad.thumbLY || pad.thumbRX || pad.thumbRY)
        return -1;
    using namespace std::chrono;
    static const auto s_start = steady_clock::now();
    const long long elapsed = duration_cast<milliseconds>(steady_clock::now() - s_start).count();
    const long long settled = elapsed - s_world_at - 2500 - s_level_ms - 500;
    return settled < 0 ? -1 : settled % 6000;
}

static int16_t AutoLookX()
{
    const long long phase = AutoWorldPhase();
    return phase >= 0 && phase < 700 ? int16_t(20000) : int16_t(0);
}

// XPORT_AUTOPRESS_WALK_MS=<ms> (default 1500): how long each cycle walks; 0 only turns in place,
// so the captures show the world around the start instead of grass right in front of the camera.
static int16_t AutoMoveY()
{
    static const long long s_walk_ms = [] {
        const char* walk = getenv("XPORT_AUTOPRESS_WALK_MS");
        return walk ? std::clamp(atoll(walk), 0LL, 4500LL) : 1500LL;
    }();
    const long long phase = AutoWorldPhase();
    return phase >= 1500 && phase < 1500 + s_walk_ms ? int16_t(32767) : int16_t(0);
}

// Pad 0: the touch controls (os::touch_pad, set by the Android app) plus XPORT_AUTOPRESS.
uint32_t XamInputGetState(uint32_t userIndex, uint32_t flags, XAMINPUT_STATE* state)
{
    LogInputQuery("XamInputGetState", userIndex, flags, userIndex != 0 ? ERROR_DEVICE_NOT_CONNECTED : 0);
    if (userIndex != 0)
        return ERROR_DEVICE_NOT_CONNECTED;

    static std::atomic<uint32_t> s_packet;
    const os::touch_pad::State pad = os::touch_pad::Get();
    memset(state, 0, sizeof(*state));
    // The struct's fields are plain integers in guest (big-endian) order.
    state->dwPacketNumber = ByteSwap(uint32_t(++s_packet));
    state->Gamepad.wButtons = ByteSwap(uint16_t(pad.buttons | AutoPressButtons()));
    state->Gamepad.bLeftTrigger = pad.leftTrigger;
    state->Gamepad.bRightTrigger = pad.rightTrigger;
    state->Gamepad.sThumbLX = ByteSwap(pad.thumbLX);
    const int16_t autoMove = AutoMoveY();
    state->Gamepad.sThumbLY = ByteSwap(autoMove ? autoMove : pad.thumbLY);
    const int16_t autoTurn = AutoLookX();
    state->Gamepad.sThumbRX = ByteSwap(autoTurn ? autoTurn : pad.thumbRX);
    const int16_t autoLook = AutoLookY();
    state->Gamepad.sThumbRY = ByteSwap(autoLook ? autoLook : pad.thumbRY);
    return ERROR_SUCCESS;
}

uint32_t XamInputSetState(uint32_t userIndex, uint32_t flags, XAMINPUT_VIBRATION* vibration)
{
    return userIndex == 0 ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

// Menus read the pad as keystrokes (XINPUT_KEYSTROKE). They are generated from the pad state
// (touch controls and XPORT_AUTOPRESS): one key down / key up per call for every pressed or
// released button, trigger or left stick direction, plus repeats while a direction is held. An
// empty queue returns ERROR_EMPTY (success there made the title's "drain all keystrokes" loop spin
// forever). userIndex 0xFF means any user.
namespace
{
    // Pseudo-buttons: the 16 XAMINPUT_GAMEPAD bits, then the triggers and the left stick
    // directions, with their VK_PAD_* codes.
    constexpr uint32_t kKeyLeftTrigger = 1u << 16, kKeyRightTrigger = 1u << 17;
    constexpr uint32_t kKeyThumbUp = 1u << 18, kKeyThumbDown = 1u << 19, kKeyThumbRight = 1u << 20,
        kKeyThumbLeft = 1u << 21;
    constexpr uint32_t kRepeatingKeys = XAMINPUT_GAMEPAD_DPAD_UP | XAMINPUT_GAMEPAD_DPAD_DOWN |
        XAMINPUT_GAMEPAD_DPAD_LEFT | XAMINPUT_GAMEPAD_DPAD_RIGHT | kKeyThumbUp | kKeyThumbDown |
        kKeyThumbRight | kKeyThumbLeft;

    uint16_t VirtualKey(uint32_t key)
    {
        switch (key)
        {
        case XAMINPUT_GAMEPAD_A: return 0x5800;
        case XAMINPUT_GAMEPAD_B: return 0x5801;
        case XAMINPUT_GAMEPAD_X: return 0x5802;
        case XAMINPUT_GAMEPAD_Y: return 0x5803;
        case XAMINPUT_GAMEPAD_RIGHT_SHOULDER: return 0x5804;
        case XAMINPUT_GAMEPAD_LEFT_SHOULDER: return 0x5805;
        case kKeyLeftTrigger: return 0x5806;
        case kKeyRightTrigger: return 0x5807;
        case XAMINPUT_GAMEPAD_DPAD_UP: return 0x5810;
        case XAMINPUT_GAMEPAD_DPAD_DOWN: return 0x5811;
        case XAMINPUT_GAMEPAD_DPAD_LEFT: return 0x5812;
        case XAMINPUT_GAMEPAD_DPAD_RIGHT: return 0x5813;
        case XAMINPUT_GAMEPAD_START: return 0x5814;
        case XAMINPUT_GAMEPAD_BACK: return 0x5815;
        case XAMINPUT_GAMEPAD_LEFT_THUMB: return 0x5816;
        case XAMINPUT_GAMEPAD_RIGHT_THUMB: return 0x5817;
        case kKeyThumbUp: return 0x5820;
        case kKeyThumbDown: return 0x5821;
        case kKeyThumbRight: return 0x5822;
        case kKeyThumbLeft: return 0x5823;
        default: return 0;
        }
    }

    uint32_t CurrentKeys()
    {
        const os::touch_pad::State pad = os::touch_pad::Get();
        uint32_t keys = uint32_t(pad.buttons | AutoPressButtons());
        constexpr int kThreshold = 16384;
        if (pad.leftTrigger > 64)
            keys |= kKeyLeftTrigger;
        if (pad.rightTrigger > 64)
            keys |= kKeyRightTrigger;
        if (pad.thumbLY > kThreshold)
            keys |= kKeyThumbUp;
        if (pad.thumbLY < -kThreshold)
            keys |= kKeyThumbDown;
        if (pad.thumbLX > kThreshold)
            keys |= kKeyThumbRight;
        if (pad.thumbLX < -kThreshold)
            keys |= kKeyThumbLeft;
        return keys;
    }
}

uint32_t XamInputGetKeystrokeEx(be<uint32_t>* userIndex, uint32_t flags, void* keystroke)
{
    // XINPUT_KEYSTROKE: virtual key, unicode, flags (16-bit each), user index, HID code.
    struct Keystroke
    {
        be<uint16_t> virtualKey;
        be<uint16_t> unicode;
        be<uint16_t> flags;
        uint8_t userIndex;
        uint8_t hidCode;
    };
    static_assert(sizeof(Keystroke) == 8);

    using namespace std::chrono;
    static std::mutex s_mutex;
    static uint32_t s_lastKeys;
    static steady_clock::time_point s_nextRepeat;
    std::lock_guard lock(s_mutex);
    if (keystroke == nullptr)
        return 0x10D2; // ERROR_EMPTY
    // Only pad 0 exists. A title that polls each pad by index (Halo 3's lobby asks 0..3) must not
    // get pad 0's keys for pads 1-3: it took every A press as "another player joins" and filled the
    // lobby with four players on disconnected pads, which blocked "start game".
    const uint32_t requestedUser = userIndex ? uint32_t(*userIndex) : 0xFFu;
    LogInputQuery("XamInputGetKeystrokeEx", requestedUser, flags,
                  (requestedUser != 0 && requestedUser != 0xFF) ? ERROR_DEVICE_NOT_CONNECTED : 0);
    if (requestedUser != 0 && requestedUser != 0xFF)
        return ERROR_DEVICE_NOT_CONNECTED;
    const uint32_t keys = CurrentKeys();
    const uint32_t changed = keys ^ s_lastKeys;
    auto* out = static_cast<Keystroke*>(keystroke);
    out->unicode = 0;
    out->userIndex = 0;
    out->hidCode = 0;
    if (userIndex)
        *userIndex = 0;
    const auto now = steady_clock::now();
    if (changed != 0)
    {
        const uint32_t key = changed & (0u - changed); // lowest changed bit
        const bool down = keys & key;
        s_lastKeys ^= key;
        out->virtualKey = VirtualKey(key);
        out->flags = down ? 0x0001 : 0x0002; // XINPUT_KEYSTROKE_KEYDOWN / KEYUP
        if (down && (key & kRepeatingKeys))
            s_nextRepeat = now + milliseconds(400);
        return ERROR_SUCCESS;
    }
    // A held direction repeats every 120 ms after 400 ms, for scrolling lists.
    const uint32_t held = keys & kRepeatingKeys;
    if (held != 0 && now >= s_nextRepeat)
    {
        out->virtualKey = VirtualKey(held & (0u - held));
        out->flags = 0x0001 | 0x0004; // KEYDOWN | REPEAT
        s_nextRepeat = now + milliseconds(120);
        return ERROR_SUCCESS;
    }
    return 0x10D2; // ERROR_EMPTY
}

// XAM's own allocator (Xenia: SystemHeapAlloc, always zeroed). The generated stub
// returned without an address and the caller then jumped through garbage.
uint32_t XamAlloc(uint32_t flags, uint32_t size, be<uint32_t>* address)
{
    void* block = g_userHeap.Alloc(std::max<uint32_t>(size, 1));
    memset(block, 0, size);
    *address = g_memory.MapVirtual(block);
    return ERROR_SUCCESS;
}

uint32_t XamFree(uint32_t address)
{
    if (address != 0)
        g_userHeap.Free(g_memory.Translate(address));
    return ERROR_SUCCESS;
}

