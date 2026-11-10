/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/deferred_command_buffer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/vulkan/vulkan_command_processor.h"

namespace xe {
namespace gpu {
namespace vulkan {

DeferredCommandBuffer::DeferredCommandBuffer(
    const VulkanCommandProcessor& command_processor, size_t initial_size)
    : command_processor_(command_processor) {
  command_stream_.reserve(initial_size / sizeof(uintmax_t));
}

namespace {
// state of XPORT_XENIA_PASS_TIME (see the header). Every timestamp
// closes an event: a render pass (from just before its begin to just after its
// end), a dispatch (named after its compute pipeline), a copy-like command, or
// "other" (barriers and state between the previous event and a pass).
struct XportPassTimer {
  static constexpr uint32_t kMaxQueries = 4096;
  enum class Kind { kPass, kDispatch, kCopy, kOther };
  struct Event {
    Kind kind;
    std::string label;
    uint32_t start_query, end_query;
    // Render passes only.
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    VkRect2D area;
    uint32_t draws, clears;
  };
  bool armed = false;
  VkQueryPool pool = VK_NULL_HANDLE;
  uint32_t queries = 0;
  std::vector<Event> events;
};
XportPassTimer g_xport_pass_timer;

std::mutex g_xport_pipeline_names_mutex;
std::unordered_map<VkPipeline, std::string> g_xport_pipeline_names;
}  // namespace

void XportNamePipeline(VkPipeline pipeline, std::string name) {
  std::lock_guard<std::mutex> lock(g_xport_pipeline_names_mutex);
  g_xport_pipeline_names[pipeline] = std::move(name);
}

void XportPassTimeArm(bool armed) { g_xport_pass_timer.armed = armed; }

void XportPassTimeReport(const ui::vulkan::VulkanDevice& device,
                        uint64_t frame) {
  XportPassTimer& timer = g_xport_pass_timer;
  if (timer.pool == VK_NULL_HANDLE || !timer.queries) {
    return;
  }
  std::vector<uint64_t> ticks(timer.queries);
  if (device.functions().vkGetQueryPoolResults(
          device.device(), timer.pool, 0, timer.queries,
          sizeof(uint64_t) * ticks.size(), ticks.data(), sizeof(uint64_t),
          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) {
    XELOGE("xportgpu: pass time results unavailable");
    timer.queries = 0;
    return;
  }
  VkPhysicalDeviceProperties properties;
  device.vulkan_instance()->functions().vkGetPhysicalDeviceProperties(
      device.physical_device(), &properties);
  const double ms = properties.limits.timestampPeriod * 1e-6;
  std::map<std::string, std::pair<uint32_t, double>> work;
  double in_passes = 0.0;
  uint32_t pass_index = 0;
  for (const XportPassTimer::Event& event : timer.events) {
    if (event.end_query >= timer.queries) {
      continue;
    }
    const double duration =
        double(ticks[event.end_query] - ticks[event.start_query]) * ms;
    if (event.kind == XportPassTimer::Kind::kPass) {
      in_passes += duration;
      XELOGI(
          "xportgpu: frame {} pass {}: {:.3f} ms, before it 0.000 ms (0 "
          "dispatches, 0 copies, 0 barriers), rp {:016X} fb {:016X} area "
          "{},{} {}x{}, {} draws, {} clears",
          frame, pass_index++, duration, uint64_t(event.render_pass),
          uint64_t(event.framebuffer), event.area.offset.x, event.area.offset.y,
          event.area.extent.width, event.area.extent.height, event.draws,
          event.clears);
    } else {
      auto& entry = work[event.label];
      ++entry.first;
      entry.second += duration;
    }
  }
  for (const auto& [label, entry] : work) {
    XELOGI("xportgpu: frame {} work {}: {} times, {:.3f} ms", frame, label,
           entry.first, entry.second);
  }
  const double total = double(ticks[timer.queries - 1] - ticks[0]) * ms;
  XELOGI(
      "xportgpu: frame {}: {} passes, {:.3f} ms total, {:.3f} ms in passes, "
      "0 dispatches after the last pass",
      frame, pass_index, total, in_passes);
  timer.queries = 0;
}

void DeferredCommandBuffer::Reset() {
  command_stream_.clear();
  render_area_scan_offset_ = 0;
  render_area_scissor_known_ = false;
}

// Xenia begins every render target cache pass with the whole
// framebuffer as the render area, and the attachments are loaded and stored
// (EDRAM contents must persist). On tile-based GPUs such as the Mali the render
// area decides which tiles are read from and written back to memory, so with
// The title's predicated tiling every tile of the 1280x1024 4x MSAA G-buffer (four
// attachments) loaded and stored all of it. Before the pass is closed its
// render area is reduced to the union of the scissors of its draws and the
// rectangles of its clears (nothing is rasterized outside the scissor, and
// attachments outside the render area keep their contents). Passes with clear
// values (load op clear) are left alone. XPORT_XENIA_TIGHT_RENDER_AREA=0
// disables it; =ab switches it on and off every 10 s (A/B measurements within
// one run, logged).
void DeferredCommandBuffer::CmdVkEndRenderPass() {
  static const char mode = [] {
    const char* value = std::getenv("XPORT_XENIA_TIGHT_RENDER_AREA");
    return value ? value[0] : '1';
  }();
  bool enabled = mode != '0';
  if (mode == 'a') {
    static int last_phase = -1;
    const int phase = int(std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count() /
                          10 % 2);
    enabled = phase == 0;
    if (phase != last_phase) {
      last_phase = phase;
      XELOGI("xportgpu: tight render area {}", enabled ? "on" : "off");
    }
  }
  if (enabled) {
    ShrinkRenderArea();
  }
  WriteCommand(Command::kVkEndRenderPass, 0);
}

void DeferredCommandBuffer::ShrinkRenderArea() {
  ArgsVkBeginRenderPass* pass = nullptr;
  bool whole = false;
  int64_t x0 = INT64_MAX, y0 = INT64_MAX, x1 = INT64_MIN, y1 = INT64_MIN;
  auto grow = [&](const VkRect2D& rect) {
    x0 = std::min(x0, int64_t(rect.offset.x));
    y0 = std::min(y0, int64_t(rect.offset.y));
    x1 = std::max(x1, int64_t(rect.offset.x) + rect.extent.width);
    y1 = std::max(y1, int64_t(rect.offset.y) + rect.extent.height);
  };
  uintmax_t* stream = command_stream_.data();
  size_t offset = render_area_scan_offset_;
  while (offset < command_stream_.size()) {
    const CommandHeader& header =
        *reinterpret_cast<const CommandHeader*>(stream + offset);
    uint8_t* args_ptr =
        reinterpret_cast<uint8_t*>(stream + offset + kCommandHeaderSizeElements);
    switch (header.command) {
      case Command::kVkBeginRenderPass:
        pass = reinterpret_cast<ArgsVkBeginRenderPass*>(args_ptr);
        whole = false;
        x0 = y0 = INT64_MAX;
        x1 = y1 = INT64_MIN;
        break;
      case Command::kVkSetScissor: {
        auto& args = *reinterpret_cast<const ArgsVkSetScissor*>(args_ptr);
        if (args.first_scissor == 0 && args.scissor_count) {
          render_area_scissor_ = *reinterpret_cast<const VkRect2D*>(
              args_ptr + xe::align(sizeof(ArgsVkSetScissor), alignof(VkRect2D)));
          render_area_scissor_known_ = true;
        }
      } break;
      case Command::kVkDraw:
      case Command::kVkDrawIndexed:
        if (render_area_scissor_known_) {
          grow(render_area_scissor_);
        } else {
          whole = true;
        }
        break;
      case Command::kVkClearAttachments: {
        auto& args = *reinterpret_cast<const ArgsVkClearAttachments*>(args_ptr);
        size_t rects_offset = xe::align(
            xe::align(sizeof(ArgsVkClearAttachments), alignof(VkClearAttachment)) +
                sizeof(VkClearAttachment) * args.attachment_count,
            alignof(VkClearRect));
        auto* rects =
            reinterpret_cast<const VkClearRect*>(args_ptr + rects_offset);
        for (uint32_t i = 0; i < args.rect_count; ++i) {
          grow(rects[i].rect);
        }
      } break;
      default:
        break;
    }
    offset += kCommandHeaderSizeElements + header.arguments_size_elements;
  }
  render_area_scan_offset_ = offset;
  if (!pass || whole || pass->clear_value_count) {
    return;
  }
  VkRect2D& area = pass->render_area;
  int64_t ax0 = area.offset.x, ay0 = area.offset.y;
  int64_t ax1 = ax0 + area.extent.width, ay1 = ay0 + area.extent.height;
  x0 = std::max(x0, ax0);
  y0 = std::max(y0, ay0);
  x1 = std::min(x1, ax1);
  y1 = std::min(y1, ay1);
  if (x1 <= x0 || y1 <= y0) {
    // Nothing drawn: a 1x1 area, so almost nothing is loaded or stored.
    area.extent.width = 1;
    area.extent.height = 1;
    return;
  }
  area.offset.x = int32_t(x0);
  area.offset.y = int32_t(y0);
  area.extent.width = uint32_t(x1 - x0);
  area.extent.height = uint32_t(y1 - y0);
}

void DeferredCommandBuffer::Execute(VkCommandBuffer command_buffer) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const ui::vulkan::VulkanDevice::Functions& dfn =
      command_processor_.GetVulkanDevice()->functions();

  // XPORT_XENIA_PASS_TIME timestamps (see XportPassTimer).
  XportPassTimer* pass_timer = nullptr;
  uint32_t last_query = 0;
  VkPipeline compute_pipeline = VK_NULL_HANDLE;
  auto write_timestamp = [&]() -> uint32_t {
    dfn.vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            pass_timer->pool, pass_timer->queries);
    return pass_timer->queries++;
  };
  auto close_event = [&](XportPassTimer::Kind kind, std::string label) {
    if (pass_timer->queries + 2 >= XportPassTimer::kMaxQueries) {
      return;
    }
    const uint32_t query = write_timestamp();
    XportPassTimer::Event event = {};
    event.kind = kind;
    event.label = std::move(label);
    event.start_query = last_query;
    event.end_query = query;
    pass_timer->events.push_back(std::move(event));
    last_query = query;
  };
  if (g_xport_pass_timer.armed) {
    XportPassTimer& timer = g_xport_pass_timer;
    if (timer.pool == VK_NULL_HANDLE) {
      VkQueryPoolCreateInfo query_pool_create_info = {
          VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      query_pool_create_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
      query_pool_create_info.queryCount = XportPassTimer::kMaxQueries;
      dfn.vkCreateQueryPool(command_processor_.GetVulkanDevice()->device(),
                            &query_pool_create_info, nullptr, &timer.pool);
    }
    if (timer.pool != VK_NULL_HANDLE) {
      pass_timer = &timer;
      timer.events.clear();
      dfn.vkCmdResetQueryPool(command_buffer, timer.pool, 0,
                              XportPassTimer::kMaxQueries);
      timer.queries = 0;
      last_query = write_timestamp();
    }
  }

  const uintmax_t* stream = command_stream_.data();
  size_t stream_remaining = command_stream_.size();
  while (stream_remaining) {
    const CommandHeader& header =
        *reinterpret_cast<const CommandHeader*>(stream);
    stream += kCommandHeaderSizeElements;
    stream_remaining -= kCommandHeaderSizeElements;

    if (pass_timer) {
      switch (header.command) {
        case Command::kVkBeginRenderPass:
          if (pass_timer->queries + 4 < XportPassTimer::kMaxQueries) {
            close_event(XportPassTimer::Kind::kOther, "barriers and state");
            auto& args =
                *reinterpret_cast<const ArgsVkBeginRenderPass*>(stream);
            XportPassTimer::Event event = {};
            event.kind = XportPassTimer::Kind::kPass;
            event.start_query = last_query;
            event.end_query = UINT32_MAX;
            event.render_pass = args.render_pass;
            event.framebuffer = args.framebuffer;
            event.area = args.render_area;
            pass_timer->events.push_back(std::move(event));
          }
          break;
        case Command::kVkBindPipeline: {
          auto& args = *reinterpret_cast<const ArgsVkBindPipeline*>(stream);
          if (args.pipeline_bind_point == VK_PIPELINE_BIND_POINT_COMPUTE) {
            compute_pipeline = args.pipeline;
          }
        } break;
        case Command::kVkDraw:
        case Command::kVkDrawIndexed:
          if (!pass_timer->events.empty()) {
            ++pass_timer->events.back().draws;
          }
          break;
        case Command::kVkClearAttachments:
          if (!pass_timer->events.empty()) {
            ++pass_timer->events.back().clears;
          }
          break;
        default:
          break;
      }
    }

    switch (header.command) {
      case Command::kVkBeginRenderPass: {
        auto& args = *reinterpret_cast<const ArgsVkBeginRenderPass*>(stream);
        size_t offset_bytes = sizeof(ArgsVkBeginRenderPass);
        VkRenderPassBeginInfo render_pass_begin_info;
        render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_pass_begin_info.pNext = nullptr;
        render_pass_begin_info.renderPass = args.render_pass;
        render_pass_begin_info.framebuffer = args.framebuffer;
        render_pass_begin_info.renderArea = args.render_area;
        render_pass_begin_info.clearValueCount = args.clear_value_count;
        if (render_pass_begin_info.clearValueCount) {
          offset_bytes = xe::align(offset_bytes, alignof(VkClearValue));
          render_pass_begin_info.pClearValues =
              reinterpret_cast<const VkClearValue*>(
                  reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
          offset_bytes +=
              sizeof(VkClearValue) * render_pass_begin_info.clearValueCount;
        } else {
          render_pass_begin_info.pClearValues = nullptr;
        }
        VkAttachmentSampleLocationsEXT initial_sample_locations;
        VkSubpassSampleLocationsEXT post_sample_locations;
        VkRenderPassSampleLocationsBeginInfoEXT sample_locations_begin;
        if (args.sample_locations_count) {
          offset_bytes = xe::align(offset_bytes, alignof(VkSampleLocationEXT));
          initial_sample_locations.attachmentIndex = 0;
          auto& info = initial_sample_locations.sampleLocationsInfo;
          info.sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT;
          info.pNext = nullptr;
          info.sampleLocationsPerPixel = args.sample_locations_per_pixel;
          info.sampleLocationGridSize = args.sample_location_grid_size;
          info.sampleLocationsCount = args.sample_locations_count;
          info.pSampleLocations = reinterpret_cast<const VkSampleLocationEXT*>(
              reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
          post_sample_locations.subpassIndex = 0;
          post_sample_locations.sampleLocationsInfo = info;
          sample_locations_begin.sType =
              VK_STRUCTURE_TYPE_RENDER_PASS_SAMPLE_LOCATIONS_BEGIN_INFO_EXT;
          sample_locations_begin.pNext = nullptr;
          sample_locations_begin.attachmentInitialSampleLocationsCount =
              args.has_depth_attachment ? 1 : 0;
          sample_locations_begin.pAttachmentInitialSampleLocations =
              &initial_sample_locations;
          sample_locations_begin.postSubpassSampleLocationsCount = 1;
          sample_locations_begin.pPostSubpassSampleLocations =
              &post_sample_locations;
          render_pass_begin_info.pNext = &sample_locations_begin;
        }
        dfn.vkCmdBeginRenderPass(command_buffer, &render_pass_begin_info,
                                 args.contents);
      } break;

      case Command::kVkBindDescriptorSets: {
        auto& args = *reinterpret_cast<const ArgsVkBindDescriptorSets*>(stream);
        size_t offset_bytes = xe::align(sizeof(ArgsVkBindDescriptorSets),
                                        alignof(VkDescriptorSet));
        const VkDescriptorSet* descriptor_sets =
            reinterpret_cast<const VkDescriptorSet*>(
                reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        offset_bytes += sizeof(VkDescriptorSet) * args.descriptor_set_count;
        const uint32_t* dynamic_offsets = nullptr;
        if (args.dynamic_offset_count) {
          offset_bytes = xe::align(offset_bytes, alignof(uint32_t));
          dynamic_offsets = reinterpret_cast<const uint32_t*>(
              reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
          offset_bytes += sizeof(uint32_t) * args.dynamic_offset_count;
        }
        dfn.vkCmdBindDescriptorSets(command_buffer, args.pipeline_bind_point,
                                    args.layout, args.first_set,
                                    args.descriptor_set_count, descriptor_sets,
                                    args.dynamic_offset_count, dynamic_offsets);
      } break;

      case Command::kVkBindIndexBuffer: {
        auto& args = *reinterpret_cast<const ArgsVkBindIndexBuffer*>(stream);
        dfn.vkCmdBindIndexBuffer(command_buffer, args.buffer, args.offset,
                                 args.index_type);
      } break;

      case Command::kVkBindPipeline: {
        auto& args = *reinterpret_cast<const ArgsVkBindPipeline*>(stream);
        dfn.vkCmdBindPipeline(command_buffer, args.pipeline_bind_point,
                              args.pipeline);
      } break;

      case Command::kVkBindVertexBuffers: {
        auto& args = *reinterpret_cast<const ArgsVkBindVertexBuffers*>(stream);
        size_t offset_bytes =
            xe::align(sizeof(ArgsVkBindVertexBuffers), alignof(VkBuffer));
        const VkBuffer* buffers = reinterpret_cast<const VkBuffer*>(
            reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        offset_bytes =
            xe::align(offset_bytes + sizeof(VkBuffer) * args.binding_count,
                      alignof(VkDeviceSize));
        const VkDeviceSize* offsets = reinterpret_cast<const VkDeviceSize*>(
            reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        dfn.vkCmdBindVertexBuffers(command_buffer, args.first_binding,
                                   args.binding_count, buffers, offsets);
      } break;
      case Command::kVkBeginQuery: {
        auto& args = *reinterpret_cast<const ArgsVkBeginQuery*>(stream);
        dfn.vkCmdBeginQuery(command_buffer, args.query_pool, args.query,
                            args.flags);
      } break;
      case Command::kVkEndQuery: {
        auto& args = *reinterpret_cast<const ArgsVkEndQuery*>(stream);
        dfn.vkCmdEndQuery(command_buffer, args.query_pool, args.query);
      } break;
      case Command::kVkCopyQueryPoolResults: {
        auto& args =
            *reinterpret_cast<const ArgsVkCopyQueryPoolResults*>(stream);
        dfn.vkCmdCopyQueryPoolResults(
            command_buffer, args.query_pool, args.first_query, args.query_count,
            args.dst_buffer, args.dst_offset, args.stride, args.flags);
      } break;
      case Command::kVkResetQueryPool: {
        auto& args = *reinterpret_cast<const ArgsVkResetQueryPool*>(stream);
        dfn.vkCmdResetQueryPool(command_buffer, args.query_pool,
                                args.first_query, args.query_count);
      } break;
      case Command::kVkBeginConditionalRenderingEXT: {
        auto& args =
            *reinterpret_cast<const ArgsVkBeginConditionalRenderingEXT*>(
                stream);
        VkConditionalRenderingBeginInfoEXT begin_info;
        begin_info.sType =
            VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT;
        begin_info.pNext = nullptr;
        begin_info.buffer = args.buffer;
        begin_info.offset = args.offset;
        begin_info.flags = args.flags;
        dfn.vkCmdBeginConditionalRenderingEXT(command_buffer, &begin_info);
      } break;
      case Command::kVkEndConditionalRenderingEXT: {
        dfn.vkCmdEndConditionalRenderingEXT(command_buffer);
      } break;

      case Command::kVkClearAttachments: {
        auto& args = *reinterpret_cast<const ArgsVkClearAttachments*>(stream);
        size_t offset_bytes = xe::align(sizeof(ArgsVkClearAttachments),
                                        alignof(VkClearAttachment));
        const VkClearAttachment* attachments =
            reinterpret_cast<const VkClearAttachment*>(
                reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        offset_bytes = xe::align(
            offset_bytes + sizeof(VkClearAttachment) * args.attachment_count,
            alignof(VkClearRect));
        const VkClearRect* rects = reinterpret_cast<const VkClearRect*>(
            reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        dfn.vkCmdClearAttachments(command_buffer, args.attachment_count,
                                  attachments, args.rect_count, rects);
      } break;

      case Command::kVkClearColorImage: {
        auto& args = *reinterpret_cast<const ArgsVkClearColorImage*>(stream);
        dfn.vkCmdClearColorImage(
            command_buffer, args.image, args.image_layout, &args.color,
            args.range_count,
            reinterpret_cast<const VkImageSubresourceRange*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkClearColorImage),
                          alignof(VkImageSubresourceRange))));
      } break;

      case Command::kVkCopyBuffer: {
        auto& args = *reinterpret_cast<const ArgsVkCopyBuffer*>(stream);
        dfn.vkCmdCopyBuffer(
            command_buffer, args.src_buffer, args.dst_buffer, args.region_count,
            reinterpret_cast<const VkBufferCopy*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkCopyBuffer), alignof(VkBufferCopy))));
      } break;

      case Command::kVkFillBuffer: {
        auto& args = *reinterpret_cast<const ArgsVkFillBuffer*>(stream);
        dfn.vkCmdFillBuffer(command_buffer, args.dst_buffer, args.dst_offset,
                            args.size, args.data);
      } break;

      case Command::kVkCopyBufferToImage: {
        auto& args = *reinterpret_cast<const ArgsVkCopyBufferToImage*>(stream);
        dfn.vkCmdCopyBufferToImage(
            command_buffer, args.src_buffer, args.dst_image,
            args.dst_image_layout, args.region_count,
            reinterpret_cast<const VkBufferImageCopy*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkCopyBufferToImage),
                          alignof(VkBufferImageCopy))));
      } break;

      case Command::kVkCopyImageToBuffer: {
        auto& args = *reinterpret_cast<const ArgsVkCopyImageToBuffer*>(stream);
        dfn.vkCmdCopyImageToBuffer(command_buffer, args.src_image,
                                   args.src_image_layout, args.dst_buffer, 1,
                                   &args.region);
      } break;

      case Command::kVkBlitImage: {
        auto& args = *reinterpret_cast<const ArgsVkBlitImage*>(stream);
        dfn.vkCmdBlitImage(
            command_buffer, args.src_image, args.src_image_layout,
            args.dst_image, args.dst_image_layout, args.region_count,
            reinterpret_cast<const VkImageBlit*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkBlitImage), alignof(VkImageBlit))),
            args.filter);
      } break;

