// loads default.xex into guest memory and sets up what the kernel loader would:
// the import address table, the kernel's exported variables, the executable module and its
// XEX header (RtlImageXexHeaderField, XexGetModuleHandle). Variable layouts follow Xenia's
// xboxkrnl_module.cc; UnleashedRecomp left these as stubs.
#include <stdafx.h>
#include <file.h>
#include <image.h>
#include <xex.h>
#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <os/paths.h>
#include <os/thread_dump.h>
#include "loader.h"
#include <unordered_set>

extern std::unordered_map<size_t, const char*> XamExports;
extern std::unordered_map<size_t, const char*> XboxKernelExports;

static uint32_t g_xexHeader;       // guest copy of the XEX header
static uint32_t g_executableModule; // fake LDR_DATA_TABLE_ENTRY
static uint8_t* g_keTimeStampBundle;
static std::unique_ptr<std::thread> g_timeStampThread;

static uint32_t AllocGuest(size_t size)
{
    void* ptr = g_userHeap.AllocPhysical(size, 16);
    memset(ptr, 0, size);
    return g_memory.MapVirtual(ptr);
}

template<typename T>
static void StoreBE(uint32_t address, T value)
{
    *(T*)g_memory.Translate(address) = ByteSwap(value);
}

static uint64_t GuestUptimeMillis()
{
    static const auto s_start = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s_start).count();
}

static uint32_t NewKernelVariable(const std::string_view& name);

// Address of the guest storage behind a kernel variable export (by xboxkrnl ordinal): one per
// name, shared by the executable and the DLLs it loads (Halo 3's Waves DLLs import
// KeTimeStampBundle too, and a second copy would never tick).
static uint32_t KernelVariable(const std::string_view& name)
{
    static std::mutex s_mutex;
    static std::unordered_map<std::string, uint32_t> s_variables;
    std::lock_guard lock(s_mutex);
    auto it = s_variables.find(std::string(name));
    if (it != s_variables.end())
        return it->second;
    const uint32_t address = NewKernelVariable(name);
    s_variables.emplace(std::string(name), address);
    return address;
}

static uint32_t NewKernelVariable(const std::string_view& name)
{
    if (name == "XboxKrnlVersion")
    {
        uint32_t p = AllocGuest(8);
        StoreBE<uint16_t>(p + 0, 2);
        StoreBE<uint16_t>(p + 2, 0xFFFF);
        StoreBE<uint16_t>(p + 4, 0xFFFF);
        *(uint8_t*)g_memory.Translate(p + 6) = 0x80;
        return p;
    }
    if (name == "KeTimeStampBundle")
    {
        // InterruptTime, SystemTime, TickCount (ms), padding; TickCount is refreshed every 1 ms.
        uint32_t p = AllocGuest(24);
        g_keTimeStampBundle = (uint8_t*)g_memory.Translate(p);
        StoreBE<uint32_t>(p + 16, uint32_t(GuestUptimeMillis()));
        return p;
    }
    if (name == "ExLoadedCommandLine")
    {
        uint32_t p = AllocGuest(1024);
        strcpy((char*)g_memory.Translate(p), "\"default.xex\"");
        return p;
    }
    if (name == "XexExecutableModuleHandle")
    {
        uint32_t p = AllocGuest(4);
        StoreBE<uint32_t>(p, g_executableModule);
        return p;
    }
    if (name == "KeDebugMonitorData" || name == "KeCertMonitorData")
        return AllocGuest(4); // pointer to zero: no debugger
    if (name == "VdGpuClockInMHz")
    {
        uint32_t p = AllocGuest(4);
        StoreBE<uint32_t>(p, 500);
        return p;
    }
    if (name == "VdGlobalDevice")
        return AllocGuest(4); // D3D stores its device pointer here
    if (name == "VdHSIOCalibrationLock")
        return AllocGuest(28); // RTL_CRITICAL_SECTION, unlocked
    if (name == "ExThreadObjectType")
        return AllocGuest(64); // only compared/passed through by ObReferenceObjectByHandle

    LOGF_WARNING("no storage for kernel variable {}", name);
    return AllocGuest(64);
}

