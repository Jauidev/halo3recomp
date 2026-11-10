@echo off
rem DXC (DirectXShaderCompiler v1.8.2407, the version of third_party\rexglue\thirdparty\dxc-bin) for
rem Android arm64: build\dxc\android\lib\libdxcompiler.so, which build-apk.sh puts in the APK for the
rem runtime shader translator (rex/graphics/xnative/shader_translator.h).
rem   1  sources, downloaded once into build\dxc\src (with the submodules at the tag's commits)
rem   2  TableGen for Windows (MSVC), which the Android build runs
rem   3  libdxcompiler.so with the NDK
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set ROOT=%~dp0..
set SDK=%LOCALAPPDATA%\Android\Sdk
set NDK=%SDK%\ndk\27.2.12479018
set CMAKE=%SDK%\cmake\3.31.6\bin\cmake.exe
set NINJA=%SDK%\cmake\3.31.6\bin\ninja.exe
set D=%ROOT%\build\dxc
set SRC=%D%\src
set GH=https://codeload.github.com

if exist "%SRC%\CMakeLists.txt" goto have_sources
if not exist "%D%\dl" mkdir "%D%\dl"
curl -sL -o "%D%\dl\dxc.tgz" %GH%/microsoft/DirectXShaderCompiler/tar.gz/refs/tags/v1.8.2407 || exit /b 1
curl -sL -o "%D%\dl\dxh.tgz" %GH%/microsoft/DirectX-Headers/tar.gz/980971e835876dc0cde415e8f9bc646e64667bf7 || exit /b 1
curl -sL -o "%D%\dl\spvh.tgz" %GH%/KhronosGroup/SPIRV-Headers/tar.gz/2acb319af38d43be3ea76bfabf3998e5281d8d12 || exit /b 1
curl -sL -o "%D%\dl\spvt.tgz" %GH%/KhronosGroup/SPIRV-Tools/tar.gz/0cfe9e7219148716dfd30b37f4d21753f098707a || exit /b 1
mkdir "%SRC%\external\DirectX-Headers" "%SRC%\external\SPIRV-Headers" "%SRC%\external\SPIRV-Tools"
tar -xzf "%D%\dl\dxc.tgz" -C "%SRC%" --strip-components=1 || exit /b 1
tar -xzf "%D%\dl\dxh.tgz" -C "%SRC%\external\DirectX-Headers" --strip-components=1 || exit /b 1
tar -xzf "%D%\dl\spvh.tgz" -C "%SRC%\external\SPIRV-Headers" --strip-components=1 || exit /b 1
tar -xzf "%D%\dl\spvt.tgz" -C "%SRC%\external\SPIRV-Tools" --strip-components=1 || exit /b 1
:have_sources
rem LLVM builds its own host TableGen when cross-compiling unless told not to; this one is given.
py -3 -I -c "import sys; p=sys.argv[1]; t=open(p).read(); o='if(CMAKE_CROSSCOMPILING OR'; n='if((CMAKE_CROSSCOMPILING AND NOT LLVM_TABLEGEN) OR'; t=t.replace(o, n) if n not in t else t; open(p,'w').write(t)" "%SRC%\CMakeLists.txt" || exit /b 1
rem Nor a native llvm-config, which depends on that native build (only dxcompiler is built here);
rem the endif() repeating the condition changes with it.
rem Generated headers get LF when Python runs on Windows, not when the target is Windows: with CRLF
rem they differ from the sources only in line ends and the build's comparison fails.
py -3 -I -c "import sys; p=sys.argv[1]; t=open(p).read(); o='if(WIN32 AND NOT HLSL_AUTOCRLF)'; n='if(CMAKE_HOST_WIN32 AND NOT HLSL_AUTOCRLF)'; t=t.replace(o, n) if n not in t else t; open(p,'w').write(t)" "%SRC%\cmake\modules\HCT.cmake" || exit /b 1
py -3 -I -c "import sys; p=sys.argv[1]; t=open(p).read(); o='if(CMAKE_CROSSCOMPILING)'; n='if(CMAKE_CROSSCOMPILING AND NOT LLVM_TABLEGEN)'; t=t.replace(o, n) if n not in t else t; open(p,'w').write(t)" "%SRC%\tools\llvm-config\CMakeLists.txt" || exit /b 1

set COMMON=-G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=Release -C "%SRC%\cmake\caches\PredefinedParams.cmake" ^
  -DHLSL_INCLUDE_TESTS=OFF -DSPIRV_BUILD_TESTS=OFF -DSPIRV_SKIP_TESTS=ON -DLLVM_INCLUDE_TESTS=OFF ^
  -DCLANG_INCLUDE_TESTS=OFF -DLLVM_APPEND_VC_REV=OFF -DHLSL_ENABLE_FIXED_VER=OFF
if exist "%D%\host\bin\clang-tblgen.exe" goto have_host
rem DXC's Windows code needs ATL (atlbase.h), an optional part of the Build Tools.
if not exist "%VCToolsInstallDir%atlmfc\include\atlbase.h" (
  echo Falta ATL en las Build Tools de Visual Studio. Instalalo con:
  echo   "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vs_installer.exe" modify --installPath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools" --add Microsoft.VisualStudio.Component.VC.ATL --passive
  exit /b 1
)
"%CMAKE%" -S "%SRC%" -B "%D%\host" %COMMON% -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
  -DENABLE_SPIRV_CODEGEN=OFF || exit /b 1
"%NINJA%" -C "%D%\host" llvm-tblgen clang-tblgen || exit /b 1
:have_host
rem The host triple is given: without it LLVM runs autoconf\config.guess with sh, which cmd lacks.
"%CMAKE%" -S "%SRC%" -B "%D%\android" %COMMON% -DCMAKE_TOOLCHAIN_FILE="%NDK%\build\cmake\android.toolchain.cmake" ^
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static ^
  -DLLVM_INFERRED_HOST_TRIPLE=aarch64-unknown-linux-android ^
  -DLLVM_TABLEGEN="%D%\host\bin\llvm-tblgen.exe" -DCLANG_TABLEGEN="%D%\host\bin\clang-tblgen.exe" || exit /b 1
"%NINJA%" -C "%D%\android" dxcompiler || exit /b 1
"%NDK%\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe" -o "%D%\libdxcompiler.so" "%D%\android\lib\libdxcompiler.so" || exit /b 1
echo built %D%\libdxcompiler.so
