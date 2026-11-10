@echo off
rem Builds the runtime for the Windows PC (x64), used to debug before moving to the phone.
rem NDK clang-cl + MSVC Build Tools 2022 (headers, libraries, link.exe). Usage: tools\build-host.bat [target]
rem The game: XPORT_GAME, a folder under games\ (default gta4), built in build\x64-<game>.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set LLVM=%LOCALAPPDATA%\Android\Sdk\ndk\27.2.12479018\toolchains\llvm\prebuilt\windows-x86_64\bin
set CM=%LOCALAPPDATA%\Android\Sdk\cmake\3.31.6\bin
set ROOT=%~dp0..
if "%XPORT_GAME%"=="" set XPORT_GAME=gta4
set OUT=%ROOT%\build\x64-%XPORT_GAME%
if not exist "%OUT%\build.ninja" (
  "%CM%\cmake" -S "%ROOT%" -B "%OUT%" -G Ninja -DCMAKE_MAKE_PROGRAM="%CM%\ninja.exe" ^
    -DCMAKE_C_COMPILER="%LLVM%\clang-cl.exe" -DCMAKE_CXX_COMPILER="%LLVM%\clang-cl.exe" ^
    -DCMAKE_LINKER="%VCToolsInstallDir%bin\Hostx64\x64\link.exe" ^
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DXPORT_GAME=%XPORT_GAME% || exit /b 1
)
rem Stubs for the imports no source implements (rewritten only when that set changes).
pushd "%ROOT%" && py -3 tools\gen_stubs.py & popd
if "%JOBS%"=="" set JOBS=6
"%CM%\cmake" --build "%OUT%" -j %JOBS% %1 %2