static void StartTimeStampThread()
{
    g_timeStampThread = std::make_unique<std::thread>([]
    {
        auto next = std::chrono::steady_clock::now();
        while (true)
        {
            *(be<uint32_t>*)(g_keTimeStampBundle + 16) = uint32_t(GuestUptimeMillis());
            next += std::chrono::milliseconds(1);
            std::this_thread::sleep_until(next);
        }
    });
}

static void SetupModule(const uint8_t* xex)
{
    auto* header = reinterpret_cast<const Xex2Header*>(xex);
    g_xexHeader = AllocGuest(header->headerSize);
    memcpy(g_memory.Translate(g_xexHeader), xex, header->headerSize);

    // Xenia's layout: the xex header pointer sits at +0x58 of the module structure.
    g_executableModule = AllocGuest(0x100);
    StoreBE<uint32_t>(g_executableModule + 0x58, g_xexHeader);
}

static void BindImports(const uint8_t* xex, const Image& image)
{
    auto* imports = reinterpret_cast<const Xex2ImportHeader*>(getOptHeaderPtr(xex, XEX_HEADER_IMPORT_LIBRARIES));
    if (!imports)
        return;

    std::vector<std::string> names;
    const char* strings = reinterpret_cast<const char*>(imports + 1);
    for (size_t i = 0, offset = 0; i < imports->numImports; i++)
    {
        names.emplace_back(strings + offset);
        offset += ((names.back().size() + 1) + 3) & ~3;
    }

    auto* library = reinterpret_cast<const Xex2ImportLibrary*>(reinterpret_cast<const char*>(imports) + sizeof(Xex2ImportHeader) + imports->sizeOfStringTable);
    size_t variables = 0, functions = 0;
    for (size_t i = 0; i < names.size(); i++)
    {
        const std::string& libraryName = names[library->name];
        const auto& table = libraryName == "xam.xex" ? XamExports : XboxKernelExports;
        auto* descriptors = reinterpret_cast<const Xex2ImportDescriptor*>(library + 1);

        for (size_t k = 0; k < library->numberOfImports; k++)
        {
            // Xex2LoadImage left variable records byteswapped in the image and replaced function
            // thunks with a stub; a record followed by a thunk is a function import.
            uint32_t address = descriptors[k].firstThunk;
            uint32_t record;
            memcpy(&record, image.Find(address), 4);
            if (record == 0x00000060)
                continue;

            uint32_t ordinal = record & 0xFFFF;
            auto it = table.find(ordinal);
            std::string_view name = it != table.end() ? std::string_view(it->second).substr(strlen("__imp__")) : "?";

            uint32_t next = 0;
            if (k + 1 < library->numberOfImports)
                memcpy(&next, image.Find(descriptors[k + 1].firstThunk), 4);

            if (next == 0x00000060)
            {
                // The IAT slot gets the thunk, whose address maps to the __imp__ function.
                StoreBE<uint32_t>(address, descriptors[k + 1].firstThunk);
                functions++;
            }
            else
            {
                StoreBE<uint32_t>(address, KernelVariable(name));
                variables++;
            }
        }
        library = reinterpret_cast<const Xex2ImportLibrary*>(reinterpret_cast<const char*>(library + 1) + library->numberOfImports * sizeof(Xex2ImportDescriptor));
    }
    LOGF("bound {} function and {} variable imports", functions, variables);
}

uint32_t LdrLoadModule(const std::filesystem::path& path)
{
    const auto file = LoadFile(path);
    if (file.empty())
    {
        LOGF_ERROR("cannot read {}", path.string());
        return 0;
    }

    // XenonUtils decrypts (retail AES) and decompresses (basic/LZX) the image.
    Image image = Image::ParseImage(file.data(), file.size());
    if (!image.data)
    {
        LOGF_ERROR("cannot load image from {}", path.string());
        return 0;
    }

    // The whole image, as the kernel maps it: the XEX resources (XexGetModuleSection) lie past
    // the last PE section. Halo 3's "mix_l360" (0x83270500, the Waves preset WavesLibDLL loads)
    // was zeros when only the sections were copied, and the game stopped with "dirty disc".
    // XenonUtils' buffer holds at least image.size bytes, zero past the data.
    memcpy(g_memory.Translate(image.base), image.data.get(), image.size);

    SetupModule(file.data());
    BindImports(file.data(), image);
    StartTimeStampThread();

    LOGF("loaded {} at 0x{:08X}, entry 0x{:08X}", path.filename().string(), image.base, image.entry_point);
    return uint32_t(image.entry_point);
}

