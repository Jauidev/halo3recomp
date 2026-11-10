// Xenia's XMA contexts (xenia/apu/xma_context_new.cc with FFmpeg, or
// xma_context_fake.cc) driven by the runtime instead of Xenia's XmaDecoder, which is built around
// Xenia's kernel and MMIO handlers. Register semantics follow XmaDecoder::WriteRegister: a kick
// enables a context and decodes it at once (Xenia without use_dedicated_xma_thread), a lock
// disables it, a clear resets it.
#include "xport_apu.h"

#include <bit>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "xenia/apu/xma_context_fake.h"
#include "xenia/apu/xma_context_new.h"
#include "xenia/base/logging.h"
#include "xenia/memory.h"

extern "C" {
#include "third_party/FFmpeg/libavutil/log.h"
}

namespace {

// Register indices (xenia/apu/xma_register_table.inc): ten 32-context groups each.
constexpr uint32_t kRegisterKick = 0x650;
constexpr uint32_t kRegisterLock = 0x690;
constexpr uint32_t kRegisterClear = 0x6A0;
constexpr uint32_t kGroupCount = 10;

struct State {
  std::unique_ptr<xe::Memory> memory;
  std::vector<std::unique_ptr<xe::apu::XmaContext>> contexts;
  std::mutex mutex;
};
State* g_state = nullptr;

void FfmpegLog(void*, int level, const char* format, va_list args) {
  if (level > AV_LOG_WARNING) {
    return;
  }
  char text[512];
  std::vsnprintf(text, sizeof(text), format, args);
  XELOGW("xportapu: ffmpeg: {}", text);
}

}  // namespace

namespace xportapu {

bool Initialize(uint8_t* guestBase, uint8_t* physicalBase, uint32_t contextTable,
                uint32_t count) {
  if (g_state) {
    return true;
  }
  const char* mode = std::getenv("XPORT_XMA");
  if (mode && std::strcmp(mode, "off") == 0) {
    // No decoding at all: register writes are ignored (the runtime before the APU existed).
    return false;
  }
  const bool fake = mode && std::strcmp(mode, "fake") == 0;
  av_log_set_callback(FfmpegLog);
  auto* state = new State;
  state->memory = std::make_unique<xe::Memory>(guestBase, physicalBase);
  for (uint32_t i = 0; i < count; ++i) {
    std::unique_ptr<xe::apu::XmaContext> context;
    if (fake) {
      context = std::make_unique<xe::apu::XmaContextFake>();
    } else {
      context = std::make_unique<xe::apu::XmaContextNew>();
    }
    if (context->Setup(i, state->memory.get(),
                       contextTable + i * sizeof(xe::apu::XMA_CONTEXT_DATA))) {
      XELOGE("xportapu: XMA context {} setup failed", i);
      delete state;
      return false;
    }
    state->contexts.push_back(std::move(context));
  }
  g_state = state;
  XELOGI("xportapu: {} XMA contexts ({} decoder)", count, fake ? "fake" : "FFmpeg");
  return true;
}

void AllocateContext(uint32_t index) {
  if (g_state && index < g_state->contexts.size()) {
    g_state->contexts[index]->set_is_allocated(true);
  }
}

void ReleaseContext(uint32_t index) {
  if (g_state && index < g_state->contexts.size() &&
      g_state->contexts[index]->is_allocated()) {
    g_state->contexts[index]->Release();
  }
}

void WriteRegister(uint32_t registerIndex, uint32_t value) {
  if (!g_state) {
    return;
  }
  auto group_of = [registerIndex](uint32_t first) -> int {
    return registerIndex >= first && registerIndex < first + kGroupCount
               ? int(registerIndex - first)
               : -1;
  };
  std::lock_guard<std::mutex> lock(g_state->mutex);
  auto for_each = [value](int group, auto&& action) {
    uint32_t remaining = value;
    while (remaining) {
      uint32_t index = uint32_t(group) * 32 + uint32_t(std::countr_zero(remaining));
      remaining &= remaining - 1;
      if (index < g_state->contexts.size()) {
        action(*g_state->contexts[index]);
      }
    }
  };
  if (int group = group_of(kRegisterKick); group >= 0) {
    for_each(group, [](xe::apu::XmaContext& context) {
      context.Enable();
      context.Work();
    });
  } else if (int group = group_of(kRegisterLock); group >= 0) {
    for_each(group, [](xe::apu::XmaContext& context) { context.Disable(); });
  } else if (int group = group_of(kRegisterClear); group >= 0) {
    for_each(group, [](xe::apu::XmaContext& context) { context.Clear(); });
  }
}

}  // namespace xportapu
