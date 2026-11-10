// the game's XAudio client loop. It calls the client 256 samples at 48 kHz at a time
// and hands the frames to the sound device (apu/output.cpp). With a device the loop follows its
// consumption (keeps audio_output::QueueTarget frames queued); without one, a timer at the real rate. The structure
// follows UnleashedRecomp's apu/driver/sdl2_driver.cpp.
#include <stdafx.h>
#include <apu/audio.h>
#include <apu/output.h>
#include <cpu/guest_thread.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <os/background.h>
#include <os/logger.h>
#include <os/thread_policy.h>

static PPCFunc* g_clientCallback{};
static uint32_t g_clientCallbackParam{}; // pointer in guest memory
static std::unique_ptr<std::thread> g_audioThread;
static std::atomic<bool> g_audioThreadShouldExit;
static bool g_audioDevice;

void XmaInitialize();

void XAudioInitializeSystem()
{
    // The game reads the XMA context table's address from the APU before creating contexts.
    XmaInitialize();
    g_audioDevice = audio_output::Open();
}

static void AudioThread()
{
    using namespace std::chrono_literals;

    NameCurrentThread("xport-audio");
    GuestThreadContext ctx(0);
    constexpr auto INTERVAL = 1000000000ns * XAUDIO_NUM_SAMPLES / XAUDIO_SAMPLES_HZ;
    auto next = std::chrono::steady_clock::now();
    // Every 10 s with a device: the gaps the device found and the game's slowest audio frames.
    auto nextReport = next + 10s;
    std::chrono::steady_clock::duration slowest{};
    uint32_t overFrame = 0; // client calls longer than the 5.3 ms frame they produce
    uint32_t frames = 0;    // client calls; 1875 in 10 s is the real rate
    uint64_t lastConsumed = 0;
    auto lastPull = next;   // when the device was last seen pulling

    while (!g_audioThreadShouldExit)
    {
        if (BackgroundHeld())
        {
            // The app is in the background (os/background.h): the game's audio clock stops, and
            // the device too, so neither thread wakes. The pacing starts over from now.
            audio_output::SetPaused(true);
            WaitWhileBackgroundHold("audio");
            audio_output::SetPaused(false);
            next = std::chrono::steady_clock::now();
            lastPull = next;
            lastConsumed = audio_output::Consumed();
        }
        ctx.ppcContext.r3.u32 = g_clientCallbackParam;
        const auto callStart = std::chrono::steady_clock::now();
        g_clientCallback(ctx.ppcContext, g_memory.base);
        const auto took = std::chrono::steady_clock::now() - callStart;
        slowest = std::max(slowest, took);
        overFrame += took > INTERVAL;
        frames++;
        if (g_audioDevice && callStart >= nextReport)
        {
            uint32_t pulls;
            uint64_t silentFrames;
            audio_output::TakeShortfalls(pulls, silentFrames);
            LOGFN("audio: {} game frames in 10 s (1875 = real time), {} short device pulls ({:.0f} ms of silence), slowest game audio frame {:.1f} ms, {} over 5.3 ms",
                frames, pulls, silentFrames * 1000.0 / XAUDIO_SAMPLES_HZ,
                std::chrono::duration<double, std::milli>(slowest).count(), overFrame);
            nextReport = callStart + 10s;
            slowest = {};
            overFrame = 0;
            frames = 0;
        }

        next += INTERVAL;
        if (g_audioDevice)
        {
            // Follow the device: keep QueueTarget frames queued. A stream that stops pulling
            // (paused or stalled) must not stop the game's audio clock: after two of the device's
            // periods without a pull the loop runs on the timer at the real rate, and the output
            // drops what does not fit. The old limit, four XAudio frames (21 ms), was shorter than
            // a deep-buffer period (107 ms) and let the game's audio run ahead between pulls.
            const auto stall = std::max<std::chrono::steady_clock::duration>(INTERVAL * 4,
                1000000000ns * audio_output::BurstFrames() * 2 / XAUDIO_SAMPLES_HZ);
            bool stalled = false;
            while (audio_output::Queued() > audio_output::QueueTarget())
            {
                const auto now = std::chrono::steady_clock::now();
                const uint64_t consumed = audio_output::Consumed();
                if (consumed != lastConsumed)
                {
                    lastConsumed = consumed;
                    lastPull = now;
                }
                else if (now - lastPull > stall)
                {
                    stalled = true;
                    break;
                }
                std::this_thread::sleep_for(1ms);
            }
            if (stalled)
                std::this_thread::sleep_until(next);
            else
                next = std::chrono::steady_clock::now();
        }
        else
        {
            std::this_thread::sleep_until(next);
        }
    }
}

void XAudioRegisterClient(PPCFunc* callback, uint32_t param)
{
    auto* pClientParam = static_cast<uint32_t*>(g_userHeap.Alloc(sizeof(param)));
    ByteSwapInplace(param);
    *pClientParam = param;
    g_clientCallbackParam = g_memory.MapVirtual(pClientParam);
    g_clientCallback = callback;

    g_audioThreadShouldExit = false;
    g_audioThread = std::make_unique<std::thread>(AudioThread);
}

void XAudioSubmitFrame(void* samples)
{
    audio_output::SubmitFrame(samples);
}
