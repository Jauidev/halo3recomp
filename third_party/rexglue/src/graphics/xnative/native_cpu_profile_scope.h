#pragma once
#include <rex/chrono/clock.h>
#include <atomic>
#include <type_traits>
#include <utility>
#include "native_cpu_profile.h"

namespace rex::graphics::xnative::profile {
inline thread_local CpuRecorder* current_cpu_recorder = nullptr;
// how many threads have a recorder bound. libgame.so is dlopen'ed, so each read of
// current_cpu_recorder goes through the linker's dynamic TLS resolver; the scopes below run several
// times per draw and that resolver alone was 1% of the recorder's CPU (2026-10-27 profile) with no
// capture running. A plain global read skips it until a capture binds a recorder.
inline std::atomic<uint32_t> bound_cpu_recorders{0};
inline CpuRecorder* CurrentCpuRecorder() {
  return bound_cpu_recorders.load(std::memory_order_relaxed) ? current_cpu_recorder : nullptr;
}
inline uint64_t CpuTick() {
  return rex::chrono::Clock::QueryHostTickCount();
}
class CpuScope {
 public:
  explicit CpuScope(CpuOp op) : owner_(CurrentCpuRecorder()) {
    if (owner_ && owner_->active())
      token_ = owner_->Enter(op, CpuTick());
  }
  ~CpuScope() { End(); }
  void End() {
    if (token_) {
      owner_->Leave(token_, CpuTick());
      token_ = {};
    }
  }
  CpuScope(const CpuScope&) = delete;
  CpuScope& operator=(const CpuScope&) = delete;

 private:
  CpuRecorder* owner_ = nullptr;
  CpuRecorder::Token token_{};
};
class CpuPhaseScope {
 public:
  explicit CpuPhaseScope(CpuPhase phase) : owner_(CurrentCpuRecorder()) {
    if (owner_)
      prior_ = owner_->SetPhase(phase);
  }
  ~CpuPhaseScope() {
    if (owner_)
      owner_->SetPhase(prior_);
  }
  void Set(CpuPhase phase) {
    if (owner_)
      owner_->SetPhase(phase);
  }

 private:
  CpuRecorder* owner_ = nullptr;
  CpuPhase prior_ = CpuPhase::kPublish;
};
class CpuContextScope {
 public:
  explicit CpuContextScope(CpuContext context) : owner_(CurrentCpuRecorder()) {
    if (owner_)
      prior_ = owner_->SetContext(context);
  }
  // Builds the context only while a capture is running: per-command contexts
  // otherwise chase pipeline and shader pointers for nothing.
  template <typename MakeContext,
            typename = std::enable_if_t<std::is_invocable_r_v<CpuContext, MakeContext&>>>
  explicit CpuContextScope(MakeContext&& make)
      : owner_(CurrentCpuRecorder()) {
    if (owner_ && !owner_->active())
      owner_ = nullptr;
    if (owner_)
      prior_ = owner_->SetContext(make());
  }
  ~CpuContextScope() {
    if (owner_)
      owner_->SetContext(prior_);
  }

 private:
  CpuRecorder* owner_ = nullptr;
  CpuContext prior_{};
};
class CpuRecorderBinding {
 public:
  explicit CpuRecorderBinding(CpuRecorder* recorder) : bound_(recorder != nullptr) {
    if (!bound_)
      return;
    bound_cpu_recorders.fetch_add(1, std::memory_order_relaxed);
    prior_ = current_cpu_recorder;
    current_cpu_recorder = recorder;
  }
  ~CpuRecorderBinding() {
    if (!bound_)
      return;
    current_cpu_recorder = prior_;
    bound_cpu_recorders.fetch_sub(1, std::memory_order_relaxed);
  }

 private:
  bool bound_;
  CpuRecorder* prior_ = nullptr;
};
template <typename Fn>
decltype(auto) CpuCall(CpuOp op, Fn&& fn) {
  CpuScope scope(op);
  return std::forward<Fn>(fn)();
}
inline uint64_t CalibrateCpuClock() {
  uint64_t best = UINT64_MAX;
  for (unsigned i = 0; i < 256; ++i) {
    auto a = CpuTick();
    auto b = CpuTick();
    if (b >= a)
      best = std::min(best, b - a);
  }
  return best == UINT64_MAX ? 0 : best;
}
}  // namespace rex::graphics::xnative::profile
