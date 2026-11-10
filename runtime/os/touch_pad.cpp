#include <stdafx.h>
#include <os/touch_pad.h>

namespace
{
    std::mutex g_mutex;
    os::touch_pad::State g_state;
    uint16_t g_pressed;
    std::chrono::steady_clock::time_point g_pressedUntil;
}

void os::touch_pad::Set(const State& state)
{
    std::lock_guard lock(g_mutex);
    g_state = state;
}

void os::touch_pad::Press(uint16_t buttons, uint32_t milliseconds)
{
    std::lock_guard lock(g_mutex);
    g_pressed = buttons;
    g_pressedUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
}

os::touch_pad::State os::touch_pad::Get()
{
    std::lock_guard lock(g_mutex);
    State state = g_state;
    if (g_pressed && std::chrono::steady_clock::now() < g_pressedUntil)
        state.buttons |= g_pressed;
    return state;
}
