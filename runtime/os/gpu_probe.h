#pragma once
// what the phone's own Vulkan driver is and whether the native renderer can run on it
// (Android). vk_driver.cpp uses it to choose between that driver and Turnip, and the launcher
// (launcher_ui.cpp) to show the GPU, its requirements and why the game cannot start.
#include <cstdint>
#include <string>

struct GpuProbe
{
    bool found = false;      // a Vulkan instance and a GPU were found
    std::string device;      // VkPhysicalDeviceProperties::deviceName, e.g. "Adreno (TM) 610"
    std::string driver;      // driverName and driverInfo (VK_KHR_driver_properties), if exposed
    uint32_t vendor = 0;     // PCI vendor: 0x5143 Qualcomm, 0x13B5 Arm (Mali)
    uint32_t api = 0;        // the device's Vulkan version
    std::string missing;     // what the renderer needs and the driver lacks; empty when it can run
};

// Asks the system libvulkan.so once (later calls return the first answer) and logs a line:
// "gpu: <device> (<vendor>), Vulkan x.y.z, <driver>: renderer OK | missing ...".
const GpuProbe& ProbeSystemGpu();

const char* GpuVendorName(uint32_t vendor);
