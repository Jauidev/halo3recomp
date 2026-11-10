// fake GPU for the headless phases. The game's statically linked D3D writes PM4 packets
// into a ring buffer, kicks the GPU through CP_RB_WPTR and then waits for the GPU's answers:
// the ring read pointer written back to memory, fences written by EVENT_WRITE_SHD / MEM_WRITE,
// scratch registers, and interrupts (source 1 from PM4_INTERRUPT, source 0 at every vblank) that
// make D3D run its swap-completion callbacks. This command processor gives exactly those answers
// and skips everything that would draw. Packet semantics follow Xenia's CommandProcessor (BSD).
//
// Registers live where the CPU reads them: the MMIO window at 0x7FC80000 + index * 4 (big-endian),
// which in this runtime is plain guest memory. Indices of 0x4000 and up (shader constants) are
// not CPU-visible and stay in a host array.
#include <stdafx.h>
#include <cpu/ppc_context.h>
#include <cpu/guest_thread.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <kernel/xdm.h>
#include <os/background.h>
#include <os/logger.h>
#include <os/thread_policy.h>
#include <os/thread_dump.h>
#include <xport_gpu.h>
#include <xnative.h>

#include <cmath>
// the game's D3D device fields (games/<game>/xd3d_device.h, tools/find_d3d.py)
#include "xd3d_device.h"

std::string VmemPhysicalStats(); // kernel/vmem.cpp

extern std::atomic<uint64_t> g_fileReads;
extern std::atomic<uint64_t> g_fileBytesRead;

struct ANativeWindow;

// The display's gamma ramp, which the renderer's present applies (hdr_present_ps.glsl): 32 knots
// at the 10-bit inputs 0, 33, 66 ... 1023, as half floats, two to a dword (16 dwords of push
// constants). Set from whichever ramp the game programs: the 256-entry table, or the 128-entry
// PWL ramp (Halo 3: its frontbuffer holds the raw bits of its 7e3 target, which the ramp decodes).
uint32_t g_xportGammaRamp[16] = {};
std::atomic<bool> g_xportGammaRampSet{false};

namespace
{
    // [0, 1] to a half float, rounded to nearest.
    uint16_t GammaKnotHalf(float value)
    {
        value = std::clamp(value, 0.0f, 1.0f);
        if (value < 6.103515625e-05f) // below the smallest normal half: a subnormal one
            return uint16_t(std::lround(value * 16777216.0f));
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        uint32_t half = ((((bits >> 23) & 0xFF) - 112) << 10) | ((bits & 0x7FFFFF) >> 13);
        if (bits & 0x1000)
            ++half;
        return uint16_t(half);
    }

    // curve: a 10-bit input (0-1023) to the output (0-1).
    template <typename Curve>
    void SetGammaRampKnots(Curve curve)
    {
        uint32_t packed[16] = {};
        for (uint32_t knot = 0; knot < 32; ++knot)
            packed[knot >> 1] |= uint32_t(GammaKnotHalf(curve(knot * 33))) << ((knot & 1) * 16);
        std::memcpy(g_xportGammaRamp, packed, sizeof(packed));
        g_xportGammaRampSet.store(true, std::memory_order_release);
    }

    void XeniaInterrupt(uint32_t source, uint32_t cpu);

    constexpr uint32_t MMIO_BASE = 0x7FC80000;
    constexpr uint32_t PHYSICAL_BASE = 0xA0000000; // where runtime/kernel/vmem.cpp maps physical RAM
    constexpr uint32_t TITLE_ID = XPORT_TITLE_ID;  // games/<title>/game.cmake

    constexpr uint32_t REG_CP_RB_RPTR = 0x01C4;
    constexpr uint32_t REG_CP_RB_WPTR = 0x01C5;
    constexpr uint32_t REG_SCRATCH_UMSK = 0x01DC;
    constexpr uint32_t REG_SCRATCH_ADDR = 0x01DD;
    constexpr uint32_t REG_SCRATCH_REG0 = 0x0578;
    constexpr uint32_t REG_COHER_STATUS_HOST = 0x0A2F;
    constexpr uint32_t REG_DC_LUT_RW_INDEX = 0x1922;
    constexpr uint32_t REG_DC_LUT_PWL_DATA = 0x1924;
    constexpr uint32_t REG_DC_LUT_30_COLOR = 0x1925;
    constexpr uint32_t REG_VGT_EVENT_INITIATOR = 0x21F9;
    constexpr uint32_t REG_RB_SAMPLE_COUNT_ADDR = 0x2325;

    enum : uint32_t
    {
        PM4_NOP = 0x10,
        PM4_REG_RMW = 0x21,
        PM4_DRAW_INDX = 0x22,
        PM4_SET_CONSTANT = 0x2D,
        PM4_LOAD_ALU_CONSTANT = 0x2F,
        PM4_DRAW_INDX_2 = 0x36,
        PM4_INDIRECT_BUFFER_PFD = 0x37,
        PM4_WAIT_REG_MEM = 0x3C,
        PM4_MEM_WRITE = 0x3D,
        PM4_REG_TO_MEM = 0x3E,
        PM4_INDIRECT_BUFFER = 0x3F,
        PM4_COND_WRITE = 0x45,
        PM4_EVENT_WRITE = 0x46,
        PM4_ME_INIT = 0x48,
        PM4_INTERRUPT = 0x54,
        PM4_SET_CONSTANT2 = 0x55,
        PM4_SET_SHADER_CONSTANTS = 0x56,
        PM4_EVENT_WRITE_SHD = 0x58,
        PM4_EVENT_WRITE_EXT = 0x5A,
        PM4_EVENT_WRITE_ZPD = 0x5B,
        PM4_SET_BIN_MASK = 0x50,
        PM4_SET_BIN_SELECT = 0x51,
        PM4_SET_BIN_MASK_LO = 0x60,
        PM4_SET_BIN_MASK_HI = 0x61,
        PM4_SET_BIN_SELECT_LO = 0x62,
        PM4_SET_BIN_SELECT_HI = 0x63,
    };

