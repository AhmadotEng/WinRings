# WinRings — System Architecture & Module Reference (v4.0.0)

WinRings is a three-binary (two runtime binaries + single-file installer) pure native C++17 (`x64`, static `/MT` CRT, zero `.NET`/CLR) application integrating cleanly into Windows 11 `explorer.exe` while keeping the Explorer supervisor, resident tray icon, on-demand Handy-style Settings UI (`WinRingsHost.exe`), and installer (`WinRingsSetup.exe`) out-of-process.

---

## 1. Binary Decomposition & Process Model

```mermaid
flowchart LR
    Setup["WinRingsSetup.exe<br/>(BCrypt SHA-256 Installer)"] -->|Extracts & Verifies| Host["WinRingsHost.exe<br/>(Supervisor, Tray Icon & On-Demand Handy-Style UI)"]
    Setup -->|Extracts & Verifies| Shell["WinRingsShell.dll<br/>(Explorer In-Process Switcher)"]

    Host -->|CreateRemoteThread + LoadLibraryW| Shell
    Host <-->|Direct Single-Hop Messages + WM_COPYDATA| Shell
```

### Combined Host + Zero-Resource Hidden Mode
1. **Single Combined `WinRingsHost.exe`:** Owns the headless Explorer supervisor, the resident blue interlocking twin-ring tray icon, and the on-demand `840x560 DIP` Handy-style Direct2D Settings window (`SettingsWindow.cpp` — **General**, **Appearance**, and unified **Profiles** sidebar with embedded `winrings_logo.png` sticker wordmark and clickable bottom-left status pill). Passing `--no-tray` runs strictly headless, while closing (`X`) the Settings window destroys its `HWND`, releases all D3D11/DXGI/Direct2D/DirectWrite/WIC GPU resources, and trims process working set memory so idle tray usage stays at `0%` GPU / minimal RAM.
2. **Crash-loop safety (`CrashGuard`):** `WinRingsHost.exe` monitors `explorer.exe` via `MsgWaitForMultipleObjectsEx`. If `explorer.exe` exits abnormally **3 times within 180 seconds** (`kCrashLoopThreshold = 3`, `kCrashLoopWindow100ns` in `src/shared/CrashGuard.h`), `WinRingsHost.exe` writes `InjectionDisabled = 1` under `HKCU\Software\WinRings\HostSafety` and halts future injection until `--reset-safety`.
3. **Resilient recovery:** Receiving `TaskbarCreated` re-injects `WinRingsShell.dll` automatically if `explorer.exe` restarted and re-registers the tray icon cleanly.

---

## 2. Explorer Injection, Surgical Delta IPC & ABI Contract

Defined in `src/shared/WinRingsIdentity.h`, `src/shared/WinRingsApi.h`, and `src/shared/WinRingsDeltaIpc.h`:

- **Target validation (`WinRingsHost.cpp`):** Verifies `Shell_TrayWnd`/`Progman` belongs to `%SystemRoot%\explorer.exe` in the same session, same user SID, and x64 architecture.
- **Cryptographic attempt cookie:** Generated via `SystemFunction036` (`RtlGenRandom`) and stored on `WinRings.Host.Window.v1` property `WinRings.Host.AttemptCookie.v1`.
- **Reattach support:** If the exact `WinRingsShell.dll` path is already loaded in `explorer.exe`, `WinRingsHost` sends `WINRINGS_CMD_REATTACH` rather than loading a second instance.
- **Lock / RDP / UAC suppression:** `WinRingsHost.cpp` uses `WTSRegisterSessionNotification` + `EVENT_SYSTEM_DESKTOPSWITCH` to send `WINRINGS_CMD_SUSPEND_INPUT` on lock/RDP/secure desktop transitions and `WINRINGS_CMD_REFRESH_INPUT` on unlock (preserving user pause state).
- **Direct Single-Hop `WM_COPYDATA` Delta IPC (`WINRINGS_COPYDATA_DELTA_V1`):** `SettingsWindow.cpp` inside `WinRingsHost.exe` sends `WinRingsDeltaPacket` (`SetLook`, `SetGestures`, `SetTriggers`, `PreviewLiveRing`, `FlushSaveToDisk`, `SetRingPaused`) directly to `ShellController` in memory (or `WinRings.Preview.Control.v1` in `--preview`) — zero intermediate relay and zero `config.json` re-parsing per slider/color/shortcut change; atomic save runs once when the Settings window closes.

