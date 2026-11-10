// rexglue_glue/xd3d/android/android_frame_wait_hook.cpp at the title addresses (plan P12;
// that file has TU8 addresses and is not built). XD3D_QueryFrameSlot(device r3, out r4) writes
// {0, submitted frame, submitted - completed} (+16544/+16552); its only caller, sub_828BEBB8,
// polls it without sleeping while two frames are pending. Sleep briefly when the title is about
// to spin; the game's own loop still decides when to stop waiting.
//
// The 2026-10-27 profile had the spin idle (2 of 11,915 XpRenderThread samples): this keeps a
// core free when the renderer does fall behind, it is not an fps change. Extra frames in flight
// stay 0 by default (each is one frame of input latency).
#include <chrono>
#include <thread>

#include <rex/cvar.h>

#include "xd3d_init.h"


// runtime/os/background.cpp: waits while the app is in the background.
bool WaitWhileBackgroundHold(const char* who);

REXCVAR_DEFINE_BOOL(xport_frame_wait_sleep, true, "Xcompiler/Performance",
                    "Sleep instead of spinning while the title waits for a frame slot");
REXCVAR_DEFINE_UINT32(xport_extra_frames_in_flight, 0, "Xcompiler/Performance",
                      "Frames the title may run ahead beyond its own limit of two pending "
                      "frames (more throughput, one frame of input latency each)")
    .range(0, 2);

// Only when tools/find_d3d.py found it (another XDK's build may not have it).
#ifdef XD3D_QueryFrameSlot
PPC_FUNC(XD3D_QueryFrameSlot) {
  // The render thread asks here for a frame slot: while the app is in the background it waits
  // (os/background.h), and the game's main thread, which waits for it, stops too.
  WaitWhileBackgroundHold("render");
  const uint32_t out = ctx.r4.u32;
  XD3D_IMP(QueryFrameSlot)(ctx, base);
  uint32_t pending = PPC_LOAD_U32(out + 8);
  if (pending >= 0x80000000u) return;
  const uint32_t extra = REXCVAR_GET(xport_extra_frames_in_flight);
  if (extra) {
    pending = pending > extra ? pending - extra : 0;
    PPC_STORE_U32(out + 8, pending);
  }
  if (pending >= 2 && REXCVAR_GET(xport_frame_wait_sleep)) {
    std::this_thread::sleep_for(std::chrono::microseconds(250));
  }
}
#endif
