# rexgpu-xenos: Xenos GPU emulation plugin (ported from Xenia)
# Namespace: rex::graphics
#
# Runtime-loaded via rex::system::LoadGpuPlugin; consumers never link this
# target. The Xenos format layer is compiled into rexcore.

set(REXGPU_XENOS_SOURCES
    # Core GPU types
    flags.cpp
    register_file.cpp
    registers.cpp
    pipeline/shader/shader.cpp
    format/ucode.cpp
    sampler_info.cpp

    # GPU system
    graphics_system.cpp
    command_processor.cpp
    trace_writer.cpp
    util/draw_extent_estimator.cpp
    util/draw.cpp
    packet_disassembler.cpp
    primitive_processor.cpp
    pipeline/render_target/cache.cpp
    shared_memory.cpp
    pipeline/texture/cache.cpp
    pipeline/shader/interpreter.cpp
    pipeline/shader/translator.cpp
    pipeline/shader/translator_disasm.cpp
)

# SPIR-V shader translation
set(REXGPU_XENOS_SPIRV_SOURCES
    pipeline/shader/spirv_builder.cpp
    pipeline/shader/spirv.cpp
    pipeline/shader/spirv_translator.cpp
    pipeline/shader/spirv_translator_alu.cpp
    pipeline/shader/spirv_translator_fetch.cpp
    pipeline/shader/spirv_translator_memexport.cpp
    pipeline/shader/spirv_translator_rb.cpp
)

# Vulkan backend
set(REXGPU_XENOS_VULKAN_SOURCES
    vulkan/graphics_system.cpp
    vulkan/deferred_command_buffer.cpp
    vulkan/command_processor.cpp
    vulkan/pipeline_cache.cpp
    vulkan/primitive_processor.cpp
    vulkan/render_target_cache.cpp
    vulkan/shader.cpp
    vulkan/shared_memory.cpp
    vulkan/texture_cache.cpp
)

# D3D12 backend
if(REXGLUE_USE_D3D12)
    # DXBC third-party dependency
    set(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES
        ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
    )

    # DXBC shader translation (required for D3D12)
    set(REXGPU_XENOS_DXBC_SOURCES
        pipeline/shader/dxbc.cpp
        pipeline/shader/dxbc_translator.cpp
        pipeline/shader/dxbc_translator_alu.cpp
        pipeline/shader/dxbc_translator_fetch.cpp
        pipeline/shader/dxbc_translator_memexport.cpp
        pipeline/shader/dxbc_translator_om.cpp
    )

    # D3D12 GPU backend
    set(REXGPU_XENOS_D3D12_SOURCES
        d3d12/graphics_system.cpp
        d3d12/command_processor.cpp
        d3d12/primitive_processor.cpp
        d3d12/render_target_cache.cpp
        d3d12/shader.cpp
        d3d12/shared_memory.cpp
        d3d12/texture_cache.cpp
        d3d12/deferred_command_list.cpp
        d3d12/pipeline_cache.cpp
    )
endif()

if(REXGLUE_USE_VULKAN)
    list(APPEND REXGPU_XENOS_SOURCES ${REXGPU_XENOS_SPIRV_SOURCES} ${REXGPU_XENOS_VULKAN_SOURCES})
endif()

if(REXGLUE_USE_D3D12)
    list(APPEND REXGPU_XENOS_SOURCES
        ${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES}
        ${REXGPU_XENOS_DXBC_SOURCES}
        ${REXGPU_XENOS_D3D12_SOURCES})
endif()

function(rexgpu_configure_plugin target export_name)
    set_target_properties(${target} PROPERTIES
        EXPORT_NAME ${export_name}
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )

    target_include_directories(${target} PRIVATE
        ${REXGLUE_ROOT}/thirdparty/renderdoc
        ${REXGLUE_ROOT}  # For thirdparty/dxbc/DXBCChecksum.h include
    )

    target_compile_options(${target} PRIVATE -Wno-unused-variable)

    # Everything PRIVATE: plugins have no link-time consumers. Never link the
    # OBJECT libraries (rexcore/rexui) here; rexruntime already contains those
    # objects and relinking them would duplicate global state.
    target_link_libraries(${target} PRIVATE
        rexruntime
        xxHash::xxhash
        snappy
    )

    if(ANDROID)
        # Guest vblanks follow the display through AChoreographer.
        target_link_libraries(${target} PRIVATE android)
    endif()

    if(REXGLUE_USE_VULKAN)
        target_link_libraries(${target} PRIVATE
            volk::volk
            glslang::SPIRV
            GPUOpen::VulkanMemoryAllocator
            glslang
            MachineIndependent
            GenericCodeGen
            OSDependent
            OGLCompiler
        )
    endif()

    if(REXGLUE_USE_D3D12)
        target_link_libraries(${target} PRIVATE
            d3d12
            dxgi
            dxguid
        )
    endif()
