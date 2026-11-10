@echo off
rem Builds the Xcompiler window (tools\gui\xport_gui.cpp, Dear ImGui on Win32 + DirectX 11) into
rem build\host\gui\xcompiler.exe. NDK clang-cl + MSVC Build Tools 2022, like tools\build-host.bat.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set LLVM=%LOCALAPPDATA%\Android\Sdk\ndk\27.2.12479018\toolchains\llvm\prebuilt\windows-x86_64\bin
set ROOT=%~dp0..\..
set I=%ROOT%\third_party\xenia-gpu\third_party\imgui
set OUT=%ROOT%\build\host\gui
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
"%LLVM%\clang-cl.exe" /nologo /O2 /EHsc /std:c++20 /MT /DNDEBUG /DUNICODE /D_UNICODE /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH ^
  /I"%I%" /I"%I%\backends" ^
  "%~dp0xport_gui.cpp" "%I%\imgui.cpp" "%I%\imgui_draw.cpp" "%I%\imgui_tables.cpp" "%I%\imgui_widgets.cpp" ^
  "%I%\backends\imgui_impl_win32.cpp" "%I%\backends\imgui_impl_dx11.cpp" ^
  /c /Fo"%OUT%\obj\\" || exit /b 1
rc.exe /nologo /fo "%OUT%\obj\xcompiler.res" "%~dp0xcompiler.rc" || exit /b 1
rem An open xcompiler.exe cannot be overwritten but can be renamed: it goes on running as xcompiler.old.exe.
if exist "%OUT%\xcompiler.old.exe" del "%OUT%\xcompiler.old.exe" 2>nul
if exist "%OUT%\xcompiler.exe" move /y "%OUT%\xcompiler.exe" "%OUT%\xcompiler.old.exe" >nul 2>nul
"%VCToolsInstallDir%bin\Hostx64\x64\link.exe" /nologo /SUBSYSTEM:WINDOWS /OUT:"%OUT%\xcompiler.exe" "%OUT%\obj\*.obj" "%OUT%\obj\xcompiler.res" ^
  d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib ole32.lib shell32.lib dwmapi.lib windowscodecs.lib || exit /b 1
echo built %OUT%\xcompiler.exe
