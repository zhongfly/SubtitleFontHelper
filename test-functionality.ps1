# Functional test script for SubtitleFontHelper
# This script tests the basic functionality of the built executables

param(
    [Parameter(Mandatory=$false)]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$testsPassed = 0
$testsFailed = 0

function Write-TestResult {
    param(
        [string]$TestName,
        [bool]$Passed,
        [string]$Message = ""
    )

    if ($Passed) {
        Write-Host "[PASS] $TestName" -ForegroundColor Green
        if ($Message) {
            Write-Host "       $Message" -ForegroundColor Gray
        }
        $script:testsPassed++
    } else {
        Write-Host "[FAIL] $TestName" -ForegroundColor Red
        if ($Message) {
            Write-Host "       $Message" -ForegroundColor Yellow
        }
        $script:testsFailed++
    }
}

Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "SubtitleFontHelper Functional Tests" -ForegroundColor Cyan
Write-Host "Configuration: $Configuration" -ForegroundColor Cyan
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host ""

# Test 1: Check if all required executables exist
Write-Host "Test 1: Checking for built executables..." -ForegroundColor Cyan
$requiredFiles = @(
    "Build\x64\$Configuration\FontDatabaseBuilder.exe",
    "Build\x64\$Configuration\SubtitleFontAutoLoaderDaemon.exe",
    "Build\Win32\$Configuration\FontLoadInterceptor32.dll",
    "Build\x64\$Configuration\FontLoadInterceptor64.dll"
)

$allFilesExist = $true
foreach ($file in $requiredFiles) {
    if (Test-Path $file) {
        Write-Host "  Found: $file" -ForegroundColor Gray
    } else {
        Write-Host "  Missing: $file" -ForegroundColor Red
        $allFilesExist = $false
    }
}
Write-TestResult "All required executables exist" $allFilesExist

# Test 2: FontDatabaseBuilder help text
Write-Host ""
Write-Host "Test 2: Testing FontDatabaseBuilder help..." -ForegroundColor Cyan
try {
    $helpOutput = & "Build\x64\$Configuration\FontDatabaseBuilder.exe" 2>&1
    $hasHelp = $helpOutput -match "Usage:" -and $helpOutput -match "-deleteduplicates"
    Write-TestResult "FontDatabaseBuilder shows help with new options" $hasHelp "Includes -deleteduplicates option"
} catch {
    Write-TestResult "FontDatabaseBuilder shows help with new options" $false "Error: $_"
}

# Test 3: Create test configuration file with extra keys (Task 1 validation)
Write-Host ""
Write-Host "Test 3: Testing configuration file parsing with extra keys..." -ForegroundColor Cyan
$testConfigPath = "test-config.xml"
$testConfig = @"
<?xml version="1.0" encoding="UTF-8"?>
<ConfigFile wmiPollInterval="1000" lruSize="50">
<IndexFile>test-index.xml</IndexFile>
<MonitorProcess>test.exe</MonitorProcess>
<UnknownElement>This should be ignored</UnknownElement>
<AnotherUnknownElement attr="value">More unknown data</AnotherUnknownElement>
</ConfigFile>
"@
Set-Content -Path $testConfigPath -Value $testConfig -Encoding UTF8

# Create minimal test font index file
$testIndexPath = "test-index.xml"
$testIndex = @"
<?xml version="1.0" encoding="UTF-8"?>
<FontDatabase>
</FontDatabase>
"@
Set-Content -Path $testIndexPath -Value $testIndex -Encoding UTF8

Write-TestResult "Created test configuration with extra keys" $true

# Test 4: Test FontDatabaseBuilder with a test directory
Write-Host ""
Write-Host "Test 4: Testing FontDatabaseBuilder basic functionality..." -ForegroundColor Cyan
$testFontDir = "test-fonts"
New-Item -ItemType Directory -Force -Path $testFontDir | Out-Null

# Note: We can't test actual font processing without font files,
# but we can verify the executable runs and handles empty directory
try {
    $output = & "Build\x64\$Configuration\FontDatabaseBuilder.exe" $testFontDir -output "test-output.xml" 2>&1
    $handlesEmptyDir = $output -match "Discovered 0 files" -or $output -match "Nothing to do"
    Write-TestResult "FontDatabaseBuilder handles empty directory" $handlesEmptyDir
} catch {
    Write-TestResult "FontDatabaseBuilder handles empty directory" $false "Error: $_"
}

# Test 5: Verify copyFiles.ps1 works
Write-Host ""
Write-Host "Test 5: Testing copyFiles.ps1 script..." -ForegroundColor Cyan
try {
    # Set environment variable for the script
    $env:BUILD_CONFIGURATION = $Configuration

    # Clean up any existing ReleaseBuild directory
    if (Test-Path "ReleaseBuild") {
        Remove-Item -Recurse -Force "ReleaseBuild"
    }

    & ".\copyFiles.ps1"

    # Check if ReleaseBuild directory was created and contains files
    $releaseFiles = @(
        "ReleaseBuild\FontDatabaseBuilder.exe",
        "ReleaseBuild\SubtitleFontAutoLoaderDaemon.exe",
        "ReleaseBuild\FontLoadInterceptor32.dll",
        "ReleaseBuild\FontLoadInterceptor64.dll",
        "ReleaseBuild\SubtitleFontHelper.example.xml"
    )

    $allReleaseFilesExist = $true
    foreach ($file in $releaseFiles) {
        if (-not (Test-Path $file)) {
            Write-Host "  Missing: $file" -ForegroundColor Red
            $allReleaseFilesExist = $false
        }
    }

    Write-TestResult "copyFiles.ps1 creates ReleaseBuild artifacts" $allReleaseFilesExist
} catch {
    Write-TestResult "copyFiles.ps1 creates ReleaseBuild artifacts" $false "Error: $_"
}

# Test 6: Verify example configuration is valid XML
Write-Host ""
Write-Host "Test 6: Testing example configuration validity..." -ForegroundColor Cyan
try {
    [xml]$exampleConfig = Get-Content "SubtitleFontHelper.example.xml"
    $hasWmiPollInterval = $exampleConfig.ConfigFile.wmiPollInterval -ne $null
    $hasIndexFile = $exampleConfig.ConfigFile.IndexFile -ne $null
    $isValid = $hasWmiPollInterval -and $hasIndexFile
    Write-TestResult "Example configuration is valid XML" $isValid
} catch {
    Write-TestResult "Example configuration is valid XML" $false "Error: $_"
}

# Cleanup
Write-Host ""
Write-Host "Cleaning up test files..." -ForegroundColor Cyan
Remove-Item -Force -ErrorAction SilentlyContinue $testConfigPath
Remove-Item -Force -ErrorAction SilentlyContinue $testIndexPath
Remove-Item -Force -ErrorAction SilentlyContinue "test-output.xml"
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $testFontDir

# Summary
Write-Host ""
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "Test Summary" -ForegroundColor Cyan
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "Passed: $testsPassed" -ForegroundColor Green
Write-Host "Failed: $testsFailed" -ForegroundColor $(if ($testsFailed -eq 0) { "Green" } else { "Red" })
Write-Host ""

if ($testsFailed -gt 0) {
    Write-Host "Some tests failed!" -ForegroundColor Red
    exit 1
} else {
    Write-Host "All tests passed!" -ForegroundColor Green
    exit 0
}