endfunction()

add_library(rexgpu-xenos SHARED
    plugin_main.cpp
    ${REXGPU_XENOS_SOURCES}
)
add_library(rex::gpu-xenos ALIAS rexgpu-xenos)
rexgpu_configure_plugin(rexgpu-xenos gpu-xenos)

if(REXGLUE_USE_VULKAN)
    set(XNATIVE_SHADER_CACHE_ROOT
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/LibertyRecompLib")
    set(XNATIVE_SMOLV_ROOT
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/XenosRecomp/thirdparty/smol-v/source")
    set(XNATIVE_ZSTD_ROOT
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/XenosRecomp/thirdparty/zstd")
    set(XNATIVE_SHADER_OVERRIDE_ROOT
        "${XNATIVE_SHADER_CACHE_ROOT}/shader_overrides")
    set(XNATIVE_SHADER_OVERRIDE_MANIFEST
        "${XNATIVE_SHADER_OVERRIDE_ROOT}/manifest.json")
    set(XNATIVE_SHADER_OVERRIDE_COMPILER
        "${REXGLUE_ROOT}/tools/xport_shader_override_compiler.py")
    set(XNATIVE_SHADER_OVERRIDE_CPP
        "${CMAKE_CURRENT_BINARY_DIR}/generated/xnative/shader_override_cache.cpp")
    set(XNATIVE_SHADER_OVERRIDE_DEPFILE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/xnative/shader_override_cache.d")
    if(NOT EXISTS "${XNATIVE_SHADER_CACHE_ROOT}/shader/shader_cache.cpp")
        message(FATAL_ERROR "the title native shader cache source was not found")
    endif()
    if(NOT EXISTS "${XNATIVE_SHADER_OVERRIDE_MANIFEST}" OR
       NOT EXISTS "${XNATIVE_SHADER_OVERRIDE_COMPILER}")
        message(FATAL_ERROR "the title native shader override inputs were not found")
    endif()

    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    find_program(XNATIVE_GLSLANG_VALIDATOR
        NAMES glslangValidator
        HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
    find_program(XNATIVE_SPIRV_VAL
        NAMES spirv-val
        HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")

    string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" XNATIVE_HOST_PROCESSOR)
    if(XNATIVE_HOST_PROCESSOR MATCHES "arm64|aarch64")
        set(XNATIVE_DXC_ARCH arm64)
    else()
        set(XNATIVE_DXC_ARCH x64)
    endif()
    set(XNATIVE_DXC_ROOT
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/XenosRecomp/thirdparty/dxc-bin")
    set(XNATIVE_DXC_LIBRARY_PATH "" CACHE PATH
        "Directory containing a DXC shared library compatible with xnative shader overrides")
    set(XNATIVE_SHADER_COMMON
        "${XNATIVE_SHADER_OVERRIDE_ROOT}/xenos_shader_common.h")
    if(CMAKE_HOST_WIN32)
        # The bundled DXC 1.8 crashes on the Bink pixel shaders here as well
        # (see the macOS branch). XNATIVE_DXC_LIBRARY_PATH may name the bin
        # directory of a newer DXC release.
        if(XNATIVE_DXC_LIBRARY_PATH AND EXISTS "${XNATIVE_DXC_LIBRARY_PATH}/dxc.exe")
            set(XNATIVE_DXC "${XNATIVE_DXC_LIBRARY_PATH}/dxc.exe")
        else()
            set(XNATIVE_DXC
                "${XNATIVE_DXC_ROOT}/bin/${XNATIVE_DXC_ARCH}/dxc.exe")
        endif()
    elseif(CMAKE_HOST_APPLE)
        set(XNATIVE_DXC
            "${XNATIVE_DXC_ROOT}/bin/${XNATIVE_DXC_ARCH}/dxc-macos")
        if(NOT XNATIVE_DXC_LIBRARY_PATH)
            # The bundled DXC 1.8 crashes in SPIRV-Tools AggressiveDCE for the title's
            # Bink pixel shaders. Prefer a separately installed newer DXC when one
            # is available, while retaining the bundled compiler as a fallback.
            find_library(XNATIVE_HOST_DXCOMPILER
                NAMES dxcompiler libdxcompiler
                PATHS /usr/local/lib /opt/homebrew/lib
                NO_DEFAULT_PATH
                NO_CACHE)
            if(XNATIVE_HOST_DXCOMPILER)
                get_filename_component(XNATIVE_DXC_LIBRARY_PATH
                    "${XNATIVE_HOST_DXCOMPILER}" DIRECTORY)
            else()
                set(XNATIVE_DXC_LIBRARY_PATH
                    "${XNATIVE_DXC_ROOT}/lib/${XNATIVE_DXC_ARCH}")
            endif()
        endif()
    elseif(CMAKE_HOST_UNIX AND XNATIVE_DXC_ARCH STREQUAL "x64")
        set(XNATIVE_DXC "${XNATIVE_DXC_ROOT}/bin/x64/dxc-linux")
        if(NOT XNATIVE_DXC_LIBRARY_PATH)
            set(XNATIVE_DXC_LIBRARY_PATH "${XNATIVE_DXC_ROOT}/lib/x64")
        endif()
    endif()

    set(XNATIVE_SHADER_OVERRIDE_COMMAND
        "${Python3_EXECUTABLE}"
        "${XNATIVE_SHADER_OVERRIDE_COMPILER}"
        --manifest "${XNATIVE_SHADER_OVERRIDE_MANIFEST}"
        --source-root "${XNATIVE_SHADER_OVERRIDE_ROOT}"
        --output "${XNATIVE_SHADER_OVERRIDE_CPP}"
        --depfile "${XNATIVE_SHADER_OVERRIDE_DEPFILE}")
    if(XNATIVE_DXC AND EXISTS "${XNATIVE_DXC}")
        list(APPEND XNATIVE_SHADER_OVERRIDE_COMMAND --dxc "${XNATIVE_DXC}")
        if(XNATIVE_DXC_LIBRARY_PATH)
            list(APPEND XNATIVE_SHADER_OVERRIDE_COMMAND
                --dxc-library-path "${XNATIVE_DXC_LIBRARY_PATH}")
        endif()
    endif()
    if(XNATIVE_GLSLANG_VALIDATOR)
        list(APPEND XNATIVE_SHADER_OVERRIDE_COMMAND
            --glslang-validator "${XNATIVE_GLSLANG_VALIDATOR}")
    endif()
    if(XNATIVE_SPIRV_VAL)
        list(APPEND XNATIVE_SHADER_OVERRIDE_COMMAND
            --spirv-val "${XNATIVE_SPIRV_VAL}")
    endif()

    add_custom_command(
        OUTPUT "${XNATIVE_SHADER_OVERRIDE_CPP}"
        COMMAND ${XNATIVE_SHADER_OVERRIDE_COMMAND}
        DEPENDS
            "${XNATIVE_SHADER_OVERRIDE_MANIFEST}"
            "${XNATIVE_SHADER_OVERRIDE_COMPILER}"
            "${XNATIVE_SHADER_OVERRIDE_ROOT}/shader_override_cache.h"
            "${XNATIVE_SHADER_COMMON}"
        DEPFILE "${XNATIVE_SHADER_OVERRIDE_DEPFILE}"
        COMMENT "Compiling the title hash-specific shader overrides"
        VERBATIM)

    if(NOT TARGET libzstd_static)
        set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        add_subdirectory("${XNATIVE_ZSTD_ROOT}/build/cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/xnative_zstd")
    endif()

    add_library(rexgpu-xnative SHARED
        xnative/graphics_system.cpp
        xnative/shader_override_policy.h
        xnative/native_buffer_arena.cpp
        xnative/native_descriptor_backend.cpp
        xnative/native_frame_context.cpp
        xnative/native_gpu_attribution.cpp
        xnative/native_memory_samples.cpp
        xnative/native_performance_samples.cpp
        xnative/native_submission_lifetime.cpp
        xnative/postfx_resource_pool.cpp
        xnative/smaa_pipeline.cpp
        xnative/plugin_main.cpp
        "${XNATIVE_SHADER_CACHE_ROOT}/shader/shader_cache.cpp"
        "${XNATIVE_SHADER_OVERRIDE_CPP}"
        "${XNATIVE_SMOLV_ROOT}/smolv.cpp"
    )
    set_source_files_properties(
        "${XNATIVE_SHADER_CACHE_ROOT}/shader/shader_cache.cpp"
        PROPERTIES COMPILE_OPTIONS "-Wno-missing-field-initializers"
    )
    if(APPLE)
        target_sources(rexgpu-xnative PRIVATE xnative/native_resource_reclaimer_mac.mm)
    endif()
    add_library(rex::gpu-xnative ALIAS rexgpu-xnative)
    rexgpu_configure_plugin(rexgpu-xnative gpu-xnative)
    target_include_directories(rexgpu-xnative PRIVATE
        "${XNATIVE_SHADER_CACHE_ROOT}"
        "${XNATIVE_SMOLV_ROOT}"
    )
    target_compile_definitions(rexgpu-xnative PRIVATE
        XNATIVE_FONT_ASSET_ROOT="${LIBERTY_RECOMP_WORKSPACE_ROOT}/LibertyRecompLib/font_atlases"
    )
    target_link_libraries(rexgpu-xnative PRIVATE
        libzstd_static
        SPIRV-Headers::SPIRV-Headers
    )
    if(TARGET rexglue_fidelityfx_vulkan)
        target_link_libraries(rexgpu-xnative PRIVATE rexglue_fidelityfx_vulkan)
    endif()
    # ThinLTO inside the renderer plugin (recorder and render worker).
    if(ANDROID)
        option(LIBERTY_ANDROID_RENDERER_THINLTO "ThinLTO for the native renderer on Android" ON)
        if(LIBERTY_ANDROID_RENDERER_THINLTO)
            target_compile_options(rexgpu-xnative PRIVATE -flto=thin)
            target_link_options(rexgpu-xnative PRIVATE -flto=thin "-Wl,--thinlto-jobs=8"
                "-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache-renderer")
        endif()
    endif()
    # The function order libmain uses (xport-recomp/CMakeLists.txt).
    if(ANDROID)
        set(_renderer_symbol_order "${CMAKE_CURRENT_SOURCE_DIR}/../../xport-recomp/android_symbol_order.txt")
        if(EXISTS "${_renderer_symbol_order}")
            target_link_options(rexgpu-xnative PRIVATE
                "-Wl,--symbol-ordering-file=${_renderer_symbol_order}"
                "-Wl,--no-warn-symbol-ordering")
            set_property(TARGET rexgpu-xnative APPEND PROPERTY LINK_DEPENDS "${_renderer_symbol_order}")
        endif()
    endif()
    # Same switch as libmain (xport-recomp/CMakeLists.txt): "generate" records
    # files/pgo/rexgpu-*.profraw, a .profdata path optimizes with it.
    if(ANDROID AND LIBERTY_ANDROID_PGO)
        target_sources(rexgpu-xnative PRIVATE xnative/android_pgo_dump.cpp)
        if(LIBERTY_ANDROID_PGO STREQUAL "generate")
            target_compile_options(rexgpu-xnative PRIVATE -fprofile-generate)
            target_link_options(rexgpu-xnative PRIVATE -fprofile-generate)
            target_compile_definitions(rexgpu-xnative PRIVATE LIBERTY_PGO_GENERATE=1)
        else()
            target_compile_options(rexgpu-xnative PRIVATE "-fprofile-use=${LIBERTY_ANDROID_PGO}"
                -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date)
            target_link_options(rexgpu-xnative PRIVATE "-fprofile-use=${LIBERTY_ANDROID_PGO}")
        endif()
    endif()
endif()

