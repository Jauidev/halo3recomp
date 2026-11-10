#include <stdafx.h>

#include <bit>

#include <apu/audio.h>
#include <apu/output.h>

#if defined(__ANDROID__)
#include <aaudio/AAudio.h>
#endif

namespace
{
    // Single producer (the audio thread) and single consumer (the device), in stereo frames.
    // 683 ms: it must hold the queue target, two device bursts. The deep-buffer stream Android
    // gives some sessions pulls 5130 frames (107 ms) at a time; with 8192 frames here the target
    // (10260) could never be reached, the audio thread never waited, and the game's audio ran as
    // fast as the CPU allowed with the excess dropped: sped-up sound and ~78% of a big core
    // (2026-10-27).
    constexpr uint32_t RING_FRAMES = 32768;
    float g_ring[RING_FRAMES * 2];
    std::atomic<uint64_t> g_written;
    std::atomic<uint64_t> g_read;
    std::atomic<uint32_t> g_queueTarget{XAUDIO_NUM_SAMPLES * 3}; // audio_output::QueueTarget
    std::atomic<uint32_t> g_burst{XAUDIO_NUM_SAMPLES};          // audio_output::BurstFrames
    std::atomic<uint32_t> g_shortPulls;   // device pulls that found fewer frames than they asked for
    std::atomic<uint64_t> g_silentFrames; // and the frames of silence they filled in

    uint32_t Pull(float* out, uint32_t frames)
    {
        const uint64_t read = g_read.load(std::memory_order_relaxed);
        const uint64_t available = g_written.load(std::memory_order_acquire) - read;
        const uint32_t count = uint32_t(std::min<uint64_t>(available, frames));
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t slot = uint32_t((read + i) % RING_FRAMES);
            out[i * 2] = g_ring[slot * 2];
            out[i * 2 + 1] = g_ring[slot * 2 + 1];
        }
        std::fill(out + count * 2, out + frames * 2, 0.0f);
        if (count < frames)
        {
            g_shortPulls.fetch_add(1, std::memory_order_relaxed);
            g_silentFrames.fetch_add(frames - count, std::memory_order_relaxed);
        }
        g_read.store(read + count, std::memory_order_release);
        return count;
    }

    FILE* g_dump;

#if defined(__ANDROID__)
    AAudioStream* g_stream;
    std::atomic<bool> g_reopen;

    aaudio_data_callback_result_t DataCallback(AAudioStream*, void*, void* audioData, int32_t numFrames)
    {
        Pull(static_cast<float*>(audioData), uint32_t(numFrames));
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    void ErrorCallback(AAudioStream*, void*, aaudio_result_t error)
    {
        // Device change (headphones, Bluetooth): the stream is dead; the audio thread reopens it
        // (it cannot be closed from this callback).
        LOGF_WARNING("AAudio stream error {}", AAudio_convertResultToText(error));
        g_reopen = true;
    }

    // XPORT_AUDIO_MODE: low asks for a low-latency stream, which Android gives as a fast track:
    // AudioFlinger's FastMixer and this process's callback thread wake every 256 frames (5.3 ms,
    // ~190 times a second; 84 ms of latency, 2026-10-26). normal (the default since 2026-10-26)
    // uses the ordinary mixer (20-40 ms periods, a few dozen wakeups a second, somewhat more
    // latency) and power the deep buffer path: fewer wakeups of two threads for the whole session,
    // at the cost of audio delay.
    aaudio_performance_mode_t PerformanceMode()
    {
        const char* mode = getenv("XPORT_AUDIO_MODE");
        if (mode && strcmp(mode, "low") == 0)
            return AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
        if (mode && strcmp(mode, "power") == 0)
            return AAUDIO_PERFORMANCE_MODE_POWER_SAVING;
        return AAUDIO_PERFORMANCE_MODE_NONE;
    }

    bool OpenStream()
    {
        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK)
            return false;
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount(builder, 2);
        AAudioStreamBuilder_setSampleRate(builder, XAUDIO_SAMPLES_HZ);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(builder, PerformanceMode());
        AAudioStreamBuilder_setDataCallback(builder, DataCallback, nullptr);
        AAudioStreamBuilder_setErrorCallback(builder, ErrorCallback, nullptr);
        const aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &g_stream);
        AAudioStreamBuilder_delete(builder);
        if (result != AAUDIO_OK)
        {
            LOGF_ERROR("AAudio open failed: {}", AAudio_convertResultToText(result));
            g_stream = nullptr;
            return false;
        }
        if (AAudioStream_requestStart(g_stream) != AAUDIO_OK)
        {
            AAudioStream_close(g_stream);
            g_stream = nullptr;
            return false;
        }
        const int32_t burst = AAudioStream_getFramesPerBurst(g_stream);
        g_burst = std::max<uint32_t>(XAUDIO_NUM_SAMPLES, burst > 0 ? uint32_t(burst) : 0);
        // Xcompiler (Halo 3 on the phone): at least ~96 ms queued (XPORT_AUDIO_QUEUE_MS). Two bursts
        // (43 ms) were less than the game's slowest audio frames there (35-60 ms while shaders
        // compile), so the device found the queue empty 10-20 times every 10 s: crackling sound.
        uint32_t queueMs = 96;
        if (const char* value = getenv("XPORT_AUDIO_QUEUE_MS"); value && *value)
            queueMs = uint32_t(std::clamp(atoi(value), 0, 500));
        const uint32_t queueFrames = uint32_t(uint64_t(queueMs) * XAUDIO_SAMPLES_HZ / 1000);
        g_queueTarget = std::min<uint32_t>(std::max<uint32_t>({XAUDIO_NUM_SAMPLES * 3, g_burst * 2, queueFrames}),
            RING_FRAMES - XAUDIO_NUM_SAMPLES * 4);
        LOGF_UTILITY("AAudio: {} Hz, burst {} frames, buffer {} frames, {} frames queued",
            AAudioStream_getSampleRate(g_stream), burst,
            AAudioStream_getBufferSizeInFrames(g_stream), g_queueTarget.load());
        return true;
    }
