# Xenia's GPU (BSD, third_party/xenia-gpu, xenia-canary) built as libraries for the
# runtime: xenia-base, the UI pieces the Vulkan presenter needs, xenia-ui-vulkan, xenia-gpu and
# xenia-gpu-vulkan, plus the third-party code they use. Xenia's own CMake does not know Android
# (it built Android with premake), so the source lists are made here with the same rules.
#
# Xenia's CPU, kernel and emulator are not used: xenia_glue/ adapts xe::Memory, KernelState,
# XHostThread and GraphicsSystem to this runtime. Its shim headers (xenia_glue/shim) come before
# Xenia's sources in the include path, so "xenia/memory.h" & co. resolve to them.
#
# Built-in shaders: py -3 tools/build-xenia-shaders.py (writes shaders/bytecode/vulkan_spirv).

set(XENIA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xenia-gpu)
set(XENIA_SRC ${XENIA_ROOT}/src/xenia)
set(XENIA_TP ${XENIA_ROOT}/third_party)
set(XENIA_GLUE ${CMAKE_CURRENT_SOURCE_DIR}/xenia_glue)

# Platform files (Xenia's suffix convention): Android on the phone; Windows x64 on the PC, where the
# same GPU runs offscreen to compare with the phone (XPORT_GPU=xenia on build/x64-<game>/<game>.exe).
if (WIN32)
    set(XENIA_DROP_SUFFIXES _posix _android _linux _gnulinux _x11 _gtk _mac _arm64)
else()
    set(XENIA_DROP_SUFFIXES _win _linux _gnulinux _x11 _gtk _mac _amd64 _x64)
endif()

