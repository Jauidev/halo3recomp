@echo off
rem Builds bin\xexinfo.exe and bin\ppcdis.exe (what tools\xport.py runs) against the XenonUtils library:
rem build\host's (tools\build-host.bat builds it).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set LLVM=%LOCALAPPDATA%\Android\Sdk\ndk\27.2.12479018\toolchains\llvm\prebuilt\windows-x86_64\bin
set XR=%~dp0..\..\third_party\XenonRecomp
set B=%~dp0..\..\build\host
set L=%B%
if not exist "%L%\XenonUtils\XenonUtils.lib" (echo run tools\build-host.bat first & exit /b 1)
set OUT=%~dp0..\..\bin
if not exist "%B%" mkdir "%B%"
"%LLVM%\clang-cl.exe" /nologo /c /O2 /EHsc /MT -std:c++17 /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /DNOMINMAX ^
  /D_CRT_SECURE_NO_WARNINGS /I"%XR%\XenonUtils" /I"%XR%\thirdparty\simde" "%~dp0xexinfo.cpp" /Fo"%B%\xexinfo.obj" || exit /b 1
"%VCToolsInstallDir%bin\Hostx64\x64\link.exe" /nologo /OUT:"%OUT%\xexinfo.exe" "%B%\xexinfo.obj" ^
  "%L%\XenonUtils\XenonUtils.lib" "%L%\thirdparty\disasm\disasm.lib" || exit /b 1
"%LLVM%\clang-cl.exe" /nologo /c /O2 /EHsc /MT -std:c++17 /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /DNOMINMAX ^
  /D_CRT_SECURE_NO_WARNINGS /I"%XR%\XenonUtils" /I"%XR%\thirdparty\simde" /I"%XR%\thirdparty\disasm" "%~dp0ppcdis.cpp" /Fo"%B%\ppcdis.obj" || exit /b 1
"%VCToolsInstallDir%bin\Hostx64\x64\link.exe" /nologo /OUT:"%OUT%\ppcdis.exe" "%B%\ppcdis.obj" ^
  "%L%\XenonUtils\XenonUtils.lib" "%L%\thirdparty\disasm\disasm.lib" || exit /b 1
echo built %OUT%\xexinfo.exe and ppcdis.exe