      case Command::kVkDispatch: {
        auto& args = *reinterpret_cast<const ArgsVkDispatch*>(stream);
        dfn.vkCmdDispatch(command_buffer, args.group_count_x,
                          args.group_count_y, args.group_count_z);
      } break;

      case Command::kVkDraw: {
        auto& args = *reinterpret_cast<const ArgsVkDraw*>(stream);
        dfn.vkCmdDraw(command_buffer, args.vertex_count, args.instance_count,
                      args.first_vertex, args.first_instance);
      } break;

      case Command::kVkDrawIndexed: {
        auto& args = *reinterpret_cast<const ArgsVkDrawIndexed*>(stream);
        dfn.vkCmdDrawIndexed(command_buffer, args.index_count,
                             args.instance_count, args.first_index,
                             args.vertex_offset, args.first_instance);
      } break;

      case Command::kVkEndRenderPass:
        dfn.vkCmdEndRenderPass(command_buffer);
        if (pass_timer && !pass_timer->events.empty() &&
            pass_timer->events.back().kind == XportPassTimer::Kind::kPass &&
            pass_timer->events.back().end_query == UINT32_MAX) {
          last_query = pass_timer->events.back().end_query = write_timestamp();
        }
        break;

      case Command::kVkPipelineBarrier: {
        auto& args = *reinterpret_cast<const ArgsVkPipelineBarrier*>(stream);
        size_t barrier_offset_bytes = sizeof(ArgsVkPipelineBarrier);
        const VkMemoryBarrier* memory_barriers = nullptr;
        if (args.memory_barrier_count) {
          barrier_offset_bytes =
              xe::align(barrier_offset_bytes, alignof(VkMemoryBarrier));
          memory_barriers = reinterpret_cast<const VkMemoryBarrier*>(
              reinterpret_cast<const uint8_t*>(stream) + barrier_offset_bytes);
          barrier_offset_bytes +=
              sizeof(VkMemoryBarrier) * args.memory_barrier_count;
        }
        const VkBufferMemoryBarrier* buffer_memory_barriers = nullptr;
        if (args.buffer_memory_barrier_count) {
          barrier_offset_bytes =
              xe::align(barrier_offset_bytes, alignof(VkBufferMemoryBarrier));
          buffer_memory_barriers =
              reinterpret_cast<const VkBufferMemoryBarrier*>(
                  reinterpret_cast<const uint8_t*>(stream) +
                  barrier_offset_bytes);
          barrier_offset_bytes +=
              sizeof(VkBufferMemoryBarrier) * args.buffer_memory_barrier_count;
        }
        const VkImageMemoryBarrier* image_memory_barriers = nullptr;
        if (args.image_memory_barrier_count) {
          barrier_offset_bytes =
              xe::align(barrier_offset_bytes, alignof(VkImageMemoryBarrier));
          image_memory_barriers = reinterpret_cast<const VkImageMemoryBarrier*>(
              reinterpret_cast<const uint8_t*>(stream) + barrier_offset_bytes);
          barrier_offset_bytes +=
              sizeof(VkImageMemoryBarrier) * args.image_memory_barrier_count;
        }
        if (args.sample_locations_info_count) {
          image_barriers_with_sample_locations_.assign(
              image_memory_barriers,
              image_memory_barriers + args.image_memory_barrier_count);
          barrier_sample_locations_.resize(args.sample_locations_info_count);
          for (uint32_t i = 0; i < args.sample_locations_info_count; ++i) {
            barrier_offset_bytes = xe::align(barrier_offset_bytes,
                                             alignof(ArgsSampleLocationsInfo));
            const auto& locations_args =
                *reinterpret_cast<const ArgsSampleLocationsInfo*>(
                    reinterpret_cast<const uint8_t*>(stream) +
                    barrier_offset_bytes);
            barrier_offset_bytes += sizeof(ArgsSampleLocationsInfo);
            barrier_offset_bytes =
                xe::align(barrier_offset_bytes, alignof(VkSampleLocationEXT));
            auto& info = barrier_sample_locations_[i];
            info.sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT;
            info.pNext = nullptr;
            info.sampleLocationsPerPixel = locations_args.samples_per_pixel;
            info.sampleLocationGridSize = locations_args.grid_size;
            info.sampleLocationsCount = locations_args.count;
            info.pSampleLocations =
                reinterpret_cast<const VkSampleLocationEXT*>(
                    reinterpret_cast<const uint8_t*>(stream) +
                    barrier_offset_bytes);
            barrier_offset_bytes +=
                sizeof(VkSampleLocationEXT) * locations_args.count;
            image_barriers_with_sample_locations_
                [locations_args.image_memory_barrier_index]
                    .pNext = &info;
          }
          image_memory_barriers = image_barriers_with_sample_locations_.data();
        }
        dfn.vkCmdPipelineBarrier(
            command_buffer, args.src_stage_mask, args.dst_stage_mask,
            args.dependency_flags, args.memory_barrier_count, memory_barriers,
            args.buffer_memory_barrier_count, buffer_memory_barriers,
            args.image_memory_barrier_count, image_memory_barriers);
      } break;

