$scriptpath = $MyInvocation.MyCommand.Path
$scriptdir = Split-Path $scriptpath
$configuration = $env:BUILD_CONFIGURATION
if([string]::IsNullOrWhiteSpace($configuration))
{
	$configuration = "Release"
}
else
{
	$configuration = $configuration.Trim()
}

Set-Location $scriptdir
New-Item "ReleaseBuild" -ItemType Directory -Force | Out-Null
Set-Location ReleaseBuild

Copy-Item "../enableAutoStart.ps1" "." -Force
Copy-Item "../disableAutoStart.ps1" "." -Force
Copy-Item "../SubtitleFontHelper.example.toml" "." -Force
Copy-Item "../Build/Win32/$configuration/FontLoadInterceptor32.dll" "." -Force
Copy-Item "../Build/Win32/$configuration/Generated32.dll" "." -Force
Copy-Item "../Build/x64/$configuration/FontLoadInterceptor64.dll" "." -Force
Copy-Item "../Build/x64/$configuration/Generated64.dll" "." -Force
Copy-Item "../Build/x64/$configuration/FontDatabaseBuilder.exe" "." -Force
Copy-Item "../Build/x64/$configuration/SubtitleFontAutoLoaderDaemon.exe" "." -Force
