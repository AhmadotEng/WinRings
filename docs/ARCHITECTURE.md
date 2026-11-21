# WinRings — Technical Architecture & Internals

This document details the internal architecture of **WinRings**, how it integrates into Windows 11 `explorer.exe`, and how the Radial Switcher composites native WCA acrylic rings alongside live DWM window thumbnails. All Windows taskbar customization has been cleanly separated into the standalone **ClearTaskbar** host so **A4S-Menu**, **WinRings**, and **ClearTaskbar** run independently with zero hook overlap.

---

## 1. Binary Decomposition & Process Model

WinRings is split into four pure native x64 binaries compiled with `/std:c++17 /MT /O2 /EHsc /W4 /WX` (static CRT, zero `.NET`/CLR dependencies):

```text
WinRingsSetup.exe (Single-File Installer)
  ├── WinRingsHost.exe   (Out-of-process supervisor, Tray UI, CrashGuard, CLI)
  ├── WinRingsShell.dll  (In-process Explorer shell extension: Radial Switcher)
  └── WinRingsTools.exe  (Out-of-process dark-mode 4-page Settings UI & 1:1 GPU preview)
```

### Why Separate `WinRingsHost.exe`, `WinRingsShell.dll`, and `WinRingsTools.exe`?
1. **`explorer.exe` Isolation & Safety:** Heavy configuration UI rendering (the 4-tab Direct2D 1.1 Settings & 1:1 live preview dialog) runs out-of-process inside `WinRingsTools.exe` so `explorer.exe` stays lean and responsive.
2. **Lifecycle Supervision & Crash-Loop Circuit Breaker:** `WinRingsHost.exe` monitors the target `explorer.exe` process handle (`MsgWaitForMultipleObjectsEx`). If `explorer.exe` exits abnormally 3 times within a rolling 180-second window (`CrashGuard.h`), `WinRingsHost.exe` trips `HKCU\Software\WinRings\HostSafety\InjectionDisabled = 1` and refuses further injection until reset (`WinRingsHost.exe --reset-safety`).
3. **Clean Taskbar Restart Recovery:** When `explorer.exe` restarts, `WinRingsHost.exe` receives the registered `TaskbarCreated` broadcast, re-registers its promoted cyan ring tray icon, and re-injects `WinRingsShell.dll` automatically.

---

## 2. Explorer Injection & Handshake Protocol

1. **Target Validation (`WinRingsHost.cpp`):**
   - Locates `Shell_TrayWnd` / `Progman`, verifies the owning process is `%SystemRoot%\explorer.exe` in the same session, running under the same user SID, and matching x64 architecture.
2. **Cryptographic Attempt Cookie:**
   - Generates a random 64-bit cookie via `RtlGenRandom` (`SystemFunction036`) and publishes it on `WinRingsHost.exe`'s message window (`WinRings.HostAttemptCookie`).
3. **In-Process Bootstrap (`DllMain.cpp` & `ShellController.cpp`):**
   - `WinRingsHost.exe` maps `WinRingsShell.dll` into `explorer.exe` via `CreateRemoteThread` + `LoadLibraryW` (or sends `WINRINGS_CMD_REATTACH` if the exact DLL is already pinned in `explorer.exe`).
   - `DllMain` (`DLL_PROCESS_ATTACH`) calls `DisableThreadLibraryCalls` and spawns a lightweight bootstrap thread (`ShellController`).
   - `ShellController` creates a message-only control window (`WinRings.ShellControlWindow`), reads the host's attempt cookie, and replies with `WINRINGS_DLL_STATUS_READY`.

---

## 3. Radial Switcher, Live Acrylic Backdrop & DWM Thumbnail Engine

