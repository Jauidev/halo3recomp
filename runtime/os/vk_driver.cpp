// the Vulkan loader on Android. Since 0.7.0D there is one APK for every GPU: it carries
// Mesa's Turnip as libvulkan_turnip.so (Vulkan 1.3 on a6xx, the A610 included) and libadrenotools'
// hook libraries, and the launcher (launcher_ui.cpp) lets an Adreno phone pick its driver. The
// Redmi Note 11's own driver (Adreno 610, 2026-10-29) exposes Vulkan 1.1 without shaderInt64,
// descriptor indexing or dynamic rendering, so the native renderer refused the device and the
// screen stayed black. adrenotools opens a private copy of the system libvulkan.so whose driver
// load is redirected to Turnip (or an imported driver); ReXGlue's VulkanInstance takes that handle
// instead of loading libvulkan.so (ui/vulkan/vulkan_instance.cpp).
// The choice, logged as "vk-driver: ...":
// - no Qualcomm kernel driver (KGSL, /dev/kgsl-3d0; Mali has /dev/mali0): the phone's own driver,
//   since Turnip would find no GPU;
// - files/vk_driver (written by the launcher's GPU y drivers page): "system" (or 0) the phone's
//   driver, "turnip" (or 1) Turnip, "custom" followed by a line with the driver's folder and one
//   with its library an imported driver (files/drivers/<name>/ in the app's internal storage);
// - otherwise the phone's driver is asked what it is (gpu_probe.cpp): Turnip if it is not a
//   Qualcomm GPU the renderer can run on, the phone's driver if it is (a newer Adreno).
// Turnip exposes its full Vulkan version only on GPUs it passed conformance on; on the Adreno 610
// it said Vulkan 1.0 and the renderer refused it again, so TU_DEBUG gets "noconform" (Vulkan
// 1.3.335 then, 2026-10-29).
#include <stdafx.h>
#if defined(__ANDROID__)
#include <os/logger.h>
#include <adrenotools/driver.h>
#include <dlfcn.h>
#include <elf.h>
#include <unistd.h>
#include "gpu_probe.h"

namespace
{
    constexpr const char* TURNIP = "libvulkan_turnip.so";

    struct Forced
    {
        char driver = 0; // 's' the phone's, 't' Turnip, 'c' an imported one, 0 automatic
        std::string dir, library;
    };

    std::string Line(FILE* file)
    {
        char text[1024] = {};
        if (!fgets(text, sizeof(text), file))
            return {};
        std::string line = text;
        line.erase(line.find_last_not_of(" \r\n") + 1);
        return line;
    }

    Forced ForcedDriver()
    {
        const char* save = getenv("XPORT_SAVE_DIR");
        const std::string path = std::string(save ? save : ".") + "/../vk_driver";
        Forced forced;
        if (FILE* file = fopen(path.c_str(), "r"))
        {
            const std::string first = Line(file);
            if (first == "custom")
            {
                forced.dir = Line(file);
                forced.library = Line(file);
                if (!forced.dir.empty() && forced.dir.back() != '/')
                    forced.dir += '/';
                if (!forced.dir.empty() && !forced.library.empty())
                    forced.driver = 'c';
            }
            else if (!first.empty() && (first[0] == 's' || first[0] == '0'))
            {
                forced.driver = 's';
            }
            else if (!first.empty() && (first[0] == 't' || first[0] == '1'))
            {
                forced.driver = 't';
            }
            fclose(file);
        }
        return forced;
    }

