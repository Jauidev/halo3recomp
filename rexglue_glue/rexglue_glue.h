#pragma once

// internal interface of rexglue_glue/ (the native renderer's adapters to this runtime),
// also force-included into the ported hooks, whose ReXGlue runtime lookups
// (tools/port_native_hooks.py REWRITES) become these calls.
#include <cstdint>

namespace rex::memory {
class Memory;
}
namespace rex::system {
class IGraphicsSystem;
}
namespace rex::ui {
class Window;
}

namespace xnative {

// A rex::memory::Memory that translates over the runtime's guest memory (memory.cpp).
rex::memory::Memory* CreateMemory(uint8_t* guest_base, uint8_t* physical_base);

// Null until Initialize succeeded (graphics.cpp).
rex::memory::Memory* GuestMemory();
rex::system::IGraphicsSystem* GraphicsSystem();
rex::ui::Window* DisplayWindow();

}  // namespace xnative
