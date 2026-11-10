// does Xenia's logger start on its own on the PC? (Diagnostics for the PC build of the
// Xenia GPU: inside the game's .exe, xe::InitializeLogging never returned.)
#include <windows.h>

#include <cstdio>

#include "xenia/base/logging.h"

static LONG WINAPI OnException(EXCEPTION_POINTERS* info) {
  const auto* record = info->ExceptionRecord;
  if (record->ExceptionCode == 0x406D1388 || record->ExceptionCode < 0x80000000) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  HMODULE module = GetModuleHandleW(nullptr);
  fprintf(stderr, "exception 0x%08lX at +0x%llX, address 0x%llX\n",
          record->ExceptionCode,
          (unsigned long long)(uintptr_t(record->ExceptionAddress) - uintptr_t(module)),
          record->NumberParameters >= 2 ? (unsigned long long)record->ExceptionInformation[1] : 0ull);
  void* frames[32];
  const USHORT count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
  for (USHORT i = 0; i < count; ++i) {
    fprintf(stderr, "  #%02u +0x%llX\n", i, (unsigned long long)(uintptr_t(frames[i]) - uintptr_t(module)));
  }
  fflush(stderr);
  return EXCEPTION_CONTINUE_SEARCH;
}

int main() {
  AddVectoredExceptionHandler(1, OnException);
  fprintf(stderr, "before InitializeLogging\n");
  fflush(stderr);
  try {
    xe::InitializeLogging("xenia-log-test");
  } catch (const std::exception& e) {
    fprintf(stderr, "C++ exception: %s\n", e.what());
    fflush(stderr);
    return 1;
  }
  fprintf(stderr, "after InitializeLogging\n");
  fflush(stderr);
  XELOGI("hello from the Xenia logger");
  xe::ShutdownLogging();
  fprintf(stderr, "done\n");
  return 0;
}
