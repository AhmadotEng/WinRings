param(
    [switch]$AllowExplorerInjection
)

$ErrorActionPreference = 'Stop'

if (-not $AllowExplorerInjection) {
    Write-Host 'Refusing live Explorer injection test without explicit -AllowExplorerInjection switch.'
    exit 2
}

$root = Split-Path -Parent $PSScriptRoot
$hostExe = "$root\build\stage\WinRings\WinRingsHost.exe"
$shellDll = "$root\build\stage\WinRings\WinRingsShell.dll"
if (-not (Test-Path $hostExe) -or -not (Test-Path $shellDll)) {
    throw "Staged binaries missing. Run Build-WinRings.ps1 first."
}

$explorer = Get-Process -Name 'explorer' -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $explorer) { throw "explorer.exe is not running" }
$origExplorerPid = $explorer.Id

# Check whether an earlier WinRingsShell.dll is already loaded in explorer from a different path
$mapped = $explorer.Modules | Where-Object { $_.ModuleName -ieq 'WinRingsShell.dll' } | Select-Object -First 1
if ($mapped -and ($mapped.FileName -ine $shellDll)) {
    Write-Host "Refusing test: WinRingsShell.dll is already mapped into explorer.exe from another path: $($mapped.FileName)"
    exit 4
}

Write-Host "==> Starting WinRingsHost.exe (Explorer PID = $origExplorerPid)"
$proc = Start-Process -FilePath $hostExe -ArgumentList '--no-tray' -PassThru
$ready = $false
try {
    for ($i = 0; $i -lt 48; $i++) {
        Start-Sleep -Milliseconds 250
        & $hostExe --status | Out-Null
        if ($LASTEXITCODE -eq 0) {
            $ready = $true
            break
        }
    }
    if (-not $ready) { throw "WinRingsHost did not reach READY state within 12s" }
    Write-Host '  Status: ready'

    foreach ($cmd in @('--toggle', '--toggle', '--reload')) {
        & $hostExe $cmd
        if ($LASTEXITCODE -ne 0) { throw "Command $cmd failed ($LASTEXITCODE)" }
        Start-Sleep -Milliseconds 350
    }

    & $hostExe --exit
    if (-not $proc.WaitForExit(6000)) {
        throw "WinRingsHost did not exit within 6s after --exit"
    }
    Write-Host '  Exited cleanly'
}
finally {
    if ($proc -and -not $proc.HasExited) {
        & $hostExe --exit 2>$null
        $proc.WaitForExit(3000) | Out-Null
    }
}

$afterExplorer = Get-Process -Id $origExplorerPid -ErrorAction SilentlyContinue
if (-not $afterExplorer -or $afterExplorer.HasExited) {
    throw "explorer.exe PID $origExplorerPid exited unexpectedly!"
}

Write-Host "Test-WinRingsLifecycle: PASSED (explorer.exe PID $origExplorerPid stayed healthy)"
