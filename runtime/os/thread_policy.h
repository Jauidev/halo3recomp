#pragma once
// names and core placement of host threads. The game names its threads through the
// debugger exception 0x406D1388 (RtlRaiseException); the runtime names its own. On Android the
// name becomes the thread's comm (visible in top -H and traces) and selects a CPU set for the
// Dimensity 8300: cores 0-3 Cortex-A510, 4-6 Cortex-A715, 7 the fastest A715. The policy (a
// runtime thread that measures the game's threads and places them) is in thread_policy.cpp.

// Called on every thread that runs guest code or works for it, from that thread.
void RegisterHostThread(uint32_t guestThreadId);

// The game names threads by handle as well as by id.
void RegisterHostThreadHandle(uint32_t handle);
void RegisterHostThreadHandle(uint32_t handle, uint32_t guestThreadId);

// Names a thread ExCreateThread just created, which may not have registered yet (it registers when
// it first runs, and suspended threads run at NtResumeThread).
void NameGuestThreadAtStart(uint32_t guestThreadId, const char* name);

// threadId: a guest thread id, or 0xFFFFFFFF for the calling thread.
void SetGuestThreadName(uint32_t threadId, const char* name);

// A game that does not name its threads: the runtime names the thread it sees presenting frames
// (XpRenderThread) or submitting audio (XpSoundThread), once, unless the game named it already.
void NameUnnamedCurrentThread(const char* name);

// Runtime threads (fake GPU, audio...): names the calling thread and applies the policy.
void NameCurrentThread(const char* name);