    // Why an imported driver can't run, or nullptr if it looks fine. adrenotools doesn't say: given
    // a bad file it still returns a loader, and its hook logs "bad ELF magic ... falling back!" and
    // loads the phone's driver, which on the Redmi is the Vulkan 1.1 one that leaves the screen
    // black. Android's loader then asks the driver for HMI, its HAL entry point.
    const char* DriverProblem(const std::string& path)
    {
        FILE* file = fopen(path.c_str(), "rb");
        if (!file)
            return "it can't be read";
        fseek(file, 0, SEEK_END);
        const long fileSize = ftell(file);
        fseek(file, 0, SEEK_SET);
        std::string data(fileSize > 0 ? size_t(fileSize) : 0, '\0');
        const bool complete = fread(data.data(), 1, data.size(), file) == data.size();
        fclose(file);
        if (!complete)
            return "it can't be read";

        Elf64_Ehdr header;
        if (data.size() < sizeof(header))
            return "it is too short to be a library";
        memcpy(&header, data.data(), sizeof(header));
        if (memcmp(header.e_ident, ELFMAG, SELFMAG) != 0)
            return "it is not a library (no ELF magic)";
        constexpr uint16_t kMachineArm64 = 183; // EM_AARCH64, which the elf.h this build finds lacks
        if (header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_machine != kMachineArm64)
            return "it is not a 64-bit ARM library";
        if (header.e_type != ET_DYN)
            return "it is not a shared library";
        if (header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phoff > data.size() ||
            header.e_phnum > (data.size() - header.e_phoff) / sizeof(Elf64_Phdr))
            return "it is cut short";
        for (uint32_t index = 0; index < header.e_phnum; ++index)
        {
            Elf64_Phdr segment;
            memcpy(&segment, data.data() + header.e_phoff + index * sizeof(segment), sizeof(segment));
            if (segment.p_type == PT_LOAD &&
                (segment.p_offset > data.size() || segment.p_filesz > data.size() - segment.p_offset))
                return "it is cut short";
        }
        if (data.find(std::string("\0HMI\0", 5)) == std::string::npos)
            return "it is not a Vulkan driver (no HMI)";
        return nullptr;
    }

    void NoConformance()
    {
        const char* tuDebug = getenv("TU_DEBUG");
        if (!tuDebug || !*tuDebug)
            setenv("TU_DEBUG", "noconform", 1);
        else if (!strstr(tuDebug, "noconform"))
            setenv("TU_DEBUG", (std::string(tuDebug) + ",noconform").c_str(), 1);
    }
}

extern "C" void* xport_open_vulkan_loader()
{
    if (access("/dev/kgsl-3d0", F_OK) != 0)
    {
        LOGN("vk-driver: no Adreno GPU (/dev/kgsl-3d0), the phone's own Vulkan driver");
        return nullptr;
    }
    // The app's native library folder (getApplicationInfo().nativeLibraryDir), where the package
    // manager extracted libgame.so, Turnip and the hooks (extractNativeLibs="true").
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&xport_open_vulkan_loader), &info) || !info.dli_fname)
        return nullptr;
    std::string libraryDir = info.dli_fname;
    libraryDir.erase(libraryDir.rfind('/') + 1);
    const Forced forced = ForcedDriver();
    if (forced.driver == 's')
    {
        LOGN("vk-driver: files/vk_driver = system, the phone's own Vulkan driver");
        return nullptr;
    }
    if (forced.driver == 'c')
    {
        const std::string path = forced.dir + forced.library;
        if (access(path.c_str(), R_OK) != 0)
        {
            LOGFN_WARNING("vk-driver: imported driver {} is missing; automatic choice instead", path);
        }
        else if (const char* problem = DriverProblem(path))
        {
            LOGFN_WARNING("vk-driver: imported driver {} can't run, {}; automatic choice instead", path, problem);
        }
        else
        {
            NoConformance();
            void* loader = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr,
                libraryDir.c_str(), forced.dir.c_str(), forced.library.c_str(), nullptr, nullptr);
            if (loader)
            {
                LOGFN("vk-driver: imported driver {} through adrenotools", path);
                return loader;
            }
            LOGFN_WARNING("vk-driver: adrenotools could not open {}; automatic choice instead", path);
        }
    }
    if (access((libraryDir + TURNIP).c_str(), R_OK) != 0)
    {
        LOGFN_WARNING("vk-driver: no {} in the APK, the phone's own Vulkan driver", TURNIP);
        return nullptr;
    }
    if (forced.driver != 't')
    {
        const GpuProbe& gpu = ProbeSystemGpu();
        if (gpu.found && gpu.vendor == 0x5143 && gpu.missing.empty())
        {
            LOGFN("vk-driver: {}'s own driver runs the renderer, not Turnip", gpu.device);
            return nullptr;
        }
    }
    NoConformance();
    void* loader = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr,
        libraryDir.c_str(), libraryDir.c_str(), TURNIP, nullptr, nullptr);
    if (loader)
        LOGFN("vk-driver: Turnip ({}) through adrenotools{}", libraryDir + TURNIP,
            forced.driver == 't' ? " (files/vk_driver = turnip)" : "");
    else
        LOGFN_WARNING("vk-driver: adrenotools could not open libvulkan.so with {}; the phone's own driver instead", TURNIP);
    return loader;
}
#endif
