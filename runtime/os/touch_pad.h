#pragma once

#include <cstdint>

// the state of the virtual pad drawn by the Android app (touch controls,
// android/src/com/xport/TouchControls.java). XamInputGetState reports it as pad 0; the
// app sets it through JNI (MainActivity.nativeSetPad) on every change.
namespace os::touch_pad
{
    struct State
    {
        // XAMINPUT_GAMEPAD_* bits.
        uint16_t buttons = 0;
        uint8_t leftTrigger = 0;
        uint8_t rightTrigger = 0;
        int16_t thumbLX = 0;
        int16_t thumbLY = 0;
        int16_t thumbRX = 0;
        int16_t thumbRY = 0;
    };

    void Set(const State& state);
    // Holds buttons (XAMINPUT_GAMEPAD_*) down for the given time on top of the touch state,
    // e.g. START to open the pause menu when the app loses focus.
    void Press(uint16_t buttons, uint32_t milliseconds);
    State Get();
}
