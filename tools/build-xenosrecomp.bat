@echo off
rem Builds XenosRecomp (Xenos shader microcode -> HLSL -> SPIR-V with DXC) into build\host\XenosRecomp,
rem for tools\shaders.py. NDK clang-cl + MSVC Build Tools 2022, like tools\build-host.bat.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set LLVM=%LOCALAPPDATA%\Android\Sdk\ndk\27.2.12479018\toolchains\llvm\prebuilt\windows-x86_64\bin
set ROOT=%~dp0..
set T=%ROOT%\third_party\rexglue\thirdparty
set X=%T%\XenosRecomp\XenosRecomp
set Z=%T%\zstd\lib
set XT=%ROOT%\third_party\XenonRecomp\thirdparty
set OUT=%ROOT%\build\host\XenosRecomp
rem shader_recompiler.cpp has title-gated paths (Halo 3: 256 pixel constants, jump flags), which the
rem game's runtime translator gets from its build: XPORT_TITLE_ID=0x... builds the tool for that game
rem (tools\shaders.py sets it from games\<game>\game.cmake). The id built in goes to title_id.txt.
set XDEF=
if defined XPORT_TITLE_ID set XDEF=/DXPORT_TITLE_ID=%XPORT_TITLE_ID%
if not exist "%OUT%\obj\zstd" mkdir "%OUT%\obj\zstd"
"%LLVM%\clang-cl.exe" /nologo /O2 /MT -w /DNDEBUG /I"%XT%\xxHash" "%XT%\xxHash\xxhash.c" /c /Fo"%OUT%\obj\\" || exit /b 1
rem zstd has its own xxhash.c (ZSTD_XXH64_*), so its objects get their own folder.
"%LLVM%\clang-cl.exe" /nologo /O2 /MT -w /DNDEBUG /DZSTD_DISABLE_ASM /DZSTD_MULTITHREAD=0 ^
  /I"%Z%" /I"%Z%\common" "%Z%\common\xxhash.c" ^
  "%Z%\common\debug.c" "%Z%\common\entropy_common.c" "%Z%\common\error_private.c" "%Z%\common\fse_decompress.c" ^
  "%Z%\common\pool.c" "%Z%\common\threading.c" "%Z%\common\zstd_common.c" ^
  "%Z%\compress\fse_compress.c" "%Z%\compress\hist.c" "%Z%\compress\huf_compress.c" "%Z%\compress\zstd_compress.c" ^
  "%Z%\compress\zstd_compress_literals.c" "%Z%\compress\zstd_compress_sequences.c" "%Z%\compress\zstd_compress_superblock.c" ^
  "%Z%\compress\zstd_double_fast.c" "%Z%\compress\zstd_fast.c" "%Z%\compress\zstd_lazy.c" "%Z%\compress\zstd_ldm.c" ^
  "%Z%\compress\zstd_opt.c" "%Z%\compress\zstdmt_compress.c" "%Z%\compress\zstd_preSplit.c" ^
  "%Z%\decompress\huf_decompress.c" "%Z%\decompress\zstd_ddict.c" "%Z%\decompress\zstd_decompress.c" ^
  "%Z%\decompress\zstd_decompress_block.c" ^
  /c /Fo"%OUT%\obj\zstd\\" || exit /b 1
rem The C++ files take pch.h first, as XenosRecomp's CMake precompiles it.
"%LLVM%\clang-cl.exe" /nologo /O2 /EHsc /std:c++20 /MT -w /D_CRT_SECURE_NO_WARNINGS /DNDEBUG /DFMT_HEADER_ONLY ^
  /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH -fms-extensions -Wno-switch %XDEF% /FI"%X%\pch.h" ^
  /I"%X%" /I"%T%\dxc-bin\inc" /I"%T%\smol-v\source" /I"%Z%" /I"%XT%\fmt\include" /I"%XT%\xxHash" ^
  "%X%\main.cpp" "%X%\shader_recompiler.cpp" "%X%\dxc_compiler.cpp" "%T%\smol-v\source\smolv.cpp" ^
  /c /Fo"%OUT%\obj\\" || exit /b 1
"%VCToolsInstallDir%bin\Hostx64\x64\link.exe" /nologo /OUT:"%OUT%\XenosRecomp.exe" "%OUT%\obj\*.obj" "%OUT%\obj\zstd\*.obj" ^
  "%T%\dxc-bin\lib\x64\dxcompiler.lib" || exit /b 1
copy /y "%T%\dxc-bin\bin\x64\dxcompiler.dll" "%OUT%\" >nul
copy /y "%T%\dxc-bin\bin\x64\dxil.dll" "%OUT%\" >nul
(echo.%XPORT_TITLE_ID%)> "%OUT%\title_id.txt"
echo built %OUT%\XenosRecomp.exe
