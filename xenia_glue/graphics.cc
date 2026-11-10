// Xenia's GPU in the runtime. Replaces Xenia's graphics_system.cc (built around its
// emulator) with the same GraphicsSystem on this runtime's terms, and provides what Xenia's
// presenter needs on Android without Xenia's app: a UI thread of its own (xport-ui) and a Window
// over the activity's SurfaceView. The runtime talks to all of it through xport_gpu.h.
//
// Threads: the command processor runs its own worker (Xenia's "GPU Commands"), which also runs
// guest interrupt callbacks through the runtime; the presenter's lifecycle and UI-thread paints
// happen on xport-ui; guest frames are presented from the GPU thread when the presenter allows it.
#include "xport_gpu.h"

#ifdef __ANDROID__
#include <android/native_window.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb/stb_image_write.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/threading.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/texture_address.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/ui/presenter.h"
#ifdef __ANDROID__
#include "xenia/ui/surface_android.h"
#endif
#include "xenia/ui/vulkan/vulkan_device.h"
#include "xenia/ui/vulkan/vulkan_provider.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

namespace xe::gpu::vulkan {
extern void (*xport_swap_hook)();  // vulkan_command_processor.cc (xport)
extern void (*xport_resolve_hook)(uint32_t address, uint32_t length,
                                     const uint8_t* data, uint32_t dest_info);
// Draws per IssueDraw return statement (vulkan_command_processor.cc, xport).
extern uint32_t xport_draw_stats[64];
extern std::atomic<uint32_t> xport_dbg_stage;
extern std::atomic<uint32_t> xport_dbg_draws;
extern uint64_t xport_transfer_tiles[4];
extern uint64_t xport_render_target_memory;
void XportTextureAllocatorStats(uint64_t& block_bytes_out,
                               uint64_t& allocation_bytes_out);
extern uint32_t xport_render_target_count;
}  // namespace xe::gpu::vulkan
namespace xe::gpu {
extern uint64_t xport_texture_memory;
}  // namespace xe::gpu
namespace xe::gpu::vulkan {
// Logs ownership transfers (vulkan_render_target_cache.cc, xport).
extern bool xport_log_transfers;
extern bool xport_log_transfer_lines;
}

namespace {
xportgpu::InterruptFunction g_interrupt = nullptr;
}  // namespace

// ---------------------------------------------------------------------------
// GraphicsSystem (Xenia's graphics_system.h, implemented for this runtime).
// ---------------------------------------------------------------------------
namespace xe {
namespace gpu {

GraphicsSystem::GraphicsSystem() : frame_limiter_worker_running_(false) {
  register_file_ = reinterpret_cast<RegisterFile*>(memory::AllocFixed(
      nullptr, sizeof(RegisterFile), memory::AllocationType::kReserveCommit,
      memory::PageAccess::kReadWrite));
}

GraphicsSystem::~GraphicsSystem() = default;

X_STATUS GraphicsSystem::Setup(cpu::Processor* processor,
                               kernel::KernelState* kernel_state,
                               ui::WindowedAppContext* app_context,
                               bool with_presentation) {
  memory_ = kernel_state->memory();
  processor_ = processor;
  kernel_state_ = kernel_state;
  app_context_ = app_context;
  scaled_aspect_x_ = 16;
  scaled_aspect_y_ = 9;

  if (with_presentation && provider_ && app_context_) {
    app_context_->CallInUIThreadSynchronous([this]() {
      presenter_ = provider_->CreatePresenter(
          [this](bool is_responsible, bool statically_from_ui_thread) {
            OnHostGpuLossFromAnyThread(is_responsible);
          });
    });
  }

  command_processor_ = CreateCommandProcessor();
  if (!command_processor_->Initialize()) {
    XELOGE("Unable to initialize command processor");
    return X_STATUS_UNSUCCESSFUL;
  }
  // Vblanks come from the runtime (xportgpu::MarkVblank), not from a frame
  // limiter thread here.
  return X_STATUS_SUCCESS;
}

void GraphicsSystem::Shutdown() {
  if (command_processor_) {
    command_processor_->Shutdown();
    command_processor_.reset();
  }
  if (presenter_ && app_context_) {
    app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
  }
  presenter_.reset();
  provider_.reset();
}

void GraphicsSystem::OnHostGpuLossFromAnyThread(bool is_responsible) {
  if (host_gpu_loss_reported_.test_and_set(std::memory_order_relaxed)) {
    return;
  }
  XELOGE("Vulkan device lost");
}

uint32_t GraphicsSystem::ReadRegisterThunk(void* ppc_context,
                                           GraphicsSystem* gs, uint32_t addr) {
  return gs->ReadRegister(addr);
}

void GraphicsSystem::WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs,
                                        uint32_t addr, uint32_t value) {
  gs->WriteRegister(addr, value);
}

uint32_t GraphicsSystem::ReadRegister(uint32_t addr) {
  const uint32_t r = (addr & 0xFFFF) / 4;
  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x0F2D:  // RB_HSIO_INTERFACE_ALIGNER_VALUE
      return 0x00BBBBBB;
    case 0x1951:  // interrupt status: vblank
      return 1;
    case 0x1961:  // AVIVO_D1MODE_VIEWPORT_SIZE: 1280x720
      return 0x050002D0;
    default:
      return register_file()->values[r];
  }
}

