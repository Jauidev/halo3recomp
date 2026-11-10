// link probe for the native renderer libraries (target rex_link_probe, not built by
// default): its unresolved symbols are what rexglue_glue still has to provide.
#include "graphics/xnative/graphics_system.h"

int main() {
  auto* system = new rex::graphics::xnative::NativeGraphicsSystem();
  return system != nullptr ? 0 : 1;
}
