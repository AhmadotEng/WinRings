# WinRings — Building, Verification & Diagnostics (v4.0.0)

Zero-external-dependency MSVC C++17 (`x64`) build and verification pipeline.

---

## 1. Compiler & Security Flags (`tools/Build-WinRings.ps1`)

- **Toolchain:** Visual Studio 2022 (`MSVC v143` x64 `cl.exe`, `link.exe`, `rc.exe`, `dumpbin.exe`) + Windows 11 SDK (`vcvars64.bat` auto-discovered across BuildTools / Community / Professional / Enterprise).
- **Compiler:** `/nologo /std:c++17 /O2 /MT /EHsc /W4 /WX /guard:cf /Brepro /utf-8 /DUNICODE /D_UNICODE`
- **Linker:** `/CETCOMPAT /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /GUARD:CF /Brepro`

---

## 2. Build Stages (`.\tools\Build-WinRings.ps1`)

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\Build-WinRings.ps1
```

Compiles 4 targets cleanly and writes staged outputs under `build\stage\`:

| Stage | Output | Inputs / Packaging Notes |
| :--- | :--- | :--- |
| **`[1/4]`** | `build\tests\WinRingsTests.exe` | Links `WinRingsConfig.cpp` + `tests\WinRingsTests.cpp` (`/SUBSYSTEM:CONSOLE`). |
| **`[2/4]`** | `build\bin\WinRingsShell.dll` | Links `WinRingsConfig.cpp` + all `src\shell\*.cpp` + `WinRingsShell.def` (`D3D11`, `D2D1`, `DWrite`, `DComp`, `DXGI`, `Dwmapi`, `WIC`, `WinHTTP`, `UxTheme`, `WindowsApp`). |
| **`[3/4]`** | `build\bin\WinRingsHost.exe` | Links `WinRingsConfig.cpp` + `SettingsWindow.cpp` + `WinRingsHost.cpp` + `WinRingsHost.rc` (embeds `winrings_logo.png` as `RT_RCDATA 201`) + `WinRingsHost.manifest` (`D3D11`, `D2D1`, `DWrite`, `DXGI`, `Dwmapi`, `UxTheme`, `ComDlg32`, `Wtsapi32`, `Bcrypt`, `Windowscodecs`). |
| **`[4/4]`** | `build\bin\WinRingsSetup.exe` | Computes SHA-256 digests of `WinRingsHost.exe` and `WinRingsShell.dll` into `build\gen\payload_hashes.h`, embeds both binaries as `RT_RCDATA` (`101`, `102`), and links `WinRingsSetup.cpp`. |

Staged deliverables:
- `build\stage\WinRings\WinRingsHost.exe`
- `build\stage\WinRings\WinRingsShell.dll`
- `build\stage\WinRingsSetup\WinRingsSetup.exe`

---

## 3. Verification Gates (`.\tools\Test-WinRingsProduct.ps1`)

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\Test-WinRingsProduct.ps1
```

Must finish with **`Test-WinRingsProduct: ALL CHECKS PASSED`** across all 5 automated checks:

