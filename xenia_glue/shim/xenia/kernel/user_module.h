// stand-in for Xenia's UserModule (the GPU only reads the title id, for trace names).
#ifndef XENIA_KERNEL_USER_MODULE_H_
#define XENIA_KERNEL_USER_MODULE_H_

#include <cstdint>

namespace xe {
namespace kernel {

class UserModule {
 public:
  explicit UserModule(uint32_t title_id) : title_id_(title_id) {}
  uint32_t title_id() const { return title_id_; }

 private:
  uint32_t title_id_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_USER_MODULE_H_