void GraphicsSystem::WriteRegister(uint32_t addr, uint32_t value) {
  const uint32_t r = (addr & 0xFFFF) / 4;
  if (r == 0x01C5) {  // CP_RB_WPTR
    command_processor_->UpdateWritePointer(value);
  }
  register_file()->values[r] = value;
}

void GraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  command_processor_->InitializeRingBuffer(ptr, size_log2);
}

void GraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr,
                                                uint32_t block_size_log2) {
  command_processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void GraphicsSystem::SetInterruptCallback(uint32_t callback,
                                          uint32_t user_data) {
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
}

void GraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  if (g_interrupt) {
    g_interrupt(source, cpu);
  }
}

void GraphicsSystem::MarkVblank() {
  command_processor_->increment_counter();
  DispatchInterruptCallback(0, 2);
}

void GraphicsSystem::ClearCaches() {
  command_processor_->CallInThread(
      [this]() { command_processor_->ClearCaches(); });
}

// Xenia's shader storage (translated shaders and pipeline
// descriptions, recreated at startup) and the driver's VkPipelineCache, kept
// between runs so the shaders are not translated and compiled again on every
// launch. XPORT_XENIA_SHADER_STORAGE=0 disables it.
void GraphicsSystem::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  const char* setting = std::getenv("XPORT_XENIA_SHADER_STORAGE");
  if (setting && setting[0] == '0') {
    if (completion_callback) {
      completion_callback();
    }
    return;
  }
  xe::threading::Fence fence;
  command_processor_->CallInThread(
      [this, cache_root, title_id, &fence,
       completion_callback = std::move(completion_callback)]() mutable {
        command_processor_->InitializeShaderStorage(
            cache_root, title_id, true, std::move(completion_callback));
        fence.Signal();
      });
  fence.Wait();
}

void GraphicsSystem::RequestFrameTrace() {}
void GraphicsSystem::BeginTracing() {}
void GraphicsSystem::EndTracing() {}

void GraphicsSystem::Pause() {
  paused_ = true;
  command_processor_->Pause();
}

void GraphicsSystem::Resume() {
  paused_ = false;
  command_processor_->Resume();
}

bool GraphicsSystem::Save(ByteStream* stream) { return false; }
bool GraphicsSystem::Restore(ByteStream* stream) { return false; }

std::pair<uint32_t, uint32_t> GraphicsSystem::GetResolution() const {
  return {1280, 720};
}

}  // namespace gpu
}  // namespace xe

// ---------------------------------------------------------------------------
// UI thread and window.
// ---------------------------------------------------------------------------
namespace {

class UiContext final : public xe::ui::WindowedAppContext {
 public:
  // Runs the UI loop on the thread that created the context.
  void Run() {
    while (true) {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return pending_; });
        pending_ = false;
      }
      ExecutePendingFunctionsFromUIThread();
    }
  }

 protected:
  void NotifyUILoopOfPendingFunctions() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_ = true;
    }
    condition_.notify_one();
  }
  void PlatformQuitFromUIThread() override {}

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool pending_ = false;
};

// A Window over the activity's SurfaceView: fullscreen, no menus, no input
// (input reaches the game through the runtime).
class SurfaceWindow final : public xe::ui::Window {
 public:
  explicit SurfaceWindow(xe::ui::WindowedAppContext& app_context)
      : Window(app_context, "the title", 1280, 720) {}
  ~SurfaceWindow() override { EnterDestructor(); }

