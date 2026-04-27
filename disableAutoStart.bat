@echo off
setlocal

set "run_key=HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
set "run_value_name=SubtitleFontAutoLoaderDaemon"
set "legacy_startup_dir=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup"
set "legacy_shortcut_path=%legacy_startup_dir%\SubtitleFontAutoLoaderDaemon.lnk"

set "SFH_RUN_KEY=%run_key%"
set "SFH_RUN_VALUE_NAME=%run_value_name%"
set "SFH_LEGACY_SHORTCUT_PATH=%legacy_shortcut_path%"

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$runKey = [Environment]::GetEnvironmentVariable('SFH_RUN_KEY'); " ^
  "$valueName = [Environment]::GetEnvironmentVariable('SFH_RUN_VALUE_NAME'); " ^
  "$legacyShortcutPath = [Environment]::GetEnvironmentVariable('SFH_LEGACY_SHORTCUT_PATH'); " ^
  "if (Test-Path -LiteralPath $runKey) { " ^
  "  $property = Get-ItemProperty -LiteralPath $runKey -Name $valueName -ErrorAction SilentlyContinue; " ^
  "  if ($null -ne $property) { Remove-ItemProperty -LiteralPath $runKey -Name $valueName -ErrorAction Stop } " ^
  "}; " ^
  "if (Test-Path -LiteralPath $legacyShortcutPath) { Remove-Item -LiteralPath $legacyShortcutPath -Force -ErrorAction Stop }"

set "exit_code=%ERRORLEVEL%"

if not "%exit_code%"=="0" (
	echo Error: Failed to remove HKCU Run startup entry or legacy startup shortcut.
	exit /b %exit_code%
)

echo Removed HKCU Run startup entry and legacy startup shortcut if present: "%run_value_name%"
exit /b 0
