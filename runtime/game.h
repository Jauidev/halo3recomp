#pragma once

// What the runtime asks of the game being built. One implementation per game: games/<title>/game.cpp.
#include <cstdint>

class Memory;

namespace game
{
    // Shown on the status screen.
    const char* Title();
    // Entry points the recompiler did not put in its function table.
    void RegisterExtraFunctions(Memory& memory);
    // A guest thread is starting: the name that keeps it on the big cores (os/thread_policy.cpp),
    // or null. entry is the function it really runs, for engines that start threads through a wrapper.
    const char* ThreadName(uint32_t startAddress, uint32_t startContext, uint32_t& entry);
    // The image was just loaded: changes it needs before the first instruction runs.
    void AfterImageLoad();
    // Renderer options it needs, "name=value,..." like XNATIVE_CVARS (which comes after and
    // wins). Empty for none.
    const char* RendererOptions();
    // True when the game's own pause menu is open, so losing focus does not press START again.
    bool PauseMenuOpen();
}