// Optional header lookup on a guest XEX header (Xenia's UserModule::GetOptHeader):
// key low byte 0x00 returns the value, 0x01 a pointer to the value, otherwise a pointer to the data.
static uint32_t GetOptHeader(uint32_t xexHeader, uint32_t key)
{
    auto* header = reinterpret_cast<const Xex2Header*>(g_memory.Translate(xexHeader));
    auto* opts = reinterpret_cast<const Xex2OptHeader*>(header + 1);
    for (uint32_t i = 0; i < header->headerCount; i++)
    {
        if (opts[i].key != key)
            continue;
        switch (key & 0xFF)
        {
        case 0x00: return opts[i].value;
        case 0x01: return g_memory.MapVirtual((void*)&opts[i].value);
        default: return xexHeader + opts[i].offset;
        }
    }
    return 0;
}

uint32_t RtlImageXexHeaderField(uint32_t xexHeader, uint32_t key)
{
    return GetOptHeader(xexHeader ? xexHeader : g_xexHeader, key);
}

// xex2_opt_tls_info: slot count, raw data address, data size, raw data size.
bool LdrGetTlsTemplate(uint32_t& rawAddress, uint32_t& rawSize, uint32_t& dataSize)
{
    if (!g_xexHeader)
        return false;
    const uint32_t info = GetOptHeader(g_xexHeader, 0x00020104); // XEX_HEADER_TLS_INFO
    if (!info)
        return false;
    auto word = [&](uint32_t offset) { return ByteSwap(*reinterpret_cast<const uint32_t*>(g_memory.Translate(info + offset))); };
    rawAddress = word(4);
    dataSize = word(8);
    rawSize = word(12);
    return true;
}

// Title DLLs (Halo 3: Waves' audio DLLs, loaded with XexLoadImage). Their code is recompiled into
// the port (tools/slot_dlls.py; the game registers their functions at startup), so loading one is
// the kernel loader's data work: map the image at its address, bind its imports, run its entry
// point. A DLL whose entry point has no recompiled function stays "not there".
namespace
{
    struct TitleModule
    {
        std::string name;          // file name, lower case ("wavelibdll.dll")
        std::filesystem::path path;
        uint32_t handle = 0;       // fake LDR_DATA_TABLE_ENTRY
        uint32_t base = 0;
        uint32_t size = 0;
        uint32_t entry = 0;
        uint32_t exportTable = 0;  // the XEX export table (security info +0x160), an image address
        uint32_t references = 0;
    };

    std::recursive_mutex g_moduleMutex; // a DLL's entry point may load another DLL
    std::list<TitleModule> g_titleModules;

    std::string LowerName(std::string_view s)
    {
        std::string out(s);
        for (char& c : out)
            c = char(tolower((unsigned char)c));
        return out;
    }

    // "game:\waves\L360.dll" -> "l360.dll"
    std::string ModuleFileName(std::string_view path)
    {
        const size_t cut = path.find_last_of("\\/:");
        return LowerName(cut == std::string_view::npos ? path : path.substr(cut + 1));
    }

    TitleModule* FindTitleModule(uint32_t handle)
    {
        for (TitleModule& module : g_titleModules)
            if (module.handle == handle)
                return &module;
        return nullptr;
    }

    TitleModule* FindTitleModule(std::string_view name)
    {
        const std::string wanted = ModuleFileName(name);
        for (TitleModule& module : g_titleModules)
            if (module.name == wanted)
                return &module;
        return nullptr;
    }

