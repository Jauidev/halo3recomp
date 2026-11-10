#pragma once
// forced into the recompiled code (CMakeLists.txt puts it before ppc_recomp_shared.h in
// xportppc's precompiled header) so it defines PPC_MM_STORE_U32 before ppc_context.h does.
// XenonRecomp emits PPC_MM_* for the stores followed by eieio, i.e. the hardware register writes.
// The store still reaches guest memory (the GPU registers are read from there); writes to the
// APU block (0x7FEA0000) also go to the XMA decoder (runtime/apu/mmio.cpp).
#include <cstdint>

// value: the y argument of PPC_STORE_U32, i.e. before its byte swap.
void GuestApuStore32(uint32_t address, uint32_t value);

#define PPC_MM_STORE_U32(x, y)                         \
    do                                                 \
    {                                                  \
        const uint32_t mmAddress_ = (x);               \
        const uint32_t mmValue_ = (y);                 \
        PPC_STORE_U32(mmAddress_, mmValue_);           \
        if ((mmAddress_ & 0xFFFF0000) == 0x7FEA0000)   \
            GuestApuStore32(mmAddress_, mmValue_);     \
    } while (0)
