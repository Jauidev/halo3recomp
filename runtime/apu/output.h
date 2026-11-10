#pragma once
// the sound device. The game's XAudio frames (6 channels, 256 samples at 48 kHz, see
// apu/audio.h) are mixed down to stereo and queued here; the device pulls them from its own
// thread. Android: AAudio. Elsewhere there is no device; XPORT_AUDIO_DUMP=<file> writes the stereo
// stream as raw 32-bit float PCM (48 kHz, 2 channels) to check the decoding on the PC.

namespace audio_output
{
    // false: no device (the caller paces the game's audio clock with a timer instead).
    bool Open();

    // 256 samples per channel, channel after channel, big-endian floats (XAudio's layout).
    void SubmitFrame(const void* samples);

    // Stereo frames waiting for the device.
    uint32_t Queued();

    // How many frames the game's audio thread keeps queued: three XAudio frames, or two of the
    // device's bursts when those are bigger (the normal and power-saving streams pull ~20 ms at a
    // time; with three frames queued every pull would come up short).
    uint32_t QueueTarget();

    // Frames the device pulls at a time.
    uint32_t BurstFrames();

    // Frames the device has pulled so far; it stops growing while the stream is stalled.
    uint64_t Consumed();

    // Since the last call: the device's pulls that found the queue short (heard as gaps or
    // crackling) and the frames of silence they filled in.
    void TakeShortfalls(uint32_t& pulls, uint64_t& silentFrames);

    // Stops or restarts the device's pulls (the app in the background). The audio thread only.
    void SetPaused(bool paused);
}