    // VdSwap reserves 64 dwords in the ring; it fills them with a NOP whose first word is this.
    constexpr uint32_t SWAP_MARKER = 0x53574150; // 'SWAP'

    uint8_t* Physical(uint32_t address)
    {
        return g_memory.base + PHYSICAL_BASE + (address & 0x1FFFFFFF);
    }

    // The low two bits of a GPU memory address select a byte order (xenos::Endian).
    uint32_t GpuSwap(uint32_t value, uint32_t endian)
    {
        switch (endian & 3)
        {
        case 1: return ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF); // 8 in 16
        case 2: return ByteSwap(value);                                           // 8 in 32
        case 3: return (value << 16) | (value >> 16);                             // 16 in 32
        default: return value;
        }
    }

    // GPU progress for D3D's wait loops (XD3D_GpuWaitCheck at the end): a counter bumped each time
    // this side writes what the CPU may be polling (memory, the ring read pointer, an interrupt's
    // work), and a condition the waiting thread sleeps on instead of spinning. Both atomics are
    // sequentially consistent: either the notifier sees the waiter or the waiter sees the bump.
    std::atomic<uint32_t> g_gpuProgress{0};
    std::atomic<uint32_t> g_gpuProgressWaiters{0};
    std::mutex g_gpuProgressMutex;
    std::condition_variable g_gpuProgressCondition;

    void NotifyGpuProgress()
    {
        g_gpuProgress.fetch_add(1);
        if (g_gpuProgressWaiters.load() != 0)
        {
            std::lock_guard guard{ g_gpuProgressMutex };
            g_gpuProgressCondition.notify_all();
        }
    }

    // Sleeps until the counter moves past seen, or timeout passes.
    void WaitGpuProgress(uint32_t seen, std::chrono::microseconds timeout)
    {
        if (g_gpuProgress.load() != seen)
            return;
        std::unique_lock lock{ g_gpuProgressMutex };
        g_gpuProgressWaiters.fetch_add(1);
        g_gpuProgressCondition.wait_for(lock, timeout, [seen] { return g_gpuProgress.load() != seen; });
        g_gpuProgressWaiters.fetch_sub(1);
    }

    // value = host order; memory gets it in the order the address asks for.
    void WriteMemory(uint32_t address, uint32_t value)
    {
        *reinterpret_cast<uint32_t*>(Physical(address & ~3u)) = GpuSwap(value, address & 3);
        NotifyGpuProgress();
    }

    uint32_t ReadMemory(uint32_t address)
    {
        return GpuSwap(*reinterpret_cast<uint32_t*>(Physical(address & ~3u)), address & 3);
    }

    bool Compare(uint32_t function, uint32_t value, uint32_t reference)
    {
        switch (function & 7)
        {
        case 0: return false;
        case 1: return value < reference;
        case 2: return value <= reference;
        case 3: return value == reference;
        case 4: return value != reference;
        case 5: return value >= reference;
        case 6: return value > reference;
        default: return true;
        }
    }

    // Reads big-endian dwords from a ring (wrapping) or from a linear indirect buffer.
    struct PacketReader
    {
        uint32_t base;      // physical address
        uint32_t size;      // ring size in dwords, 0 for a linear buffer
        uint32_t index;
        uint32_t end;

        bool AtEnd() const
        {
            return index == end;
        }

        uint32_t Read()
        {
            const uint32_t value = ByteSwap(*reinterpret_cast<uint32_t*>(Physical(base + index * 4)));
            index = size ? (index + 1) % size : index + 1;
            return value;
        }

        void Skip(uint32_t count)
        {
            index = size ? (index + count) % size : index + count;
        }
    };

    class CommandProcessor
    {
    public:
        void InitializeRingBuffer(uint32_t address, uint32_t sizeLog2)
        {
            std::lock_guard guard{ m_mutex };
            m_ringBase = address & 0x1FFFFFFF;
            m_ringSize = (1u << (sizeLog2 + 3)) / 4;
            m_readIndex = 0;
            LOGF("ring buffer at 0x{:08X}, {} dwords", m_ringBase, m_ringSize);
            // XPORT_GPU=xenia: Xenia's GPU (xenia_glue/) consumes the ring and draws with Vulkan.
            const char* gpu = getenv("XPORT_GPU");
#ifdef XPORT_NO_NATIVE_RENDERER
            // The slot lacks D3D functions the native renderer's hooks need (cmake/rexglue.cmake):
            // nothing would reach it, so Xenia's GPU draws instead.
            if (gpu && strcmp(gpu, "native") == 0)
            {
                LOG_WARNING("GPU: this game has no native renderer (D3D functions missing), Xenia's GPU instead");
                gpu = "xenia";
            }
#endif
            if (gpu && strcmp(gpu, "xenia") == 0 && g_memory.physical)
            {
                m_xenia = xportgpu::Initialize(g_memory.base, g_memory.physical, TITLE_ID,
                    XeniaInterrupt);
                LOGF("GPU: {}", m_xenia ? "Xenia (Vulkan)" : "Xenia failed to start, fake GPU instead");
                if (m_xenia)
                {
                    xportgpu::InitializeRingBuffer(address, sizeLog2);
                    ApplyPendingSurface();
                }
            }
            // XPORT_GPU=native: LibertyRecomp's native renderer (rexglue_glue/) draws from the
            // game's D3D calls; the fake GPU still consumes the ring (fences, interrupts).
            if (gpu && strcmp(gpu, "native") == 0 && g_memory.physical)
            {
                m_native = xnative::Initialize(g_memory.base, g_memory.physical);
                LOGF("GPU: {}", m_native ? "native renderer (Vulkan) + fake GPU"
                                         : "native renderer failed to start, fake GPU only");
                if (m_native)
                    ApplyPendingSurface();
            }
            Start();
        }

        void EnableReadPointerWriteBack(uint32_t address, uint32_t blockLog2)
        {
            std::lock_guard guard{ m_mutex };
            m_writeBackAddress = address;
            LOGF("read pointer write-back to 0x{:08X} (block log2 {})", address, blockLog2);
            if (m_xenia)
                xportgpu::EnableReadPointerWriteBack(address, blockLog2);
        }

        bool UsesXenia() const
        {
            return m_xenia;
        }

        // Interrupts delivered by Xenia's command processor run here, as the fake GPU's do.
        void RunInterrupt(uint32_t source, uint32_t cpu)
        {
            DispatchInterrupt(source, cpu);
        }

        // The activity's surface may come before or after the GPU starts.
        void SetSurface(ANativeWindow* window)
        {
            std::lock_guard guard{ m_surfaceMutex };
            m_surface = window;
            if (m_xenia)
                xportgpu::SetSurface(window);
            if (m_native)
                xnative::SetSurface(window);
        }

        void CountSwap()
        {
            ++m_swapCounter;
        }

        void SetInterruptCallback(uint32_t callback, uint32_t userData)
        {
            std::lock_guard guard{ m_interruptMutex };
            m_interruptCallback = callback;
            m_interruptUserData = userData;
            LOGF("interrupt callback 0x{:08X}, data 0x{:08X}", callback, userData);
        }

        uint32_t GetSwapCount() const
        {
            return m_swapCounter;
        }

        void Kick(uint32_t device)
        {
            m_device = device;
            ++m_kicks;
            if (m_xenia)
            {
                xportgpu::UpdateWritePointer(ReadRegister(REG_CP_RB_WPTR));
                return;
            }
            {
                std::lock_guard guard{ m_kickMutex };
                m_kicked = true;
            }
            m_kickCondition.notify_one();
        }

    private:
        void Start()
        {
            if (m_started)
                return;

            m_started = true;

            // Values Xenia's GraphicsSystem::ReadRegister returns for display registers the CPU polls.
            WriteMmio(0x3C00 / 4, 0x08100748);
            WriteMmio(0x3C04 / 4, 0x0000200E);
            WriteMmio(0x6530 / 4, 0x000002D0); // scanline
            WriteMmio(0x6544 / 4, 0x00000001); // D1MODE_VBLANK_STATUS
            WriteMmio(0x6584 / 4, 0x050002D0); // 1280x720

            if (!m_xenia)
                std::thread([this] { CommandThread(); }).detach();
            else
                std::thread([this] { XeniaWritePointerThread(); }).detach();
            std::thread([this] { VblankThread(); }).detach();
        }

        // Xenia hears of new commands from Kick (the hooked D3D kickoff). A game whose KickOff
        // tools/find_d3d.py did not find never kicks: pass it the CP_RB_WPTR the game writes instead,
        // polled as CommandThread does. Once a kick arrives the hook does it and this stays idle.
        void XeniaWritePointerThread()
        {
            NameCurrentThread("xport-gpu-wptr");
            uint32_t last = 0;  // the ring starts empty: any other value is news
            while (true)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (m_kicks.load() != 0)
                    continue;
                const uint32_t value = ReadRegister(REG_CP_RB_WPTR);
                if (value != last)
                {
                    last = value;
                    xportgpu::UpdateWritePointer(value);
                }
            }
        }

        void ApplyPendingSurface()
        {
            std::lock_guard guard{ m_surfaceMutex };
            if (m_surface && m_xenia)
                xportgpu::SetSurface(m_surface);
            if (m_surface && m_native)
                xnative::SetSurface(m_surface);
        }

        void WriteMmio(uint32_t index, uint32_t value)
        {
            *reinterpret_cast<uint32_t*>(g_memory.base + MMIO_BASE + index * 4) = ByteSwap(value);
        }

        uint32_t ReadRegister(uint32_t index)
        {
            index &= 0x7FFF;
            if (index < 0x4000)
                return ByteSwap(*reinterpret_cast<uint32_t*>(g_memory.base + MMIO_BASE + index * 4));
            return m_constants[index - 0x4000];
        }

        void WriteRegister(uint32_t index, uint32_t value)
        {
            index &= 0x7FFF;
            if (index >= 0x4000)
            {
                if (index - 0x4000 < std::size(m_constants))
                    m_constants[index - 0x4000] = value;
                return;
            }

            // The display's gamma ramp: the renderer's present applies it (g_xportGammaRamp).
            // Only its green channel is kept (the games seen so far set the same for all three).
            if (index == REG_DC_LUT_RW_INDEX)
            {
                m_gammaRampIndex = value & 0xFF;
                m_gammaRampComponent = 0;
            }
            else if (index == REG_DC_LUT_30_COLOR)
            {
                // 256-entry table, one write per entry: 0:9 blue, 10:19 green, 20:29 red.
                m_gammaTable[m_gammaRampIndex] = uint16_t((value >> 10) & 0x3FF);
                if (m_gammaRampIndex == 255)
                {
                    const auto curve = [this](uint32_t input) {
                        const float position = float(input) * (255.0f / 1023.0f);
                        const uint32_t entry = std::min(uint32_t(position), 254u);
                        const float low = m_gammaTable[entry], high = m_gammaTable[entry + 1];
                        return (low + (high - low) * (position - float(entry))) / 1023.0f;
                    };
                    if (!m_gammaPwlSet)
                        SetGammaRampKnots(curve);
                    if (std::memcmp(m_gammaTableLogged, m_gammaTable, sizeof(m_gammaTable)) != 0 && m_gammaRampLogs < 16)
                    {
                        std::memcpy(m_gammaTableLogged, m_gammaTable, sizeof(m_gammaTable));
                        ++m_gammaRampLogs;
                        LOGF("gamma: 256-entry table {} (swap {}{}), input 0 64 128 256 512 768 1023 -> {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f}",
                            m_gammaRampLogs, m_swapCounter.load(), m_gammaPwlSet ? ", PWL kept" : "", curve(0), curve(64),
                            curve(128), curve(256), curve(512), curve(768), curve(1023));
                    }
                }
                m_gammaRampIndex = (m_gammaRampIndex + 1) & 0xFF;
            }
            else if (index == REG_DC_LUT_PWL_DATA)
            {
                // 128-entry PWL ramp, three writes (R, G, B) per entry: 0:15 base, 16:31 delta,
                // both 10-bit in bits 6:15. An entry covers 8 inputs (DC_LUTA_CONTROL 3, which
                // Halo 3 sets for its 10-bit frontbuffer): base + delta * (input & 7) / 8.
                const uint32_t entry = m_gammaRampIndex & 0x7F;
                if (m_gammaRampComponent == 1)
                    m_gammaPwl[entry] = value;
                if (++m_gammaRampComponent == 3)
                {
                    m_gammaRampComponent = 0;
                    if (entry == 127)
                    {
                        m_gammaPwlSet = true;
                        const auto curve = [this](uint32_t input) {
                            const uint32_t data = m_gammaPwl[std::min(input, 1023u) >> 3];
                            const float base = float((data & 0xFFFF) >> 6), delta = float(data >> 22);
                            return (base + delta * float(input & 7) / 8.0f) / 1023.0f;
                        };
                        SetGammaRampKnots(curve);
                        // Each different ramp the game programs (a few: setup, then the game's own).
                        if (std::memcmp(m_gammaPwlLogged, m_gammaPwl, sizeof(m_gammaPwl)) != 0 && m_gammaRampLogs < 16)
                        {
                            std::memcpy(m_gammaPwlLogged, m_gammaPwl, sizeof(m_gammaPwl));
                            ++m_gammaRampLogs;
                            LOGF("gamma: PWL ramp {} (swap {}), input 0 64 128 256 384 512 640 768 896 1023 -> {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f}",
                                m_gammaRampLogs, m_swapCounter.load(), curve(0), curve(64), curve(128), curve(256), curve(384), curve(512),
                                curve(640), curve(768), curve(896), curve(1023));
                        }
                    }
                    m_gammaRampIndex = (m_gammaRampIndex + 1) & 0xFF;
                }
            }

            // Coherency requests complete at once: nothing is cached on this side.
            if (index == REG_COHER_STATUS_HOST)
                value &= ~0x80000000u;

            WriteMmio(index, value);

            // Scratch registers are also written to memory when their bit is set in SCRATCH_UMSK.
            if (index >= REG_SCRATCH_REG0 && index < REG_SCRATCH_REG0 + 8)
            {
                const uint32_t scratch = index - REG_SCRATCH_REG0;
                if (ReadRegister(REG_SCRATCH_UMSK) & (1u << scratch))
                {
                    *reinterpret_cast<uint32_t*>(Physical((ReadRegister(REG_SCRATCH_ADDR) & ~3u) + scratch * 4)) = ByteSwap(value);
                    NotifyGpuProgress();
                }
            }
        }

