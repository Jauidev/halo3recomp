#pragma once
// diagnostics for headless runs. Every guest thread registers its PPCContext; with
// XPORT_DUMP_AFTER=<seconds> a watchdog prints where each one is (LR, CTR and guest backtrace),
// which shows what a stalled game is waiting for.

struct PPCContext;

void RegisterGuestContext(PPCContext* ctx);
void UnregisterGuestContext(PPCContext* ctx);
void StartThreadDumpWatchdog();
size_t GetGuestThreadCount();

// Prints the r1 back chain of a guest context (also used by the crash handler).
void PrintGuestBacktrace(const PPCContext* ctx);

// printf to stderr, and on Android to logcat as well (an app's stderr goes nowhere).
void DiagPrint(const char* format, ...);

// Crash report file: the path is set at startup; the crash handler opens it (truncated) and from
// then on DiagPrint copies everything there too, so the report survives without adb.
void DiagSetReportPath(const std::string& path);
void DiagOpenReportFile();

// Gives the calling thread an alternate signal stack so stack overflows still get reported
// (runtime/os/crash.cpp; no-op on Windows).
void PrepareCrashHandlerForThread();