### 3.1 Native A4S WCA Acrylic Backdrop + Antialiased Direct2D Foreground (`RingOverlay.cpp`, `AcrylicBackdrop.cpp`, `RingThemeEngine.h`)
- **Primary Native A4S Backdrop (`WinRings.WcaBackdrop.v1`):**
  - Synchronized non-layered `WS_POPUP` (`WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE`) locked immediately behind `WinRings.Overlay.v1`.
  - Applies the exact A4S-Menu DWM backdrop pipeline (`DWMWA_USE_IMMERSIVE_DARK_MODE = 20`, `DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_DONOTROUND`, `DWMWA_BORDER_COLOR = DWMWA_COLOR_NONE`, `WCA_FORCE_ACTIVEWINDOW_APPEARANCE = 15`, `WCA_ACCENT_POLICY = 19` with `state = 4`, `flags = 0x13`, `gradientColor = 0x80202020` at default `A4S`, and `BeginBufferedPaint(..., BPPF_ERASE)` transparent 32-bit surface).
  - Hash-gated `SetWindowRgn` clips the native WCA acrylic to the hollow ring annulus (`CombineRgn` `RGN_DIFF`), curved outer arcs (`CreatePolygonRgn`), window cards, and search/center pills, inset by `1.0 px` on every perimeter.
- **Antialiased Direct2D + DirectComposition Foreground (`WinRings.Overlay.v1`):**
  - Renders per-pixel antialiased `2.0 DIP` perimeter edge-bridging, top-lit 3D outer glass bezel gradients, optically recessed 3D inner bevel gradients, soft elevation shadow lips, icons, running dots, hover reveal, and typography via lightweight `IDCompositionDevice` + `IDXGISwapChain1` without painting synthetic noise over the native WCA texture.
- **Automatic Composition Fallback (`AcrylicBackdrop.cpp`) & Shared Material Engine (`RingThemeEngine.h`):**
  - If `SetWindowCompositionAttribute` is unavailable, `AcrylicBackdrop.cpp` falls back to `Windows::UI::Composition` `HostBackdropBrush` (`CLSID_D2D1Flood` straight RGBA + `D2D1_BLEND_MODE_COLOR` luminosity pass + `SOURCE_OVER` tint) calibrated within `±1/255` of both live A4S-Menu and Direct2D 1.1 (`SettingsDialog.cpp` 1:1 preview).
- Honors `disableOpeningAnimation` (frame-0 instant open) as well as Windows `SPI_GETCLIENTAREAANIMATION`.

### 3.2 Pooled Non-Layered DWM Thumbnail Windows (`DwmThumbPool.cpp`)
- Windows `DwmRegisterThumbnail` requires the destination `HWND` to be a top-level window and renders poorly or fails on `WS_EX_LAYERED` windows.
- `DwmThumbPool` pre-allocates up to 9 non-layered top-level tool windows (`WinRings.ThumbHost`) with `DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_ROUND` and positions them synchronously along the outer arc (`RadialMath::ComputeOuterArcAngles`) whenever a multi-window slot is hovered.

### 3.3 Bounded Main-Ring Hit-Testing & Submenu Hover-Corridor (`RingOverlay.cpp` & `RadialMath.h`)
- Main-ring slot hover selection is strictly bounded to the ring annulus and icon band (`dist >= ringDeadzoneDip && dist <= outerRadius + 14 DIP`) so moving the cursor out into empty outer space never accidentally activates an unexpanded slot.
- When an outer-arc submenu is expanded (`m_expandedSlot >= 0`), moving the cursor toward the outer arc keeps the submenu open via `IsPointInSubmenuCorridor`, while moving onto a different main-ring slot inside the annulus switches selection immediately.

### 3.4 System-Wide Application Indexer & Top Half-Ring Search (`AppIndex.cpp` & `SearchMatch.h`)
- Enumerates Start Menu shortcuts (`.lnk`) and `shell:AppsFolder` applications asynchronously in the background and scores queries via `ScoreSearchCandidate` (exact prefix, word-boundary prefix, substring, and subsequence matching).
- Typing while the Ring is open renders a floating top search pill (`#515151` border with search icon), highlights any matching pinned or running slot on the main ring, and displays up to 6 matching unpinned system applications in a curved top half-ring.

---

## 4. Clean Separation from Taskbar & Start Menu Tools

WinRings intentionally performs **zero** `Taskbar.dll` detours, **zero** `SystemTray.dll` detours, and **zero** `InitializeXamlDiagnosticsEx` / COM class-factory exports (`WinRingsShell.def` exports only `WinRings_GetApi` and `WinRings_RunPreview`). This guarantees conflict-free coexistence with **A4S-Menu** (Start Menu) and the standalone **ClearTaskbar** host (top taskbar, clear backdrop, FullWorkArea, and optional auto-hide boundary).
