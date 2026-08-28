$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$suites = @(
    "model",
    "window_tracker",
    "render",
    "tool_icon",
    "ui_layout",
    "tooltip",
    "resource_ops",
    "resource_watcher",
    "history",
    "atomic_file",
    "png_io",
    "preview_capture",
    "preview_service"
)

$jobCount = 2
if ($env:GOLDENS_TEST_JOBS) {
    $configuredJobCount = 0
    if (-not [int]::TryParse($env:GOLDENS_TEST_JOBS, [ref]$configuredJobCount) -or
        $configuredJobCount -lt 1) {
        throw "GOLDENS_TEST_JOBS must be a positive integer."
    }
    $jobCount = $configuredJobCount
}
$jobCount = [Math]::Min($jobCount, $suites.Count)

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$worker = Join-Path $PSScriptRoot "run-test-suite.bat"
$logDirectory = Join-Path ([IO.Path]::GetTempPath()) ("goldens-tests-" + [Guid]::NewGuid())
[void](New-Item -ItemType Directory -Path $logDirectory)

function Start-TestSuite {
    param([string]$Name)

    $logPath = Join-Path $logDirectory ($Name + ".log")
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $env:ComSpec
    $startInfo.Arguments = '/d /s /c ""{0}" "{1}" > "{2}" 2>&1"' -f $worker, $Name, $logPath
    $startInfo.WorkingDirectory = $repositoryRoot
    $startInfo.UseShellExecute = $false
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Could not start test suite $Name."
    }

    return [PSCustomObject]@{
        Name = $Name
        LogPath = $logPath
        Process = $process
    }
}

$nextSuite = 0
$running = @()
$failures = @()

Write-Host "Running $($suites.Count) test suites with $jobCount parallel jobs."

try {
    while ($nextSuite -lt $suites.Count -or $running.Count -gt 0) {
        while ($nextSuite -lt $suites.Count -and $running.Count -lt $jobCount) {
            $running += (Start-TestSuite -Name $suites[$nextSuite])
            $nextSuite++
        }

        $completed = $null
        foreach ($candidate in $running) {
            if ($candidate.Process.WaitForExit(50)) {
                $completed = $candidate
                break
            }
        }
        if ($null -eq $completed) {
            continue
        }

        $completed.Process.WaitForExit()
        $exitCode = $completed.Process.ExitCode
        Write-Host ""
        Write-Host "=== $($completed.Name) ==="
        if (Test-Path $completed.LogPath) {
            Get-Content $completed.LogPath | ForEach-Object { Write-Host $_ }
        }
        if ($exitCode -ne 0) {
            $failures += $completed.Name
            Write-Host "FAILED ($exitCode): $($completed.Name)"
        }

        $completed.Process.Dispose()
        $running = @($running | Where-Object { $_.Name -ne $completed.Name })
    }
}
finally {
    foreach ($item in $running) {
        if (-not $item.Process.HasExited) {
            $item.Process.Kill()
            $item.Process.WaitForExit()
        }
        $item.Process.Dispose()
    }
    Remove-Item -Recurse -Force $logDirectory
}

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "Failed test suites: $($failures -join ', ')"
    exit 1
}

Write-Host ""
Write-Host "All $($suites.Count) test suites passed."
