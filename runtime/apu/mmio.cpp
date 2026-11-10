// the game's writes to the APU registers (0x7FEA0000, see apu/guest_mmio.h). The XMA
// library kicks, locks and clears contexts with stwbrx of a one-bit mask per 32-context group
// (sub_82193A98, sub_82193CC0, sub_82193F58); the registers are little-endian, so the value the
// hardware sees is the byte swap of what PPC_STORE_U32 receives.
#include <stdafx.h>
#include <apu/guest_mmio.h>
#include <xport_apu.h>

void GuestApuStore32(uint32_t address, uint32_t value)
{
    xportapu::WriteRegister((address & 0xFFFF) / 4, ByteSwap(value));
}
