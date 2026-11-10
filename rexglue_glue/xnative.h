#pragma once

// the native renderer (LibertyRecomp's xnative, rexglue_glue/ + third_party/rexglue)
// as seen by the runtime. XPORT_GPU=native: the fake GPU keeps consuming the ring buffer (fences,
// interrupts, vblank) and the game's D3D calls, caught by the ported hooks (rexglue_glue/xd3d),
// are drawn by this renderer instead.
#include <cstdint>

struct ANativeWindow;

namespace xnative {

// Creates the renderer over the runtime's guest memory. False if Vulkan could not start.
bool Initialize(uint8_t* guest_base, uint8_t* physical_base);
bool IsActive();
// The activity's surface (Android); null when it goes away.
void SetSurface(ANativeWindow* window);

}  // namespace xnative

