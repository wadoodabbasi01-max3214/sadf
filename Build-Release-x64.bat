@echo off
setlocal
set "ROOT=%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%VSWHERE%" (
  echo ERROR: Visual Studio Installer / vswhere.exe was not found.
  echo Install Visual Studio 2022 with the "Desktop development with C++" workload.
  exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
  echo ERROR: No Visual Studio installation with MSBuild was found.
  exit /b 1
)

call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -arch=amd64
if errorlevel 1 exit /b 1

msbuild "%ROOT%RiggaRun.sln" /m /p:Configuration=Release /p:Platform=x64 /v:minimal
if errorlevel 1 (
  echo.
  echo BUILD FAILED. Copy the complete MSBuild output when asking for troubleshooting.
  exit /b 1
)

echo.
echo BUILD SUCCEEDED.
echo Executable: "%ROOT%bin\Release\RiggaRun.exe"
echo Keep vmm.dll, leechcore.dll, and FTD3XX.dll beside the EXE.
exit /b 0
