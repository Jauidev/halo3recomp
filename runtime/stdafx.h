#pragma once
// Common includes for the the title (the title) runtime. Derived from UnleashedRecomp's stdafx.h with the
// UI, SDL, imgui and graphics dependencies removed.

#define NOMINMAX

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#include <sys/mman.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <semaphore>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <xbox.h>
#include <xxhash.h>
#include <ankerl/unordered_dense.h>
#include <o1heap.h>
#include <fmt/core.h>
#include <ppc_recomp_shared.h>

#include "framework.h"
#include "mutex.h"
#include "os/logger.h"
