// asks the phone's own Vulkan driver what it is (see gpu_probe.h). The requirements are
// the ones ReXGlue's VulkanDevice refuses a device without when it runs the native shaders
// (third_party/rexglue/src/ui/vulkan/vulkan_device.cpp): Vulkan 1.2, the base features,
// descriptor indexing, buffer device address and dynamic rendering. The Adreno 610's driver
// (Redmi Note 11, 2026-10-29) is Vulkan 1.1.128 and lacks most of them.
#include <stdafx.h>
#include "gpu_probe.h"

const char* GpuVendorName(uint32_t vendor)
{
    switch (vendor)
    {
    case 0x5143: return "Qualcomm";
    case 0x13B5: return "Arm";
    case 0x1010: return "Imagination";
    case 0x144D: return "Samsung";
    default: return "unknown vendor";
    }
}

#ifdef __ANDROID__
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <os/logger.h>
#include <dlfcn.h>
#include <vector>

namespace
{
    std::string VersionText(uint32_t version)
    {
        return fmt::format("{}.{}.{}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
            VK_API_VERSION_PATCH(version));
    }

    void Missing(GpuProbe& probe, const std::string& what)
    {
        probe.missing += probe.missing.empty() ? what : ", " + what;
    }

    void ProbeDevice(GpuProbe& probe, PFN_vkGetInstanceProcAddr getProc, VkInstance instance,
        uint32_t instanceVersion)
    {
#define XPORT_VK(name) auto name = reinterpret_cast<PFN_##name>(getProc(instance, #name))
        XPORT_VK(vkEnumeratePhysicalDevices);
        XPORT_VK(vkGetPhysicalDeviceProperties);
        XPORT_VK(vkGetPhysicalDeviceFeatures);
        XPORT_VK(vkGetPhysicalDeviceProperties2);
        XPORT_VK(vkGetPhysicalDeviceFeatures2);
        XPORT_VK(vkEnumerateDeviceExtensionProperties);
#undef XPORT_VK
        uint32_t count = 1;
        VkPhysicalDevice device = VK_NULL_HANDLE;
        if (!vkEnumeratePhysicalDevices || !vkGetPhysicalDeviceProperties || !vkGetPhysicalDeviceFeatures ||
            vkEnumeratePhysicalDevices(instance, &count, &device) < 0 || count == 0 || !device)
        {
            Missing(probe, "a Vulkan GPU");
            return;
        }
        probe.found = true;

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        probe.device = properties.deviceName;
        probe.vendor = properties.vendorID;
        probe.api = properties.apiVersion;

        std::vector<VkExtensionProperties> extensions;
        if (vkEnumerateDeviceExtensionProperties)
        {
            uint32_t extensionCount = 0;
            vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
            extensions.resize(extensionCount);
            vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());
            extensions.resize(extensionCount);
        }
        auto has = [&](const char* name)
        {
            for (const VkExtensionProperties& extension : extensions)
            {
                if (strcmp(extension.extensionName, name) == 0)
                    return true;
            }
            return false;
        };

        // Properties2 / Features2 are core from 1.1 on both the instance and the device.
        const bool chains = instanceVersion >= VK_API_VERSION_1_1 && probe.api >= VK_API_VERSION_1_1 &&
            vkGetPhysicalDeviceProperties2 && vkGetPhysicalDeviceFeatures2;
        if (chains && (probe.api >= VK_API_VERSION_1_2 || has(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME)))
        {
            VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
            vkGetPhysicalDeviceProperties2(device, &properties2);
            probe.driver = driver.driverName;
            if (driver.driverInfo[0])
                probe.driver += std::string(" (") + driver.driverInfo + ")";
        }
        if (probe.driver.empty())
            probe.driver = fmt::format("driver version 0x{:08X}", properties.driverVersion);

        if (probe.api < VK_API_VERSION_1_2)
            Missing(probe, "Vulkan 1.2 (driver has " + VersionText(probe.api) + ")");

        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(device, &features);
        // shaderInt64 is not required: without it the shaders get 32-bit pairs instead
        // (rex/ui/vulkan/spirv_int64_lowering.h).
        if (!features.depthClamp) Missing(probe, "depthClamp");
        if (!features.sampleRateShading) Missing(probe, "sampleRateShading");
        if (!features.independentBlend) Missing(probe, "independentBlend");
        // fragmentStoresAndAtomics / vertexPipelineStoresAndAtomics are required only by the GPU
        // emulation path (with_gpu_emulation); the native renderer neither checks nor enables them.

        if (chains && probe.api >= VK_API_VERSION_1_2)
        {
            VkPhysicalDeviceDynamicRenderingFeatures dynamicExtension{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
            VkPhysicalDeviceVulkan13Features vulkan13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceVulkan12Features vulkan12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            const bool core13 = probe.api >= VK_API_VERSION_1_3;
            const bool dynamicExt = !core13 && has(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
            if (core13)
                vulkan12.pNext = &vulkan13;
            else if (dynamicExt)
                vulkan12.pNext = &dynamicExtension;
            VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &vulkan12};
            vkGetPhysicalDeviceFeatures2(device, &features2);
            if (!vulkan12.runtimeDescriptorArray || !vulkan12.descriptorBindingPartiallyBound)
                Missing(probe, "descriptor indexing");
            if (!vulkan12.bufferDeviceAddress)
                Missing(probe, "bufferDeviceAddress");
            if (!(core13 ? vulkan13.dynamicRendering : dynamicExt && dynamicExtension.dynamicRendering))
                Missing(probe, "dynamic rendering");
        }
        else
        {
            // Below 1.2 the renderer refuses the device anyway; name what the extensions lack too.
            if (!has(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME))
                Missing(probe, "descriptor indexing");
            if (!has(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME))
                Missing(probe, "bufferDeviceAddress");
            if (!has(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME))
                Missing(probe, "dynamic rendering");
        }
    }

    GpuProbe Probe()
    {
        GpuProbe probe;
        void* loader = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        auto getProc = loader ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"))
                              : nullptr;
        if (!getProc)
        {
            Missing(probe, "Vulkan (no libvulkan.so)");
            if (loader)
                dlclose(loader);
            return probe;
        }
        auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            getProc(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
        uint32_t instanceVersion = VK_API_VERSION_1_0;
        if (enumerateVersion)
            enumerateVersion(&instanceVersion);
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "xport gpu probe";
        application.apiVersion = instanceVersion >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : instanceVersion;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &application;
        auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(getProc(VK_NULL_HANDLE, "vkCreateInstance"));
        VkInstance instance = VK_NULL_HANDLE;
        if (createInstance && createInstance(&info, nullptr, &instance) == VK_SUCCESS)
        {
            ProbeDevice(probe, getProc, instance, instanceVersion);
            if (auto destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(getProc(instance, "vkDestroyInstance")))
                destroyInstance(instance, nullptr);
        }
        else
        {
            Missing(probe, "a Vulkan instance");
        }
        dlclose(loader);
        return probe;
    }
}

const GpuProbe& ProbeSystemGpu()
{
    static const GpuProbe s_probe = []
    {
        GpuProbe probe = Probe();
        LOGFN("gpu: {} ({}), Vulkan {}, {}: {}", probe.found ? probe.device : "no GPU",
            GpuVendorName(probe.vendor), VersionText(probe.api), probe.driver,
            probe.missing.empty() ? "renderer OK" : "missing " + probe.missing);
        return probe;
    }();
    return s_probe;
}
#endif
