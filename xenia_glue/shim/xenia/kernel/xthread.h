// stand-in for Xenia's kernel threads. The GPU only creates host threads (the command
// processor's worker) and asks whether it runs on one; plain std::threads do (xenia_glue/kernel.cc).
#ifndef XENIA_KERNEL_XTHREAD_H_
#define XENIA_KERNEL_XTHREAD_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "xenia/base/threading.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

class KernelState;

// Owning pointer with the interface the GPU uses.
template <typename T>
class object_ref {
 public:
  object_ref() noexcept = default;
  object_ref(std::nullptr_t) noexcept {}
  explicit object_ref(T* value) noexcept : value_(value) {}
  object_ref(object_ref&& other) noexcept : value_(std::move(other.value_)) {}
  object_ref& operator=(object_ref&& other) noexcept {
    value_ = std::move(other.value_);
    return *this;
  }
  object_ref& operator=(std::nullptr_t) noexcept {
    value_.reset();
    return *this;
  }
  T* get() const noexcept { return value_.get(); }
  T* operator->() const noexcept { return value_.get(); }
  T& operator*() const noexcept { return *value_; }
  explicit operator bool() const noexcept { return value_ != nullptr; }
  void reset() noexcept { value_.reset(); }

 private:
  std::unique_ptr<T> value_;
};

class XThread {
 public:
  virtual ~XThread();
  static bool IsInThread(XThread* thread);
  static bool IsInThread();
  void set_name(const std::string& name) { name_ = name; }
  const std::string& name() const { return name_; }
  X_STATUS Wait(uint32_t wait_reason, uint32_t processor_mode,
                uint32_t alertable, uint64_t* opt_timeout);

  // Pause/Resume of the GPU (save states) is not used: Resume does nothing.
  struct HostThread {
    void Resume() {}
  };
  HostThread* thread() { return &host_thread_; }

 protected:
  HostThread host_thread_;
  std::string name_;
  std::thread thread_;
};

class XHostThread : public XThread {
 public:
  XHostThread(KernelState* kernel_state, uint32_t stack_size,
              uint32_t creation_flags, std::function<int()> host_fn,
              uint32_t guest_process = 0);
  X_STATUS Create();

 private:
  std::function<int()> host_fn_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XTHREAD_H_
