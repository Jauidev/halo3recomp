// the XMA decoder's context table (Xenia's XmaDecoder, BSD). The audio hardware owns
// 320 contexts of 64 bytes in physical memory; the game reads the table's physical address from
// the APU register at 0x7FEA1800 (little-endian, lwbrx) and turns each context it gets from
// XMACreateContext into an index with (physical address - table) / 64. Without the table the
// index was garbage and a voice object stayed null (crash in the audio thread).
// Decoding: each entry is also a Xenia XMA context (xenia_glue/apu.cc), driven by the game's
// register writes (apu/mmio.cpp).
#include <stdafx.h>
#include <xport_apu.h>
#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>

namespace
{
    constexpr uint32_t CONTEXT_COUNT = 320;
    constexpr uint32_t CONTEXT_SIZE = 64;
    constexpr uint32_t REG_CONTEXT_ARRAY = 0x7FEA1800;

    std::mutex g_mutex;
    uint8_t* g_contexts;
    bool g_used[CONTEXT_COUNT];

    void Initialize()
    {
        if (g_contexts != nullptr)
            return;

        g_contexts = static_cast<uint8_t*>(g_userHeap.AllocPhysical(CONTEXT_COUNT * CONTEXT_SIZE, 256));
        memset(g_contexts, 0, CONTEXT_COUNT * CONTEXT_SIZE);

        // Physical address, stored little-endian as the register reads it.
        const uint32_t physical = g_memory.MapVirtual(g_contexts) & 0x1FFFFFFF;
        *reinterpret_cast<uint32_t*>(g_memory.base + REG_CONTEXT_ARRAY) = physical;

        if (!xportapu::Initialize(g_memory.base, g_memory.physical, g_memory.MapVirtual(g_contexts),
                CONTEXT_COUNT))
            LOGN_ERROR("XMA decoder unavailable, audio stays silent");
    }
}

void XmaInitialize()
{
    std::lock_guard guard{ g_mutex };
    Initialize();
}

uint32_t XMACreateContext(be<uint32_t>* context)
{
    std::lock_guard guard{ g_mutex };
    Initialize();

    for (uint32_t i = 0; i < CONTEXT_COUNT; i++)
    {
        if (!g_used[i])
        {
            g_used[i] = true;
            uint8_t* data = g_contexts + i * CONTEXT_SIZE;
            memset(data, 0, CONTEXT_SIZE);
            xportapu::AllocateContext(i);
            *context = g_memory.MapVirtual(data);
            return 0;
        }
    }

    *context = 0;
    return 0xC0000017; // STATUS_NO_MEMORY
}

void XMAReleaseContext(uint32_t context)
{
    std::lock_guard guard{ g_mutex };
    if (g_contexts == nullptr || context == 0)
        return;

    const uint32_t offset = context - g_memory.MapVirtual(g_contexts);
    if (offset < CONTEXT_COUNT * CONTEXT_SIZE)
    {
        xportapu::ReleaseContext(offset / CONTEXT_SIZE);
        g_used[offset / CONTEXT_SIZE] = false;
    }
}

GUEST_FUNCTION_HOOK(__imp__XMACreateContext, XMACreateContext);
GUEST_FUNCTION_HOOK(__imp__XMAReleaseContext, XMAReleaseContext);
