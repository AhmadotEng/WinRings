# WinRings — Building, Verification & Diagnostics Guide

This guide explains how to build all WinRings binaries from source, run the automated verification suites, and inspect runtime diagnostic telemetry.

---

## 1. Build Environment

WinRings uses a zero-external-dependency PowerShell + MSVC build pipeline (`tools\Build-WinRings.ps1`) that automatically locates the latest x64 Visual C++ toolset (`cl.exe`, `link.exe`, `rc.exe`, `dumpbin.exe`) and Windows 11 SDK (`10.0.22000.0+`).

### Compiler & Linker Flags
- **Compiler:** `/nologo /std:c++17 /utf-8 /EHsc /MT /O2 /Zi /W4 /WX /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX`
- **Linker:** `/nologo /INCREMENTAL:NO /DEBUG /OPT:REF /OPT:ICF`
- **System Libraries:** `user32.lib`, `gdi32.lib`, `shell32.lib`, `ole32.lib`, `oleaut32.lib`, `uuid.lib`, `shlwapi.lib`, `uxtheme.lib`, `comctl32.lib`, `comdlg32.lib`, `dwmapi.lib`, `d2d1.lib`, `dwrite.lib`, `d3d11.lib`, `dxgi.lib`, `dcomp.lib`, `windowscodecs.lib`, `winhttp.lib`, `windowsapp.lib`, `advapi32.lib`, `bcrypt.lib`

---

## 2. Building from Source

Run from the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\Build-WinRings.ps1
```

### Build Stages
1. **`[1/5] WinRingsTests.exe`** — Compiles unit tests against `src\shared\*`.
2. **`[2/5] WinRingsShell.dll`** — Compiles the Explorer shell module (`src\shell\*`).
3. **`[3/5] WinRingsHost.exe`** — Compiles the supervisor & tray host (`src\host\WinRingsHost.cpp`).
4. **`[4/5] WinRingsTools.exe`** — Compiles the 4-tab Settings UI and file/media tools (`src\tools\*`).
5. **`[5/5] WinRingsSetup.exe`** — Computes SHA-256 digests of the 3 payloads (`build\gen\payload_hashes.h`), embeds them as `RT_RCDATA` resources, and links the single-file installer (`src\setup\WinRingsSetup.cpp`).

Staged outputs are written to:
- `build\stage\WinRings\WinRingsHost.exe`
- `build\stage\WinRings\WinRingsShell.dll`
- `build\stage\WinRings\WinRingsTools.exe`
- `build\stage\WinRingsSetup\WinRingsSetup.exe`

---

## 3. Running the Verification Suite

### Non-Injecting Product & Unit Test Gate (`Test-WinRingsProduct.ps1`)
```powershell
powershell -ExecutionPolicy Bypass -File .\tools\Test-WinRingsProduct.ps1
```
Performs 5 automated verification checks:
1. **PE Header & Zero-CLR Check:** Verifies `8664 machine (x64)` and confirms zero `.NET` / `mscoree.dll` imports across all 4 deliverables.
2. **DLL Export Check:** Verifies `WinRings_GetApi`, `WinRings_RunPreview`, `DllGetClassObject`, and `DllCanUnloadNow` on `WinRingsShell.dll`.
3. **CLI `--version` Check:** Verifies exit code `0` for `WinRingsHost.exe --version`, `WinRingsTools.exe --version`, and `WinRingsSetup.exe --version`.
4. **ABI Probe (`WinRingsAbiProbe.exe`):** Loads `WinRingsShell.dll` outside `explorer.exe` and verifies ABI version handshake and mismatch rejection.
5. **Unit Test Suite (`WinRingsTests.exe`):** Runs deterministic tests covering radial slot math, `18 DIP` deadzone decoupled hit-testing, outer-arc submenu angles, hover-corridor persistence and direct slot switching, DPI clamping, clock formatting (`Fri 20 Nov  4:02 pm`), crash-guard 3-in-180s rolling window, config JSON round-trip serialization, and non-destructive drop output naming.

### Opt-In Live Explorer Lifecycle Test (`Test-WinRingsLifecycle.ps1`)
```powershell
powershell -ExecutionPolicy Bypass -File .\tools\Test-WinRingsLifecycle.ps1 -AllowExplorerInjection
```
Tests live `explorer.exe` injection, `--status` state bit reporting (`ring=1 xaml=1 topbar=1 clear=1 clock=1 appsHidden=1`), `--reload`, and `--exit` restoration.

---

## 4. Runtime Diagnostics (`%APPDATA%\WinRings\shell_diag.log`)

Both `WinRingsHost.exe` and `WinRingsShell.dll` write timestamped diagnostic telemetry to:
```text
%APPDATA%\WinRings\shell_diag.log
```

Example healthy log sequence:
```text
[TopBarHook] PE OK (base=0x... TS=0xA4EA993A SOI=0x002FC000) sigs=[1,1,1,1]
[TopBarHook] Install SUCCEEDED (4/4 zero-WinRT hooks patched)
[TopBarHook] SystemTray.dll PositionOverflowWindow hook installed (base=0x...)
[TopBarHook] Captured pITrayUI=0x... origSizeDip=48 origStuckPlace=3 origTrayState=0x2
[TopBarHook] Apply(en=1 top=1 dip=30 allMon=1 ok=1) edge:3->1 dip:48->30 rect:(0,1032,1920,1080)->(0,0,1920,30) work:(0,0,1920,1080)->(0,30,1920,1080) vis=1
[XamlCtrl] First taskbar/tray XAML attach! role=TaskbarFrame name=TaskbarFrame type=Taskbar.TaskbarFrame
[XamlCtrl] Applied(en=1 top=1 h=30 bd=0) total=46 bits=0xF [HostG=1 TbF=1 TbRG=1 TrayF=1 TrayG=1 TrayI=18 Rep=2 App=7 Start=1 Srch=1 TV=1 Wdg=1 BgF=2 BgS=2 NCB=1 CCB=1 Time=1 Date=1]
[TopBarHook] Repositioned tray popup hwnd=0x... from (1682,-124 148x124) to (1682,34) below top bar (barBottom=30)
```
