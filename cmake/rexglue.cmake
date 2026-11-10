# the native renderer (LibertyRecompAndroid's xnative, GPL-3) and the parts of the
# ReXGlue SDK (BSD, a renamed Xenia: namespace rex::) it needs, from third_party/rexglue.
#
# rex_core: logging, cvars, threading, Xenos format tables. rex_ui: the Vulkan provider, device and
# presenter. rex_xnative: the renderer itself plus its SPIR-V shader cache. Our runtime talks to
# it through rex::system::IGraphicsSystem (title commands from the D3D hooks), with shims for the
# few runtime objects it asks for (rexglue_glue/).
#
# fmt, xxHash, VMA, utfcpp and SPIRV-Tools come from Xenia's third_party (same commits ReXGlue
# pins); spdlog, volk, simde, Vulkan/SPIR-V headers, toml++, CLI11, zstd, smol-v, picosha2 and the
# RenderDoc header from third_party/rexglue/thirdparty (fetched at ReXGlue's pinned commits).

set(REX_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rexglue)
set(REX_SRC ${REX_ROOT}/src)
set(REX_TP ${REX_ROOT}/thirdparty)
set(REX_GLUE ${CMAKE_CURRENT_SOURCE_DIR}/rexglue_glue)
set(REX_XENIA_TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xenia-gpu/third_party)

set(REX_INCLUDES
    ${REX_GLUE}/shim
    ${REX_ROOT}/include
    ${REX_SRC}
    ${REX_ROOT}
    ${REX_TP}/cli11/include
    ${REX_TP}/vulkan-headers/include
    ${REX_TP}/volk
    ${REX_TP}/spdlog/include
    ${REX_TP}/simde
    ${REX_TP}/tomlplusplus/include
    ${REX_TP}/spirv-headers/include
    ${REX_TP}/smol-v/source
    ${REX_TP}/renderdoc
    ${REX_XENIA_TP}/SPIRV-Tools/include
    ${REX_XENIA_TP}/fmt/include
    ${REX_XENIA_TP}/xxhash
    ${REX_XENIA_TP}/utfcpp/source
    ${REX_XENIA_TP}/VulkanMemoryAllocator/include
    ${REX_TP}/zstd/lib
    ${REX_XENIA_TP}/disruptorplus/include
    ${REX_XENIA_TP}/stb
    ${REX_XENIA_TP}/imgui
    ${REX_ROOT}/native_assets
    ${XPORT_GAME_DATA}
    ${CMAKE_CURRENT_BINARY_DIR}/rexglue-generated)

set(REX_DEFINES
    SPDLOG_FMT_EXTERNAL
    SPDLOG_COMPILED_LIB
    VK_NO_PROTOTYPES
    REX_HAS_VULKAN=1
    REXGLUE_BUILD_CONFIG="Release")
if (WIN32)
    list(APPEND REX_DEFINES REX_PLATFORM_WINDOWS=1 NOMINMAX WIN32_LEAN_AND_MEAN
        VK_USE_PLATFORM_WIN32_KHR)
else()
    list(APPEND REX_DEFINES REX_PLATFORM_LINUX=1 VK_USE_PLATFORM_ANDROID_KHR)
endif()

function(rex_library name)
    add_library(${name} STATIC ${ARGN})
    set_target_properties(${name} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(${name} PRIVATE ${REX_INCLUDES})
    target_compile_definitions(${name} PRIVATE ${REX_DEFINES})
    target_compile_options(${name} PRIVATE -Wno-everything -fno-strict-aliasing
        $<$<COMPILE_LANGUAGE:CXX>:-fno-char8_t>)
    if (WIN32)
        target_compile_options(${name} PRIVATE -march=haswell /EHsc /FI${REX_GLUE}/win_fixups.h)
    else()
        # NDK 27's libc++ 18 keeps std::jthread / std::stop_token behind the experimental library.
        target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>)
    endif()
endfunction()

# --- third party ----------------------------------------------------------------------------
rex_library(rex_spdlog
    ${REX_TP}/spdlog/src/spdlog.cpp
    ${REX_TP}/spdlog/src/stdout_sinks.cpp
    ${REX_TP}/spdlog/src/color_sinks.cpp
    ${REX_TP}/spdlog/src/file_sinks.cpp
    ${REX_TP}/spdlog/src/async.cpp
    ${REX_TP}/spdlog/src/cfg.cpp)
