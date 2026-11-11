// the native renderer in the runtime. Starts LibertyRecomp's NativeGraphicsSystem
// over the runtime's guest memory with what its presenter needs and no ReXGlue application: a UI
// thread of its own (xnative-ui) and a Window over the activity's SurfaceView, the same
// arrangement as xenia_glue/graphics.cc. The ported hooks find the renderer through
// GraphicsSystem() (rexglue_glue.h).
//
// XNATIVE_CAPTURE=<directory>: every 2 s the presented guest frame is saved there as a PNG
// (the PC build runs offscreen, so this is how its frames are seen).
#include "xnative.h"
#include "rexglue_glue.h"

#ifdef __ANDROID__
#include <android/native_window.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/diagnostics/policy.h>
#include <rex/ui/presenter.h>
#ifdef __ANDROID__
#include <rex/ui/surface_android.h>
#endif
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "graphics/xnative/graphics_system.h"

#include <rex/cvar.h>

#include <initializer_list>
#include <string_view>
#include <utility>

namespace game {
const char* Title();            // runtime/game.h
const char* RendererOptions();  // runtime/game.h
}

namespace rex::graphics::xnative {
// graphics_system.cpp (xport): decode DXT on the CPU even when the GPU samples BC.
extern bool xport_force_dxt_decode;
}  // namespace rex::graphics::xnative

// graphics/xnative/graphics_system.cpp: arms one native profile capture (needs the
// native-profiler diagnostics category; xport_profile_native_autostart=false keeps the
// automatic capture from taking the first frames instead).
extern "C" int rex_xnative_profile_start();

#ifdef _WIN32
// pc_window.cpp
void PcWindowThread(std::function<rex::ui::Presenter*()> presenter);
#endif

namespace {

// Gears of War 3 on Android: a paint the presenter asks for later (its frame pacer's next slot,
// Window::RequestPaintAfterNanoseconds) waits until then. Without it SurfaceWindow fell back to
// an immediate paint, so the UI thread retried in a busy loop until the slot (2026-11-03 plaza
// profile: ~17% of a big core, a third of it in clock_gettime). XPORT_TIMED_REPAINT=0/1 overrides.
bool TimedRepaint() {
#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
  static const bool enabled = [] {
    const char* value = std::getenv("XPORT_TIMED_REPAINT");
    if (value && *value) return *value == '1';
#ifdef __ANDROID__
    return true;
#else
    return false;
#endif
  }();
  return enabled;
#else
  return false;
#endif
}

class UiContext final : public rex::ui::WindowedAppContext {
 public:
  // Runs the UI loop on the thread that created the context.
  void Run() {
    while (true) {
      std::function<void()> timed;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!pending_) {
          if (!timed_due_) {
            condition_.wait(lock);
          } else if (std::chrono::steady_clock::now() >= *timed_due_) {
            timed = std::move(timed_function_);
            timed_function_ = nullptr;
            timed_due_.reset();
            break;
          } else {
            condition_.wait_until(lock, *timed_due_);
          }
        }
        pending_ = false;
      }
      if (timed) {
        timed();
      }
      ExecutePendingFunctionsFromUIThread();
    }
  }

  // Runs function on the UI thread at due (the earliest one if asked again before then). Calls
  // in the meantime still run at once.
  void CallInUIThreadAt(std::chrono::steady_clock::time_point due, std::function<void()> function) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!timed_due_ || due < *timed_due_) {
        timed_due_ = due;
      }
      timed_function_ = std::move(function);
    }
    condition_.notify_one();
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
  std::optional<std::chrono::steady_clock::time_point> timed_due_;
  std::function<void()> timed_function_;
};

// A Window over the activity's SurfaceView: fullscreen, no menus, no input (input reaches the
// game through the runtime).
class SurfaceWindow final : public rex::ui::Window {
 public:
  explicit SurfaceWindow(UiContext& ui_context)
      : Window(ui_context, game::Title(), 1280, 720), ui_context_(ui_context) {}
  ~SurfaceWindow() override { EnterDestructor(); }

