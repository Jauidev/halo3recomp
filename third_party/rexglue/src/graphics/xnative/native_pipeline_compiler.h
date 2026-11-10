#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rex::graphics::xnative {

// One compiler, with bounded admission including unconsumed results. Callers
// publish results on their owner thread. Stop joins the active driver calls;
// shader modules, layouts, caches and the device must outlive this object.
// `threads` workers take jobs from the one queue (Halo 3 on the Mali: with one, a pipeline that
// took minutes to compile held every other one behind it).
template <typename Key, typename Result, typename Hash>
class NativePipelineCompiler {
 public:
  using Compile = std::function<Result()>;
  explicit NativePipelineCompiler(size_t capacity, std::function<void()> idle = {},
                                  size_t threads = 1)
      : capacity_(std::max<size_t>(capacity, 1)), idle_(std::move(idle)) {
    for (size_t i = 0; i < std::max<size_t>(threads, 1); ++i) {
      threads_.emplace_back([this] { Run(); });
    }
  }
  ~NativePipelineCompiler() { Stop(); }
  NativePipelineCompiler(const NativePipelineCompiler&) = delete;
  NativePipelineCompiler& operator=(const NativePipelineCompiler&) = delete;

  bool Enqueue(const Key& key, Compile compile, bool demanded = false) {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return false;
    }
    if (jobs_.contains(key)) {
      return true;
    }
    if (jobs_.size() >= capacity_) {
      if (!demanded || queue_.empty()) {
        return false;
      }
      // Drop only an unstarted speculative recipe to admit an exact first use.
      jobs_.erase(queue_.back());
      queue_.pop_back();
    }
    auto job = std::make_shared<Job>();
    job->compile = std::move(compile);
    jobs_.emplace(key, job);
    if (demanded) {
      queue_.push_front(key);
    } else {
      queue_.push_back(key);
    }
    changed_.notify_all();
    return true;
  }

  // Demand promotes an existing speculative job, preserving exact deduplication.
  // No renderer state or Vulkan lifetime is changed while waiting.
  std::optional<Result> Take(const Key& key, bool wait) {
    std::unique_lock lock(mutex_);
    const auto found = jobs_.find(key);
    if (found == jobs_.end()) {
      return std::nullopt;
    }
    const auto job = found->second;
    if (wait && !job->result) {
      const auto queued = std::find(queue_.begin(), queue_.end(), key);
      if (queued != queue_.end() && queued != queue_.begin()) {
        queue_.erase(queued);
        queue_.push_front(key);
      }
      changed_.notify_all();
      changed_.wait(lock, [&] { return job->result.has_value() || stopping_; });
    }
    if (!job->result) {
      return std::nullopt;
    }
    Result result = std::move(*job->result);
    jobs_.erase(key);
    return result;
  }

  // Take(key, true) that gives up after `timeout` (xnative_async_pipelines): the job moves to the
  // front of the queue and, if it is not done by then, stays queued for a later Take/TakeCompleted.
  std::optional<Result> TakeFor(const Key& key, std::chrono::microseconds timeout) {
    std::unique_lock lock(mutex_);
    const auto found = jobs_.find(key);
    if (found == jobs_.end()) {
      return std::nullopt;
    }
    const auto job = found->second;
    if (!job->result) {
      const auto queued = std::find(queue_.begin(), queue_.end(), key);
      if (queued != queue_.end() && queued != queue_.begin()) {
        queue_.erase(queued);
        queue_.push_front(key);
      }
      changed_.notify_all();
      if (timeout.count() > 0) {
        changed_.wait_for(lock, timeout, [&] { return job->result.has_value() || stopping_; });
      }
    }
    if (!job->result) {
      return std::nullopt;
    }
    Result result = std::move(*job->result);
    jobs_.erase(key);
    return result;
  }

  std::vector<std::pair<Key, Result>> TakeCompleted() {
    std::lock_guard lock(mutex_);
    std::vector<std::pair<Key, Result>> completed;
    for (auto it = jobs_.begin(); it != jobs_.end();) {
      if (it->second->result) {
        completed.emplace_back(it->first, std::move(*it->second->result));
        it = jobs_.erase(it);
      } else {
        ++it;
      }
    }
    return completed;
  }

  bool Contains(const Key& key) const {
    std::lock_guard lock(mutex_);
    return jobs_.contains(key);
  }
  void WaitUntilIdle() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return active_ == 0 && queue_.empty(); });
  }
  void Stop() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      queue_.clear();
      changed_.notify_all();
    }
    for (std::thread& thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

 private:
  struct Job {
    Compile compile;
    std::optional<Result> result;
  };
  void Run() {
    std::unique_lock lock(mutex_);
    for (;;) {
      changed_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) {
        return;
      }
      const Key key = queue_.front();
      queue_.pop_front();
      const auto job = jobs_.at(key);
      ++active_;
      lock.unlock();
      Result result{};
      try {
        result = job->compile();
      } catch (...) {
        // Allocation/cache I/O failure is an unsuccessful job, never process
        // termination on a background thread. The owner retains exact fallback.
      }
      lock.lock();
      job->result = std::move(result);
      job->compile = {};
      --active_;
      changed_.notify_all();
      // The idle callback runs once every worker is done and nothing is queued.
      if (!stopping_ && queue_.empty() && active_ == 0 && idle_) {
        ++active_;
        lock.unlock();
        try {
          idle_();
        } catch (...) {
        }
        lock.lock();
        --active_;
        changed_.notify_all();
      }
    }
  }

  const size_t capacity_;
  std::function<void()> idle_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool stopping_ = false;
  size_t active_ = 0;
  std::deque<Key> queue_;
  std::unordered_map<Key, std::shared_ptr<Job>, Hash> jobs_;
  std::vector<std::thread> threads_;
};

}  // namespace rex::graphics::xnative
