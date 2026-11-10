// host threads for Xenia's GPU (see shim/xenia/kernel/xthread.h).
#include "xenia/kernel/xthread.h"

#include "xenia/base/threading.h"

namespace xe {
namespace kernel {

namespace {
thread_local XThread* g_current_thread = nullptr;
}

XThread::~XThread() {
  if (thread_.joinable()) {
    thread_.join();
  }
}

bool XThread::IsInThread(XThread* thread) { return g_current_thread == thread; }

bool XThread::IsInThread() { return g_current_thread != nullptr; }

X_STATUS XThread::Wait(uint32_t, uint32_t, uint32_t, uint64_t*) {
  if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
    thread_.join();
  }
  return X_STATUS_SUCCESS;
}

XHostThread::XHostThread(KernelState*, uint32_t, uint32_t,
                         std::function<int()> host_fn, uint32_t)
    : host_fn_(std::move(host_fn)) {}

X_STATUS XHostThread::Create() {
  thread_ = std::thread([this] {
    g_current_thread = this;
    xe::threading::set_name(name_);
    host_fn_();
    g_current_thread = nullptr;
  });
  return X_STATUS_SUCCESS;
}

}  // namespace kernel
}  // namespace xe
