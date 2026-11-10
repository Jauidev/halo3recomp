#pragma once
// holds the game's threads while the Android app is in the background. A focus loss
// opens the title's pause menu (android_app.cpp), but the game kept running it: the render thread, the
// main thread behind it, the audio clock (and the AAudio stream) and the vblank interrupts, until
// Android's freezer stopped the process some seconds later. Now the activity's onStop asks for a
// hold; the render, audio and vblank threads wait at a point where they hold nothing, and the
// game's other threads wait at their next sleep or timed wait (kernel/imports.cpp). onStart
// releases them.
//
// On by default; files/bg_pause = 0 turns it off (read each time the app goes to the background).

// The activity stopped: a ticket for EngageBackgroundHold.
uint32_t RequestBackgroundHold();

// Holds the game if nothing released it since the request (false: released already, or off).
bool EngageBackgroundHold(uint32_t ticket);

// The activity started again: lets the held threads go and cancels a pending request.
void ReleaseBackgroundHold();

// Whether the game is held (a cheap check for loops that do work around the wait).
bool BackgroundHeld();

// Waits while the game is held; true if it waited. `who` names the thread in the log (nullptr:
// only counted in the release's line).
bool WaitWhileBackgroundHold(const char* who);
