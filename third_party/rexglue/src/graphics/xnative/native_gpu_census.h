#pragma once
// Xcompiler (Halo 3): the GPU census, an X-ray of what each part of the frame costs the GPU,
// measured on the phone by leaving it out ("census <n> [frames per window]" in xnative_skip.txt;
// tools/halo3-radiografia.ps1 asks for one and turns the result into a report).
//
// The frame after the request is the reference: its draws are grouped by pixel shader (by vertex
// shader for draws without one) and by pass (guest color target 0 + depth target). Then, window
// after window of frames_per_window frames, the recorder leaves out the draws of one group, with a
// window drawing everything after every two groups. NativeFrameGpuTimer gives each frame's GPU
// time; the first kDiscardFrames frames of a window are not counted. A group's cost = the GPU time
// of the full windows around it (interpolated, so a slow drift such as heat cancels out) minus the
// GPU time of its own window. The last group is every draw: what stays is the frame's work outside
// draws (resolves, copies, clears, the passes' loads and stores). Leaving an opaque draw out can
// make what is behind it cost more (less early depth rejection), and the GPU clock may drop while
// a big group is out, so small or negative costs are noise or that. The player must keep still:
// the frame has to stay the same for the whole census. The results go to xnative_census.txt in
// the app's files folder (written through a .tmp file), ending "end <n>".
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rex::graphics::xnative {

class NativeGpuCensus {
 public:
  static constexpr uint32_t kSlots = 2;
  static constexpr uint32_t kDiscardFrames = 2;

  bool active() const { return phase_ != Phase::kIdle; }

  // A "census <n> [frames]" line of the live file; a new n starts a new census (dropping one that
  // is running). folder: where xnative_census.txt goes.
  void Request(uint64_t request, uint32_t frames_per_window, const char* folder) {
    if (!request || request == request_ || !folder || !*folder) return;
    *this = NativeGpuCensus{};
    request_ = request;
    folder_ = folder;
    frames_per_window_ =
        std::clamp<uint32_t>(frames_per_window ? frames_per_window : 8, kDiscardFrames + 2, 64);
    phase_ = Phase::kWaitFrame;
    started_ = std::chrono::steady_clock::now();
    Log("xnative-census: request %llu, %u frames per window; the next whole frame is the reference",
        (unsigned long long)request_, frames_per_window_);
  }

  // A frame is about to be recorded in `slot` (after OnCompleted handed back the slot's previous
  // frame): decides what it leaves out.
  void BeginRecording(uint32_t slot) {
    if (slot >= kSlots) return;
    pending_[slot] = {};
    skip_ = -1;
    switch (phase_) {
      case Phase::kIdle:
        return;
      case Phase::kWaitFrame:  // the request arrived during a frame: count the next whole one
        phase_ = Phase::kCollect;
        return;
      case Phase::kCollect:  // the reference frame is counted: plan the windows
        Plan();
        phase_ = Phase::kRun;
        [[fallthrough]];
      case Phase::kRun: {
        const uint32_t window = run_frame_ / frames_per_window_;
        if (window >= schedule_.size()) {
          phase_ = Phase::kDrain;
          drain_frames_ = 0;
          return;
        }
        const uint32_t frame = run_frame_ % frames_per_window_;
        if (frame == 0 && window && window % 40 == 0) {
          Log("xnative-census: window %u of %zu", window, schedule_.size());
        }
        skip_ = schedule_[window];
        pending_[slot] = {true, window, frame};
        ++run_frame_;
        return;
      }
      case Phase::kDrain:
        // A few more frames, so every counted frame's GPU time has come back.
        if (++drain_frames_ > kSlots + 1) {
          Write();
          phase_ = Phase::kIdle;
        }
        return;
    }
  }

  // Per draw of the frame being recorded (vertex/pixel shader hashes, 0 = none; guest color
  // target 0 and depth target): true leaves the draw out. Counts the reference frame.
  bool ShouldSkip(uint64_t vertex_shader, uint64_t pixel_shader, uint32_t color0, uint32_t depth) {
    const uint64_t pass = uint64_t(color0) << 32 | depth;
    if (phase_ == Phase::kCollect) {
      const uint64_t shader = pixel_shader ? pixel_shader : vertex_shader;
      Group& group = pixel_shader ? pixel_[pixel_shader] : vertex_[vertex_shader];
      if (!group.draws) group.first_pass = pass;
      ++group.draws;
      Group& pass_group = passes_[pass];
      if (!pass_group.draws) pass_group.first_pass = pass;
      ++pass_group.draws;
      if (pass_shaders_[pass].insert(shader).second) ++pass_group.shaders;
      ++reference_draws_;
      return false;
    }
    if (phase_ != Phase::kRun || skip_ < 0 || size_t(skip_) >= units_.size()) return false;
    const Unit& unit = units_[size_t(skip_)];
    switch (unit.kind) {
      case Kind::kPixel:
        return pixel_shader == unit.key;
      case Kind::kVertex:
        return !pixel_shader && vertex_shader == unit.key;
      case Kind::kPass:
        return pass == unit.key;
      case Kind::kAll:
        return true;
    }
    return false;
  }

  // The slot's frame went to the GPU (vkQueueSubmit succeeded).
  void Submitted(uint32_t slot) {
    if (slot >= kSlots) return;
    submitted_[slot] = pending_[slot];
    pending_[slot] = {};
  }

  // The GPU time of the slot's last submitted frame (NativeFrameGpuTimer::BeginFrame).
  void OnCompleted(uint32_t slot, uint64_t gpu_ns) {
    if (slot >= kSlots) return;
    const Tag tag = submitted_[slot];
    submitted_[slot] = {};
    if (!tag.valid || tag.frame < kDiscardFrames || tag.window >= sums_.size()) return;
    sums_[tag.window] += double(gpu_ns);
    ++counts_[tag.window];
  }

  // A line for the log, once.
  bool TakeLog(std::string& line) {
    if (log_.empty()) return false;
    line.swap(log_);
    log_.clear();
    return true;
  }

 private:
  enum class Phase : uint8_t { kIdle, kWaitFrame, kCollect, kRun, kDrain };
  enum class Kind : uint8_t { kPixel, kVertex, kPass, kAll };
  struct Group {
    uint32_t draws = 0;
    uint32_t shaders = 0;     // distinct shaders (passes)
    uint64_t first_pass = 0;  // pass of the first draw (shaders)
  };
  struct Unit {
    Kind kind = Kind::kAll;
    uint64_t key = 0;  // shader hash, or pass (color target 0 << 32 | depth target)
    Group group;
  };
  struct Tag {
    bool valid = false;
    uint32_t window = 0;
    uint32_t frame = 0;
  };

  template <typename... Args>
  void Log(const char* format, Args... args) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), format, args...);
    log_ = buffer;
  }

  void Plan() {
    units_.clear();
    const auto add = [&](Kind kind, const std::unordered_map<uint64_t, Group>& groups) {
      std::vector<Unit> list;
      list.reserve(groups.size());
      for (const auto& [key, group] : groups) list.push_back({kind, key, group});
      std::sort(list.begin(), list.end(), [](const Unit& a, const Unit& b) {
        return a.group.draws != b.group.draws ? a.group.draws > b.group.draws : a.key < b.key;
      });
      units_.insert(units_.end(), list.begin(), list.end());
    };
    add(Kind::kPixel, pixel_);
    add(Kind::kVertex, vertex_);
    add(Kind::kPass, passes_);
    Unit all;
    all.kind = Kind::kAll;
    all.group.draws = reference_draws_;
    all.group.shaders = uint32_t(pixel_.size() + vertex_.size());
    units_.push_back(all);
    // full, unit, unit, full, unit, unit, full ... full
    schedule_.assign(1, -1);
    for (size_t i = 0; i < units_.size(); ++i) {
      schedule_.push_back(int32_t(i));
      if (i % 2 == 1 || i + 1 == units_.size()) schedule_.push_back(-1);
    }
    sums_.assign(schedule_.size(), 0.0);
    counts_.assign(schedule_.size(), 0);
    Log("xnative-census: reference frame %u draws, %zu pixel shaders, %zu vertex-only shaders, %zu "
        "passes; %zu windows of %u frames",
        reference_draws_, pixel_.size(), vertex_.size(), passes_.size(), schedule_.size(),
        frames_per_window_);
  }

  void Write() {
    const size_t windows = schedule_.size();
    std::vector<double> mean(windows, -1.0);
    for (size_t w = 0; w < windows; ++w) {
      if (counts_[w]) mean[w] = sums_[w] / double(counts_[w]) / 1e6;
    }
    std::vector<size_t> full;
    double full_sum = 0.0, full_min = 1e30, full_max = 0.0;
    for (size_t w = 0; w < windows; ++w) {
      if (schedule_[w] < 0 && mean[w] >= 0.0) {
        full.push_back(w);
        full_sum += mean[w];
        full_min = std::min(full_min, mean[w]);
        full_max = std::max(full_max, mean[w]);
      }
    }
    const double full_mean = full.empty() ? -1.0 : full_sum / double(full.size());
    double full_variance = 0.0;
    for (const size_t w : full) full_variance += (mean[w] - full_mean) * (mean[w] - full_mean);
    const double full_sd = full.size() > 1 ? std::sqrt(full_variance / double(full.size() - 1)) : 0;
    // The full windows around window w, interpolated by position.
    const auto full_at = [&](size_t w) -> double {
      size_t before = SIZE_MAX, after = SIZE_MAX;
      for (const size_t f : full) {
        if (f < w) before = f;
        if (f > w && after == SIZE_MAX) after = f;
      }
      if (before == SIZE_MAX && after == SIZE_MAX) return -1.0;
      if (before == SIZE_MAX) return mean[after];
      if (after == SIZE_MAX) return mean[before];
      const double t = double(w - before) / double(after - before);
      return mean[before] + (mean[after] - mean[before]) * t;
    };
    const std::filesystem::path folder(folder_);
    const std::filesystem::path temporary = folder / "xnative_census.txt.tmp";
    FILE* file = std::fopen(temporary.string().c_str(), "w");
    if (!file) {
      Log("xnative-census: cannot write %s", temporary.string().c_str());
      return;
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    size_t empty = 0;
    for (size_t w = 0; w < windows; ++w) empty += counts_[w] ? 0 : 1;
    std::fprintf(file,
                 "census %llu frames_per_window=%u counted=%u windows=%zu empty_windows=%zu "
                 "seconds=%.1f\n",
                 (unsigned long long)request_, frames_per_window_,
                 frames_per_window_ - kDiscardFrames, windows, empty, seconds);
    std::fprintf(file, "reference draws=%u pixel_shaders=%zu vertex_only=%zu passes=%zu\n",
                 reference_draws_, pixel_.size(), vertex_.size(), passes_.size());
    std::fprintf(file, "full ms=%.3f min=%.3f max=%.3f sd=%.3f windows=%zu\n", full_mean,
                 full.empty() ? -1.0 : full_min, full.empty() ? -1.0 : full_max, full_sd,
                 full.size());
    for (size_t w = 0; w < windows; ++w) {
      if (schedule_[w] < 0) continue;
      const Unit& unit = units_[size_t(schedule_[w])];
      const double base = full_at(w);
      const double saved = mean[w] >= 0.0 && base >= 0.0 ? base - mean[w] : 0.0;
      const char* kind = unit.kind == Kind::kPixel    ? "ps"
                         : unit.kind == Kind::kVertex ? "vs"
                         : unit.kind == Kind::kPass   ? "pass"
                                                      : "all";
      const uint64_t pass = unit.kind == Kind::kPass ? unit.key : unit.group.first_pass;
      std::fprintf(file,
                   "unit %s %016llX draws=%u shaders=%u pass=%08X:%08X ms=%.3f base=%.3f "
                   "saved=%.3f frames=%u\n",
                   kind, (unsigned long long)(unit.kind == Kind::kPass ? 0 : unit.key),
                   unit.group.draws, unit.group.shaders, uint32_t(pass >> 32), uint32_t(pass),
                   mean[w], base, saved, counts_[w]);
    }
    std::fprintf(file, "end %llu\n", (unsigned long long)request_);
    std::fclose(file);
    std::error_code error;
    std::filesystem::rename(temporary, folder / "xnative_census.txt", error);
    Log("xnative-census: done in %.0f s, full frame %.2f ms (%.2f-%.2f), %zu windows without "
        "GPU times%s",
        seconds, full_mean, full.empty() ? -1.0 : full_min, full.empty() ? -1.0 : full_max, empty,
        error ? ", RENAME FAILED" : "");
  }

  Phase phase_ = Phase::kIdle;
  uint64_t request_ = 0;
  std::string folder_;
  uint32_t frames_per_window_ = 8;
  std::chrono::steady_clock::time_point started_{};
  // The reference frame.
  std::unordered_map<uint64_t, Group> pixel_, vertex_, passes_;
  std::unordered_map<uint64_t, std::unordered_set<uint64_t>> pass_shaders_;
  uint32_t reference_draws_ = 0;
  // The run.
  std::vector<Unit> units_;
  std::vector<int32_t> schedule_;  // per window: the unit left out, -1 = everything drawn
  std::vector<double> sums_;       // per window: GPU ns of its counted frames
  std::vector<uint32_t> counts_;
  uint32_t run_frame_ = 0;
  uint32_t drain_frames_ = 0;
  int32_t skip_ = -1;
  Tag pending_[kSlots]{};
  Tag submitted_[kSlots]{};
  std::string log_;
};

}  // namespace rex::graphics::xnative
