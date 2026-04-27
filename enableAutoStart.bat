@echo off
setlocal

set "script_dir=%~dp0"
set "exe_path=%script_dir%SubtitleFontAutoLoaderDaemon.exe"
set "run_key=HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
set "run_value_name=SubtitleFontAutoLoaderDaemon"
set "legacy_startup_dir=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup"
set "legacy_shortcut_path=%legacy_startup_dir%\SubtitleFontAutoLoaderDaemon.lnk"

if not exist "%exe_path%" (
	echo Error: SubtitleFontAutoLoaderDaemon.exe not found next to this script.
	exit /b 1
)

set "SFH_EXE_PATH=%exe_path%"
set "SFH_RUN_KEY=%run_key%"
set "SFH_RUN_VALUE_NAME=%run_value_name%"
set "SFH_LEGACY_SHORTCUT_PATH=%legacy_shortcut_path%"

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$exePath = [Environment]::GetEnvironmentVariable('SFH_EXE_PATH'); " ^
  "$runKey = [Environment]::GetEnvironmentVariable('SFH_RUN_KEY'); " ^
  "$valueName = [Environment]::GetEnvironmentVariable('SFH_RUN_VALUE_NAME'); " ^
  "$legacyShortcutPath = [Environment]::GetEnvironmentVariable('SFH_LEGACY_SHORTCUT_PATH'); " ^
  "$command = [char]34 + $exePath + [char]34; " ^
  "New-Item -Path $runKey -Force -ErrorAction Stop | Out-Null; " ^
  "New-ItemProperty -Path $runKey -Name $valueName -Value $command -PropertyType String -Force -ErrorAction Stop | Out-Null; " ^
  "if (Test-Path -LiteralPath $legacyShortcutPath) { Remove-Item -LiteralPath $legacyShortcutPath -Force -ErrorAction Stop }"

set "exit_code=%ERRORLEVEL%"

if not "%exit_code%"=="0" (
	echo Error: Failed to create HKCU Run startup entry or remove legacy startup shortcut.
	exit /b %exit_code%
)

echo Created HKCU Run startup entry: "%run_value_name%" = "\"%exe_path%\""
exit /b 0
