@echo off
rem Build fgswap with MSVC (Visual Studio 2022 or newer, Windows 11 SDK). Usage: build.cmd [dll] [host] [probe] [hwfq]  (default: all)
setlocal
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (echo Visual Studio with C++ tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set ROOT=%~dp0
if not exist "%ROOT%bin" mkdir "%ROOT%bin"
if not exist "%ROOT%obj" mkdir "%ROOT%obj"
set WHAT=%*
if "%WHAT%"=="" set WHAT=dll host probe hwfq
set CF=/nologo /std:c++17 /O2 /EHsc /W3 /Zi /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /Fo"%ROOT%obj\\"
set LIBS=d3d12.lib dxgi.lib user32.lib ole32.lib winmm.lib dwmapi.lib
for %%t in (%WHAT%) do (
  if "%%t"=="dll" cl %CF% /LD /Fd"%ROOT%obj\fgswap.pdb" /Fe"%ROOT%bin\fgswap.dll" "%ROOT%src\main.cpp" "%ROOT%src\swapchain.cpp" "%ROOT%src\common.cpp" "%ROOT%src\ffx_abi.cpp" %LIBS% /link /DEBUG /INCREMENTAL:NO || exit /b 1
  if "%%t"=="host" cl %CF% /Fd"%ROOT%obj\fg_host.pdb" /Fe"%ROOT%bin\fg_host.exe" "%ROOT%src\fg_host.cpp" "%ROOT%src\ffx_abi.cpp" %LIBS% /link /INCREMENTAL:NO || exit /b 1
  if "%%t"=="hwfq" for %%p in (caps_probe vblank_probe modes_probe media_probe pace_test) do cl %CF% /Fd"%ROOT%obj\%%p.pdb" /Fe"%ROOT%bin\%%p.exe" "%ROOT%hwfq\%%p.cpp" %LIBS% d3d11.lib gdi32.lib dcomp.lib mincore.lib avrt.lib /link /INCREMENTAL:NO || exit /b 1
  if "%%t"=="probe" cl %CF% /Fd"%ROOT%obj\fg_probe.pdb" /Fe"%ROOT%bin\fg_probe.exe" "%ROOT%src\fg_probe.cpp" "%ROOT%src\ffx_abi.cpp" %LIBS% /link /INCREMENTAL:NO || exit /b 1
)