#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
        // Gears of War 3: count consecutive registers from base, written in bulk (in the
        // 2026-11-03 plaza profile type 0 packets cost ~10% of a big core, one call, compare
        // chain and ring modulo per dword). Leaves the same state as WriteRegister per dword:
        // registers below 0x4000 hold the ring's big-endian bytes in MMIO memory, constants
        // (0x4000-0x5FFF) are stored host-endian, 0x6000-0x7FFF are dropped. Returns false, having
        // read nothing, for runs that need WriteRegister: the registers it treats specially, a
        // run across 0x4000 or one that wraps through index & 0x7FFF.
        bool WriteRegisterRun(PacketReader& reader, uint32_t base, uint32_t count)
        {
            const uint32_t last = base + count;  // exclusive
            if (last > 0x8000 || (base < 0x4000 && last > 0x4000))
                return false;
            if (base < 0x4000)
            {
                const auto inside = [&](uint32_t index) { return index - base < count; };
                if (inside(REG_DC_LUT_RW_INDEX) || inside(REG_DC_LUT_30_COLOR) || inside(REG_DC_LUT_PWL_DATA) ||
                    inside(REG_COHER_STATUS_HOST) ||
                    (base < REG_SCRATCH_REG0 + 8 && last > REG_SCRATCH_REG0))
                    return false;
            }
            uint32_t index = base;
            uint32_t remaining = count;
            while (remaining != 0)
            {
                // Contiguous dwords up to the end of the ring (a linear buffer has no end here).
                const uint32_t run = reader.size ? std::min(remaining, reader.size - reader.index) : remaining;
                const auto* source = reinterpret_cast<const uint32_t*>(Physical(reader.base + reader.index * 4));
                if (index < 0x4000)
                {
                    std::memcpy(g_memory.base + MMIO_BASE + index * 4, source, size_t(run) * 4);
                }
                else if (index - 0x4000 < std::size(m_constants))
                {
                    const uint32_t first = index - 0x4000;
                    const uint32_t stored = std::min<uint32_t>(run, uint32_t(std::size(m_constants)) - first);
                    for (uint32_t i = 0; i < stored; i++)
                        m_constants[first + i] = ByteSwap(source[i]);
                }
                reader.Skip(run);
                index += run;
                remaining -= run;
            }
            return true;
        }
