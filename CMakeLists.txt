cmake_minimum_required(VERSION 3.20)
project(xport C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(XENON_RECOMP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/XenonRecomp)
set(XENON_UTILS_ROOT ${XENON_RECOMP_ROOT}/XenonUtils)
set(XENON_THIRDPARTY ${XENON_RECOMP_ROOT}/thirdparty)
set(XPORT_RUNTIME_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
# The game being built: its addresses and fixes (games/<title>), the rest is shared.
set(XPORT_GAME gta4 CACHE STRING "Folder under games/")
set(XPORT_GAME_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/games/${XPORT_GAME})
# Its shaders: generated and large.
set(XPORT_GAME_DATA ${CMAKE_CURRENT_SOURCE_DIR}/games/${XPORT_GAME}/data)
include(${CMAKE_CURRENT_SOURCE_DIR}/games/${XPORT_GAME}/game.cmake)
# Its recompiled code (XenonRecomp output).
set(XPORT_PPC_ROOT ${XPORT_GAME_ROOT}/ppc)
# What the runtime and the launcher need to know about it.
add_compile_definitions(XPORT_TITLE_ID=${XPORT_TITLE_ID} XPORT_XEX_MEDIA=${XPORT_XEX_MEDIA}
    XPORT_XEX_VERSION=${XPORT_XEX_VERSION})
# The launcher's own colours, when game.cmake sets them: only on launcher_ui.cpp (a global
# definition would rebuild every file, the recompiled code too), outside the precompiled header.
if (DEFINED XPORT_LAUNCHER_ACCENT AND DEFINED XPORT_LAUNCHER_TINT)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/runtime/os/launcher_ui.cpp PROPERTIES
        COMPILE_DEFINITIONS "XPORT_LAUNCHER_ACCENT=${XPORT_LAUNCHER_ACCENT};XPORT_LAUNCHER_TINT=${XPORT_LAUNCHER_TINT}"
        SKIP_PRECOMPILE_HEADERS ON)
endif()

if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # clang-cl on the PC (debug host). The MSVC STL in Build Tools 17.14 wants clang 19; the NDK has 18.
    add_compile_definitions(_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH _CRT_SECURE_NO_WARNINGS NOMINMAX)
    add_compile_options(/EHsc -march=x86-64-v2)
    set(XPORT_FP_STRICT /fp:strict)
else()
    set(XPORT_FP_STRICT -ffp-model=strict)
endif()
# since 0.7.0D one APK for every phone, xport.apk (android/AndroidManifest.xml,
# build/arm64): its launcher (runtime/os/launcher.h) tells Mali from Adreno, and on Adreno
# libadrenotools can load Mesa's Turnip or an imported driver (runtime/os/vk_driver.cpp).
# XPORT_SNAPDRAGON only changes the CPU tuning (the old xportsnap.apk line, build/arm64-snap,
# tools/build-android.sh snap, tools/build-apk.sh snap), for comparisons on the Snapdragon 680.
option(XPORT_SNAPDRAGON "Android: CPU tuning for the Snapdragon 680 (Cortex-A73, inline atomics)" OFF)
if (ANDROID AND XPORT_SNAPDRAGON)
    add_compile_definitions(XPORT_SNAPDRAGON=1)
    # Snapdragon 680 (Redmi Note 11): Cortex-A73 + A53, ARMv8.0 without LSE, so atomics are inline
    # LL/SC loops instead of outline helpers that test for LSE on every call.
    add_compile_options(-march=armv8-a -mno-outline-atomics -mtune=cortex-a73)
elseif (ANDROID)
    # the NDK's ARMv8.0 baseline, so the build runs on both test phones. ARMv8.2 (LSE,
    # fp16, dotprod, 2026-10-26 to 0.6.1D) died with SIGILL on an ldadd while loading on the Redmi
    # Note 11 (Snapdragon 680: Cortex-A73 + A53, ARMv8.0). Outline atomics keep LSE on the
    # Dimensity 8300 (Cortex-A715 + A510): each atomic read-modify-write calls a __aarch64_ldadd*/
    # cas* helper that tests for LSE at run time. Tuned for the A715. Floating-point semantics are
    # the same either way.
    add_compile_options(-march=armv8-a -moutline-atomics -mtune=cortex-a715)
endif()

# Recompiled game code (XenonRecomp output). Regenerate with tools/recompile.bat.
file(GLOB XPORT_PPC_SOURCES CONFIGURE_DEPENDS ${XPORT_PPC_ROOT}/ppc_recomp.*.cpp)

add_library(xportppc STATIC ${XPORT_PPC_SOURCES} ${XPORT_PPC_ROOT}/ppc_func_mapping.cpp)
# Same flags UnleashedRecomp uses for its recompiled library: the generated code type-puns
# through PPCRegister and relies on strict IEEE behaviour.
target_compile_options(xportppc PRIVATE -fno-strict-aliasing ${XPORT_FP_STRICT})
target_include_directories(xportppc PUBLIC ${XPORT_PPC_ROOT} ${XENON_THIRDPARTY}/simde)
# guest_mmio.h first: it redefines PPC_MM_STORE_U32 to forward the APU register writes.
target_precompile_headers(xportppc PRIVATE ${XPORT_RUNTIME_ROOT}/apu/guest_mmio.h ${XPORT_PPC_ROOT}/ppc_recomp_shared.h)

# Title DLLs recompiled like the game (game.cmake's XPORT_EXTRA_PPC_DIRS, written by
# tools/slot_dlls.py: Halo 3's Waves audio DLLs), one library each with its own declarations. Their
# ppc_config.h is the game's, so their indirect calls use the game's function table.
set(XPORT_EXTRA_PPC_LIBRARIES)
foreach (XPORT_EXTRA_DIR IN LISTS XPORT_EXTRA_PPC_DIRS)
    get_filename_component(XPORT_EXTRA_NAME ${XPORT_EXTRA_DIR} NAME)
    file(GLOB XPORT_EXTRA_SOURCES CONFIGURE_DEPENDS ${XPORT_EXTRA_DIR}/ppc_recomp.*.cpp)
    add_library(xportppc_${XPORT_EXTRA_NAME} STATIC ${XPORT_EXTRA_SOURCES} ${XPORT_EXTRA_DIR}/ppc_func_mapping.cpp)
    target_compile_options(xportppc_${XPORT_EXTRA_NAME} PRIVATE -fno-strict-aliasing ${XPORT_FP_STRICT})
    target_include_directories(xportppc_${XPORT_EXTRA_NAME} PRIVATE ${XPORT_EXTRA_DIR} ${XENON_THIRDPARTY}/simde)
    target_precompile_headers(xportppc_${XPORT_EXTRA_NAME} PRIVATE ${XPORT_RUNTIME_ROOT}/apu/guest_mmio.h ${XPORT_EXTRA_DIR}/ppc_recomp_shared.h)
    list(APPEND XPORT_EXTRA_PPC_LIBRARIES xportppc_${XPORT_EXTRA_NAME})
endforeach()

# Link probe: a shared library with --no-undefined lists every symbol the runtime must provide.
# Built on demand (Android/Linux linker only): cmake --build <dir> --target xportppc_linkprobe
if (NOT WIN32)
    add_library(xportppc_linkprobe SHARED EXCLUDE_FROM_ALL ${CMAKE_CURRENT_SOURCE_DIR}/tools/linkprobe.cpp)
    target_link_libraries(xportppc_linkprobe PRIVATE -Wl,--whole-archive xportppc -Wl,--no-whole-archive)
    target_link_options(xportppc_linkprobe PRIVATE -Wl,--no-undefined -Wl,--error-limit=0)
endif()

# Third-party pieces of the runtime: XEX loading (AES + LZX), fmt, xxHash, o1heap.
add_library(xportdeps STATIC
    ${XENON_UTILS_ROOT}/xex.cpp
    ${XENON_UTILS_ROOT}/image.cpp
    ${XENON_UTILS_ROOT}/xex_patcher.cpp
    ${XENON_UTILS_ROOT}/memory_mapped_file.cpp
    ${XENON_THIRDPARTY}/tiny-AES-c/aes.c
    ${XENON_THIRDPARTY}/libmspack/libmspack/mspack/lzxd.c
    ${XENON_THIRDPARTY}/fmt/src/format.cc
    ${XENON_THIRDPARTY}/xxHash/xxhash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/o1heap/o1heap/o1heap.c)
target_include_directories(xportdeps
    PUBLIC
        ${XENON_UTILS_ROOT}
        ${XENON_THIRDPARTY}/simde
        ${XENON_THIRDPARTY}/fmt/include
        ${XENON_THIRDPARTY}/xxHash
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/o1heap/o1heap
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/unordered_dense/include
    PRIVATE
        ${XENON_THIRDPARTY}/libmspack/libmspack/mspack
        ${XENON_THIRDPARTY}/tiny-AES-c
        ${XENON_THIRDPARTY}/TinySHA1)

# o1heap counts leading zeros of size_t with __builtin_clzl, which is 32-bit on Windows (LLP64)
# and corrupts its bins; force the 64-bit builtin everywhere.
target_compile_definitions(xportdeps PRIVATE O1HEAP_CLZ=__builtin_clzll)

# The runtime: kernel/XAM HLE, memory, threads, file system. Headless until phase 4.
file(GLOB_RECURSE XPORT_RUNTIME_SOURCES CONFIGURE_DEPENDS ${XPORT_RUNTIME_ROOT}/*.cpp)
add_executable(xport ${XPORT_RUNTIME_SOURCES} ${XPORT_GAME_ROOT}/game.cpp)
# The PC build is named after the game: build/x64-<game>/<game>.exe.
set_target_properties(xport PROPERTIES OUTPUT_NAME ${XPORT_GAME})
target_include_directories(xport PRIVATE ${XPORT_RUNTIME_ROOT} ${XPORT_GAME_ROOT})
target_link_libraries(xport PRIVATE xportppc ${XPORT_EXTRA_PPC_LIBRARIES} xportdeps)
target_precompile_headers(xport PRIVATE ${XPORT_RUNTIME_ROOT}/stdafx.h)
# Xenia's GPU (phase 4): cmake/xenia.cmake. On the PC it runs offscreen to compare with the phone.
include(cmake/xenia.cmake)
# Native renderer (LibertyRecomp's xnative on ReXGlue) and its D3D hooks, in progress.
include(cmake/rexglue.cmake)
if (NOT XPORT_NATIVE_HOOKS)
    target_compile_definitions(xport PRIVATE XPORT_NO_NATIVE_RENDERER)
endif()
# The hooks replace weak recompiled functions, so nothing references them: link the whole archive.
if (WIN32)
    target_link_libraries(xport PRIVATE ${REX_HOOK_LIBRARIES})
    # xenia_ui_vulkan and rex_ui each compile VulkanMemoryAllocator; the static libraries only clash
    # on the PC's linker (the APK's lld takes the first).
    target_link_options(xport PRIVATE /WHOLEARCHIVE:rex_xd3dhooks.lib /FORCE:MULTIPLE)
    # DXC for the runtime shader translator, next to the exe.
    add_custom_command(TARGET xport POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${XNATIVE_DXC_DLL} $<TARGET_FILE_DIR:xport>)
endif()
target_include_directories(xport PRIVATE ${XENIA_GLUE} ${REX_GLUE})
if (WIN32)
    target_link_libraries(xport PRIVATE ${XENIA_LIBRARIES})
endif()
if (ANDROID)
    target_link_libraries(xport PRIVATE log aaudio)
    target_link_libraries(xport PRIVATE -Wl,--start-group ${XENIA_LIBRARIES} -Wl,--end-group)
    target_link_libraries(xport PRIVATE -Wl,--whole-archive rex_xd3dhooks -Wl,--no-whole-archive
        -Wl,--start-group ${REX_LIBRARIES} -Wl,--end-group)

    # The Android app (android/, tools/build-apk.sh) loads the same runtime as libgame.so and
    # starts it through JNI (runtime/os/android_app.cpp) instead of main().
    add_library(xportapp SHARED ${XPORT_RUNTIME_SOURCES} ${XPORT_GAME_ROOT}/game.cpp)
    set_target_properties(xportapp PROPERTIES OUTPUT_NAME game)
    target_compile_definitions(xportapp PRIVATE XPORT_SHARED)
    if (NOT XPORT_NATIVE_HOOKS)
        target_compile_definitions(xportapp PRIVATE XPORT_NO_NATIVE_RENDERER)
    endif()
    target_include_directories(xportapp PRIVATE ${XPORT_RUNTIME_ROOT} ${XPORT_GAME_ROOT})
    # The title DLLs (Halo 3's Waves audio) as in the PC build: without them libgame.so did not link.
    target_link_libraries(xportapp PRIVATE xportppc ${XPORT_EXTRA_PPC_LIBRARIES} xportdeps log aaudio)
    # libadrenotools (runtime/os/vk_driver.cpp) loads Mesa's Turnip on Adreno phones. Its hook
    # libraries (libhook_impl, libmain_hook, libfile_redirect_hook, libgsl_alloc_hook) are built
    # with the app and packaged by tools/build-apk.sh. Its CMake checks the ABI in
    # CMAKE_ANDROID_ARCH_ABI, which the NDK's legacy toolchain leaves unset.
    if (NOT CMAKE_ANDROID_ARCH_ABI)
        set(CMAKE_ANDROID_ARCH_ABI ${ANDROID_ABI})
    endif()
    add_subdirectory(third_party/libadrenotools EXCLUDE_FROM_ALL)
    target_link_libraries(xport PRIVATE adrenotools)
    target_link_libraries(xportapp PRIVATE adrenotools)
    add_dependencies(xportapp hook_impl main_hook file_redirect_hook gsl_alloc_hook)
    # The launcher (runtime/os/launcher.cpp, launcher_ui.cpp): Xenia's Dear ImGui (xe_imgui) drawn
    # with its OpenGL ES 3 backend on an EGL context, stb_image for the backgrounds.
    set(XPORT_IMGUI_GL ${XENIA_TP}/imgui/backends/imgui_impl_opengl3.cpp)
    target_sources(xportapp PRIVATE ${XPORT_IMGUI_GL})
    set_source_files_properties(${XPORT_IMGUI_GL} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(xportapp PRIVATE ${XENIA_TP}/imgui ${XENIA_TP}/imgui/backends ${XENIA_TP}/stb)
    target_link_libraries(xportapp PRIVATE EGL GLESv3 android)
    target_include_directories(xportapp PRIVATE ${XENIA_GLUE} ${REX_GLUE})
    target_link_libraries(xportapp PRIVATE -Wl,--start-group ${XENIA_LIBRARIES} -Wl,--end-group)
    target_link_libraries(xportapp PRIVATE -Wl,--whole-archive rex_xd3dhooks -Wl,--no-whole-archive
        -Wl,--start-group ${REX_LIBRARIES} -Wl,--end-group)
    target_precompile_headers(xportapp PRIVATE ${XPORT_RUNTIME_ROOT}/stdafx.h)
    target_compile_options(xportapp PRIVATE -g)
    # The recompiled functions are weak (PPC_WEAK_FUNC) and every function of a shared library can
    # be interposed by default, so ~28,700 of them, 22,840 guest functions among them, were called
    # through PLT stubs (GOT load + indirect branch on every guest call). Nothing interposes this
    # library: bind its function references to its own definitions so they become direct calls.
    target_link_options(xportapp PRIVATE -Wl,-Bsymbolic-functions)
endif()
# Debug info for the runtime only (the recompiled library would be enormous): crash offsets
# printed by runtime/os/crash.cpp are symbolized with llvm-symbolizer.
if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(xport PRIVATE /Z7)
    target_compile_options(xportdeps PRIVATE /Z7)
    target_link_options(xport PRIVATE /DEBUG)
    target_link_libraries(xport PRIVATE dbghelp)
else()
    target_compile_options(xport PRIVATE -g)
endif()