    // A guest path ("game:\waves\L360.dll", "d:\...", or a bare name) to the file in the game
    // folder, matching each component without regard to case.
    std::filesystem::path FindGameFile(std::string_view guestPath)
    {
        std::string rest(guestPath);
        if (const size_t colon = rest.find(':'); colon != std::string::npos)
            rest = rest.substr(colon + 1);
        std::filesystem::path path = os::paths::GetGamePath();
        size_t start = 0;
        while (start <= rest.size())
        {
            size_t end = rest.find_first_of("\\/", start);
            if (end == std::string::npos)
                end = rest.size();
            const std::string component = rest.substr(start, end - start);
            start = end + 1;
            if (component.empty() || component == ".")
                continue;
            std::filesystem::path next = path / component;
            std::error_code ec;
            if (!std::filesystem::exists(next, ec))
            {
                const std::string lower = LowerName(component);
                for (const auto& entry : std::filesystem::directory_iterator(path, ec))
                {
                    if (LowerName(entry.path().filename().string()) == lower)
                    {
                        next = entry.path();
                        break;
                    }
                }
            }
            path = next;
        }
        return path;
    }

    uint32_t LoadGuestU32(uint32_t address)
    {
        return ByteSwap(*(const uint32_t*)g_memory.Translate(address));
    }

    // ordinal -> address through the XEX export table (Xenia's xex2_export_table: three magic words,
    // module numbers, versions, then the image base >> 16, count, first ordinal, and an offset per
    // ordinal from the image base).
    uint32_t ExportByOrdinal(const TitleModule& module, uint32_t ordinal)
    {
        if (!module.exportTable)
            return 0;
        const uint32_t imageBase = LoadGuestU32(module.exportTable + 32) << 16;
        const uint32_t count = LoadGuestU32(module.exportTable + 36);
        const uint32_t first = LoadGuestU32(module.exportTable + 40);
        if (ordinal < first || ordinal - first >= count)
            return 0;
        const uint32_t offset = LoadGuestU32(module.exportTable + 44 + 4 * (ordinal - first));
        return offset ? imageBase + offset : 0;
    }

    uint32_t LoadTitleModule(const std::string& guestPath, uint32_t& status);

    // The DLL's imports: kernel/XAM functions as in the executable (each IAT slot gets its thunk,
    // whose address the recompiled code maps to the __imp__ function); variables of another title
    // DLL get that DLL's export, loading it first.
    bool BindModuleImports(const uint8_t* xex, const Image& image, const TitleModule& module)
    {
        auto* imports = reinterpret_cast<const Xex2ImportHeader*>(getOptHeaderPtr(xex, XEX_HEADER_IMPORT_LIBRARIES));
        if (!imports)
            return true;

        std::vector<std::string> names;
        const char* strings = reinterpret_cast<const char*>(imports + 1);
        for (size_t i = 0, offset = 0; i < imports->numImports; i++)
        {
            names.emplace_back(strings + offset);
            offset += ((names.back().size() + 1) + 3) & ~3;
        }

        auto* library = reinterpret_cast<const Xex2ImportLibrary*>(reinterpret_cast<const char*>(imports) + sizeof(Xex2ImportHeader) + imports->sizeOfStringTable);
        for (size_t i = 0; i < names.size(); i++)
        {
            const std::string& libraryName = names[library->name];
            const bool system = libraryName == "xam.xex" || libraryName == "xboxkrnl.exe";
            const auto& table = libraryName == "xam.xex" ? XamExports : XboxKernelExports;
            const TitleModule* provider = nullptr;
            if (!system)
            {
                provider = FindTitleModule(libraryName);
                if (!provider)
                {
                    // Next to the importing DLL first, then at the root of the game folder.
                    uint32_t dependencyStatus = 0;
                    const std::string sibling = "game:\\" + std::filesystem::relative(module.path.parent_path(),
                        os::paths::GetGamePath()).string() + "\\" + libraryName;
                    const std::filesystem::path siblingPath = FindGameFile(sibling);
                    std::error_code ec;
                    if (std::filesystem::exists(siblingPath, ec))
                        LoadTitleModule(sibling, dependencyStatus);
                    else
                        LoadTitleModule("game:\\" + libraryName, dependencyStatus);
                    provider = FindTitleModule(libraryName);
                }
                if (!provider)
                {
                    LOGF_ERROR("{}: needs {}, which did not load", module.name, libraryName);
                    return false;
                }
            }
            auto* descriptors = reinterpret_cast<const Xex2ImportDescriptor*>(library + 1);
            for (size_t k = 0; k < library->numberOfImports; k++)
            {
                const uint32_t address = descriptors[k].firstThunk;
                uint32_t record;
                memcpy(&record, image.Find(address), 4);
                if (record == 0x00000060)
                    continue;
                const uint32_t ordinal = record & 0xFFFF;
                uint32_t next = 0;
                if (k + 1 < library->numberOfImports)
                    memcpy(&next, image.Find(descriptors[k + 1].firstThunk), 4);
                if (next == 0x00000060)
                {
                    StoreBE<uint32_t>(address, descriptors[k + 1].firstThunk);
                }
                else if (system)
                {
                    auto it = table.find(ordinal);
                    StoreBE<uint32_t>(address, KernelVariable(it != table.end() ? std::string_view(it->second).substr(strlen("__imp__")) : "?"));
                }
                else
                {
                    const uint32_t exported = ExportByOrdinal(*provider, ordinal);
                    if (!exported)
                        LOGF_WARNING("{}: {} exports no ordinal {}", module.name, libraryName, ordinal);
                    StoreBE<uint32_t>(address, exported);
                }
            }
            library = reinterpret_cast<const Xex2ImportLibrary*>(reinterpret_cast<const char*>(library + 1) + library->numberOfImports * sizeof(Xex2ImportDescriptor));
        }
        return true;
    }