  // UI thread.
  void SetNativeWindow(ANativeWindow* window) {
    if (window == native_window_) {
      return;
    }
    // Disconnect from the old surface before it goes away.
    native_window_ = nullptr;
    OnSurfaceChanged(false);
    native_window_ = window;
#ifdef __ANDROID__
    if (window) {
      WindowDestructionReceiver destruction_receiver(this);
      OnActualSizeUpdate(uint32_t(ANativeWindow_getWidth(window)),
                         uint32_t(ANativeWindow_getHeight(window)),
                         WindowResizeAction::kManual, destruction_receiver);
      OnSurfaceChanged(true);
    }
#endif
  }

 protected:
  bool OpenImpl() override {
    OnDesiredFullscreenUpdate(true);
    return true;
  }
  void RequestCloseImpl() override {}
  std::unique_ptr<xe::ui::Surface> CreateSurfaceImpl(
      xe::ui::Surface::TypeFlags allowed_types) override {
#ifdef __ANDROID__
    if (native_window_ &&
        (allowed_types & xe::ui::Surface::kTypeFlag_AndroidNativeWindow)) {
      return std::make_unique<xe::ui::AndroidNativeWindowSurface>(
          native_window_);
    }
#endif
    // On the PC the GPU runs offscreen (frames are captured, not shown).
    return nullptr;
  }
  void RequestPaintImpl() override {
    app_context().CallInUIThreadDeferred([this]() { OnPaint(); });
  }

 private:
  ANativeWindow* native_window_ = nullptr;
};

struct State {
  std::unique_ptr<xe::Memory> memory;
  std::unique_ptr<xe::kernel::KernelState> kernel_state;
  std::unique_ptr<xe::gpu::vulkan::VulkanGraphicsSystem> graphics;
  UiContext* ui_context = nullptr;
  SurfaceWindow* window = nullptr;
};

// Startup progress on stderr, unbuffered (Xenia's log is asynchronous and is
// lost if startup hangs and the process is killed).
void StartupStep(const char* step) {
  fprintf(stderr, "xportgpu: %s\n", step);
  fflush(stderr);
}

State* g_state = nullptr;
std::atomic<bool> g_active{false};
bool g_invalidate_each_frame = false;
std::string g_capture_directory;

// XPORT_XENIA_RESOLVE_LOG=<swap>: every resolve of the frames after that swap
// and the next one is read back (readback_resolve=full only there) and logged
// with simple statistics, to compare one frame between two GPUs.
constexpr uint32_t kResolveLogFrames = 2;
uint32_t g_resolve_log_swap = 0;
// XPORT_XENIA_TRANSFER_LOG=<swap>: only the render target transfer lines of the
// frame after that swap (nothing read back).
uint32_t g_transfer_log_swap = 0;
uint32_t g_swap_index = 0;
uint32_t g_resolve_index = 0;
// XPORT_XENIA_RESOLVE_DUMP=<directory>: the logged resolves are also saved there
// as PNGs (whole destination texture, untiled; depth as gray, floats tone
// mapped), named <swap>_<index>_f<format>.png.
std::string g_resolve_dump_directory;

// A guest 32-bit word of a resolve destination (big-endian in memory, the
// resolve's endian swap is assumed to be the usual 8-in-32).
uint32_t LoadGuestWord(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 |
         uint32_t(p[3]);
}

uint8_t ToneMapToByte(float value) {
  if (!(value > 0.0f)) {
    return 0;
  }
  return uint8_t(255.0f * value / (1.0f + value) + 0.5f);
}