      case Command::kVkPushConstants: {
        auto& args = *reinterpret_cast<const ArgsVkPushConstants*>(stream);
        dfn.vkCmdPushConstants(command_buffer, args.layout, args.stage_flags,
                               args.offset, args.size,
                               reinterpret_cast<const uint8_t*>(stream) +
                                   sizeof(ArgsVkPushConstants));
      } break;

      case Command::kVkSetBlendConstants: {
        auto& args = *reinterpret_cast<const ArgsVkSetBlendConstants*>(stream);
        dfn.vkCmdSetBlendConstants(command_buffer, args.blend_constants);
      } break;

      case Command::kVkSetDepthBias: {
        auto& args = *reinterpret_cast<const ArgsVkSetDepthBias*>(stream);
        dfn.vkCmdSetDepthBias(command_buffer, args.depth_bias_constant_factor,
                              args.depth_bias_clamp,
                              args.depth_bias_slope_factor);
      } break;

      case Command::kVkSetDepthBias2EXT: {
        auto& args = *reinterpret_cast<const ArgsVkSetDepthBias2EXT*>(stream);
        VkDepthBiasRepresentationInfoEXT representation_info = {};
        representation_info.sType =
            VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT;
        representation_info.depthBiasRepresentation =
            args.depth_bias_representation;
        representation_info.depthBiasExact = args.depth_bias_exact;
        VkDepthBiasInfoEXT depth_bias_info = {};
        depth_bias_info.sType = VK_STRUCTURE_TYPE_DEPTH_BIAS_INFO_EXT;
        depth_bias_info.pNext = &representation_info;
        depth_bias_info.depthBiasConstantFactor =
            args.depth_bias_constant_factor;
        depth_bias_info.depthBiasClamp = args.depth_bias_clamp;
        depth_bias_info.depthBiasSlopeFactor = args.depth_bias_slope_factor;
        dfn.vkCmdSetDepthBias2EXT(command_buffer, &depth_bias_info);
      } break;

