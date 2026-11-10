#pragma once
// the runtime's view of Xenia's GPU (xenia_glue/graphics.cc). Plain types only: the
// runtime builds with another fmt version and its own precompiled header, so no Xenia header may
// reach it. Enabled with XPORT_GPU=xenia (runtime/gpu/command_processor.cpp keeps the fake GPU
// otherwise).
#include <cstdint>
#include <string>

struct ANativeWindow;

namespace xportgpu {

// Runs the guest's graphics interrupt callback on the calling thread (source 0: vblank,
// 1: PM4_INTERRUPT; cpu: the hardware thread it is delivered to).
using InterruptFunction = void (*)(uint32_t source, uint32_t cpu);

// guestBase: the 4 GB guest space; physicalBase: an unprotected view of the 512 MB of physical
// memory. Starts the UI thread, Vulkan and the command processor's thread.
bool Initialize(uint8_t* guestBase, uint8_t* physicalBase, uint32_t titleId,
                InterruptFunction interrupt);
bool IsActive();
// Debugging: what the GPU thread is doing.
std::string DebugStatus();

// The SurfaceView's window (the caller holds a reference until it replaces it), or nullptr when
// the surface is destroyed.
void SetSurface(ANativeWindow* window);

void InitializeRingBuffer(uint32_t address, uint32_t sizeLog2);
void EnableReadPointerWriteBack(uint32_t address, uint32_t blockLog2);
void UpdateWritePointer(uint32_t writeIndex);
void SetInterruptCallbackKnown();

// Vblank: advances the GPU's frame counter and delivers interrupt 0 to cpu 2.
void MarkVblank();

// VdSwap: fills the 64 dwords D3D reserved in the ring with Xenia's swap packet. fetch is the
// guest address of the front buffer's texture fetch constant (6 dwords).
void WriteSwapPacket(uint32_t bufferAddress, uint32_t fetchAddress);

// Physical memory write watches: the fault handler asks first; host code about to write guest
// memory (file reads) announces it, because a syscall writing a protected page fails instead of
// faulting.
bool HandleWriteFault(void* hostAddress);
void PrepareHostWrite(uint32_t guestAddress, uint32_t length);

}  // namespace xportgpu