// Saves the destination of the current resolve (registers still hold it).
void DumpResolveImage(uint32_t index, xe::gpu::xenos::ColorFormat format) {
  const xe::gpu::RegisterFile& regs = *g_state->graphics->register_file();
  const uint32_t base = regs.values[xe::gpu::XE_GPU_REG_RB_COPY_DEST_BASE];
  xe::gpu::reg::RB_COPY_DEST_PITCH pitch_reg;
  pitch_reg.value = regs.values[xe::gpu::XE_GPU_REG_RB_COPY_DEST_PITCH];
  const uint32_t width = pitch_reg.copy_dest_pitch;
  const uint32_t height = pitch_reg.copy_dest_height;
  uint32_t bpp_log2;
  switch (format) {
    case xe::gpu::xenos::ColorFormat::k_8_8_8_8:
    case xe::gpu::xenos::ColorFormat::k_32_FLOAT:
    case xe::gpu::xenos::ColorFormat(23):  // k_24_8_FLOAT as a texture format (depth)
      bpp_log2 = 2;
      break;
    case xe::gpu::xenos::ColorFormat::k_16_16_16_16_FLOAT:
    case xe::gpu::xenos::ColorFormat::k_32_32_FLOAT:
      bpp_log2 = 3;
      break;
    default:
      return;
  }
  if (!width || !height || width > 4096 || height > 4096) {
    return;
  }
  const uint32_t pitch_aligned = (width + 31) & ~31u;
  const uint8_t* texture = g_state->memory->TranslatePhysical(base);
  std::vector<uint8_t> rgb(size_t(width) * height * 3);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* p =
          texture + xe::gpu::texture_address::Tiled2D(int32_t(x), int32_t(y),
                                                      pitch_aligned, bpp_log2);
      uint8_t* out = &rgb[(size_t(y) * width + x) * 3];
      switch (format) {
        case xe::gpu::xenos::ColorFormat::k_8_8_8_8: {
          const uint32_t v = LoadGuestWord(p);
          out[0] = uint8_t(v >> 16);
          out[1] = uint8_t(v >> 8);
          out[2] = uint8_t(v);
        } break;
        case xe::gpu::xenos::ColorFormat(23): {
          // 20e4 depth in the upper 24 bits, stencil in the lower 8.
          const uint32_t v = LoadGuestWord(p);
          const float depth = xe::gpu::xenos::Float20e4To32(v >> 8);
          out[0] = out[1] = uint8_t(std::clamp(depth, 0.0f, 1.0f) * 255.0f);
          out[2] = uint8_t(v);  // stencil in blue
        } break;
        case xe::gpu::xenos::ColorFormat::k_16_16_16_16_FLOAT: {
          // Two 32-bit words, each two big-endian halves.
          for (uint32_t c = 0; c < 3; ++c) {
            const uint32_t word = LoadGuestWord(p + (c >> 1) * 4);
            const uint16_t half =
                uint16_t((c & 1) ? (word & 0xFFFF) : (word >> 16));
            out[c] = ToneMapToByte(xe::xenos_half_to_float(half));
          }
        } break;
        default: {
          const uint32_t v = LoadGuestWord(p);
          float value;
          std::memcpy(&value, &v, sizeof(value));
          out[0] = out[1] = out[2] = ToneMapToByte(value);
        } break;
      }
    }
  }
  const std::string path =
      fmt::format("{}/{}_{:02}_f{}.png", g_resolve_dump_directory,
                  g_swap_index, index, uint32_t(format));
  stbi_write_png(path.c_str(), int(width), int(height), 3, rgb.data(),
                 int(width * 3));
}

// GPU thread, for every resolve read back to guest memory.
void OnResolveReadback(uint32_t address, uint32_t length, const uint8_t* data,
                       uint32_t dest_info) {
  if (!g_resolve_log_swap || g_swap_index < g_resolve_log_swap ||
      g_swap_index >= g_resolve_log_swap + kResolveLogFrames) {
    return;
  }
  uint64_t sum = 0;
  uint32_t nonzero = 0;
  for (uint32_t i = 0; i < length; ++i) {
    sum += data[i];
    nonzero += data[i] != 0;
  }
  xe::gpu::reg::RB_COPY_DEST_INFO info;
  info.value = dest_info;
  XELOGI(
      "xportgpu: resolve {} #{} at 0x{:08X} size 0x{:X} format {} number {} "
      "exp_bias {}: mean byte {:.2f}, nonzero {:.1f}%",
      g_swap_index, g_resolve_index++, address, length,
      uint32_t(info.copy_dest_format), uint32_t(info.copy_dest_number),
      int32_t(info.copy_dest_exp_bias), length ? double(sum) / length : 0.0,
      length ? 100.0 * nonzero / length : 0.0);
  if (uint32_t(info.copy_dest_format) == 23) {
    // k_24_8_FLOAT depth: range of the 20e4 depth and of the stencil.
    float depth_min = 2.0f, depth_max = -1.0f;
    uint32_t stencil_min = 255, stencil_max = 0;
    for (uint32_t i = 0; i + 4 <= length; i += 4) {
      const uint32_t v = LoadGuestWord(data + i);
      const float depth = xe::gpu::xenos::Float20e4To32(v >> 8);
      depth_min = std::min(depth_min, depth);
      depth_max = std::max(depth_max, depth);
      stencil_min = std::min(stencil_min, v & 0xFF);
      stencil_max = std::max(stencil_max, v & 0xFF);
    }
    XELOGI("xportgpu: resolve {} #{} depth {:.6f}..{:.6f} stencil {}..{}",
           g_swap_index, g_resolve_index - 1, depth_min, depth_max,
           stencil_min, stencil_max);
  }
  if (!g_resolve_dump_directory.empty()) {
    DumpResolveImage(g_resolve_index - 1, info.copy_dest_format);
  }
}