function(xenia_sources out_var dir)
    file(GLOB _all ${dir}/*.cc ${dir}/*.cpp)
    set(_kept)
    foreach(_src ${_all})
        get_filename_component(_name ${_src} NAME_WE)
        if (_name MATCHES "_(main|test|demo)$")
            continue()
        endif()
        set(_drop FALSE)
        foreach(_suffix ${XENIA_DROP_SUFFIXES})
            if (_name MATCHES "${_suffix}$")
                set(_drop TRUE)
            endif()
        endforeach()
        if (NOT _drop)
            list(APPEND _kept ${_src})
        endif()
    endforeach()
    set(${out_var} ${_kept} PARENT_SCOPE)
endfunction()

set(XENIA_INCLUDES
    ${XENIA_GLUE}/shim
    ${XENIA_ROOT}
    ${XENIA_ROOT}/src
    ${XENIA_TP}
    ${XENIA_TP}/Vulkan-Headers/include
    ${XENIA_TP}/glslang
    ${XENIA_TP}/fmt/include
    ${XENIA_TP}/imgui
    ${CMAKE_CURRENT_BINARY_DIR}/xenia-generated)
set(XENIA_DEFINES
    XE_OPTION_PROFILING=0
    XE_OPTION_PROFILING_UI=0
    VULKAN_HPP_NO_TO_STRING
    IMGUI_DISABLE_DEFAULT_FONT
    USE_CPP17
    _LIB)
if (WIN32)
    list(APPEND XENIA_DEFINES VK_USE_PLATFORM_WIN32_KHR WIN32 _WIN64=1 _AMD64=1 UNICODE _UNICODE
        _CRT_NONSTDC_NO_DEPRECATE _CRT_SECURE_NO_WARNINGS NOMINMAX IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
else()
    list(APPEND XENIA_DEFINES VK_USE_PLATFORM_ANDROID_KHR)
endif()

function(xenia_library name)
    add_library(${name} STATIC ${ARGN})
    target_include_directories(${name} PRIVATE ${XENIA_INCLUDES})
    target_compile_definitions(${name} PRIVATE ${XENIA_DEFINES})
    target_compile_options(${name} PRIVATE -Wno-everything -fno-strict-aliasing)
    if (WIN32)
        target_compile_options(${name} PRIVATE -march=haswell /EHsc)
    endif()
endfunction()

# Third-party code.
xenia_library(xe_fmt ${XENIA_TP}/fmt/src/format.cc ${XENIA_TP}/fmt/src/os.cc)
file(GLOB XENIA_SPIRV_SOURCES ${XENIA_TP}/glslang/SPIRV/*.cpp)
list(FILTER XENIA_SPIRV_SOURCES EXCLUDE REGEX "GlslangToSpv\\.cpp$|SpvTools\\.cpp$")
xenia_library(xe_glslang_spirv ${XENIA_SPIRV_SOURCES})
xenia_library(xe_imgui
    ${XENIA_TP}/imgui/imgui.cpp
    ${XENIA_TP}/imgui/imgui_draw.cpp
    ${XENIA_TP}/imgui/imgui_tables.cpp
    ${XENIA_TP}/imgui/imgui_widgets.cpp)
target_include_directories(xe_imgui PRIVATE ${XENIA_TP}/stb)
xenia_library(xe_snappy
    ${XENIA_TP}/snappy/snappy-sinksource.cc
    ${XENIA_TP}/snappy/snappy-stubs-internal.cc
    ${XENIA_TP}/snappy/snappy.cc)
# snappy's CMake generates this header; same values for bionic (sys/uio.h exists).
if (WIN32)
    set(HAVE_SYS_UIO_H_01 0)
else()
    set(HAVE_SYS_UIO_H_01 1)
endif()
set(PROJECT_VERSION_MAJOR 1)
set(PROJECT_VERSION_MINOR 2)
set(PROJECT_VERSION_PATCH 2)
configure_file(${XENIA_TP}/snappy/snappy-stubs-public.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/xenia-generated/snappy-stubs-public.h)
# Xenia's build writes version.h from git; the trace writer stores it in trace files.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/xenia-generated/version.h.in
"#define XE_BUILD_BRANCH \"xport\"
#define XE_BUILD_COMMIT \"e6cd2e4\"
#define XE_BUILD_COMMIT_SHORT \"e6cd2e4\"
#define XE_BUILD_DATE \"2026-10-23\"
#define XE_BUILD_PR_NUMBER \"0\"
")
configure_file(${CMAKE_CURRENT_BINARY_DIR}/xenia-generated/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/xenia-generated/version.h COPYONLY)
xenia_library(xe_dxbc ${XENIA_TP}/dxbc/DXBCChecksum.cpp)

# Xenia proper.
xenia_sources(XENIA_BASE_SOURCES ${XENIA_SRC}/base)
list(FILTER XENIA_BASE_SOURCES EXCLUDE REGEX "console_app_main_|main_init_")
xenia_library(xenia_base ${XENIA_BASE_SOURCES})

# UI: only what the Vulkan presenter and the GPU need (no windows, menus or file pickers).
xenia_library(xenia_ui
    ${XENIA_SRC}/ui/graphics_upload_buffer_pool.cc
    ${XENIA_SRC}/ui/graphics_util.cc
    ${XENIA_SRC}/ui/presenter.cc
    ${XENIA_SRC}/ui/immediate_drawer.cc
    $<IF:$<BOOL:${WIN32}>,${XENIA_SRC}/ui/surface_win.cc,${XENIA_SRC}/ui/surface_android.cc>
    ${XENIA_SRC}/ui/window.cc
    ${XENIA_SRC}/ui/windowed_app_context.cc
    ${XENIA_SRC}/ui/renderdoc_api.cc)

xenia_sources(XENIA_UI_VULKAN_SOURCES ${XENIA_SRC}/ui/vulkan)
xenia_sources(XENIA_UI_VULKAN_FUNCTION_SOURCES ${XENIA_SRC}/ui/vulkan/functions)
xenia_library(xenia_ui_vulkan ${XENIA_UI_VULKAN_SOURCES} ${XENIA_UI_VULKAN_FUNCTION_SOURCES})

xenia_sources(XENIA_GPU_SOURCES ${XENIA_SRC}/gpu)
# graphics_system.cc is replaced by xenia_glue (it is built around Xenia's emulator); the trace
# tools are not needed.
list(FILTER XENIA_GPU_SOURCES EXCLUDE REGEX "/(graphics_system|trace_viewer|trace_player|trace_dump|shader_compiler_main|dxbc_[a-z_]+)\\.cc$")
xenia_library(xenia_gpu ${XENIA_GPU_SOURCES})

xenia_sources(XENIA_GPU_VULKAN_SOURCES ${XENIA_SRC}/gpu/vulkan)
list(FILTER XENIA_GPU_VULKAN_SOURCES EXCLUDE REGEX "trace_")
xenia_library(xenia_gpu_vulkan ${XENIA_GPU_VULKAN_SOURCES})

# APU: only the XMA decoder contexts, with FFmpeg (cmake/ffmpeg.cmake). Xenia's XmaDecoder, audio
# system and drivers are not used: xenia_glue/apu.cc drives the contexts for the runtime.
include(${CMAKE_CURRENT_LIST_DIR}/ffmpeg.cmake)
xenia_library(xenia_apu
    ${XENIA_SRC}/apu/xma_context.cc
    ${XENIA_SRC}/apu/xma_context_new.cc
    ${XENIA_SRC}/apu/xma_context_fake.cc)
target_include_directories(xenia_apu PRIVATE ${XENIA_TP}/ffmpeg-xenia ${XENIA_TP}/FFmpeg)

# The glue: adapters and the runtime-facing interfaces (xenia_glue/xport_gpu.h, xport_apu.h).
file(GLOB XENIA_GLUE_SOURCES CONFIGURE_DEPENDS ${XENIA_GLUE}/*.cc)
xenia_library(xenia_glue ${XENIA_GLUE_SOURCES})
target_compile_options(xenia_glue PRIVATE -Wno-everything)
target_include_directories(xenia_glue PRIVATE ${XENIA_TP}/ffmpeg-xenia ${XENIA_TP}/FFmpeg)

set(XENIA_LIBRARIES
    xenia_glue xenia_apu libavcodec libavutil xenia_gpu_vulkan xenia_gpu xenia_ui_vulkan
    xenia_ui xenia_base xe_glslang_spirv xe_imgui xe_snappy xe_dxbc xe_fmt)
if (WIN32)
    list(APPEND XENIA_LIBRARIES dxgi ntdll ws2_32 comctl32 shcore shlwapi dxguid bcrypt dwmapi winmm user32 gdi32 ole32 shell32)
else()
    list(APPEND XENIA_LIBRARIES android)
endif()

# Diagnostics: Xenia's logger alone (tools/xenia_log_test.cc), PC only.
if (WIN32)
    add_executable(xenia_log_test EXCLUDE_FROM_ALL ${CMAKE_CURRENT_SOURCE_DIR}/tools/xenia_log_test.cc)
    target_include_directories(xenia_log_test PRIVATE ${XENIA_INCLUDES})
    target_compile_definitions(xenia_log_test PRIVATE ${XENIA_DEFINES})
    target_compile_options(xenia_log_test PRIVATE -Wno-everything -march=haswell /EHsc)
    target_link_libraries(xenia_log_test PRIVATE xenia_base xe_fmt ntdll shlwapi bcrypt winmm user32 shell32 ole32)
    target_compile_options(xenia_log_test PRIVATE /Z7)
    target_link_options(xenia_log_test PRIVATE /DEBUG)
endif()