rex_library(rex_volk ${REX_TP}/volk/volk.c)
rex_library(rex_smolv ${REX_TP}/smol-v/source/smolv.cpp)
file(GLOB REX_ZSTD_SOURCES ${REX_TP}/zstd/lib/common/*.c ${REX_TP}/zstd/lib/compress/*.c
    ${REX_TP}/zstd/lib/decompress/*.c)
rex_library(rex_zstd ${REX_ZSTD_SOURCES})
target_compile_definitions(rex_zstd PRIVATE ZSTD_DISABLE_ASM)
rex_library(rex_xxhash ${REX_XENIA_TP}/xxhash/xxhash.c)

# --- core -----------------------------------------------------------------------------------
set(REX_CORE_SOURCES
    arena.cpp bit_map.cpp bit_stream.cpp byte_stream.cpp clock.cpp cvar.cpp
    diagnostics_policy.cpp filesystem.cpp filesystem_wildcard.cpp input_trace.cpp
    gpu_flight_recorder.cpp logging.cpp memory.cpp mutex.cpp ring_buffer.cpp sha256.cpp
    string_buffer.cpp threading.cpp timer_queue.cpp utf8.cpp vec128.cpp)
if (WIN32)
    list(APPEND REX_CORE_SOURCES atomic_win.cpp clock_win.cpp dbg_win.cpp dynlib_win.cpp
        filesystem_win.cpp mapped_memory_win.cpp math_msvc.cpp memory_win.cpp
        platform/env_win.cpp system_win.cpp threading_win.cpp)
else()
    list(APPEND REX_CORE_SOURCES atomic_posix.cpp clock_posix.cpp dbg_posix.cpp dynlib_posix.cpp
        filesystem_posix.cpp mapped_memory_posix.cpp math_gcc.cpp
        memory_posix.cpp platform/env_posix.cpp
        system_posix.cpp threading_posix.cpp)
endif()
list(TRANSFORM REX_CORE_SOURCES PREPEND ${REX_SRC}/core/)
list(APPEND REX_CORE_SOURCES
    ${REX_SRC}/graphics/xnative/core/options.cpp
    ${REX_SRC}/graphics/xenos.cpp
    ${REX_SRC}/graphics/pipeline/texture/info.cpp
    ${REX_SRC}/graphics/pipeline/texture/info_formats.cpp
    ${REX_SRC}/graphics/pipeline/texture/extent.cpp
    ${REX_SRC}/graphics/pipeline/texture/util.cpp
    ${REX_SRC}/graphics/pipeline/texture/conversion.cpp
    # Native LZX (XMemDecompress) decoder for rexglue_glue/xd3d/lzx_hooks.cpp.
    ${REX_SRC}/system/lzx.cpp)
rex_library(rex_core ${REX_CORE_SOURCES})
# libmspack's LZX decompressor (third_party/libmspack, LGPL-2.1, from libmspack's cabextract tree as
# LibertyRecomp builds it).
add_library(rex_mspack STATIC ${CMAKE_SOURCE_DIR}/third_party/libmspack/mspack/lzxd.c)
target_include_directories(rex_mspack PUBLIC ${CMAKE_SOURCE_DIR}/third_party/libmspack/mspack)
target_compile_options(rex_mspack PRIVATE -w)
target_include_directories(rex_core PRIVATE ${CMAKE_SOURCE_DIR}/third_party/libmspack/mspack)
# xportdeps links XenonRecomp's unpatched lzxd.c (XEX loading) and the linker picked its lzxd_* for
# lzx.cpp too: odd XMem raw blocks then decoded with CAB padding (native LZX failed at ~1 MiB) and
# lzx.cpp wrote uncompressed_block_padding past the end of the smaller struct. Rename this copy.
set(REX_MSPACK_RENAMES lzxd_init=rex_lzxd_init lzxd_free=rex_lzxd_free
    lzxd_decompress=rex_lzxd_decompress lzxd_set_output_length=rex_lzxd_set_output_length
    lzxd_set_reference_data=rex_lzxd_set_reference_data)
target_compile_definitions(rex_mspack PRIVATE ${REX_MSPACK_RENAMES})
target_compile_definitions(rex_core PRIVATE ${REX_MSPACK_RENAMES})

# --- ui (Vulkan) ----------------------------------------------------------------------------
set(REX_UI_SOURCES
    graphics_upload_buffer_pool.cpp graphics_util.cpp image_decode.cpp immediate_drawer.cpp
    presenter.cpp renderdoc_api.cpp window.cpp windowed_app_context.cpp
    vulkan/linked_type_descriptor_set_allocator.cpp vulkan/single_layout_descriptor_set_pool.cpp
    vulkan/spirv_int64_lowering.cpp vulkan/spirv_tools_context.cpp vulkan/ui_samplers.cpp
    vulkan/vulkan_device.cpp
    vulkan/vulkan_immediate_drawer.cpp vulkan/vulkan_instance.cpp vulkan/vulkan_mem_alloc.cpp
    vulkan/vulkan_presenter.cpp vulkan/vulkan_provider.cpp vulkan/vulkan_submission_tracker.cpp
    vulkan/vulkan_upload_buffer_pool.cpp vulkan/vulkan_util.cpp)
if (WIN32)
    list(APPEND REX_UI_SOURCES surface_win.cpp)
else()
    list(APPEND REX_UI_SOURCES surface_android.cpp)
endif()
list(TRANSFORM REX_UI_SOURCES PREPEND ${REX_SRC}/ui/)
rex_library(rex_ui ${REX_UI_SOURCES})

# --- the renderer ---------------------------------------------------------------------------
set(REX_NATIVE ${REX_SRC}/graphics/xnative)
rex_library(rex_xnative
    ${REX_NATIVE}/graphics_system.cpp
    ${REX_NATIVE}/native_buffer_arena.cpp
    ${REX_NATIVE}/native_descriptor_backend.cpp
    ${REX_NATIVE}/native_frame_context.cpp
    ${REX_NATIVE}/native_gpu_attribution.cpp
    ${REX_NATIVE}/native_memory_samples.cpp
    ${REX_NATIVE}/native_performance_samples.cpp
    ${REX_NATIVE}/native_submission_lifetime.cpp
    ${REX_NATIVE}/native_texture_disk_cache.cpp
    ${REX_NATIVE}/postfx_resource_pool.cpp
    ${REX_NATIVE}/smaa_pipeline.cpp
    ${REX_NATIVE}/shader_translator.cpp
    ${XPORT_GAME_DATA}/shader/shader_cache.cpp
    ${XPORT_GAME_DATA}/shader_override_cache.cpp
    ${REX_GLUE}/memory.cpp)
# The pipeline recipes of the port's PC boot (tools/port.py), replayed on a device's first run.
# GLOB so that the build reconfigures when port.py adds the file.
file(GLOB XNATIVE_PIPELINE_SEED CONFIGURE_DEPENDS ${XPORT_GAME_DATA}/pipeline_recipes.bin)
if (XNATIVE_PIPELINE_SEED)
    set_source_files_properties(${REX_NATIVE}/graphics_system.cpp PROPERTIES
        COMPILE_DEFINITIONS "XNATIVE_PIPELINE_SEED_FILE=\"${XNATIVE_PIPELINE_SEED}\""
        OBJECT_DEPENDS ${XNATIVE_PIPELINE_SEED})
endif()

# --- the runtime shader translator (rex/graphics/xnative/shader_translator.h) ------------------
# XenosRecomp's HLSL front end as tools/shaders.py builds it, with its own fmt (11, header only)
# and pch (xenos_recomp_prefix.h); DXC is loaded at run time. The ABI header goes in as text.
set(REX_XENOSRECOMP ${REX_TP}/XenosRecomp/XenosRecomp)
add_library(rex_xenosrecomp STATIC ${REX_XENOSRECOMP}/shader_recompiler.cpp
    ${REX_NATIVE}/xenos_recomp_bridge.cpp)
set_target_properties(rex_xenosrecomp PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(rex_xenosrecomp PRIVATE ${REX_XENOSRECOMP}
    ${XENON_THIRDPARTY}/fmt/include)
target_compile_definitions(rex_xenosrecomp PRIVATE FMT_HEADER_ONLY _CRT_SECURE_NO_WARNINGS)
target_compile_options(rex_xenosrecomp PRIVATE -Wno-everything -fms-extensions)
if (WIN32)
    target_compile_options(rex_xenosrecomp PRIVATE /EHsc /FI${REX_NATIVE}/xenos_recomp_prefix.h)
else()
    target_compile_options(rex_xenosrecomp PRIVATE -include ${REX_NATIVE}/xenos_recomp_prefix.h)
endif()
set(XNATIVE_SHADER_COMMON ${CMAKE_CURRENT_SOURCE_DIR}/tools/shaders/xenos_shader_common.h)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${XNATIVE_SHADER_COMMON})
file(READ ${XNATIVE_SHADER_COMMON} XNATIVE_SHADER_COMMON_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," XNATIVE_SHADER_COMMON_HEX
    "${XNATIVE_SHADER_COMMON_HEX}")
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/rexglue-generated/xenos_shader_common.inc
    CONTENT "${XNATIVE_SHADER_COMMON_HEX}\n")
target_include_directories(rex_xnative PRIVATE ${REX_TP}/dxc-bin/inc ${XPORT_GAME_ROOT})
# dxcapi.h's COM interfaces use __declspec(uuid) and __uuidof outside Windows too.
set_source_files_properties(${REX_NATIVE}/shader_translator.cpp PROPERTIES COMPILE_OPTIONS
    -fms-extensions)
if (WIN32)
    # The translator loads it from the exe's folder.
    set(XNATIVE_DXC_DLL ${REX_TP}/dxc-bin/bin/x64/dxcompiler.dll)
endif()

set(REX_LIBRARIES rex_xnative rex_xenosrecomp rex_ui rex_core rex_mspack rex_spdlog rex_volk
    rex_smolv rex_zstd rex_xxhash xe_fmt)
if (ANDROID)
    list(APPEND REX_LIBRARIES dl)
endif()
if (WIN32)
    list(APPEND REX_LIBRARIES dxgi)
endif()

add_executable(rex_link_probe EXCLUDE_FROM_ALL ${REX_GLUE}/link_probe.cpp)
set_target_properties(rex_link_probe PROPERTIES CXX_STANDARD 23)
target_include_directories(rex_link_probe PRIVATE ${REX_INCLUDES})
target_compile_definitions(rex_link_probe PRIVATE ${REX_DEFINES})
target_compile_options(rex_link_probe PRIVATE -Wno-everything -fno-char8_t)
if (WIN32)
    target_compile_options(rex_link_probe PRIVATE -march=haswell /EHsc /FI${REX_GLUE}/win_fixups.h)
endif()
target_link_libraries(rex_link_probe PRIVATE ${REX_LIBRARIES})

# --- LibertyRecomp's the title hooks, ported to the title ----------------------------------------------
# rexglue_glue/xd3d is generated by tools/port_native_hooks.py; rexglue_glue/hooks_shim puts this
# runtime's PPCContext and recompiled functions under the names the hooks expect.
set(REX_XD3D ${REX_GLUE}/xd3d)
# tools/find_d3d.py writes the D3D and XDK functions it did not find as "// XD3D_<name>: not found"
# (or "inlined in ...", or "several: ...") in the slot's xd3d_functions.h. With GTA IV's XDK
# (kReferenceXdk in xd3d_device.h) the hooks stand in for those functions and need every one. With
# another (e.g. Gears of War's 2.0.3529) they run the game's own and only tell the renderer, so
# only the functions that do that are needed (XD3D_CORE); the others' hooks are left out
# (#ifdef XD3D_<name> in d3d_hooks.cpp). A game short of what it needs builds without the native
# renderer's hooks and draws with Xenia's GPU (XPORT_NO_NATIVE_RENDERER), as one whose game.cmake
# sets XPORT_NATIVE_RENDERER OFF; without the LZX ones, the recompiled XMemDecompress runs.
set(XD3D_CORE DrawVertices DrawIndexedVertices ClearRectangle Resolve Swap SetStreamSource SetIndices
    SetTexture SetVertexShader SetPixelShader CreateVertexShader CreatePixelShader SetRenderTarget
    SetDepthStencilSurface LockResource UnlockResource ReleaseResource LockTextureRect LockTextureRectArray
    ADDR_NullClearColor)
file(STRINGS ${XPORT_GAME_ROOT}/xd3d_functions.h XD3D_MISSING REGEX "^// XD3D_[A-Za-z0-9_]+: (not found|inlined in|several)")
# The same for the device fields the hooks and the renderer use (xd3d_device.h, written with it).
if (NOT EXISTS ${XPORT_GAME_ROOT}/xd3d_device.h)
    message(FATAL_ERROR "games/${XPORT_GAME}/xd3d_device.h missing: tools/find_d3d.py writes it")
endif()
file(STRINGS ${XPORT_GAME_ROOT}/xd3d_device.h XD3D_DEVICE_MISSING REGEX "^// (DeviceOffset\\([0-9xA-Fa-f]+\\)|k[A-Za-z]+): not found")
file(STRINGS ${XPORT_GAME_ROOT}/xd3d_device.h XD3D_REFERENCE_XDK REGEX "^inline constexpr bool kReferenceXdk = true;")
# decided again when tools/slot.py writes a different one
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${XPORT_GAME_ROOT}/xd3d_functions.h
    ${XPORT_GAME_ROOT}/xd3d_device.h)
set(XPORT_NATIVE_HOOKS ON)
set(XPORT_LZX_HOOKS ON)
set(XD3D_LACKING ${XD3D_DEVICE_MISSING})
foreach (missing IN LISTS XD3D_MISSING)
    string(REGEX REPLACE "^// XD3D_([A-Za-z0-9_]+):.*" "\\1" name "${missing}")
    if (name MATCHES "^Lzx")
        set(XPORT_LZX_HOOKS OFF)
    elseif (XD3D_REFERENCE_XDK OR name IN_LIST XD3D_CORE)
        list(APPEND XD3D_LACKING ${name})
    endif()
endforeach()
if (XD3D_LACKING)
    set(XPORT_NATIVE_HOOKS OFF)
    list(JOIN XD3D_LACKING ", " lacking)
    message(STATUS "${XPORT_GAME}: missing for the native renderer (games/${XPORT_GAME}/xd3d_functions.h, xd3d_device.h): ${lacking}; Xenia's GPU draws")
elseif (DEFINED XPORT_NATIVE_RENDERER AND NOT XPORT_NATIVE_RENDERER)
    set(XPORT_NATIVE_HOOKS OFF)
    message(STATUS "${XPORT_GAME}: XPORT_NATIVE_RENDERER OFF (games/${XPORT_GAME}/game.cmake): Xenia's GPU draws")
elseif (NOT XD3D_REFERENCE_XDK)
    message(STATUS "${XPORT_GAME}: another XDK than GTA IV's: the D3D hooks run the game's functions and tell the native renderer")
endif()
set(REX_XD3D_SOURCES
    ${REX_GLUE}/graphics.cpp
    ${REX_GLUE}/native_options.cpp
    # XPORT_PC_WINDOW=1: the game in a window with keyboard and pad on the PC (nothing on Android).
    ${REX_GLUE}/pc_window.cpp
    ${REX_GLUE}/native_stubs.cpp)
if (XPORT_NATIVE_HOOKS)
    list(APPEND REX_XD3D_SOURCES ${REX_XD3D}/d3d_hooks.cpp)
    # Sleep instead of spinning while the title waits for a frame slot (xport_frame_wait_sleep;
    # only with XD3D_QueryFrameSlot, the file checks).
    if (ANDROID)
        list(APPEND REX_XD3D_SOURCES ${REX_GLUE}/frame_wait.cpp)
    endif()
endif()
if (XPORT_LZX_HOOKS)
    # Resource decompression in native code instead of the recompiled XMemDecompress (the six
    # hooked functions are instruction-identical between TU8 and the title).
    list(APPEND REX_XD3D_SOURCES ${REX_XD3D}/lzx_hooks.cpp)
endif()
rex_library(rex_xd3dhooks ${REX_XD3D_SOURCES})
target_include_directories(rex_xd3dhooks BEFORE PRIVATE ${REX_GLUE}/hooks_shim ${REX_XD3D} ${XPORT_GAME_ROOT}
    ${XPORT_PPC_ROOT} ${XENON_THIRDPARTY}/simde ${REX_GLUE})
if (WIN32)
    target_compile_options(rex_xd3dhooks PRIVATE /FI${REX_GLUE}/rexglue_glue.h)
else()
    target_compile_options(rex_xd3dhooks PRIVATE -include ${REX_GLUE}/rexglue_glue.h)
endif()
set(REX_HOOK_LIBRARIES rex_xd3dhooks ${REX_LIBRARIES})