    uint32_t LoadTitleModule(const std::string& guestPath, uint32_t& status)
    {
        if (TitleModule* loaded = FindTitleModule(guestPath))
        {
            loaded->references++;
            status = 0;
            return loaded->handle;
        }
        status = 0xC000000F; // X_STATUS_NO_SUCH_FILE
        const std::filesystem::path path = FindGameFile(guestPath);
        const auto file = LoadFile(path);
        if (file.empty())
        {
            LOGF_WARNING("{}: no such file ({})", guestPath, path.string());
            return 0;
        }
        Image image = Image::ParseImage(file.data(), file.size());
        if (!image.data)
        {
            LOGF_ERROR("{}: cannot read the XEX", guestPath);
            status = 0xC000007B; // X_STATUS_INVALID_IMAGE_FORMAT
            return 0;
        }
        if (!image.entry_point || !g_memory.FindFunction(uint32_t(image.entry_point)))
        {
            LOGF_WARNING("{} (0x{:08X}, entry 0x{:08X}): not recompiled into this port", guestPath, uint32_t(image.base), uint32_t(image.entry_point));
            return 0;
        }

        memcpy(g_memory.Translate(image.base), image.data.get(), image.size); // resources too

        auto* header = reinterpret_cast<const Xex2Header*>(file.data());
        const uint32_t xexHeader = AllocGuest(header->headerSize);
        memcpy(g_memory.Translate(xexHeader), file.data(), header->headerSize);

        TitleModule& module = g_titleModules.emplace_back();
        module.name = ModuleFileName(guestPath);
        module.path = path;
        module.base = uint32_t(image.base);
        module.size = uint32_t(image.size);
        module.entry = uint32_t(image.entry_point);
        module.references = 1;
        const uint32_t security = header->securityOffset;
        module.exportTable = ByteSwap(*reinterpret_cast<const uint32_t*>(file.data() + security + 0x160));
        // X_LDR_DATA_TABLE_ENTRY (Xenia): dll_base +0x18, image_base +0x1C, image_size +0x20,
        // entry_point +0x3C, xex_header_base +0x58.
        module.handle = AllocGuest(0x100);
        StoreBE<uint32_t>(module.handle + 0x18, module.base);
        StoreBE<uint32_t>(module.handle + 0x1C, module.base);
        StoreBE<uint32_t>(module.handle + 0x20, module.size);
        StoreBE<uint32_t>(module.handle + 0x3C, module.entry);
        StoreBE<uint32_t>(module.handle + 0x58, xexHeader);

        if (!BindModuleImports(file.data(), image, module))
        {
            g_titleModules.pop_back();
            status = 0xC0000135; // X_STATUS_DLL_NOT_FOUND
            return 0;
        }
        LOGF("{} at 0x{:08X} (0x{:X} bytes), entry 0x{:08X}", module.name, module.base, module.size, module.entry);

        // DllMain(handle, DLL_PROCESS_ATTACH, 0) through the DLL's CRT entry point, on a copy of
        // the caller's context (its stack below the caller's frame) that the thread dump sees too.
        const uint32_t handle = module.handle;
        const uint32_t entry = module.entry;
        const std::string name = module.name;
        LOGF("{}: running its entry point", name);
        PPCContext& caller = *GetPPCContext();
        PPCContext ctx = caller;
        ctx.r3.u64 = handle;
        ctx.r4.u64 = 1; // DLL_PROCESS_ATTACH
        ctx.r5.u64 = 0;
        RegisterGuestContext(&ctx);
        SetPPCContext(ctx);
        g_memory.FindFunction(entry)(ctx, g_memory.base);
        caller.fpscr = ctx.fpscr;
        SetPPCContext(caller);
        UnregisterGuestContext(&ctx);
        const uint32_t ok = ctx.r3.u32;
        LOGF("{}: entry point returned {}", name, ok);
        if (!ok)
        {
            LOGF_ERROR("{}: its entry point refused to load (DllMain returned 0)", ModuleFileName(guestPath));
            status = 0xC0000142; // X_STATUS_DLL_INIT_FAILED
            return 0;
        }
        status = 0;
        return handle;
    }
}

