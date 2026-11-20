$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot

function Find-Tool([string]$ToolName) {
    $hit = Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\*\bin\Hostx64\x64\$ToolName" -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
    if (-not $hit) {
        $hit = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\2022\*\VC\Tools\MSVC\*\bin\Hostx64\x64\$ToolName" -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $hit) { throw "Could not locate $ToolName" }
    return $hit
}

$dumpbin = Find-Tool 'dumpbin.exe'

$vcvars = @(
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat'
) | Where-Object { Test-Path $_ } | Select-Object -First 1

$stagedFiles = @(
    "$root\build\stage\WinRings\WinRingsHost.exe",
    "$root\build\stage\WinRings\WinRingsShell.dll",
    "$root\build\stage\WinRings\WinRingsTools.exe",
    "$root\build\stage\WinRingsSetup\WinRingsSetup.exe"
)

Write-Host '==> [1/5] Checking staged files, x64 PE headers & zero .NET/CLR imports'
foreach ($f in $stagedFiles) {
    if (-not (Test-Path $f)) { throw "Missing staged binary: $f" }

    $hdr = (& $dumpbin /headers $f 2>&1) | Out-String
    if ($hdr -notmatch '8664 machine \(x64\)') {
        throw "Not x64 PE: $f"
    }

    $imp = (& $dumpbin /imports $f 2>&1) | Out-String
    if ($imp -match 'mscoree|clr\.dll|coreclr|hostfxr|hostpolicy|System\.Runtime') {
        throw "Forbidden .NET/CLR import detected in: $f"
    }
    Write-Host "  OK (x64, pure native): $(Split-Path -Leaf $f)"
}

Write-Host '==> [2/5] Checking WinRingsShell.dll exports'
$exp = (& $dumpbin /exports "$root\build\stage\WinRings\WinRingsShell.dll" 2>&1) | Out-String
foreach ($req in @('WinRings_GetApi', 'WinRings_RunPreview')) {
    if ($exp -notmatch "\b$req\b") {
        throw "Missing required export '$req' in WinRingsShell.dll"
    }
}
if ($exp -match 'clr|mscoree') { throw "Unexpected CLR symbol exported" }
Write-Host '  OK: WinRings_GetApi & WinRings_RunPreview verified'

Write-Host '==> [3/5] Checking --version exit codes'
foreach ($exe in @(
    "$root\build\stage\WinRings\WinRingsHost.exe",
    "$root\build\stage\WinRings\WinRingsTools.exe",
    "$root\build\stage\WinRingsSetup\WinRingsSetup.exe"
)) {
    $p = Start-Process -FilePath $exe -ArgumentList '--version' -Wait -PassThru -NoNewWindow
    if ($p.ExitCode -ne 0) {
        throw "$exe --version exited with $($p.ExitCode)"
    }
    Write-Host "  OK (--version exit 0): $(Split-Path -Leaf $exe)"
}

Write-Host '==> [4/5] Running non-injecting WinRingsShell.dll ABI probe'
$probeCpp = "$root\build\tests\WinRingsAbiProbe.cpp"
$probeExe = "$root\build\tests\WinRingsAbiProbe.exe"
$probeSrc = @"
#include <Windows.h>
#include <cstdio>
#include "../../src/shared/WinRingsApi.h"

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) return 10;
    HMODULE mod = LoadLibraryW(argv[1]);
    if (!mod) return 11;

    auto fn = reinterpret_cast<WinRingsGetApiFn>(GetProcAddress(mod, WINRINGS_GET_API_EXPORT_A));
    if (!fn) return 12;

    if (fn(0xDEADBEEFu) != nullptr) return 13;

    const WinRingsApi* api = fn(WINRINGS_ABI_VERSION);
    if (!api) return 14;
    if (api->cbSize != sizeof(WinRingsApi)) return 15;
    if (api->abiVersion != WINRINGS_ABI_VERSION) return 16;
    if (api->productVersion != WINRINGS_PRODUCT_VERSION) return 17;
    if (!api->RequestShutdown) return 18;

    auto previewFn = reinterpret_cast<WinRingsRunPreviewFn>(GetProcAddress(mod, WINRINGS_RUN_PREVIEW_EXPORT_A));
    if (!previewFn) return 19;

    FreeLibrary(mod);
    std::printf("WinRingsAbiProbe: OK\n");
    return 0;
}
"@
[System.IO.File]::WriteAllText($probeCpp, $probeSrc, [System.Text.Encoding]::UTF8)

$batProbe = "$root\build\obj\run_abi_probe.bat"
try {
    $cmd = @"
@echo off
call "$vcvars" >nul || exit /b 1
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /WX /utf-8 /DUNICODE /D_UNICODE /Fo"$root\build\obj\tests\\" "$probeCpp" /link /SUBSYSTEM:CONSOLE /OUT:"$probeExe" || exit /b 1
"@
    [System.IO.File]::WriteAllText($batProbe, $cmd, [System.Text.Encoding]::ASCII)
    & cmd.exe /c $batProbe
    if ($LASTEXITCODE -ne 0) { throw "Failed building ABI probe" }
}
finally {
    if (Test-Path $batProbe) { Remove-Item -Force $batProbe -ErrorAction SilentlyContinue }
}

$probeOut = & $probeExe "$root\build\stage\WinRings\WinRingsShell.dll" 2>&1
if ($LASTEXITCODE -ne 0) {
    throw "ABI probe failed ($LASTEXITCODE): $probeOut"
}
Write-Host "  $probeOut"

Write-Host '==> [5/5] Running WinRingsTests.exe'
$testExe = "$root\build\tests\WinRingsTests.exe"
if (-not (Test-Path $testExe)) { throw "Missing unit tests binary: $testExe" }
$testOut = & $testExe 2>&1
if ($LASTEXITCODE -ne 0) {
    throw "WinRingsTests.exe failed ($LASTEXITCODE): $testOut"
}
Write-Host "  $testOut"
if (($testOut | Out-String) -notmatch 'WinRingsTests: ALL PASSED') {
    throw "Expected 'WinRingsTests: ALL PASSED' sentinel not printed"
}

Write-Host 'Test-WinRingsProduct: ALL CHECKS PASSED'
