$ErrorActionPreference = 'Stop'

$taskbarDll = 'C:\Windows\System32\Taskbar.dll'
$taskbarViewDll = 'C:\Windows\SystemApps\MicrosoftWindows.Client.Core_cw5n1h2txyewy\Taskbar.View.dll'

Write-Output '=== PE Headers ==='
foreach ($path in @($taskbarDll, $taskbarViewDll)) {
    if (-not (Test-Path $path)) {
        Write-Output "MISSING: $path"
        continue
    }
    $bytes = [IO.File]::ReadAllBytes($path)
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    $timestamp = [BitConverter]::ToUInt32($bytes, $peOffset + 8)
    $sizeOfImage = [BitConverter]::ToUInt32($bytes, $peOffset + 24 + 56)
    $ver = (Get-Item $path).VersionInfo.FileVersion
    Write-Output ("{0} -> Machine=0x{1:X4} TimeDateStamp=0x{2:x8} SizeOfImage=0x{3:x} FileVer={4}" -f $path, $machine, $timestamp, $sizeOfImage, $ver)
}

Write-Output ''
Write-Output '=== Local PDBs in C:\SymCache ==='
if (Test-Path 'C:\SymCache') {
    Get-ChildItem 'C:\SymCache' -Recurse -Filter '*Taskbar*' -ErrorAction SilentlyContinue | Select-Object FullName, Length | Format-Table -AutoSize
    Get-ChildItem 'C:\SymCache' -Recurse -Filter '*explorer*' -ErrorAction SilentlyContinue | Select-Object FullName, Length | Format-Table -AutoSize
} else {
    Write-Output 'No C:\SymCache directory'
}

Write-Output ''
Write-Output '=== Debug Tools / dbghelp ==='
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
$dumpbin = Get-ChildItem "$vs\VC\Tools\MSVC" -Recurse -Filter 'dumpbin.exe' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1 -ExpandProperty FullName
Write-Output "dumpbin: $dumpbin"
$dbghelpCandidates = @(
    'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\dbghelp.dll',
    'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\symsrv.dll',
    'C:\Windows\System32\dbghelp.dll'
)
foreach ($c in $dbghelpCandidates) {
    Write-Output ("{0}: {1}" -f $c, (Test-Path $c))
}

if ($dumpbin) {
    Write-Output ''
    Write-Output '=== Taskbar.dll exports & interesting imports ==='
    & $dumpbin /exports $taskbarDll | Select-Object -First 40
    Write-Output '--- Taskbar.dll imports matching Rect / Monitor / AppBar / Reg / WindowPos / Height ---'
    & $dumpbin /imports $taskbarDll | Select-String -Pattern 'PtInRect|Monitor|Work|WindowPos|Rect|Taskbar|AppBar|RegGet|RegQuery|SHAppBar|SystemParameters|AdjustWindow|Dpi'
}