uint32_t XexGetModuleHandle(const char* moduleName, be<uint32_t>* moduleHandle)
{
    // The executable and the title DLLs loaded; xam.xex/xboxkrnl.exe exports are reached through imports.
    if (moduleName && strcmp(moduleName, "default.xex") != 0)
    {
        {
            std::lock_guard lock(g_moduleMutex);
            if (TitleModule* module = FindTitleModule(std::string_view(moduleName)))
            {
                *moduleHandle = module->handle;
                return 0;
            }
        }
        LOGF_WARNING("module {} not found", moduleName);
        *moduleHandle = 0;
        return 0x00000490; // X_ERROR_NOT_FOUND
    }
    *moduleHandle = g_executableModule;
    return 0;
}

uint32_t XexGetProcedureAddress(uint32_t moduleHandle, uint32_t ordinal, be<uint32_t>* functionPtr)
{
    {
        std::lock_guard lock(g_moduleMutex);
        if (TitleModule* module = FindTitleModule(moduleHandle); module && !(ordinal & 0xFFFF0000))
        {
            if (const uint32_t address = ExportByOrdinal(*module, ordinal))
            {
                static int s_logged;
                if (s_logged++ < 64)
                    LOGF("{} ordinal {}: 0x{:08X}", module->name, ordinal, address);
                *functionPtr = address;
                return 0;
            }
        }
    }
    if (ordinal & 0xFFFF0000)
        LOGF_WARNING("by name {} (module 0x{:X}): not found", (const char*)g_memory.Translate(ordinal), moduleHandle);
    else
        LOGF_WARNING("ordinal {} (module 0x{:X}): not found", ordinal, moduleHandle);
    *functionPtr = 0;
    return 0xC0000139; // X_STATUS_ENTRYPOINT_NOT_FOUND
}

// Titles load modules by name at run time. A title DLL recompiled into the port (Halo 3's Waves
// DLLs) loads; anything else is not there (Xenia's answer for a module it lacks): Gears of War 3's
// XPARTY library loads xam.xex for the XamParty* exports, and its wrapper (82CE5CB0) then returns a
// null handle instead of the one the generated stub never wrote.
uint32_t XexLoadImage(const char* moduleName, uint32_t flags, uint32_t minVersion, be<uint32_t>* moduleHandle)
{
    uint32_t status = 0xC000000F; // X_STATUS_NO_SUCH_FILE
    uint32_t handle = 0;
    if (moduleName)
    {
        std::lock_guard lock(g_moduleMutex);
        handle = LoadTitleModule(moduleName, status);
    }
    if (!handle)
        LOGF_WARNING("{} (flags 0x{:X}, version 0x{:X}): not loaded", moduleName ? moduleName : "(null)", flags, minVersion);
    if (moduleHandle)
        *moduleHandle = handle;
    return status;
}

uint32_t XexUnloadImage(uint32_t moduleHandle)
{
    // Title DLLs stay mapped: their code is part of the port, and the game frees them at exit.
    std::lock_guard lock(g_moduleMutex);
    if (TitleModule* module = FindTitleModule(moduleHandle); module && module->references)
        module->references--;
    return 0;
}