1. **PE Architecture & Zero `.NET`/CLR Check:** Runs `dumpbin /headers` (`8664 machine (x64)`) and `dumpbin /imports` (rejects `mscoree`, `clr.dll`, `coreclr`, `hostfxr`, `hostpolicy`, `System.Runtime`) across all 3 staged deliverables.
2. **DLL Export Audit:** Confirms `WinRingsShell.dll` exports `WinRings_GetApi` and `WinRings_RunPreview` cleanly.
3. **CLI `--version` Smoke Check:** Verifies exit code `0` for `WinRingsHost.exe` and `WinRingsSetup.exe`.
4. **ABI Probe (`build\tests\WinRingsAbiProbe.exe`):** Loads `WinRingsShell.dll` outside `explorer.exe`, confirms `WinRings_GetApi(0xDEADBEEF) == nullptr`, validates `WINRINGS_ABI_VERSION`, `cbSize`, `productVersion`, `RequestShutdown`, and `WinRings_RunPreview`.
5. **Unit Test Suite (`WinRingsTests.exe` -> `WinRingsTests: ALL PASSED`):**
   - `TestRadialMath` — Sector anchors, `18 DIP` deadzone, outer-arc symmetry, hover corridors, DPI clamping, scroll wrapping.
   - `TestDownloadsAndSubmenuHoverTransitions` — Annular arc band hit-testing, direct switch across main-ring slots, top search arc transition (`-2`), outer reach cap.
   - `TestSearchScoring` — 4-tier matcher (prefix, word/CamelCase, acronym, substring, aliases).
   - `TestCrashGuard` — 3-in-180s sliding failure window and reset behavior.
   - `TestConfigRoundTrip` — JSON v4 serialization/deserialization, defaults (`A4SAcrylic`, `glassLevel = 50`, `lightDarkMode = 0`, 2 default triggers), clamps, pinned slots & collections, and v2/v3 -> v4 migration.
   - `TestThemeMaterialEngine` — `A4SAcrylic` calibrated native values, `TaskbarGlass` wide HostBackdrop curves, `PureMatte` zero-blur tint, arbitrary custom `surfaceColor` / `accentColor` parsing, and `GenerateAcrylicNoiseBgra256` mid-gray peak bounds.
   - `TestTriggerModelAndDeltaIpc` — Key normalization (`RWin`/`RAlt`/`RCtrl`/`RShift`), `FormatTriggerBinding`, `AnalyzeTriggerConflict` notes, and surgical `ApplyDeltaToConfig` updates preserving `pinnedSlots`.
   - `TestSlotMatchingAndPreviews` — Explorer path detection, Downloads/Recycle Bin matching, PiP gating strictly on `SlotKind::App`.
   - `TestDragSpringAndReorder` — `500 ms` dwell + `420 ms` double-heartbeat pulse curve, reorder target clamping, vector element rotation.
   - `TestHiddenPinsSearchAndReorderMapping` — Hidden search-only pin visibility, primary vs. secondary alias ranking, search selection persistence, and visible-to-config reorder mapping.
   - `TestSettingsStageAndPreviewMath` — Tab preview visibility (`General`/`Appearance`/`Profiles`), DPI stage-center calculation, preview clamping, active `SetWindowRgn` hit-region clipping, `WM_LBUTTONDOWN` outer-radius press guard, sidebar hover alphas, unified profile container height, footer status toggle, and profile delete-action gating.

---

## 4. Safe Interactive & Live Testing

### Non-Injecting Preview & Settings Window (Recommended During Development)
```powershell
# Standalone radial overlay outside explorer.exe (tests WCA glass, TaskbarGlass, PureMatte, 3D bezels, hit-testing)
.\build\stage\WinRings\WinRingsHost.exe --preview

# Combined Tray + Handy-style Settings Window (destroys GPU/UI resources when closed)
.\build\stage\WinRings\WinRingsHost.exe --open
```

### Opt-In Live Explorer Lifecycle Gate
> Ensure any previously injected `WinRingsShell.dll` from another folder is exited (`--exit`) before running, because a loaded DLL stays pinned in `explorer.exe` until Explorer restarts.

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\Test-WinRingsLifecycle.ps1 -AllowExplorerInjection
```
Validates live injection, `--status` readiness (`ring=1`), `--toggle` open/close, `--reload`, `--exit` cleanup, and confirms the original `explorer.exe` PID stays healthy.

---

## 5. Runtime State & Telemetry Locations

| Artifact | Path | Purpose |
| :--- | :--- | :--- |
| **User Config** | `%APPDATA%\WinRings\config.json` & `profiles\*.json` | JSON v6 global settings + standalone per-profile pin files (`SwitcherConfig`) |
| **Favicon Cache** | `%APPDATA%\WinRings\icons\` | Cached website icons downloaded async by `SlotModel::FetchFaviconAsync` |
| **Diagnostic Log** | `%APPDATA%\WinRings\shell_diag.log` | Host/shell handshake, trigger, DWM/WCA activation, and error telemetry |
| **Safety Lock Registry** | `HKCU\Software\WinRings\HostSafety` | `CrashGuard` crash timestamps & `InjectionDisabled` flag (cleared via `--reset-safety`) |
| **Installed Binaries** | `%LOCALAPPDATA%\WinRings\v1.0.0.0\` | `WinRingsSetup.exe` target installation folder |
