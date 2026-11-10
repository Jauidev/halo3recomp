#include "native_resource_reclaimer.h"
#import <Foundation/Foundation.h>

namespace rex::graphics::xnative {
void WithNativeAutoreleasePool(const std::function<void()>& work) {
  @autoreleasepool { work(); }
}
}  // namespace rex::graphics::xnative