#endif

    void Push(const float* stereo, uint32_t frames)
    {
        if (g_dump != nullptr)
            fwrite(stereo, sizeof(float) * 2, frames, g_dump);

#if defined(__ANDROID__)
        if (g_reopen.exchange(false))
        {
            if (g_stream != nullptr)
                AAudioStream_close(g_stream);
            OpenStream();
        }
        if (g_stream == nullptr)
            return;
#else
        return;
#endif
        // Ring full (device stalled): drop the new audio rather than block the game.
        const uint64_t written = g_written.load(std::memory_order_relaxed);
        if (written - g_read.load(std::memory_order_acquire) + frames > RING_FRAMES)
            return;
        for (uint32_t i = 0; i < frames; i++)
        {
            const uint32_t slot = uint32_t((written + i) % RING_FRAMES);
            g_ring[slot * 2] = stereo[i * 2];
            g_ring[slot * 2 + 1] = stereo[i * 2 + 1];
        }
        g_written.store(written + frames, std::memory_order_release);
    }
}

bool audio_output::Open()
{
    if (const char* path = getenv("XPORT_AUDIO_DUMP"))
        g_dump = fopen(path, "wb");

#if defined(__ANDROID__)
    if (getenv("XPORT_AUDIO") != nullptr && strcmp(getenv("XPORT_AUDIO"), "0") == 0)
        return false;
    return OpenStream();
#else
    return false;
#endif
}

void audio_output::SubmitFrame(const void* samples)
{
    // 5.1 to stereo as Xenia does it (apu/conversion.h): (front + back + centre / 2) / 2.5; the
    // LFE channel is dropped.
    const auto* input = static_cast<const uint32_t*>(samples);
    auto channel = [input](uint32_t index, uint32_t sample)
    {
        return std::bit_cast<float>(ByteSwap(input[index * XAUDIO_NUM_SAMPLES + sample]));
    };
    float stereo[XAUDIO_NUM_SAMPLES * 2];
    for (uint32_t i = 0; i < XAUDIO_NUM_SAMPLES; i++)
    {
        const float centre = channel(2, i) * 0.5f;
        stereo[i * 2] = (channel(0, i) + channel(4, i) + centre) * 0.4f;
        stereo[i * 2 + 1] = (channel(1, i) + channel(5, i) + centre) * 0.4f;
    }
    Push(stereo, XAUDIO_NUM_SAMPLES);
}

uint32_t audio_output::Queued()
{
    return uint32_t(g_written.load(std::memory_order_acquire) - g_read.load(std::memory_order_acquire));
}

uint32_t audio_output::QueueTarget()
{
    return g_queueTarget.load(std::memory_order_relaxed);
}

uint32_t audio_output::BurstFrames()
{
    return g_burst.load(std::memory_order_relaxed);
}

uint64_t audio_output::Consumed()
{
    return g_read.load(std::memory_order_acquire);
}

void audio_output::TakeShortfalls(uint32_t& pulls, uint64_t& silentFrames)
{
    pulls = g_shortPulls.exchange(0, std::memory_order_relaxed);
    silentFrames = g_silentFrames.exchange(0, std::memory_order_relaxed);
}

void audio_output::SetPaused(bool paused)
{
#if defined(__ANDROID__)
    if (g_stream == nullptr)
        return;
    const aaudio_result_t result = paused ? AAudioStream_requestPause(g_stream) : AAudioStream_requestStart(g_stream);
    if (result != AAUDIO_OK)
        LOGF_WARNING("AAudio {} failed: {}", paused ? "pause" : "start", AAudio_convertResultToText(result));
#endif
}
