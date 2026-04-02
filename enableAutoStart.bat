@echo off
setlocal

set "script_dir=%~dp0"
set "startup_dir=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup"
set "shortcut_path=%startup_dir%\SubtitleFontAutoLoaderDaemon.lnk"
set "exe_path=%script_dir%SubtitleFontAutoLoaderDaemon.exe"

if not exist "%exe_path%" (
	echo Error: SubtitleFontAutoLoaderDaemon.exe not found next to this script.
	exit /b 1
)

if not exist "%startup_dir%" (
	echo Error: Startup folder not found: "%startup_dir%"
	exit /b 1
)

set "SFH_SHORTCUT_PATH=%shortcut_path%"
set "SFH_EXE_PATH=%exe_path%"
set "SFH_WORKING_DIR=%script_dir%"

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$shell = New-Object -ComObject WScript.Shell; " ^
  "$shortcut = $shell.CreateShortcut([Environment]::GetEnvironmentVariable('SFH_SHORTCUT_PATH')); " ^
  "$shortcut.TargetPath = [Environment]::GetEnvironmentVariable('SFH_EXE_PATH'); " ^
  "$shortcut.WorkingDirectory = [Environment]::GetEnvironmentVariable('SFH_WORKING_DIR'); " ^
  "$shortcut.Save()"

set "exit_code=%ERRORLEVEL%"

if not "%exit_code%"=="0" (
	echo Error: Failed to create startup shortcut.
	exit /b %exit_code%
)

echo Created startup shortcut: "%shortcut_path%"
exit /b 0
