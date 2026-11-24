# AGENTS.md — WinRings Engineering Guide

Pure native C++17 (`x64`, static `/MT` CRT, zero `.NET`/CLR) **Windows 11 Radial Switcher & Gesture Launcher** (`WinRingsHost.exe`, `WinRingsShell.dll`, `WinRingsTools.exe`, `WinRingsSetup.exe`).

---

## Core Engineering Methodology & Invariants

1. **Pure logic first, Win32 wiring second:** Put geometry, hit-testing, theme material math, search ranking, slot matching, config parsing, and safety state machines into header-only `src/shared/*.h`. Cover every new algorithm in `tests/WinRingsTests.cpp` before wiring `src/shell/`, `src/host/`, `src/tools/`, or `src/setup/`.
2. **Single visual source of truth:** All presets (`A4S`, `DarkWood`, `Blue`), slider curves (`glassLevel`, `lightDarkMode`), strokes, and noise textures live in `src/shared/RingThemeEngine.h` and drive both live `RingOverlay` and `SettingsDialog`'s 1:1 GPU preview.
3. **Preserve the approved A4S native WCA material & 3D edges:** Keep `WinRings.WcaBackdrop.v1`'s native `ACCENT_ENABLE_ACRYLICBLURBEHIND` interior untouched (paint zero synthetic noise when WCA is active), clip `SetWindowRgn` inset by `1.0 px`, and bridge the perimeter on `WinRings.Overlay.v1` with the `2.0 DIP` antialiased edge-bridge, elevation shadow lip, and 3D glass bezel gradients.
4. **Protect `explorer.exe`:** Keep heavy Settings/tools UI out-of-process inside `WinRingsTools.exe`. Keep `WinRingsShell.dll` scoped strictly to the Radial Switcher (exports only `WinRings_GetApi` and `WinRings_RunPreview`; leave taskbar and Start Menu customization to separate companion tools). Validate changes via `--preview` and `Test-WinRingsProduct.ps1` before running live Explorer injection, and leave the running `explorer.exe` process undisturbed unless explicitly asked.
5. **Compiler & native binary bar:** Compile all targets clean with `/std:c++17 /W4 /WX /MT /guard:cf` and maintain zero `.NET`/CLR imports.

---

## Standard Development & Verification Loop

Execute steps in order; do not report work complete until every required gate prints its pass sentinel:

1. **Pure logic & unit tests:** Update `src/shared/*.h` and add deterministic assertions in `tests/WinRingsTests.cpp`.
2. **Runtime implementation:** Wire shell, host, tools, or setup changes under `src/`.
3. **Build all 5 targets:**
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\tools\Build-WinRings.ps1
   ```
   Done when output ends with `Build-WinRings: SUCCEEDED` (zero `/W4 /WX` warnings/errors).
4. **Non-injecting product gate:**
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\tools\Test-WinRingsProduct.ps1
   ```
   Done when output prints `Test-WinRingsProduct: ALL CHECKS PASSED` (verifies x64 PE headers, zero CLR imports, DLL exports, `--version`, `WinRingsAbiProbe: OK`, and `WinRingsTests: ALL PASSED`).
5. **Safe UI & preview verification (no Explorer injection):**
   - `.\build\stage\WinRings\WinRingsHost.exe --preview`
   - `.\build\stage\WinRings\WinRingsTools.exe --settings`
6. **Optional live Explorer test / install (only when explicitly requested or validating shell hooks):**
   - `powershell -ExecutionPolicy Bypass -File .\tools\Test-WinRingsLifecycle.ps1 -AllowExplorerInjection`
   - `.\build\stage\WinRingsSetup\WinRingsSetup.exe --install --silent`

---

## Context Pointers (Read On Demand by Task Domain)

- **`docs/ARCHITECTURE.md`** — Consult when modifying process boundaries, Explorer injection/attempt-cookie handshake, IPC (`WinRingsApi.h`), `CrashGuard`, `SlotModel`, `WindowTracker`, `AppIndex`, `DwmThumbPool`, or decomposing large modules (`RingOverlay.cpp`, `SettingsDialog.cpp`).
- **`docs/VISUAL_SYSTEM.md`** — Consult when changing themes, native WCA acrylic backdrop, `AcrylicBackdrop` (`dwmcore`), `1.0 px`-inset regions, 3D glass rims, `RingThemeEngine.h`, frosted noise texture, or the `SettingsDialog` 1:1 GPU preview.
- **`docs/INTERACTION_AND_MATH.md`** — Consult when modifying `InputEngine` triggers, click-vs-drag behavior, `RadialMath.h` geometry/corridors, OLE drag-and-drop (`DropTarget`), spring-loaded hover-to-open, drag-to-reorder, Explorer folder/Recycle Bin matching (`SlotMatching.h`), or 4-tier app search (`SearchMatch.h`).
- **`docs/BUILDING_AND_TESTING.md`** — Consult when changing compiler flags/libraries, adding `.cpp` files to `Build-WinRings.ps1`, changing DLL exports/ABI, updating installer payloads, or reading `%APPDATA%\WinRings\shell_diag.log`.
- **`docs/ROADMAP_AND_IDEAS.md`** — Consult when designing WinRings v3.1+ enhancements, proposing new ring interactions, or planning structural refactors.
