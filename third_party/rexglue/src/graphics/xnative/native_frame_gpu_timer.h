#pragma once
// xport: how long the GPU takes for each frame's command buffer (a timestamp at its start, one at
// its end), for the ADPF session (runtime/os/perf_hint.cpp, xport_perf_hint_report_work2):
// Android 15+ takes the GPU part of the work with the CPU part, so the system sees what the GPU
// does, not only the recorder's time. One pair of queries per frame slot. A slot's pair is read
// when the slot is recorded again (its command pool was reset, so its submission has completed)
// and handed back with that frame's recorder work. XPORT_ADPF_GPU=0 keeps the CPU-only reports.
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>

namespace rex::graphics::xnative {

class NativeFrameGpuTimer {
 public:
  static constexpr uint32_t kSlots = 2;

  struct Frame {
    uint64_t work_start_ns = 0;  // when the recorder started it (steady_clock, CLOCK_MONOTONIC)
    uint64_t cpu_ns = 0;         // the recorder's work, start to vkQueueSubmit
    uint64_t gpu_ns = 0;         // the command buffer on the GPU
  };

  // False (and inert) when the queue family writes no timestamps.
  bool Initialize(const ui::vulkan::VulkanDevice* device, uint32_t queue_family) {
    device_ = device;
    if (!device) return false;
    const auto& ifn = device->vulkan_instance()->functions();
    VkPhysicalDeviceProperties properties{};
    ifn.vkGetPhysicalDeviceProperties(device->physical_device(), &properties);
    if (properties.limits.timestampPeriod <= 0.0f) return false;
    uint32_t family_count = 0;
    ifn.vkGetPhysicalDeviceQueueFamilyProperties(device->physical_device(), &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    ifn.vkGetPhysicalDeviceQueueFamilyProperties(device->physical_device(), &family_count,
                                                 families.data());
    if (queue_family >= family_count || !families[queue_family].timestampValidBits) return false;
    const uint32_t bits = families[queue_family].timestampValidBits;
    mask_ = bits >= 64 ? ~uint64_t(0) : (uint64_t(1) << bits) - 1;
    period_ns_ = double(properties.limits.timestampPeriod);
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = 2 * kSlots;
    if (device->functions().vkCreateQueryPool(device->device(), &info, nullptr, &pool_) !=
        VK_SUCCESS) {
      pool_ = VK_NULL_HANDLE;
      return false;
    }
    ready_ = true;
    return true;
  }

  void Shutdown() {
    if (device_ && pool_) device_->functions().vkDestroyQueryPool(device_->device(), pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
    ready_ = false;
  }

  bool ready() const { return ready_; }
  double period_ns() const { return period_ns_; }

  // First thing in the slot's command buffer (no render pass is open). Returns true with the
  // slot's previous frame in `completed` when its timestamps are there.
  bool BeginFrame(VkCommandBuffer command_buffer, uint32_t slot, Frame& completed) {
    if (!ready_ || slot >= kSlots) return false;
    const auto& dfn = device_->functions();
    bool have = false;
    if (submitted_[slot]) {
      uint64_t stamps[2] = {};
      if (dfn.vkGetQueryPoolResults(device_->device(), pool_, 2 * slot, 2, sizeof(stamps), stamps,
                                    sizeof(stamps[0]), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        const uint64_t begin = stamps[0] & mask_, end = stamps[1] & mask_;
        uint64_t ticks = (end - begin) & mask_;
        // A frame whose command buffer started while the previous one still ran on the GPU
        // only counts from the previous one's end.
        if (have_last_end_) ticks = std::min(ticks, (end - last_end_) & mask_);
        last_end_ = end;
        have_last_end_ = true;
        completed = frames_[slot];
        completed.gpu_ns = uint64_t(double(ticks) * period_ns_);
        have = true;
      }
      submitted_[slot] = false;
    }
    dfn.vkCmdResetQueryPool(command_buffer, pool_, 2 * slot, 2);
    dfn.vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool_, 2 * slot);
    recording_[slot] = true;
    return have;
  }

  // Last thing before vkEndCommandBuffer (no render pass is open).
  void EndFrame(VkCommandBuffer command_buffer, uint32_t slot) {
    if (!ready_ || slot >= kSlots || !recording_[slot]) return;
    device_->functions().vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                             pool_, 2 * slot + 1);
  }

  // After the slot's vkQueueSubmit succeeded: its queries will be written.
  void Submitted(uint32_t slot, uint64_t work_start_ns, uint64_t submit_ns) {
    if (!ready_ || slot >= kSlots || !recording_[slot]) return;
    recording_[slot] = false;
    submitted_[slot] = true;
    frames_[slot].work_start_ns = work_start_ns;
    frames_[slot].cpu_ns = submit_ns > work_start_ns ? submit_ns - work_start_ns : 0;
    frames_[slot].gpu_ns = 0;
  }

  // A recording that was not submitted (its pool is reset): its queries are never written.
  void Discard(uint32_t slot) {
    if (slot < kSlots) recording_[slot] = false;
  }

 private:
  const ui::vulkan::VulkanDevice* device_ = nullptr;
  VkQueryPool pool_ = VK_NULL_HANDLE;
  double period_ns_ = 1.0;
  uint64_t mask_ = ~uint64_t(0);
  std::array<bool, kSlots> recording_{};
  std::array<bool, kSlots> submitted_{};
  std::array<Frame, kSlots> frames_{};
  uint64_t last_end_ = 0;
  bool have_last_end_ = false;
  bool ready_ = false;
};

}  // namespace rex::graphics::xnative