  // UI thread.
  void SetNativeWindow(ANativeWindow* window) {
    if (window == native_window_) {
      return;
    }
    native_window_ = nullptr;
    OnSurfaceChanged(false);
    native_window_ = window;
#ifdef __ANDROID__
    if (window) {
      WindowDestructionReceiver destruction_receiver(this);
      OnActualSizeUpdate(uint32_t(ANativeWindow_getWidth(window)),
                         uint32_t(ANativeWindow_getHeight(window)), destruction_receiver);
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
  std::unique_ptr<rex::ui::Surface> CreateSurfaceImpl(
      rex::ui::Surface::TypeFlags allowed_types) override {
#ifdef __ANDROID__
    if (native_window_ && (allowed_types & rex::ui::Surface::kTypeFlag_AndroidNativeWindow)) {
      return std::make_unique<rex::ui::AndroidNativeWindowSurface>(native_window_);
    }
#endif
    (void)allowed_types;
    return nullptr;
  }
  void RequestPaintImpl() override {
    app_context().CallInUIThreadDeferred([this]() { OnPaint(); });
  }
  void RequestPaintAfterNanosecondsImpl(uint64_t delay_ns) override {
    if (!TimedRepaint()) {
      Window::RequestPaintAfterNanosecondsImpl(delay_ns);
      return;
    }
    // At most 250 ms, as a guard against a bogus delay (the pacer's are below one frame).
    const auto delay = std::chrono::nanoseconds(std::min<uint64_t>(delay_ns, 250'000'000));
    ui_context_.CallInUIThreadAt(std::chrono::steady_clock::now() + delay, [this]() { OnPaint(); });
  }

 private:
  UiContext& ui_context_;
  ANativeWindow* native_window_ = nullptr;
};

struct State {
  rex::memory::Memory* memory = nullptr;
  std::unique_ptr<rex::graphics::xnative::NativeGraphicsSystem> graphics;
  UiContext* ui_context = nullptr;
  SurfaceWindow* window = nullptr;
};

State* g_state = nullptr;
std::atomic<bool> g_active{false};

void StartupStep(const char* step) {
  fprintf(stderr, "xnative: %s\n", step);
  fflush(stderr);
}

void CaptureThread(std::string directory) {
  // Also each frame XNATIVE_DRAW_TRACE traces, as trace_<frame>.png (a few frames later).
  uint32_t handled_trace = 0;
  auto next_periodic = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  for (uint32_t index = 0;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint32_t trace =
        rex::graphics::xnative::g_trace_capture_frame.load(std::memory_order_acquire);
    char name[64];
    if (trace && trace != handled_trace) {
      handled_trace = trace;
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
      snprintf(name, sizeof(name), "/trace_%05u.png", trace);
    } else if (std::chrono::steady_clock::now() >= next_periodic) {
      next_periodic += std::chrono::seconds(2);
      snprintf(name, sizeof(name), "/native_%05u.png", index++);
    } else {
      continue;
    }
    rex::ui::Presenter* presenter = g_state->graphics->presenter();
    rex::ui::RawImage image;
    if (!presenter || !presenter->CaptureGuestOutput(image)) {
      continue;
    }
    for (size_t y = 0; y < image.height; ++y) {
      for (size_t x = 0; x < image.width; ++x) {
        image.data[y * image.stride + x * 4 + 3] = 0xFF;  // R8G8B8X8
      }
    }
    const std::string path = directory + name;
    stbi_write_png(path.c_str(), int(image.width), int(image.height), 4, image.data.data(),
                   int(image.stride));
    REXLOG_INFO("xnative: captured {}", path);
  }
}

// Test switches next to the saves (files/ on the phone), read every second while the game runs;
// without a file the configured value stays:
// - frame_limit: a number replaces xport_frame_limit (0, 20, 30, 40, 60, 120).
// - pacing_mode: 'A' paces on the display's real refresh (vulkan_pacer_refresh_lock), 'B' on
//   1 s / limit as before 2026-10-26.
// - thermal_ceiling, store_none, batch_barriers, batch_detile, early_tests: 1/A on, 0/B off for
//   xport_thermal_ceiling, xnative_store_op_none,
//   xnative_batch_attachment_barriers, xnative_batched_detile_barrier,
//   xnative_early_fragment_tests (the 2026-10-27 plan's changes, each with its off switch).
// - quality: xport_resolution_quality, dynamic resolution (nativa, alta, equilibrada, rendimiento,
//   bateria); applies in game.
// The first read is before the first frame, so a switch that only takes effect at startup works
// too.
void PresentationSwitchThread(std::string directory) {
  struct Switch {
    const char* file;
    const char* cvar;
    std::string configured, applied;
  };
  Switch switches[] = {{"frame_limit", "xport_frame_limit"},
                       {"pacing_mode", "vulkan_pacer_refresh_lock"},
                       {"thermal_ceiling", "xport_thermal_ceiling"},
                       {"store_none", "xnative_store_op_none"},
                       {"batch_barriers", "xnative_batch_attachment_barriers"},
                       {"batch_detile", "xnative_batched_detile_barrier"},
                       {"early_tests", "xnative_early_fragment_tests"},
                       {"quality", "xport_resolution_quality"}};
  for (Switch& item : switches) {
    item.configured = item.applied = rex::cvar::GetFlagByName(item.cvar);
  }
  for (;;) {
    for (Switch& item : switches) {
      const bool boolean = item.configured == "true" || item.configured == "false";
      std::string wanted = item.configured;
      if (FILE* file = fopen((directory + item.file).c_str(), "r")) {
        char text[16] = {};
        if (fgets(text, sizeof(text), file)) {
          if (boolean && (text[0] == '0' || text[0] == '1')) {
            wanted = text[0] == '1' ? "true" : "false";
          } else if ((text[0] >= '0' && text[0] <= '9') || (text[0] >= 'a' && text[0] <= 'z')) {
            wanted = text;
            wanted.erase(wanted.find_last_not_of(" \r\n") + 1);
          } else if (text[0] == 'A' || text[0] == 'B') {
            wanted = text[0] == 'A' ? "true" : "false";
          }
        }
        fclose(file);
      }
      if (wanted == item.applied) continue;
      const bool ok = rex::cvar::SetFlagByName(item.cvar, wanted);
      REXLOG_INFO("xnative: {} {} -> {} ({})", item.file, item.applied, wanted,
                  ok ? "set" : "rejected");
      item.applied = wanted;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace

namespace xnative {

rex::memory::Memory* GuestMemory() { return g_state ? g_state->memory : nullptr; }

rex::system::IGraphicsSystem* GraphicsSystem() {
  return g_active.load(std::memory_order_acquire) ? g_state->graphics.get() : nullptr;
}

rex::ui::Window* DisplayWindow() { return g_state ? g_state->window : nullptr; }

bool IsActive() { return g_active.load(std::memory_order_relaxed); }

bool Initialize(uint8_t* guest_base, uint8_t* physical_base) {
  if (g_state) {
    return g_active;
  }
  // The renderer's log (REXLOG_*): XNATIVE_LOG=<file>; by default xnative.log on the PC,
  // logcat only on Android.
  // ReXGlue logs only when its diagnostics policy enables the logging category;
  // XNATIVE_DIAG=<comma list> (rex::diagnostics categories, e.g. logging,native-trace) or all.
  {
    const char* diag = getenv("XNATIVE_DIAG");
    rex::diagnostics::Configure(true, diag && *diag ? diag : "logging");
  }
  rex::InitLoggingEarly();
  const char* log_file = getenv("XNATIVE_LOG");
#ifndef __ANDROID__
  if (!log_file || !*log_file) {
    log_file = "xnative.log";
  }
#endif
  rex::InitLogging(log_file && *log_file ? log_file : nullptr, spdlog::level::info);

  // XNATIVE_ANDROID_PATH=1 (PC): the settings Android builds use (REX_PLATFORM_ANDROID
  // defaults) plus CPU DXT decoding, to reproduce phone-only code paths on the PC.
  // Then the title's RendererOptions() and XNATIVE_CVARS="name=value,...": any ReXGlue /
  // renderer option, the environment's last so they win.
  if (const char* android_path = getenv("XNATIVE_ANDROID_PATH");
      android_path && android_path[0] == '1') {
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"xnative_anti_aliasing", "fxaa"},
             {"xnative_early_fragment_tests", "false"}}) {
      rex::cvar::SetFlagByName(name, value);
    }
    rex::graphics::xnative::xport_force_dxt_decode = true;
  }
  const auto apply_options = [](std::string_view rest) {
    while (!rest.empty()) {
      const size_t comma = rest.find(',');
      const std::string_view item = rest.substr(0, comma);
      rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
      const size_t equals = item.find('=');
      if (equals == std::string_view::npos) {
        continue;
      }
      const bool ok = rex::cvar::SetFlagByName(item.substr(0, equals), item.substr(equals + 1));
      fprintf(stderr, "xnative: option %.*s %s\n", int(item.size()), item.data(),
              ok ? "set" : "UNKNOWN");
    }
  };
  apply_options(game::RendererOptions());
  if (const char* list = getenv("XNATIVE_CVARS"); list && *list) {
    apply_options(list);
  }
  // The renderer options of the launcher's patches (MainActivity's Patches.java,
  // config/opciones.txt "cvar name=value"), after the app's own.
  if (const char* list = getenv("XPORT_PATCH_CVARS"); list && *list) {
    fprintf(stderr, "xnative: patch options %s\n", list);
    apply_options(list);
  }
  // XPORT_PROFILE_AT=<seconds>: start the native profile capture that many seconds after
  // launch (in the level, not the menu). Use with XNATIVE_DIAG=logging,native-profiler.
  if (const char* at = getenv("XPORT_PROFILE_AT"); at && *at) {
    const int seconds = atoi(at);
    std::thread([seconds]() {
      std::this_thread::sleep_for(std::chrono::seconds(seconds));
      rex_xnative_profile_start();
      fprintf(stderr, "xnative: profile capture requested at %d s\n", seconds);
    }).detach();
  }

  auto* state = new State;
  state->memory = CreateMemory(guest_base, physical_base);

  // The UI thread creates the context (which binds it to that thread) and the window, then runs
  // the loop for the rest of the process.
  std::mutex ready_mutex;
  std::condition_variable ready_condition;
  bool ready = false;
  StartupStep("starting the UI thread");
  std::thread([&]() {
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
  g_state = state;

  StartupStep("creating Vulkan and the native renderer");
  state->graphics = std::make_unique<rex::graphics::xnative::NativeGraphicsSystem>();
  if (XFAILED(state->graphics->SetupPresentation(state->ui_context))) {
    StartupStep("presentation setup failed");
    return false;
  }
  // Persistent pipeline cache next to the saves (on the phone: the app's files directory), so
  // pipelines are not compiled again on every launch.
  {
    const char* save_dir = getenv("XPORT_SAVE_DIR");
    const std::filesystem::path cache_root =
        save_dir && *save_dir ? std::filesystem::path(save_dir).parent_path() / "native-cache"
                              : std::filesystem::path("native-cache");
    std::error_code error;
    std::filesystem::create_directories(cache_root, error);
    state->graphics->InitializeShaderStorage(cache_root, rex::graphics::xnative::kTitleId,
                                             true);
  }
  if (XFAILED(state->graphics->SetupGuestGpuWithMemory(state->memory))) {
    StartupStep("guest GPU setup failed");
    return false;
  }
  state->ui_context->CallInUIThreadSynchronous(
      [state]() { state->window->SetPresenter(state->graphics->presenter()); });

  if (const char* capture = getenv("XNATIVE_CAPTURE"); capture && *capture) {
    std::thread(CaptureThread, std::string(capture)).detach();
  }
#ifdef _WIN32
  if (const char* window = getenv("XPORT_PC_WINDOW"); window && window[0] == '1') {
    std::thread(PcWindowThread, [] { return g_state->graphics->presenter(); }).detach();
  }
#endif
  if (const char* save_dir = getenv("XPORT_SAVE_DIR"); save_dir && *save_dir) {
    std::thread(PresentationSwitchThread, std::string(save_dir) + "/../").detach();
  }
  g_active.store(true, std::memory_order_release);
  StartupStep("ready");
  return true;
}

void SetSurface(ANativeWindow* window) {
  if (!g_state) {
    return;
  }
  g_state->ui_context->CallInUIThreadSynchronous(
      [window]() { g_state->window->SetNativeWindow(window); });
}

}  // namespace xnative