      case Command::kVkSetScissor: {
        auto& args = *reinterpret_cast<const ArgsVkSetScissor*>(stream);
        dfn.vkCmdSetScissor(
            command_buffer, args.first_scissor, args.scissor_count,
            reinterpret_cast<const VkRect2D*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkSetScissor), alignof(VkRect2D))));
      } break;

      case Command::kVkSetStencilCompareMask: {
        auto& args =
            *reinterpret_cast<const ArgsSetStencilMaskReference*>(stream);
        dfn.vkCmdSetStencilCompareMask(command_buffer, args.face_mask,
                                       args.mask_reference);
      } break;

      case Command::kVkSetStencilReference: {
        auto& args =
            *reinterpret_cast<const ArgsSetStencilMaskReference*>(stream);
        dfn.vkCmdSetStencilReference(command_buffer, args.face_mask,
                                     args.mask_reference);
      } break;

      case Command::kVkSetStencilWriteMask: {
        auto& args =
            *reinterpret_cast<const ArgsSetStencilMaskReference*>(stream);
        dfn.vkCmdSetStencilWriteMask(command_buffer, args.face_mask,
                                     args.mask_reference);
      } break;

      case Command::kVkSetViewport: {
        auto& args = *reinterpret_cast<const ArgsVkSetViewport*>(stream);
        dfn.vkCmdSetViewport(
            command_buffer, args.first_viewport, args.viewport_count,
            reinterpret_cast<const VkViewport*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkSetViewport), alignof(VkViewport))));
      } break;

      default:
        assert_unhandled_case(header.command);
        break;
    }

    if (pass_timer) {
      switch (header.command) {
        case Command::kVkDispatch: {
          std::string label = "dispatch ";
          {
            std::lock_guard<std::mutex> lock(g_xport_pipeline_names_mutex);
            auto it = g_xport_pipeline_names.find(compute_pipeline);
            label += it != g_xport_pipeline_names.end()
                         ? it->second
                         : fmt::format("{:016X}", uint64_t(compute_pipeline));
          }
          close_event(XportPassTimer::Kind::kDispatch, std::move(label));
        } break;
        case Command::kVkCopyBuffer:
          close_event(XportPassTimer::Kind::kCopy, "copy buffer");
          break;
        case Command::kVkCopyBufferToImage:
          close_event(XportPassTimer::Kind::kCopy, "copy buffer to image");
          break;
        case Command::kVkCopyImageToBuffer:
          close_event(XportPassTimer::Kind::kCopy, "copy image to buffer");
          break;
        case Command::kVkBlitImage:
          close_event(XportPassTimer::Kind::kCopy, "blit image");
          break;
        case Command::kVkFillBuffer:
          close_event(XportPassTimer::Kind::kCopy, "fill buffer");
          break;
        case Command::kVkClearColorImage:
          close_event(XportPassTimer::Kind::kCopy, "clear color image");
          break;
        default:
          break;
      }
    }

    stream += header.arguments_size_elements;
    stream_remaining -= header.arguments_size_elements;
  }

  if (pass_timer) {
    close_event(XportPassTimer::Kind::kOther, "barriers and state");
  }
}

