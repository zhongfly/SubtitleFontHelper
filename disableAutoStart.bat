@echo off
setlocal

set "startup_dir=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup"
set "shortcut_path=%startup_dir%\SubtitleFontAutoLoaderDaemon.lnk"

if not exist "%shortcut_path%" (
	echo Startup shortcut not found: "%shortcut_path%"
	exit /b 0
)

del /f /q "%shortcut_path%"
if exist "%shortcut_path%" (
	echo Error: Failed to remove startup shortcut: "%shortcut_path%"
	exit /b 1
)

echo Removed startup shortcut: "%shortcut_path%"
exit /b 0