#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
// A resource of the XEX (XEX_HEADER_RESOURCE_INFO: 8-byte name, address, size), already in the
// image. Gears of War 3 asks for "HashSec" (its files' SHA-1 list, 0x835A0000) at boot (8295B5E8)
// and parses it when the call succeeds; the stub in kernel/imports.cpp returned success with
// neither pointer written, so it parsed what was on the stack. Only for this title: the other
// games keep the stub their finished ports were built with.
uint32_t XexGetModuleSection(uint32_t moduleHandle, const char* name, be<uint32_t>* data, be<uint32_t>* size)
{
    // A title DLL's resources are in its own XEX header (its fake LDR entry +0x58).
    uint32_t xexHeader = g_xexHeader;
    if (moduleHandle && moduleHandle != g_executableModule)
    {
        std::lock_guard lock(g_moduleMutex);
        if (FindTitleModule(moduleHandle))
            xexHeader = ByteSwap(*reinterpret_cast<const uint32_t*>(g_memory.Translate(moduleHandle + 0x58)));
        else
            LOGF_WARNING("module 0x{:X} is not the executable's (0x{:X}): looking in it anyway", moduleHandle, g_executableModule);
    }
    auto* xex = reinterpret_cast<const uint8_t*>(g_memory.Translate(xexHeader));
    auto* info = reinterpret_cast<const uint8_t*>(getOptHeaderPtr(xex, XEX_HEADER_RESOURCE_INFO));
    if (info && name)
    {
        const uint32_t count = (ByteSwap(*reinterpret_cast<const uint32_t*>(info)) - 4) / 16;
        for (uint32_t i = 0; i < count; i++)
        {
            const uint8_t* entry = info + 4 + 16 * i;
            if (strncmp(reinterpret_cast<const char*>(entry), name, 8) != 0)
                continue;
            const uint32_t address = ByteSwap(*reinterpret_cast<const uint32_t*>(entry + 8));
            const uint32_t bytes = ByteSwap(*reinterpret_cast<const uint32_t*>(entry + 12));
            if (data)
                *data = address;
            if (size)
                *size = bytes;
            // Logged once per resource: the Waves DLLs ask for theirs many times a frame (Halo 3:
            // thousands of lines a minute).
            static std::mutex s_loggedMutex;
            static std::unordered_set<uint64_t> s_logged;
            const uint64_t key = uint64_t(xexHeader) << 32 | address;
            bool first;
            {
                std::lock_guard logLock(s_loggedMutex);
                first = s_logged.insert(key).second;
            }
            if (first)
                LOGF("{}: 0x{:08X}, {} bytes", name, address, bytes);
            return 0;
        }
    }
    LOGF_WARNING("{}: no such resource", name ? name : "(null)");
    return 0xC0000001; // X_STATUS_UNSUCCESSFUL, as Xenia's UserModule::GetSection
}

GUEST_FUNCTION_HOOK(__imp__XexGetModuleSection, XexGetModuleSection);

// The executable's XEX_HEADER_EXECUTION_INFO (media ID, version, base version, title ID, ...), in the
// guest copy of the header. Gears of War 3 checks a title ID against it before
// XamUserReadProfileSettings (82CE9108); the stub left the pointer unwritten and returned the
// pointer's address in r3 (not negative: success), so it read through stack garbage.
uint32_t XamGetExecutionId(be<uint32_t>* info)
{
    const uint32_t address = GetOptHeader(g_xexHeader, 0x00040006); // XEX_HEADER_EXECUTION_INFO
    if (info)
        *info = address;
    return address ? 0 : 0xC0000001; // X_STATUS_UNSUCCESSFUL
}

GUEST_FUNCTION_HOOK(__imp__XamGetExecutionId, XamGetExecutionId);
#endif

GUEST_FUNCTION_HOOK(__imp__RtlImageXexHeaderField, RtlImageXexHeaderField);
GUEST_FUNCTION_HOOK(__imp__XexLoadImage, XexLoadImage);
GUEST_FUNCTION_HOOK(__imp__XexUnloadImage, XexUnloadImage);
GUEST_FUNCTION_HOOK(__imp__XexGetModuleHandle, XexGetModuleHandle);
GUEST_FUNCTION_HOOK(__imp__XexGetProcedureAddress, XexGetProcedureAddress);