// GPU thread, at the end of every guest swap (xport_swap_hook).
void OnGuestSwap();
}  // namespace

std::string xportgpu::DebugStatus() {
  return fmt::format("gpu thread stage {} draws {} swaps {}",
                     xe::gpu::vulkan::xport_dbg_stage.load(),
                     xe::gpu::vulkan::xport_dbg_draws.load(), g_swap_index);
}

namespace {
void OnGuestSwap() {
  const uint32_t swap_index = ++g_swap_index;
  if (!g_state) {
    return;
  }
  if (g_transfer_log_swap) {
    xe::gpu::vulkan::xport_log_transfer_lines =
        swap_index == g_transfer_log_swap;
  }
  // Render target ownership transfers per frame, every 300 frames.
  if (swap_index % 300 == 0) {
    uint64_t* const tiles = xe::gpu::vulkan::xport_transfer_tiles;
    XELOGI(
        "xportgpu: EDRAM tiles per frame: {} color transfers, {} depth "
        "transfers, {} as clears; {} resolve clears",
        tiles[0] / 300, tiles[1] / 300, tiles[2] / 300, tiles[3] / 300);
    uint64_t texture_blocks, texture_allocations;
    xe::gpu::vulkan::XportTextureAllocatorStats(texture_blocks,
                                               texture_allocations);
    XELOGI(
        "xportgpu: texture cache {} MB (allocator blocks {} MB, in use {} MB), "
        "{} render targets {} MB",
        xe::gpu::xport_texture_memory >> 20, texture_blocks >> 20,
        texture_allocations >> 20,
        xe::gpu::vulkan::xport_render_target_count,
        xe::gpu::vulkan::xport_render_target_memory >> 20);
    if (std::string report = xe::ui::vulkan::XportDeviceMemoryReport();
        !report.empty()) {
      XELOGI("xportgpu: {}", report);
    }
    std::fill(tiles, tiles + 4, uint64_t(0));
  }
  if (g_resolve_log_swap) {
    uint32_t* const stats = xe::gpu::vulkan::xport_draw_stats;
    if (swap_index > g_resolve_log_swap &&
        swap_index <= g_resolve_log_swap + kResolveLogFrames) {
      std::string line;
      for (uint32_t i = 0; i < 64; ++i) {
        if (stats[i]) {
          line += fmt::format(" r{}={}", i, stats[i]);
        }
      }
      XELOGI("xportgpu: draws in frame {}:{}", swap_index - 1, line);
    }
    std::fill(stats, stats + 64, 0u);
    if (swap_index == g_resolve_log_swap) {
      g_resolve_index = 0;
      xe::gpu::SetReadbackResolveMode("full");
      xe::gpu::vulkan::xport_log_transfers = true;
    } else if (swap_index == g_resolve_log_swap + kResolveLogFrames) {
      xe::gpu::SetReadbackResolveMode("none");
      xe::gpu::vulkan::xport_log_transfers = false;
    }
  }
  if (!g_capture_directory.empty() && swap_index % 60 == 0) {
    xe::ui::Presenter* presenter = g_state->graphics->presenter();
    xe::ui::RawImage image;
    if (presenter && presenter->CaptureGuestOutput(image)) {
      for (size_t y = 0; y < image.height; ++y) {
        for (size_t x = 0; x < image.width; ++x) {
          image.data[y * image.stride + x * 4 + 3] = 0xFF;  // R8G8B8X8
        }
      }
      const std::string path = fmt::format("{}/frame_{:06}.png",
                                           g_capture_directory, swap_index);
      stbi_write_png(path.c_str(), int(image.width), int(image.height), 4,
                     image.data.data(), int(image.stride));
      XELOGI("xportgpu: captured {}", path);
    }
  }
  if (g_invalidate_each_frame) {
    g_state->memory->InvalidateAllPhysicalMemory();
  }
}

// The swap packet's dword count and Xenia's swap packet layout (see Xenia's
// xboxkrnl_video.cc VdSwap).
constexpr uint32_t kSwapPacketDwords = 64;

// Which of the host formats Xenia maps Xenos render targets to this GPU can
// render to (Xenia's Vulkan render target cache assumes the color ones).
void LogFormatSupport(xe::gpu::GraphicsSystem& graphics) {
  auto* provider =
      static_cast<xe::ui::vulkan::VulkanProvider*>(graphics.provider());
  const xe::ui::vulkan::VulkanDevice* device = provider->vulkan_device();
  const auto& ifn = device->vulkan_instance()->functions();
  static constexpr std::pair<VkFormat, const char*> kFormats[] = {
      {VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM (8_8_8_8)"},
      {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10_UNORM (2_10_10_10)"},
      {VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT (2_10_10_10_FLOAT, 16 float)"},
      {VK_FORMAT_R16G16_SNORM, "R16G16_SNORM (16_16)"},
      {VK_FORMAT_R16G16B16A16_SNORM, "R16G16B16A16_SNORM (16_16_16_16)"},
      {VK_FORMAT_R16G16_SFLOAT, "R16G16_SFLOAT (16_16_FLOAT)"},
      {VK_FORMAT_R32_SFLOAT, "R32_SFLOAT (32_FLOAT)"},
      {VK_FORMAT_R32G32_SFLOAT, "R32G32_SFLOAT (32_32_FLOAT)"},
      {VK_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT (depth 24)"},
      {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32_SFLOAT_S8_UINT (depth float)"},
  };
  for (const auto& [format, name] : kFormats) {
    VkFormatProperties properties;
    ifn.vkGetPhysicalDeviceFormatProperties(device->physical_device(), format,
                                            &properties);
    const VkFormatFeatureFlags features = properties.optimalTilingFeatures;
    XELOGI("xportgpu: format {}: color attachment {}, blend {}, depth {}, sampled {}",
           name, (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ? "yes" : "NO",
           (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) ? "yes" : "no",
           (features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? "yes" : "no",
           (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? "yes" : "NO");
  }
}

}  // namespace

namespace xportgpu {

bool Initialize(uint8_t* guestBase, uint8_t* physicalBase, uint32_t titleId,
                InterruptFunction interrupt) {
  if (g_state) {
    return true;
  }
  StartupStep("starting Xenia logging");
  xe::InitializeLogging("xenia");
  StartupStep("logging ready");
  g_interrupt = interrupt;

  // XPORT_XENIA_SYNC=1: create pipelines synchronously (no draw skipped while a
  // pipeline compiles, at the cost of stutter).
  if (const char* sync = getenv("XPORT_XENIA_SYNC"); sync && sync[0] == '1') {
    cvars::async_shader_compilation = false;
    XELOGI("xportgpu: synchronous pipeline creation");
  }
  // XPORT_XENIA_CVARS="name=value,name=value": any Xenia configuration
  // variable, as in its TOML config (strings are quoted here).
  // XPORT_XENIA_CVARS_DEFAULTS, the same, applied first (the app's defaults).
  for (const char* variable :
       {"XPORT_XENIA_CVARS_DEFAULTS", "XPORT_XENIA_CVARS"}) {
    if (const char* list = getenv(variable); list && cvar::ConfigVars) {
      std::string_view rest(list);
      while (!rest.empty()) {
        const size_t comma = rest.find(',');
        const std::string_view item = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view()
                                               : rest.substr(comma + 1);
        const size_t equals = item.find('=');
        if (equals == std::string_view::npos) {
          continue;
        }
        const std::string name(item.substr(0, equals));
        std::string value(item.substr(equals + 1));
        const auto it = cvar::ConfigVars->find(name);
        if (it == cvar::ConfigVars->end()) {
          XELOGE("xportgpu: unknown Xenia option {}", name);
          continue;
        }
        const bool bare = value == "true" || value == "false" ||
                          value.find_first_not_of("-0123456789.") ==
                              std::string::npos;
        if (!bare) {
          value = "\"" + value + "\"";
        }
        try {
          const toml::table table = toml::parse("v = " + value);
          it->second->LoadConfigValue(table.get("v"));
          XELOGI("xportgpu: option {} = {}", name, it->second->config_value());
        } catch (const std::exception& e) {
          XELOGE("xportgpu: bad value for {}: {}", name, e.what());
        }
      }
    }
  }
  // XPORT_XENIA_INVALIDATE=1 (diagnostics): after every frame the GPU's copy of
  // all physical memory is stale, so nothing depends on the write watches.
  if (const char* invalidate = getenv("XPORT_XENIA_INVALIDATE");
      invalidate && invalidate[0] == '1') {
    g_invalidate_each_frame = true;
    XELOGI("xportgpu: physical memory invalidated after every frame");
  }
  // XPORT_XENIA_CAPTURE=<directory>: every 60th frame is saved as a PNG there
  // (the PC build runs offscreen, so this is how its frames are seen).
  if (const char* capture = getenv("XPORT_XENIA_CAPTURE"); capture && *capture) {
    g_capture_directory = capture;
    XELOGI("xportgpu: capturing frames to {}", g_capture_directory);
  }
  xe::gpu::vulkan::xport_swap_hook = OnGuestSwap;
  if (const char* swap = getenv("XPORT_XENIA_TRANSFER_LOG"); swap && *swap) {
    g_transfer_log_swap = uint32_t(strtoul(swap, nullptr, 10));
  }
  if (const char* swap = getenv("XPORT_XENIA_RESOLVE_LOG"); swap && *swap) {
    g_resolve_log_swap = uint32_t(strtoul(swap, nullptr, 10));
    xe::gpu::vulkan::xport_resolve_hook = OnResolveReadback;
    XELOGI("xportgpu: logging the resolves after swap {}", g_resolve_log_swap);
    if (const char* dump = getenv("XPORT_XENIA_RESOLVE_DUMP"); dump && *dump) {
      g_resolve_dump_directory = dump;
    }
  }

  StartupStep("creating memory and kernel state");
  auto* state = new State;
  state->memory = std::make_unique<xe::Memory>(guestBase, physicalBase);
  state->kernel_state =
      std::make_unique<xe::kernel::KernelState>(state->memory.get(), titleId);

  // The UI thread creates the context (which binds it to that thread) and the
  // window, then runs the loop for the rest of the process.
  std::mutex ready_mutex;
  std::condition_variable ready_condition;
  bool ready = false;
  StartupStep("starting the UI thread");
  std::thread([&]() {
    xe::threading::set_name("xport-ui");
    auto* context = new UiContext;
    auto* window = new SurfaceWindow(*context);
    window->Open();
    {
      std::lock_guard<std::mutex> lock(ready_mutex);
      state->ui_context = context;
      state->window = window;
      ready = true;
    }
    ready_condition.notify_one();
    context->Run();
  }).detach();
  {
    std::unique_lock<std::mutex> lock(ready_mutex);
    ready_condition.wait(lock, [&] { return ready; });
  }

  StartupStep("creating Vulkan and the command processor");
  state->graphics = std::make_unique<xe::gpu::vulkan::VulkanGraphicsSystem>();
  const xe::X_STATUS status = state->graphics->Setup(
      state->kernel_state->processor(), state->kernel_state.get(),
      state->ui_context, true);
  if (XFAILED(status)) {
    XELOGE("xportgpu: Vulkan graphics setup failed ({:08X})", status);
    return false;
  }
  StartupStep("attaching the presenter");
  state->ui_context->CallInUIThreadSynchronous([state]() {
    xe::ui::Presenter* presenter = state->graphics->presenter();
    state->window->SetPresenter(presenter);
    // XPORT_PRESENT=fsr (default) | cas | bilinear: how the game's frame is
    // scaled to the screen. FSR 1 (EASU upscaling, then RCAS sharpening) keeps
    // a lower render resolution (XPORT_RES) sharp on the 2450x1220 surface.
    // XPORT_FSR_SHARPNESS: RCAS sharpness reduction in stops (0 is the
    // sharpest, 2 the softest; Xenia's default is 0.2).
    if (presenter) {
      using PaintConfig = xe::ui::Presenter::GuestOutputPaintConfig;
      PaintConfig config = presenter->GetGuestOutputPaintConfigFromUIThread();
      const char* effect = std::getenv("XPORT_PRESENT");
      if (effect && !std::strcmp(effect, "bilinear")) {
        config.SetEffect(PaintConfig::Effect::kBilinear);
      } else if (effect && !std::strcmp(effect, "cas")) {
        config.SetEffect(PaintConfig::Effect::kCas);
      } else {
        config.SetEffect(PaintConfig::Effect::kFsr);
      }
      if (const char* sharpness = std::getenv("XPORT_FSR_SHARPNESS")) {
        config.SetFsrSharpnessReduction(std::strtof(sharpness, nullptr));
      }
      // Against banding in the sky and fog with the 8 bpc output.
      config.SetDither(true);
      presenter->SetGuestOutputPaintConfigFromUIThread(config);
      XELOGI("xportgpu: presentation effect {}",
             config.GetEffect() == PaintConfig::Effect::kFsr
                 ? "FSR"
                 : (config.GetEffect() == PaintConfig::Effect::kCas
                        ? "CAS"
                        : "bilinear"));
    }
  });
  StartupStep("loading the shader storage");
  {
    // Next to the saves (on the phone: the app's files directory).
    const char* save_dir = std::getenv("XPORT_SAVE_DIR");
    std::filesystem::path cache_root =
        save_dir && *save_dir
            ? std::filesystem::path(save_dir).parent_path() / "xenia-cache"
            : std::filesystem::path("xenia-cache");
    const uint64_t start_ms = xe::Clock::QueryHostUptimeMillis();
    state->graphics->InitializeShaderStorage(cache_root, titleId, true, nullptr);
    XELOGI("xportgpu: shader storage in {} ({} ms)", cache_root.string(),
           xe::Clock::QueryHostUptimeMillis() - start_ms);
  }
  StartupStep("ready");

  LogFormatSupport(*state->graphics);

  g_state = state;
  g_active = true;
  XELOGI("xportgpu: Xenia GPU ready (Vulkan)");
  return true;
}

bool IsActive() { return g_active; }

void SetSurface(ANativeWindow* window) {
  if (!g_state) {
    return;
  }
  g_state->ui_context->CallInUIThreadSynchronous(
      [window]() { g_state->window->SetNativeWindow(window); });
}

void InitializeRingBuffer(uint32_t address, uint32_t sizeLog2) {
  g_state->graphics->InitializeRingBuffer(address, sizeLog2);
}

void EnableReadPointerWriteBack(uint32_t address, uint32_t blockLog2) {
  g_state->graphics->EnableReadPointerWriteBack(address, blockLog2);
}

void UpdateWritePointer(uint32_t writeIndex) {
  g_state->graphics->command_processor()->UpdateWritePointer(writeIndex);
}

void SetInterruptCallbackKnown() {}

void MarkVblank() {
  if (!g_active) {
    return;
  }
  g_state->graphics->command_processor()->increment_counter();
  g_state->graphics->DispatchInterruptCallback(0, 2);
}

void WriteSwapPacket(uint32_t bufferAddress, uint32_t fetchAddress) {
  namespace xenos = xe::gpu::xenos;
  xe::Memory& memory = *g_state->memory;

  // The D3D texture header's fetch constant holds a CPU address of the front
  // buffer; the GPU wants the physical one (both guest views map physical
  // memory at offset & 0x1FFFFFFF).
  xenos::xe_gpu_texture_fetch_t fetch;
  xe::copy_and_swap_32_unaligned(
      &fetch, memory.TranslateVirtual<const uint32_t*>(fetchAddress), 6);
  const uint32_t frontbuffer_virtual = fetch.base_address << 12;
  const uint32_t frontbuffer_physical = frontbuffer_virtual & 0x1FFFFFFF;
  fetch.base_address = frontbuffer_physical >> 12;
  const uint32_t width = 1 + fetch.size_2d.width;
  const uint32_t height = 1 + fetch.size_2d.height;

  auto* dwords = memory.TranslateVirtual<xe::be<uint32_t>*>(bufferAddress);
  uint32_t offset = 0;
  dwords[offset++] =
      xenos::MakePacketType0(xe::gpu::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, 6);
  dwords[offset++] = fetch.dword_0;
  dwords[offset++] = fetch.dword_1;
  dwords[offset++] = fetch.dword_2;
  dwords[offset++] = fetch.dword_3;
  dwords[offset++] = fetch.dword_4;
  dwords[offset++] = fetch.dword_5;
  dwords[offset++] = xenos::MakePacketType3(xenos::PM4_XE_SWAP, 4);
  dwords[offset++] = xenos::kSwapSignature;
  dwords[offset++] = frontbuffer_physical;
  dwords[offset++] = width;
  dwords[offset++] = height;
  while (offset < kSwapPacketDwords) {
    dwords[offset++] = xenos::MakePacketType2();
  }
}

bool HandleWriteFault(void* hostAddress) {
  return g_state && g_state->memory->HandleWriteFault(hostAddress);
}

void PrepareHostWrite(uint32_t guestAddress, uint32_t length) {
  if (!g_state) {
    return;
  }
  // Only the guest's physical views are watched.
  if (guestAddress >= 0xA0000000 && guestAddress < 0xE0000000) {
    g_state->memory->TriggerPhysicalMemoryCallbacks(guestAddress & 0x1FFFFFFF,
                                                    length);
  }
}

}  // namespace xportgpu