#endif

        // Runs the guest interrupt callback on the calling thread, as Xenia's EmulateCPInterruptDPC does.
        void DispatchInterrupt(uint32_t source, uint32_t cpu)
        {
            std::lock_guard guard{ m_interruptMutex };
            if (m_interruptCallback == 0)
                return;

            thread_local std::unique_ptr<GuestThreadContext> s_context;
            if (!s_context)
                s_context = std::make_unique<GuestThreadContext>(cpu);

            PPCContext& ctx = s_context->ppcContext;
            s_context->thread[0x10C] = uint8_t(cpu); // PCR: current processor
            ctx.r3.u64 = source;
            ctx.r4.u64 = m_interruptUserData;
            g_memory.FindFunction(m_interruptCallback)(ctx, g_memory.base);
            ++m_interrupts;
            NotifyGpuProgress();  // the callback may complete what a D3D wait polls
        }

        void CommandThread()
        {
            NameCurrentThread("xport-gpu");
            while (true)
            {
                {
                    // Kicks come from the hooked D3D kickoff; the timeout is a fallback poll.
                    std::unique_lock lock{ m_kickMutex };
                    m_kickCondition.wait_for(lock, std::chrono::milliseconds(2), [this] { return m_kicked; });
                    m_kicked = false;
                }

                uint32_t ringBase, ringSize;
                {
                    std::lock_guard guard{ m_mutex };
                    ringBase = m_ringBase;
                    ringSize = m_ringSize;
                }
                if (ringSize == 0)
                    continue;

                const uint32_t writeIndex = ReadRegister(REG_CP_RB_WPTR) % ringSize;
                if (writeIndex == m_readIndex)
                    continue;

                PacketReader reader{ ringBase, ringSize, m_readIndex, writeIndex };
                while (!reader.AtEnd())
                    ExecutePacket(reader);

                m_readIndex = writeIndex;
                WriteRegister(REG_CP_RB_RPTR, m_readIndex);
                if (m_writeBackAddress != 0)
                    *reinterpret_cast<uint32_t*>(Physical(m_writeBackAddress)) = ByteSwap(m_readIndex.load());
                NotifyGpuProgress();  // ring space for D3D's waits
            }
        }

        void ExecuteIndirectBuffer(uint32_t address, uint32_t length)
        {
            PacketReader reader{ address & 0x1FFFFFFF, 0, 0, length };
            while (!reader.AtEnd())
                ExecutePacket(reader);
        }

        void ExecutePacket(PacketReader& reader)
        {
            const uint32_t packet = reader.Read();
            switch (packet >> 30)
            {
            case 0:
            {
                // Type 0: consecutive (or, with bit 15, repeated) register writes.
                const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
                const uint32_t base = packet & 0x7FFF;
                const bool oneRegister = (packet >> 15) & 1;
#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
                if (!oneRegister && WriteRegisterRun(reader, base, count))
                    break;
#endif
                for (uint32_t i = 0; i < count; i++)
                    WriteRegister(oneRegister ? base : base + i, reader.Read());
                break;
            }
            case 1:
            {
                // Type 1: two registers.
                const uint32_t first = reader.Read();
                const uint32_t second = reader.Read();
                WriteRegister(packet & 0x7FF, first);
                WriteRegister((packet >> 11) & 0x7FF, second);
                break;
            }
            case 2:
                break; // Type 2: padding.
            case 3:
                ExecutePacketType3(reader, packet);
                break;
            }
        }

        void ExecutePacketType3(PacketReader& reader, uint32_t packet)
        {
            const uint32_t opcode = (packet >> 8) & 0x7F;
            const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
            const uint32_t start = reader.index;
            uint32_t consumed = 0;
            auto read = [&] { consumed++; return reader.Read(); };

            // Predicated tiling: D3D runs one command buffer once per screen tile, and packets
            // with bit 0 set only run in the passes the bin select/mask let through. Without
            // this, interrupts and swaps run once per tile and D3D's per-CPU list threads replay
            // lists without end.
            if ((packet & 1) && (m_binSelect & m_binMask) == 0)
            {
                reader.Skip(count);
                return;
            }

            switch (opcode)
            {
            case PM4_SET_BIN_MASK_LO:
                m_binMask = (m_binMask & 0xFFFFFFFF00000000ull) | read();
                break;
            case PM4_SET_BIN_MASK_HI:
                m_binMask = (m_binMask & 0xFFFFFFFFull) | (uint64_t(read()) << 32);
                break;
            case PM4_SET_BIN_SELECT_LO:
                m_binSelect = (m_binSelect & 0xFFFFFFFF00000000ull) | read();
                break;
            case PM4_SET_BIN_SELECT_HI:
                m_binSelect = (m_binSelect & 0xFFFFFFFFull) | (uint64_t(read()) << 32);
                break;
            case PM4_SET_BIN_MASK:
            {
                const uint64_t high = read();
                m_binMask = (high << 32) | read();
                break;
            }
            case PM4_SET_BIN_SELECT:
            {
                const uint64_t high = read();
                m_binSelect = (high << 32) | read();
                break;
            }

            case PM4_NOP:
                if (count >= 1 && read() == SWAP_MARKER)
                    OnSwap();
                break;

            case PM4_INTERRUPT:
            {
                const uint32_t cpuMask = read();
                for (uint32_t cpu = 0; cpu < 6; cpu++)
                {
                    if (cpuMask & (1u << cpu))
                        DispatchInterrupt(1, cpu);
                }
                break;
            }

            case PM4_INDIRECT_BUFFER:
            case PM4_INDIRECT_BUFFER_PFD:
            {
                const uint32_t address = read();
                const uint32_t length = read() & 0xFFFFF;
                ExecuteIndirectBuffer(address, length);
                break;
            }

            case PM4_WAIT_REG_MEM:
            {
                const uint32_t waitInfo = read();
                const uint32_t pollAddress = read();
                const uint32_t reference = read();
                const uint32_t mask = read();
                const uint32_t wait = read();
                WaitRegisterMemory(waitInfo, pollAddress, reference, mask, wait);
                break;
            }

            case PM4_REG_RMW:
            {
                const uint32_t info = read();
                const uint32_t andMask = read();
                const uint32_t orMask = read();
                uint32_t value = ReadRegister(info & 0x1FFF);
                value &= (info >> 31) & 1 ? ReadRegister(andMask & 0x1FFF) : andMask;
                value |= (info >> 30) & 1 ? ReadRegister(orMask & 0x1FFF) : orMask;
                WriteRegister(info & 0x1FFF, value);
                break;
            }

            case PM4_REG_TO_MEM:
            {
                const uint32_t index = read();
                const uint32_t address = read();
                WriteMemory(address, ReadRegister(index));
                break;
            }

            case PM4_MEM_WRITE:
            {
                uint32_t address = read();
                while (consumed < count)
                {
                    WriteMemory(address, read());
                    address += 4;
                }
                break;
            }

            case PM4_COND_WRITE:
            {
                const uint32_t waitInfo = read();
                const uint32_t pollAddress = read();
                const uint32_t reference = read();
                const uint32_t mask = read();
                const uint32_t writeAddress = read();
                const uint32_t data = read();
                const uint32_t value = (waitInfo & 0x10) ? ReadMemory(pollAddress) : ReadRegister(pollAddress);
                if (Compare(waitInfo, value & mask, reference))
                {
                    if (waitInfo & 0x100)
                        WriteMemory(writeAddress, data);
                    else
                        WriteRegister(writeAddress, data);
                }
                break;
            }

            case PM4_EVENT_WRITE_SHD:
            {
                const uint32_t initiator = read();
                const uint32_t address = read();
                const uint32_t value = read();
                WriteRegister(REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
                // Bit 31: write the swap counter instead of the value.
                WriteMemory(address, (initiator >> 31) & 1 ? m_swapCounter.load() : value);
                break;
            }

            case PM4_EVENT_WRITE_EXT:
            {
                // Screen extents of what was drawn; report the whole 1280x720 target.
                const uint32_t initiator = read();
                const uint32_t address = read();
                WriteRegister(REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
                const uint16_t extents[] = { 0, 1280 >> 3, 0, 720 >> 3, 0, 1 };
                auto* out = reinterpret_cast<uint16_t*>(Physical(address & ~3u));
                for (size_t i = 0; i < std::size(extents); i++)
                    out[i] = (address & 3) ? ByteSwap(extents[i]) : extents[i];
                break;
            }

            case PM4_SET_CONSTANT:
            {
                const uint32_t offsetType = read();
                uint32_t index = offsetType & 0x7FF;
                static constexpr uint32_t s_bases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
                const uint32_t type = (offsetType >> 16) & 0xFF;
                if (type >= std::size(s_bases))
                    break;
                index += s_bases[type];
#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
                if (WriteRegisterRun(reader, index, count - consumed))
                    break;  // the reader is put back at the packet's end below
#endif
                while (consumed < count)
                    WriteRegister(index++, read());
                break;
            }

            case PM4_SET_CONSTANT2:
            case PM4_SET_SHADER_CONSTANTS:
            {
                uint32_t index = read() & 0xFFFF;
                while (consumed < count)
                    WriteRegister(index++, read());
                break;
            }

            case PM4_LOAD_ALU_CONSTANT:
            {
                const uint32_t address = read() & 0x3FFFFFFF;
                const uint32_t offsetType = read();
                const uint32_t size = read() & 0xFFF;
                static constexpr uint32_t s_bases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
                const uint32_t type = (offsetType >> 16) & 0xFF;
                if (type >= std::size(s_bases))
                    break;
                const uint32_t index = (offsetType & 0x7FF) + s_bases[type];
                for (uint32_t i = 0; i < size; i++)
                    WriteRegister(index + i, ByteSwap(*reinterpret_cast<uint32_t*>(Physical(address + i * 4))));
                break;
            }

            case PM4_EVENT_WRITE_ZPD:
            {
                // Occlusion query counters (Xenia's fake mode): D3D fills them with a sentinel
                // and waits until the GPU writes them; a query is the end minus the begin. Each
                // event adds samples to a running count, so every query comes out visible.
                WriteRegister(REG_VGT_EVENT_INITIATOR, read() & 0x3F);
                const uint32_t address = ReadRegister(REG_RB_SAMPLE_COUNT_ADDR);
                if (address == 0)
                    break;
                m_zpassSamples += 100;
                const uint32_t a = m_zpassSamples - (m_zpassSamples >> 1);
                const uint32_t b = m_zpassSamples >> 1;
                // Total, ZFail, ZPass, StencilFail, each as two halves, little endian.
                const uint32_t counts[] = { a, b, 0, 0, a, b, 0, 0 };
                std::memcpy(Physical(address & ~3u), counts, sizeof(counts));
                break;
            }

            case PM4_ME_INIT:
            case PM4_EVENT_WRITE:
            case PM4_DRAW_INDX:
            case PM4_DRAW_INDX_2:
                break; // nothing the CPU waits on

            default:
                if (!m_reportedOpcodes[opcode])
                {
                    m_reportedOpcodes[opcode] = true;
                    LOGF("skipping PM4 opcode 0x{:02X} ({} dwords)", opcode, count);
                }
                break;
            }

            // Every packet ends where its header says, whatever the handler consumed.
            reader.index = start;
            reader.Skip(count);
        }

        void WaitRegisterMemory(uint32_t waitInfo, uint32_t pollAddress, uint32_t reference, uint32_t mask, uint32_t wait)
        {
            const auto begin = std::chrono::steady_clock::now();
            bool reported = false;
            while (true)
            {
                const uint32_t value = (waitInfo & 0x10) ? ReadMemory(pollAddress) : ReadRegister(pollAddress);
                if (Compare(waitInfo, value & mask, reference))
                    return;

                if (!reported && std::chrono::steady_clock::now() - begin > std::chrono::seconds(2))
                {
                    reported = true;
                    LOGF_WARNING("GPU stuck in WAIT_REG_MEM: {} 0x{:08X} = 0x{:08X}, function {}, reference 0x{:08X}, mask 0x{:08X}",
                        (waitInfo & 0x10) ? "memory" : "register", pollAddress, value, waitInfo & 7, reference, mask);
                }

                if (wait >= 0x100)
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                else
                    std::this_thread::yield();
            }
        }

        void OnSwap()
        {
            ++m_swapCounter;
        }

        void VblankThread()
        {
            NameCurrentThread("xport-vblank");
            using namespace std::chrono;
            constexpr auto period = duration_cast<steady_clock::duration>(duration<double>(1.0 / 60.0));
            auto next = steady_clock::now() + period;
            auto lastReport = steady_clock::now();
            uint32_t lastSwaps = 0;

            while (true)
            {
                // No interrupts while the app is in the background (os/background.h); the
                // 60 Hz timing starts over afterwards instead of catching up.
                if (WaitWhileBackgroundHold("vblank"))
                    next = steady_clock::now() + period;
                std::this_thread::sleep_until(next);
                next += period;
                if (m_xenia)
                    xportgpu::MarkVblank();
                else
                    DispatchInterrupt(0, 2);

                const auto now = steady_clock::now();
                if (now - lastReport >= seconds(2))
                {
                    const uint32_t swaps = m_swapCounter;
                    const double elapsed = duration<double>(now - lastReport).count();
                    LOGF("{} swaps ({:.1f} fps), {} interrupts, ring read {} / write {}, file reads {} ({} KB)",
                        swaps, (swaps - lastSwaps) / elapsed, m_interrupts.load(), m_readIndex.load(), ReadRegister(REG_CP_RB_WPTR),
                        g_fileReads.load(), g_fileBytesRead.load() / 1024);
                    if (const uint32_t device = m_device)
                    {
                        // D3D device fields (GTA IV's +10908, +10896, +16544, +16552, moved to this
                        // game's by xd3d_device.h): fence the CPU has inserted and the one the GPU
                        // wrote (through a pointer); swaps presented and completed (a game may
                        // have no such field).
                        auto word = [](uint32_t address) { return ByteSwap(*reinterpret_cast<uint32_t*>(g_memory.base + address)); };
                        const uint32_t gpuFence = word(device + xd3d::DeviceOffset(10896));
                        LOGF("d3d: {} kicks, fence cpu 0x{:X} gpu 0x{:X}, swaps presented {} completed {}",
                            m_kicks.load(), word(device + xd3d::DeviceOffset(10908)), gpuFence ? word(gpuFence) : 0,
                            word(device + xd3d::DeviceOffset(16544)),
                            xd3d::HasDeviceOffset(16552) ? word(device + xd3d::DeviceOffset(16552)) : 0);
                    }
                    if (xportgpu::IsActive())
                    {
                        LOGF("{}", xportgpu::DebugStatus());
                    }
                    LOGF("{}", VmemPhysicalStats());
                    lastReport = now;
                    lastSwaps = swaps;
                }
            }
        }

        std::mutex m_mutex;
        uint32_t m_ringBase = 0;
        uint32_t m_ringSize = 0;
        uint32_t m_writeBackAddress = 0;
        std::atomic<uint32_t> m_readIndex = 0;
        uint32_t m_gammaRampIndex = 0;
        uint32_t m_gammaRampComponent = 0; // PWL: which of R, G, B the next write is
        uint16_t m_gammaTable[256] = {};
        uint32_t m_gammaPwl[128] = {};
        bool m_gammaPwlSet = false; // the PWL ramp wins over the table (as Xenia: 10-bit frontbuffer)
        uint16_t m_gammaTableLogged[256] = {};
        uint32_t m_gammaPwlLogged[128] = {};
        uint32_t m_gammaRampLogs = 0;
        bool m_started = false;
        bool m_xenia = false; // XPORT_GPU=xenia and Xenia started
        bool m_native = false; // XPORT_GPU=native and the native renderer started
        std::mutex m_surfaceMutex;
        ANativeWindow* m_surface = nullptr;

        std::mutex m_kickMutex;
        std::condition_variable m_kickCondition;
        bool m_kicked = false;
        std::atomic<uint32_t> m_device = 0;
        std::atomic<uint64_t> m_kicks = 0;

        std::recursive_mutex m_interruptMutex;
        uint32_t m_interruptCallback = 0;
        uint32_t m_interruptUserData = 0;
        std::atomic<uint64_t> m_interrupts = 0;

        std::atomic<uint32_t> m_swapCounter = 0;
        uint32_t m_constants[0x2000]{};
        bool m_reportedOpcodes[0x80]{};
        uint32_t m_zpassSamples = 0;  // command thread only
        uint64_t m_binMask = ~0ull;   // command thread only
        uint64_t m_binSelect = ~0ull;
    };

    CommandProcessor g_commandProcessor;

    void XeniaInterrupt(uint32_t source, uint32_t cpu)
    {
        g_commandProcessor.RunInterrupt(source, cpu);
    }
}