void DeferredCommandBuffer::CmdVkPipelineBarrier(
    VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
    VkDependencyFlags dependency_flags, uint32_t memory_barrier_count,
    const VkMemoryBarrier* memory_barriers,
    uint32_t buffer_memory_barrier_count,
    const VkBufferMemoryBarrier* buffer_memory_barriers,
    uint32_t image_memory_barrier_count,
    const VkImageMemoryBarrier* image_memory_barriers) {
  size_t arguments_size = sizeof(ArgsVkPipelineBarrier);
  size_t memory_barriers_offset = 0;
  if (memory_barrier_count) {
    arguments_size = xe::align(arguments_size, alignof(VkMemoryBarrier));
    memory_barriers_offset = arguments_size;
    arguments_size += sizeof(VkMemoryBarrier) * memory_barrier_count;
  }
  size_t buffer_memory_barriers_offset = 0;
  if (buffer_memory_barrier_count) {
    arguments_size = xe::align(arguments_size, alignof(VkBufferMemoryBarrier));
    buffer_memory_barriers_offset = arguments_size;
    arguments_size +=
        sizeof(VkBufferMemoryBarrier) * buffer_memory_barrier_count;
  }
  size_t image_memory_barriers_offset = 0;
  if (image_memory_barrier_count) {
    arguments_size = xe::align(arguments_size, alignof(VkImageMemoryBarrier));
    image_memory_barriers_offset = arguments_size;
    arguments_size += sizeof(VkImageMemoryBarrier) * image_memory_barrier_count;
  }
  size_t sample_locations_offset = arguments_size;
  uint32_t sample_locations_info_count = 0;
  for (uint32_t i = 0; i < image_memory_barrier_count; ++i) {
    const auto* info = static_cast<const VkSampleLocationsInfoEXT*>(
        image_memory_barriers[i].pNext);
    if (!info) {
      continue;
    }
    assert_true(info->sType == VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT);
    assert_null(info->pNext);
    ++sample_locations_info_count;
    arguments_size =
        xe::align(arguments_size, alignof(ArgsSampleLocationsInfo));
    arguments_size += sizeof(ArgsSampleLocationsInfo);
    arguments_size = xe::align(arguments_size, alignof(VkSampleLocationEXT));
    arguments_size += sizeof(VkSampleLocationEXT) * info->sampleLocationsCount;
  }
  uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
      WriteCommand(Command::kVkPipelineBarrier, arguments_size));
  auto& args = *reinterpret_cast<ArgsVkPipelineBarrier*>(args_ptr);
  args.src_stage_mask = src_stage_mask;
  args.dst_stage_mask = dst_stage_mask;
  args.dependency_flags = dependency_flags;
  args.memory_barrier_count = memory_barrier_count;
  args.buffer_memory_barrier_count = buffer_memory_barrier_count;
  args.image_memory_barrier_count = image_memory_barrier_count;
  args.sample_locations_info_count = sample_locations_info_count;
  if (memory_barrier_count) {
    std::memcpy(args_ptr + memory_barriers_offset, memory_barriers,
                sizeof(VkMemoryBarrier) * memory_barrier_count);
  }
  if (buffer_memory_barrier_count) {
    std::memcpy(args_ptr + buffer_memory_barriers_offset,
                buffer_memory_barriers,
                sizeof(VkBufferMemoryBarrier) * buffer_memory_barrier_count);
  }
  if (image_memory_barrier_count) {
    std::memcpy(args_ptr + image_memory_barriers_offset, image_memory_barriers,
                sizeof(VkImageMemoryBarrier) * image_memory_barrier_count);
  }
  for (uint32_t i = 0; i < image_memory_barrier_count; ++i) {
    const auto* info = static_cast<const VkSampleLocationsInfoEXT*>(
        image_memory_barriers[i].pNext);
    if (!info) {
      continue;
    }
    reinterpret_cast<VkImageMemoryBarrier*>(args_ptr +
                                            image_memory_barriers_offset)[i]
        .pNext = nullptr;
    sample_locations_offset =
        xe::align(sample_locations_offset, alignof(ArgsSampleLocationsInfo));
    auto& locations_args = *reinterpret_cast<ArgsSampleLocationsInfo*>(
        args_ptr + sample_locations_offset);
    locations_args.image_memory_barrier_index = i;
    locations_args.samples_per_pixel = info->sampleLocationsPerPixel;
    locations_args.grid_size = info->sampleLocationGridSize;
    locations_args.count = info->sampleLocationsCount;
    sample_locations_offset += sizeof(ArgsSampleLocationsInfo);
    sample_locations_offset =
        xe::align(sample_locations_offset, alignof(VkSampleLocationEXT));
    std::memcpy(args_ptr + sample_locations_offset, info->pSampleLocations,
                sizeof(VkSampleLocationEXT) * info->sampleLocationsCount);
    sample_locations_offset +=
        sizeof(VkSampleLocationEXT) * info->sampleLocationsCount;
  }
}

void* DeferredCommandBuffer::WriteCommand(Command command,
                                          size_t arguments_size_bytes) {
  size_t arguments_size_elements =
      (arguments_size_bytes + sizeof(uintmax_t) - 1) / sizeof(uintmax_t);
  size_t offset = command_stream_.size();
  command_stream_.resize(offset + kCommandHeaderSizeElements +
                         arguments_size_elements);
  CommandHeader& header =
      *reinterpret_cast<CommandHeader*>(command_stream_.data() + offset);
  header.command = command;
  header.arguments_size_elements = uint32_t(arguments_size_elements);
  return command_stream_.data() + (offset + kCommandHeaderSizeElements);
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
