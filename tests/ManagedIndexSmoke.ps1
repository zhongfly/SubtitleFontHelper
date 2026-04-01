[CmdletBinding()]
param(
	[string]$BinaryRoot = (Join-Path $PSScriptRoot '..\Build\x64\Release'),
	[int]$TimeoutSec = 20,
	[switch]$KeepArtifacts
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Get-LogTail {
	param(
		[Parameter(Mandatory)]
		[string]$LogPath,
		[int]$TailCount = 60
	)

	if (-not (Test-Path -LiteralPath $LogPath)) {
		return '<log file not created>'
	}

	return ((Get-Content -LiteralPath $LogPath -Tail $TailCount) -join [Environment]::NewLine)
}

function Get-LogLines {
	param(
		[Parameter(Mandatory)]
		[string]$LogPath
	)

	if (-not (Test-Path -LiteralPath $LogPath)) {
		return ,@()
	}

	return ,([string[]](Get-Content -LiteralPath $LogPath))
}

function Ensure-ProcessAlive {
	param(
		[Parameter(Mandatory)]
		[System.Diagnostics.Process]$Process,
		[Parameter(Mandatory)]
		[string]$Name,
		[Parameter(Mandatory)]
		[string]$LogPath
	)

	$Process.Refresh()
	if ($Process.HasExited) {
		throw "$Name exited unexpectedly with code $($Process.ExitCode).`n$(Get-LogTail -LogPath $LogPath)"
	}
}

function Wait-Until {
	param(
		[Parameter(Mandatory)]
		[scriptblock]$Condition,
		[Parameter(Mandatory)]
		[string]$Description,
		[Parameter(Mandatory)]
		[string]$LogPath,
		[int]$TimeoutSeconds = $TimeoutSec,
		[int]$PollMilliseconds = 250,
		[System.Diagnostics.Process[]]$WatchedProcesses = @()
	)

	$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
	while ((Get-Date) -lt $deadline) {
		$result = & $Condition
		if ($null -ne $result -and $result -ne $false) {
			return $result
		}

		foreach ($process in $WatchedProcesses) {
			if ($null -ne $process) {
				$processName = $process.ProcessName
				if ([string]::IsNullOrWhiteSpace($processName)) {
					$processName = "process $($process.Id)"
				}
				Ensure-ProcessAlive -Process $process -Name $processName -LogPath $LogPath
			}
		}

		Start-Sleep -Milliseconds $PollMilliseconds
	}

	throw "Timed out waiting for $Description.`n$(Get-LogTail -LogPath $LogPath)"
}

function Wait-ForPath {
	param(
		[Parameter(Mandatory)]
		[string]$Path,
		[Parameter(Mandatory)]
		[string]$Description,
		[Parameter(Mandatory)]
		[string]$LogPath,
		[System.Diagnostics.Process[]]$WatchedProcesses = @()
	)

	return Wait-Until -Description $Description -LogPath $LogPath -WatchedProcesses $WatchedProcesses -Condition {
		if (Test-Path -LiteralPath $Path) {
			return Get-Item -LiteralPath $Path
		}

		return $null
	}
}

function Wait-ForLogPattern {
	param(
		[Parameter(Mandatory)]
		[string]$LogPath,
		[Parameter(Mandatory)]
		[regex]$Pattern,
		[int]$StartLine = 0,
		[Parameter(Mandatory)]
		[string]$Description,
		[System.Diagnostics.Process[]]$WatchedProcesses = @()
	)

	return Wait-Until -Description $Description -LogPath $LogPath -WatchedProcesses $WatchedProcesses -Condition {
		$lines = Get-LogLines -LogPath $LogPath
		if ($StartLine -ge $lines.Count) {
			return $null
		}

		for ($i = $StartLine; $i -lt $lines.Count; ++$i) {
			if ($Pattern.IsMatch($lines[$i])) {
				return @{
					LineIndex = $i
					Line = $lines[$i]
				}
			}
		}

		return $null
	}
}

function Assert-NoLogPattern {
	param(
		[Parameter(Mandatory)]
		[string]$LogPath,
		[Parameter(Mandatory)]
		[regex]$Pattern,
		[int]$StartLine = 0,
		[Parameter(Mandatory)]
		[string]$Description
	)

	$lines = Get-LogLines -LogPath $LogPath
	$matches = @()
	for ($i = $StartLine; $i -lt $lines.Count; ++$i) {
		if ($Pattern.IsMatch($lines[$i])) {
			$matches += $lines[$i]
		}
	}

	if ($matches.Count -gt 0) {
		throw "$Description`n$($matches -join [Environment]::NewLine)"
	}
}

function Get-FileFingerprint {
	param(
		[Parameter(Mandatory)]
		[string]$Path
	)

	$item = Get-Item -LiteralPath $Path
	$hash = Get-FileHash -LiteralPath $Path -Algorithm SHA256
	return @{
		Path = $Path
		Hash = $hash.Hash
		Length = [int64]$item.Length
		LastWriteTimeUtc = $item.LastWriteTimeUtc.ToFileTimeUtc()
	}
}

function Assert-SameFingerprint {
	param(
		[Parameter(Mandatory)]
		[hashtable]$Expected,
		[Parameter(Mandatory)]
		[hashtable]$Actual,
		[Parameter(Mandatory)]
		[string]$Description
	)

	if ($Expected.Hash -ne $Actual.Hash -or $Expected.Length -ne $Actual.Length -or $Expected.LastWriteTimeUtc -ne $Actual.LastWriteTimeUtc) {
		throw "$Description`nExpected: $($Expected | Out-String)`nActual: $($Actual | Out-String)"
	}
}

function Stop-TestProcess {
	param(
		[System.Diagnostics.Process]$Process
	)

	if ($null -eq $Process) {
		return
	}

	try {
		$Process.Refresh()
		if (-not $Process.HasExited) {
			Stop-Process -Id $Process.Id -Force -ErrorAction Stop
			$null = $Process.WaitForExit(5000)
		}
	}
	catch {
	}
}

function Write-ConfigFile {
	param(
		[Parameter(Mandatory)]
		[string]$ConfigPath,
		[Parameter(Mandatory)]
		[string]$IndexRelativePath,
		[Parameter(Mandatory)]
		[string]$SourceRelativePath,
		[string[]]$MonitorProcesses = @()
	)

	$lines = @(
		'wmi_poll_interval = 1000'
		'lru_size = 10'
	)
	if ($MonitorProcesses.Count -eq 0) {
		$lines += 'monitor_processes = []'
	}
	else {
		$lines += 'monitor_processes = ['
		foreach ($processName in $MonitorProcesses) {
			$lines += "  '$processName',"
		}
		$lines += ']'
	}
	$lines += ''
	$lines += '[[index_files]]'
	$lines += "path = '$IndexRelativePath'"
	$lines += 'source_folders = ['
	$lines += "  '$SourceRelativePath',"
	$lines += ']'

	Set-Content -LiteralPath $ConfigPath -Value ($lines -join "`n") -Encoding utf8NoBOM
}

function Start-DaemonProcess {
	param(
		[Parameter(Mandatory)]
		[string]$WorkingDirectory
	)

	return Start-Process -FilePath (Join-Path $WorkingDirectory 'SubtitleFontAutoLoaderDaemon.exe') -WorkingDirectory $WorkingDirectory -WindowStyle Hidden -PassThru
}

function Start-SmokeClientProcess {
	param(
		[Parameter(Mandatory)]
		[string]$WorkingDirectory,
		[Parameter(Mandatory)]
		[string]$FaceName
	)

	$quotedFaceName = '"' + $FaceName.Replace('"', '""') + '"'
	$arguments = @(
		'--face'
		$quotedFaceName
		'--iterations'
		'60'
		'--delay-ms'
		'250'
	)

	return Start-Process -FilePath (Join-Path $WorkingDirectory 'SmokeFontClient.exe') -ArgumentList $arguments -WorkingDirectory $WorkingDirectory -WindowStyle Hidden -RedirectStandardOutput (Join-Path $WorkingDirectory 'SmokeFontClient.stdout.log') -RedirectStandardError (Join-Path $WorkingDirectory 'SmokeFontClient.stderr.log') -PassThru
}

function Start-DirectInjectionProcess {
	param(
		[Parameter(Mandatory)]
		[string]$WorkingDirectory,
		[Parameter(Mandatory)]
		[int]$ProcessId
	)

	$rundll32Path = Join-Path $env:WINDIR 'System32\rundll32.exe'
	$dllEntryPoint = '"' + (Join-Path $WorkingDirectory 'FontLoadInterceptor64.dll') + '",InjectProcess'
	return Start-Process -FilePath $rundll32Path -ArgumentList @($dllEntryPoint, [string]$ProcessId) -WorkingDirectory $WorkingDirectory -WindowStyle Hidden -PassThru
}


$requiredFiles = @(
	'SubtitleFontAutoLoaderDaemon.exe',
	'Generated64.dll',
	'FontLoadInterceptor64.dll',
	'SmokeFontClient.exe'
)

$fontCandidates = @(
	@{ File = 'consola.ttf'; Face = 'Consolas' },
	@{ File = 'arial.ttf'; Face = 'Arial' },
	@{ File = 'times.ttf'; Face = 'Times New Roman' },
	@{ File = 'cour.ttf'; Face = 'Courier New' },
	@{ File = 'calibri.ttf'; Face = 'Calibri' },
	@{ File = 'segoeui.ttf'; Face = 'Segoe UI' },
	@{ File = 'tahoma.ttf'; Face = 'Tahoma' },
	@{ File = 'verdana.ttf'; Face = 'Verdana' }
)

$daemonProcess = $null
$clientProcess = $null
$injectorProcess = $null
$workDirectory = $null
$testSucceeded = $false
$failureMessage = $null

try {
	$binaryRootPath = (Resolve-Path -LiteralPath $BinaryRoot).Path
	foreach ($requiredFile in $requiredFiles) {
		$requiredPath = Join-Path $binaryRootPath $requiredFile
		if (-not (Test-Path -LiteralPath $requiredPath)) {
			throw "required binary not found: $requiredPath"
		}
	}

	$existingDaemon = @(Get-Process -Name 'SubtitleFontAutoLoaderDaemon' -ErrorAction SilentlyContinue)
	if ($existingDaemon.Count -gt 0) {
		throw 'existing SubtitleFontAutoLoaderDaemon.exe process detected; close it before running this smoke test'
	}

	$selectedFonts = @()
	$windowsFontRoot = Join-Path $env:WINDIR 'Fonts'
	foreach ($candidate in $fontCandidates) {
		$sourcePath = Join-Path $windowsFontRoot $candidate.File
		if (Test-Path -LiteralPath $sourcePath) {
			$selectedFonts += @{
				File = $candidate.File
				Face = $candidate.Face
				SourcePath = $sourcePath
			}
		}
	}

	if ($selectedFonts.Count -lt 4) {
		throw 'failed to find at least four known font samples under %WINDIR%\Fonts'
	}

	$timestamp = Get-Date -Format 'yyyyMMdd-HHmmssfff'
	$workDirectory = Join-Path $env:TEMP "SubtitleFontHelper.ManagedIndexSmoke\$timestamp"
	$null = New-Item -ItemType Directory -Path $workDirectory -Force
	$null = New-Item -ItemType Directory -Path (Join-Path $workDirectory 'fonts-a') -Force
	$null = New-Item -ItemType Directory -Path (Join-Path $workDirectory 'fonts-b') -Force
	$null = New-Item -ItemType Directory -Path (Join-Path $workDirectory 'indexes') -Force

	foreach ($requiredFile in $requiredFiles) {
		Copy-Item -LiteralPath (Join-Path $binaryRootPath $requiredFile) -Destination (Join-Path $workDirectory $requiredFile) -Force
	}

	$fontsA = @($selectedFonts[0], $selectedFonts[1])
	$fontsB = @($selectedFonts[2], $selectedFonts[3])
	foreach ($font in $fontsA) {
		Copy-Item -LiteralPath $font.SourcePath -Destination (Join-Path $workDirectory 'fonts-a') -Force
	}
	foreach ($font in $fontsB) {
		Copy-Item -LiteralPath $font.SourcePath -Destination (Join-Path $workDirectory 'fonts-b') -Force
	}

	$configPath = Join-Path $workDirectory 'SubtitleFontHelper.toml'
	$logPath = Join-Path $workDirectory 'SubtitleFontHelper.log'
	$indexPath = Join-Path $workDirectory 'indexes/FontIndex.xml'
	$statePath = Join-Path $workDirectory 'indexes/FontIndex.xml.state.bin'
	$reloadIndexPath = Join-Path $workDirectory 'indexes/FontIndexReload.xml'
	$reloadStatePath = Join-Path $workDirectory 'indexes/FontIndexReload.xml.state.bin'

	Write-ConfigFile -ConfigPath $configPath -IndexRelativePath 'indexes/FontIndex.xml' -SourceRelativePath 'fonts-a'
	Start-Sleep -Milliseconds 500

	$daemonProcess = Start-DaemonProcess -WorkingDirectory $workDirectory
	$phase1Config = Wait-ForLogPattern -LogPath $logPath -Pattern ([regex]'managed index config: .*FontIndex\.xml".*action=build') -Description 'initial managed index build config log' -WatchedProcesses @($daemonProcess)
	$null = Wait-ForLogPattern -LogPath $logPath -Pattern ([regex]'managed index build complete: index="[^"]*FontIndex\.xml".*indexedFontCount=\d+.*fontNames=\[[^\]]+\]') -Description 'initial managed index build completion log' -WatchedProcesses @($daemonProcess) -StartLine $phase1Config.LineIndex
	$null = Wait-ForPath -Path $indexPath -Description 'initial managed index xml' -LogPath $logPath -WatchedProcesses @($daemonProcess)
	$null = Wait-ForPath -Path $statePath -Description 'initial managed index snapshot' -LogPath $logPath -WatchedProcesses @($daemonProcess)

	$initialIndexFingerprint = Get-FileFingerprint -Path $indexPath
	$initialStateFingerprint = Get-FileFingerprint -Path $statePath
	Stop-TestProcess -Process $daemonProcess
	$daemonProcess = $null

	$lineCountBeforeSecondStart = (Get-LogLines -LogPath $logPath).Count
	$daemonProcess = Start-DaemonProcess -WorkingDirectory $workDirectory
	$phase2Config = Wait-ForLogPattern -LogPath $logPath -Pattern ([regex]'managed index config: .*FontIndex\.xml".*action=load') -Description 'second managed index load config log' -WatchedProcesses @($daemonProcess) -StartLine $lineCountBeforeSecondStart
	Start-Sleep -Seconds 2
	Ensure-ProcessAlive -Process $daemonProcess -Name $daemonProcess.ProcessName -LogPath $logPath
	Start-Sleep -Seconds 3

	$secondIndexFingerprint = Get-FileFingerprint -Path $indexPath
	$secondStateFingerprint = Get-FileFingerprint -Path $statePath
	Assert-SameFingerprint -Expected $initialIndexFingerprint -Actual $secondIndexFingerprint -Description 'FontIndex.xml changed after second startup'
	Assert-SameFingerprint -Expected $initialStateFingerprint -Actual $secondStateFingerprint -Description 'FontIndex.xml.state.bin changed after second startup'
	Assert-NoLogPattern -LogPath $logPath -Pattern ([regex]'managed index build start: index="[^"]*FontIndex\.xml"') -StartLine $phase2Config.LineIndex -Description 'unexpected full rebuild detected after second startup'

	$lineCountBeforeReload = (Get-LogLines -LogPath $logPath).Count
	Write-ConfigFile -ConfigPath $configPath -IndexRelativePath 'indexes/FontIndexReload.xml' -SourceRelativePath 'fonts-b'
	Start-Sleep -Milliseconds 500
	Stop-TestProcess -Process $daemonProcess
	$daemonProcess = $null
	$daemonProcess = Start-DaemonProcess -WorkingDirectory $workDirectory
	$phase3Config = Wait-ForLogPattern -LogPath $logPath -Pattern ([regex]'managed index config: .*FontIndexReload\.xml".*action=build') -Description 'reconfigured managed index build config log' -WatchedProcesses @($daemonProcess) -StartLine $lineCountBeforeReload
	$reloadBuildComplete = Wait-ForLogPattern -LogPath $logPath -Pattern ([regex]'managed index build complete: index="[^"]*FontIndexReload\.xml".*indexedFontCount=\d+.*fontNames=\[[^\]]+\]') -Description 'reload managed index build completion log' -WatchedProcesses @($daemonProcess) -StartLine $phase3Config.LineIndex
	$null = Wait-ForPath -Path $reloadIndexPath -Description 'reloaded managed index xml' -LogPath $logPath -WatchedProcesses @($daemonProcess)
	$null = Wait-ForPath -Path $reloadStatePath -Description 'reloaded managed index snapshot' -LogPath $logPath -WatchedProcesses @($daemonProcess)

	$oldIndexAfterReload = Get-FileFingerprint -Path $indexPath
	$oldStateAfterReload = Get-FileFingerprint -Path $statePath
	Assert-SameFingerprint -Expected $initialIndexFingerprint -Actual $oldIndexAfterReload -Description 'FontIndex.xml changed during config reload'
	Assert-SameFingerprint -Expected $initialStateFingerprint -Actual $oldStateAfterReload -Description 'FontIndex.xml.state.bin changed during config reload'

	Stop-TestProcess -Process $daemonProcess
	$daemonProcess = $null

	$phase4Directory = Join-Path $workDirectory 'phase4-injection'
	$null = New-Item -ItemType Directory -Path $phase4Directory -Force
	Copy-Item -LiteralPath (Join-Path $binaryRootPath 'SmokeFontClient.exe') -Destination (Join-Path $phase4Directory 'SmokeFontClient.exe') -Force
	Copy-Item -LiteralPath (Join-Path $binaryRootPath 'FontLoadInterceptor64.dll') -Destination (Join-Path $phase4Directory 'FontLoadInterceptor64.dll') -Force
	Copy-Item -LiteralPath (Join-Path $binaryRootPath 'Generated64.dll') -Destination (Join-Path $phase4Directory 'Generated64.dll') -Force

	$phase4LogPath = Join-Path $phase4Directory 'SubtitleFontHelper.log'
	if (Test-Path -LiteralPath $phase4LogPath) {
		Remove-Item -LiteralPath $phase4LogPath -Force
	}

	$lineCountBeforeInjection = 0
	$clientProcess = Start-SmokeClientProcess -WorkingDirectory $phase4Directory -FaceName $fontsB[0].Face
	$clientPid = $clientProcess.Id
	Start-Sleep -Milliseconds 500
	$injectorProcess = Start-DirectInjectionProcess -WorkingDirectory $phase4Directory -ProcessId $clientPid
	if (-not $injectorProcess.WaitForExit($TimeoutSec * 1000)) {
		throw "rundll32 injection helper did not exit within ${TimeoutSec}s.`n$(Get-LogTail -LogPath $phase4LogPath)"
	}
	$injectorProcess.Refresh()
	if ($injectorProcess.ExitCode -ne 0) {
		throw "rundll32 injection helper exited with code $($injectorProcess.ExitCode).`n$(Get-LogTail -LogPath $phase4LogPath)"
	}

	$attachPattern = [regex]("DllAttach processId=$clientPid")
	$injectSuccessPattern = [regex]("InjectProcessSuccess processId=$clientPid")
	$injectFailurePattern = [regex]("InjectProcessFailure processId=$clientPid")

	$null = Wait-ForLogPattern -LogPath $phase4LogPath -Pattern $attachPattern -Description 'DllAttach log for SmokeFontClient' -WatchedProcesses @($clientProcess) -StartLine $lineCountBeforeInjection
	$null = Wait-ForLogPattern -LogPath $phase4LogPath -Pattern $injectSuccessPattern -Description 'InjectProcessSuccess log for SmokeFontClient' -WatchedProcesses @($clientProcess) -StartLine $lineCountBeforeInjection

	if (-not $clientProcess.WaitForExit($TimeoutSec * 1000)) {
		throw "SmokeFontClient did not exit within ${TimeoutSec}s.`n$(Get-LogTail -LogPath $phase4LogPath)"
	}
	$clientProcess.Refresh()
	if ($clientProcess.ExitCode -ne 0) {
		throw "SmokeFontClient exited with code $($clientProcess.ExitCode).`n$(Get-LogTail -LogPath $phase4LogPath)"
	}

	Start-Sleep -Seconds 1
	Assert-NoLogPattern -LogPath $phase4LogPath -Pattern $injectFailurePattern -StartLine $lineCountBeforeInjection -Description 'unexpected injection failure detected for SmokeFontClient'

	$testSucceeded = $true
}
catch {
	$errorDetails = @($_.Exception.Message)
	if ($_.ScriptStackTrace) {
		$errorDetails += "Script stack:`n$($_.ScriptStackTrace)"
	}
	if ($_.InvocationInfo.PositionMessage) {
		$errorDetails += "Position:`n$($_.InvocationInfo.PositionMessage)"
	}
	if ($workDirectory) {
		$failureMessage = ($errorDetails -join "`n`n") + "`nWork directory: $workDirectory"
	}
	else {
		$failureMessage = $errorDetails -join "`n`n"
	}
}
finally {
	Stop-TestProcess -Process $injectorProcess
	Stop-TestProcess -Process $clientProcess
	Stop-TestProcess -Process $daemonProcess

	if ($workDirectory -and -not $KeepArtifacts -and $testSucceeded) {
		Remove-Item -LiteralPath $workDirectory -Recurse -Force -ErrorAction SilentlyContinue
	}
}

if (-not $testSucceeded) {
	Write-Error $failureMessage
	exit 1
}

if ($KeepArtifacts) {
	Write-Host "Smoke test passed. Work directory: $workDirectory"
}
else {
	Write-Host 'Smoke test passed.'
}
