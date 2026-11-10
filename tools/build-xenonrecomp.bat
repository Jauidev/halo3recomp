@echo off
rem Builds XenonRecomp and XenonAnalyse for the Windows host into build\host\XenonRecomp and copies them
rem to bin\, where tools\xport.py runs them. NDK clang-cl + MSVC Build Tools 2022, like tools\build-host.bat.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set LLVM=%LOCALAPPDATA%\Android\Sdk\ndk\27.2.12479018\toolchains\llvm\prebuilt\windows-x86_64\bin
set CM=%LOCALAPPDATA%\Android\Sdk\cmake\3.31.6\bin
set ROOT=%~dp0..
set OUT=%ROOT%\build\host\XenonRecomp
if not exist "%OUT%\build.ninja" (
  "%CM%\cmake" -S "%ROOT%\third_party\XenonRecomp" -B "%OUT%" -G Ninja -DCMAKE_MAKE_PROGRAM="%CM%\ninja.exe" ^
    -DCMAKE_C_COMPILER="%LLVM%\clang-cl.exe" -DCMAKE_CXX_COMPILER="%LLVM%\clang-cl.exe" ^
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="/D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /EHsc" -DCMAKE_C_FLAGS="/D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH" || exit /b 1
)
"%CM%\cmake" --build "%OUT%" -j 8 --target XenonAnalyse XenonRecomp || exit /b 1
copy /y "%OUT%\XenonRecomp\XenonRecomp.exe" "%ROOT%\bin\" >nul || exit /b 1
copy /y "%OUT%\XenonAnalyse\XenonAnalyse.exe" "%ROOT%\bin\" >nul || exit /b 1
echo built %ROOT%\bin\XenonRecomp.exe