### Registered Messages & Commands
- **Host Control (`WinRings.Host.Control.v1`):** `TOGGLE (1)`, `EXIT (2)`, `STATUS (3)`, `RELOAD (4)`, `RESET_SAFETY (5)`, `QUERY_DETAILS (7)`, `SET_PAUSED (8)`, `OPEN_UI (9)`.
- **Shell Command (`WinRings.Shell.Command.v1`):** `TOGGLE_RING (1)`, `EXIT (2)`, `SUSPEND_INPUT (3)`, `REFRESH_INPUT (4)`, `RELOAD_CONFIG (5)`, `QUERY_SHELL_STATE (7)` (`WINRINGS_STATE_RING_READY = 1 << 0`, `WINRINGS_STATE_RING_PAUSED = 1 << 1`), `REATTACH (0xFFFF)`.
- **Shell Status (`WinRings.Shell.Status.v1`):** `READY (1)`, `FAILED (2)`, `DISARMED (3)`.
- **Exports (`src/shell/WinRingsShell.def`):** Exclusively `WinRings_GetApi` (`WINRINGS_ABI_VERSION = 0x00010000`) and `WinRings_RunPreview` (used by `--preview` outside `explorer.exe`). Zero COM class factory exports, zero taskbar/XAML hooks.

---

## 3. Complete Module Inventory

### `src/shared/` — Header-Only Pure Logic & Shared Config
| File | Responsibility |
| :--- | :--- |
| `WinRingsIdentity.h` | Product strings, mutexes (`Host`, `Setup`), window classes, message names, version constants |
| `SettingsLayoutMath.h` | Pure preview-stage visibility gating, stage-center physical coordinate math, preview-clamp bounds, active `SetWindowRgn` hit-region math (`ComputePreviewOverlayHitRadiusDip`, `ShouldClipOverlayToActiveRegion`, `IsRingSlotPressWithinOuterRadius`), sidebar hover & unified card growth (`ResolveSidebarIconHoverAlpha`, `ComputeUnifiedProfilesCardBottomDip`), and footer/profile action predicates (`ShouldFooterStatusClickTogglePause`, `ShouldShowProfileDeleteAction`) |
| `WinRingsApi.h` | Host/Shell command enums, status/pause bits, C ABI (`WinRingsApi`, `WinRings_GetApi`, `WinRings_RunPreview`) |
| `WinRingsDeltaIpc.h` | POD `WinRingsDeltaPacket` (`WM_COPYDATA`), `ApplyDeltaToConfig`, registry System Light/Dark & Accent resolvers |
| `TriggerModel.h` | Unified `TriggerBinding` (`MouseButton`, `RightLeftChord`, `KeyChord`, `SoloModifier`, `profileId`), key formatting, conflict analyzer |
| `WinRingsConfig.h` / `.cpp` | Config v6 schema (`SwitcherConfig`, `RingProfileConfig` standalone per-profile files under `%APPDATA%\WinRings\profiles\<id>.json` with `PinnedSlotConfig::hiddenFromRing` for search-only pins, one-time legacy `search.json` migration into `default`, `SMenu` factory, `SurfaceTexture`, `triggers`, `PinnedSlotConfig`), v1–v5 auto-migration, atomic JSON reader/writer (`%APPDATA%\WinRings\config.json`) |
| `RadialMath.h` | Polar sectors (`CalculateSectorIndex`), `ComputeRingGeometry`, `ComputeIconArcLayout`, `ComputeWindowOuterArcRects`, `ComputeTitleColumnRects`, `ResolveRingHover`, `IsPointInOuterSubmenuZone`, `IsPointInHoverCorridor`, safe pointer-glide math (`ResolveRestoredWindowRect`, `ComputeSafePointerGlideTarget`, `IsUserPointerOverride`), clamping/easing |
| `RingThemeEngine.h` | Decoupled `SurfaceTexture` (`A4SAcrylic`, `TaskbarGlass`, `PureMatte`) + arbitrary surface/accent colors, `glassLevel`/`lightDarkMode` curves, 3D bezel & edge-bridge tokens, `GenerateAcrylicNoiseBgra256()` |
| `SlotMatching.h` | Explorer EXE check, window PiP gating (`ShouldShowSlotWindowPreview`), `ExplorerWindowMatchesFolder` / `RecycleBin`, `EvaluateSpringHover`, `ComputeReorderTargetSlotIndex`, `ReorderVectorElement`, `MapVisiblePinnedIndexToConfigIndex`, `MapVisiblePinnedInsertPosToConfigIndex`, `ShouldKeepSearchSelectionOnPointer` |
| `SearchMatch.h` | 4-tier query matcher (`ScoreSearchMatch`, `ScoreAliases`: Prefix/Word/Acronym/Substring), alias extraction (`BuildBasicSearchAliases`), canonical key normalization (`CanonicalSearchKey`), and source-priority ranking (`CompareSearchRankKey`) |
| `CrashGuard.h` | Rolling 3-exits-in-180s crash-loop safety state machine (`AdvanceCrashGuard`) |