// Frames the game has presented so far (shown by the Android status screen).
// The activity's SurfaceView (runtime/os/android_app.cpp), nullptr when it goes away.
void SetGpuSurface(ANativeWindow* window)
{
    g_commandProcessor.SetSurface(window);
}

uint32_t GetGpuSwapCount()
{
    return g_commandProcessor.GetSwapCount();
}

void VdInitializeRingBuffer(uint32_t address, uint32_t sizeLog2)
{
    g_commandProcessor.InitializeRingBuffer(address, sizeLog2);
}

void VdEnableRingBufferRPtrWriteBack(uint32_t address, uint32_t blockLog2)
{
    g_commandProcessor.EnableReadPointerWriteBack(address, blockLog2);
}

void VdSetGraphicsInterruptCallback(uint32_t callback, uint32_t userData)
{
    g_commandProcessor.SetInterruptCallback(callback, userData);
}

// D3D reserves 64 dwords in the ring for the kernel's swap commands. A NOP carrying the swap
// marker and the front buffer address keeps the command processor in step.
void VdSwap(be<uint32_t>* buffer, uint32_t fetch, uint32_t unk2, uint32_t unk3, uint32_t unk4, be<uint32_t>* frontBuffer)
{
    NameUnnamedCurrentThread("XpRenderThread");
    if (g_commandProcessor.UsesXenia())
    {
        xportgpu::WriteSwapPacket(g_memory.MapVirtual(buffer), fetch);
        g_commandProcessor.CountSwap();
        return;
    }
    buffer[0] = 0xC0000000 | (62u << 16) | (PM4_NOP << 8);
    buffer[1] = SWAP_MARKER;
    buffer[2] = frontBuffer ? uint32_t(*frontBuffer) & 0x1FFFFFFF : 0;
    for (uint32_t i = 3; i < 64; i++)
        buffer[i] = 0;
}

