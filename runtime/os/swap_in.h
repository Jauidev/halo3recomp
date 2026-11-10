#pragma once
// brings the game's memory back from swap on a background thread (Android). With the
// phone's RAM full (other apps, 4.6 GB in zram on 2026-10-27), the kernel swaps out the game's
// pages too: 233 MB of the 512 MB guest physical memory and ~300 MB of the native heap. A thread
// that touches one of them stops until the kernel reads it back. Driving, the game took ~780 major
// faults per second, XpRenderThread 157/s and xport-main 75/s, nearly all swap-ins (pswpin). The
// thread asks for those reads ahead with MADV_WILLNEED (the read-ahead an OPPO kernel patch made
// for apps coming back to the foreground), so the game's threads find the pages in the swap cache
// (a minor fault).
//
// Off by default; files/swap_in = 1 turns it on, live. On the phone, hot (2026-10-27, 15 s riding
// each way): it cut the faults of XpRenderThread 3519 -> 484, XpRecorder 2098 -> 47 and
// XpSoundThread 1083 -> 49, but cost ~40% of a little core (the kernel read ~1 GB back from swap
// per 15 s and swapped it out again) and the worst frames got longer (recorder max 211-239 ms
// against 64-122 ms): madvise holds the address space lock while it reads, and the game's page
// faults queue behind it. No frame rate gain.

// From the Android entry point, once guest memory exists. Does nothing on other systems.
void StartSwapInThread();