### `src/shell/` — In-Process Explorer Module (`WinRingsShell.dll`)
| File | Responsibility |
| :--- | :--- |
| `DllMain.cpp` | Pins DLL module, exports `WinRings_GetApi` & `WinRings_RunPreview`, kicks off `ShellController` |
| `ShellController.h/.cpp` | Message-only control HWND (`WinRings.Shell.Control.v1` / `WinRings.Preview.Control.v1`), `WM_COPYDATA` delta handler, host watchdog, coordinates `WindowTracker`, `AppIndex`, `SlotModel`, `InputEngine`, `RingOverlay` |
| `RingOverlay.h/.cpp` + `ContextFlyout.h/.cpp` | Runtime backdrop switcher (`WcaDualHwnd` / `TaskbarComposition` / `MatteDComp`), Direct3D11/Direct2D1/DirectComposition renderer, single-instance Direct2D taskbar-style right-click popup (`End task`, `Close window`, `Run as administrator`, `Hide from ring`), hit-testing, outer-arc & search submenus, drag-reorder, spring-load pulse, file/URL drop handling |
| `AcrylicBackdrop.h/.cpp` | `Windows::UI::Composition` `HostBackdropBrush` + strict-tree `dwmcore` D2D1 effect graph powering `TaskbarGlass` |
| `DwmThumbPool.h/.cpp` | Pre-allocated pool of 9 non-layered rounded (`DWMWCP_ROUND`) `HWND`s (`WinRings.ThumbCard.v1`) for `DwmRegisterThumbnail` live window previews |
| `InputEngine.h/.cpp` | `WH_MOUSE_LL` + `WH_KEYBOARD_LL` unified `TriggerBinding` evaluator, solo `Win`/`Alt` clean tap with `0xE8` mask tap, `Alt+Tab`/`Win+Tab` chord stepping, gesture toggles, clean click-vs-drag outside-dismiss, wheel interception, type-to-search |
| `DropTarget.h/.cpp` | COM `IDropTarget` for ring drops + `IDropSource`/`SHDoDragDrop` (`StartFileDragOut`) for dragging files out of Downloads/Folder arcs |
| `WindowTracker.h/.cpp` | `EVENT_SYSTEM_FOREGROUND` WinEvent + shell hook MRU list, icon extraction/caching (preserving native `File Explorer` icon), `ActivateWindow` + cancellable 160 ms pointer glide |
| `SlotModel.h/.cpp` | Merges `pinnedSlots` with live `AppWindowGroup`s, custom `iconCacheFile` override loading, newest Downloads/Folder files, launches apps/URLs, `FO_COPY` folder drops, Recycle Bin ops, cookie-free WinHTTP favicon downloader (`%APPDATA%\WinRings\icons\`) |
| `AppIndex.h/.cpp` | Background Start Menu `.lnk` + `shell:AppsFolder` indexer, PE friendly-name lookup, search query executor |

### `src/host/`, `src/setup/`
| File | Responsibility |
| :--- | :--- |
| `src/host/WinRingsHost.cpp` | Single combined app: Explorer injection loop, `CrashGuard` Registry persistence, WTS session watch, `WM_COPYDATA` delta bridge, CLI dispatcher (`--open`, `--settings`, `--no-tray`, `--preview`, `--exit`) |
| `src/host/SettingsWindow.h/.cpp` | Resident blue twin-ring tray icon + on-demand `840x560 DIP` Handy-style Direct2D Settings window (`General`, `Appearance` with palette icon, unified expandable `Profiles` sidebar with `+`/rename/reset/delete, embedded `winrings_logo.png` RCDATA logo, clickable corner `Active`/`Paused` pill, reserved live-Ring stage, zero-GPU destroy-on-close & working-set trim) |
| `src/setup/WinRingsSetup.cpp` | Single-file installer (`RT_RCDATA` payloads verified with `BCrypt` SHA-256), installs `WinRingsHost.exe` + `WinRingsShell.dll` to `%LOCALAPPDATA%\WinRings\v1.0.0.0\`, cleans legacy `WinRingsTools.exe` / `WinRingsTray`, registers `WinRings` Run key + Start Menu shortcut (`--open`) |

---

## 4. Runtime Data Flows & Boundary Guarantee

- **Opening & Slot Assembly:** On trigger (`InputEngine`), `ShellController` asks `SlotModel::BuildRingSlots()` to merge configured `pinnedSlots` with live `WindowTracker::EnumerateGroupedWindows()` up to `maxSlots`.
- **Combined Tray & Stage Sync Flow:** Left-clicking the Tray Icon opens `WinRings.SettingsWindow.v1` directly inside `WinRingsHost.exe` from `%APPDATA%\WinRings\config.json` and `profiles\*.json`. **General** hides the Ring; switching to **Profiles** or **Appearance** sends `DeltaOp::PreviewLiveRing(1)` with `ComputeRingStageCenterPhysical` so the real `RingOverlay` (`HWND_TOPMOST`) aligns cleanly inside the window's reserved stage and follows window moves/minimization/focus automatically. Closing (`X`) the window sends `FlushSaveToDisk` once, destroys the window + D3D/D2D COM objects, and trims working set memory.
- **Independent Companion Boundary:** WinRings never hooks `Taskbar.dll`, `SystemTray.dll`, or XAML visual diagnostics, leaving taskbar and Start Menu customization strictly to separate host processes.