void VdGetSystemCommandBuffer(be<uint32_t>* buffer, be<uint32_t>* value)
{
    memset(buffer, 0, 0x94);
    buffer[0] = 0xBEEF0000;
    *value = 0xBEEF0001;
}

uint32_t VdInitializeEngines()
{
    return 1;
}

uint32_t VdIsHSIOTrainingSucceeded()
{
    return 1;
}

// Widescreen, at least 1024 pixels wide (Xenia's flags for 1280x720).
uint32_t VdQueryVideoFlags()
{
    return 3;
}

void VdGetCurrentDisplayGamma(be<uint32_t>* type, be<float>* power)
{
    *type = 2;
    *power = 2.22222233f;
}

GUEST_FUNCTION_HOOK(__imp__VdInitializeRingBuffer, VdInitializeRingBuffer);
GUEST_FUNCTION_HOOK(__imp__VdEnableRingBufferRPtrWriteBack, VdEnableRingBufferRPtrWriteBack);
GUEST_FUNCTION_HOOK(__imp__VdSetGraphicsInterruptCallback, VdSetGraphicsInterruptCallback);
GUEST_FUNCTION_HOOK(__imp__VdSwap, VdSwap);
GUEST_FUNCTION_HOOK(__imp__VdGetSystemCommandBuffer, VdGetSystemCommandBuffer);
GUEST_FUNCTION_HOOK(__imp__VdInitializeEngines, VdInitializeEngines);
GUEST_FUNCTION_HOOK(__imp__VdIsHSIOTrainingSucceeded, VdIsHSIOTrainingSucceeded);
GUEST_FUNCTION_HOOK(__imp__VdQueryVideoFlags, VdQueryVideoFlags);
GUEST_FUNCTION_HOOK(__imp__VdGetCurrentDisplayGamma, VdGetCurrentDisplayGamma);

#include "xd3d_functions.h"

// Each hook only when tools/find_d3d.py found the function (its name is then a macro); without
// KickOff the command processor wakes on its fallback poll.
#ifdef XD3D_KickOff
// D3D's kickoff (writes CP_RB_WPTR): run the original, then wake the command processor at once
// instead of waiting for its fallback poll. The recompiled function is weak, so this replaces it.
PPC_FUNC(XD3D_KickOff)
{
    const uint32_t device = ctx.r3.u32;
    XD3D_IMP(KickOff)(ctx, base);
    g_commandProcessor.Kick(device);
}
#endif

#ifdef XD3D_GpuWaitCheck
// D3D's wait loops (ring space, fences, frame slots) call this once per turn: it checks the GPU
// is still moving and returns 0 to give up (after XD3D_GpuHangHandler). The loops spin on it, so
// the game's render thread stayed busy while it only waited for the GPU (16% of the app's CPU on
// a Mali phone). Between two turns of the same wait the thread now sleeps until this side writes
// something (NotifyGpuProgress) or XPORT_D3D_WAIT_US microseconds pass (250; 0 spins as before).
// Some waits poll what this side does not write: with 1000 the worst 10% of Gears' frames fell
// from 29.8 to 29.5 fps, with 250 they stay at 29.8 (render thread 90% -> 38% of a core).
// Xenia's GPU (XPORT_GPU=xenia) does not notify: there it still spins.
PPC_FUNC(XD3D_GpuWaitCheck)
{
    struct Wait
    {
        uint32_t state, start, progress;
    };
    thread_local Wait t_wait{};
    static const std::chrono::microseconds timeout = []
    {
        const char* value = getenv("XPORT_D3D_WAIT_US");
        const auto us = std::chrono::microseconds(value ? atoi(value) : 250);
        LOGF("D3D waits sleep up to {} us between GPU progress (XPORT_D3D_WAIT_US; 0 spins)", us.count());
        return us;
    }();

    // The wait's state, on the caller's stack: +20 is the timebase when the wait began, so the same
    // address with another start is another wait (whose first turn does not sleep).
    const uint32_t state = ctx.r3.u32;
    const uint32_t start = *reinterpret_cast<const uint32_t*>(base + state + 20);
    if (timeout.count() > 0 && t_wait.state == state && t_wait.start == start && !g_commandProcessor.UsesXenia())
        WaitGpuProgress(t_wait.progress, timeout);
    XD3D_IMP(GpuWaitCheck)(ctx, base);
    // Read after the check and before the caller polls again: a write after this wakes the next turn.
    t_wait = { state, start, g_gpuProgress.load() };
}
#endif

#ifdef XD3D_GpuHangHandler
// Diagnostics: D3D's GPU hang handler (called when a fence wait sees no progress for 5000 units
// of the clock at [[r13+0x100]+0x58]).
PPC_FUNC(XD3D_GpuHangHandler)
{
    auto word = [base](uint32_t address) { return ByteSwap(*reinterpret_cast<uint32_t*>(base + address)); };
    const uint32_t device = ctx.r3.u32;
    const uint32_t thread = word(ctx.r13.u32 + 0x100);
    // The fence the CPU is at, and where the GPU writes the one it reached (GTA IV's +10908 and
    // +10896, moved to this game's by xd3d_device.h).
    const uint32_t gpuFence = word(device + xd3d::DeviceOffset(10896));
    DiagPrint("D3D GPU hang handler: thread %08X, clock [+0x58] = 0x%08X, fence cpu 0x%X gpu 0x%X\n",
        GuestThread::GetCurrentThreadId(), word(thread + 0x58), word(device + xd3d::DeviceOffset(10908)),
        gpuFence ? word(gpuFence) : 0);
    PrintGuestBacktrace(&ctx);
    XD3D_IMP(GpuHangHandler)(ctx, base);
}
#endif
