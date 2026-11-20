# WinRings — Technical Architecture & Internals

This document details the internal architecture of **WinRings**, how it integrates into Windows 11 `explorer.exe`, how the Radial Switcher composites GPU-accelerated rings alongside live DWM window thumbnails, and how the Unified Minimal Top Bar docks and compacts the Windows 11 XAML taskbar without instability.

---

## 1. Binary Decomposition & Process Model

WinRings is split into four pure native x64 binaries compiled with `/std:c++17 /MT /O2 /EHsc /W4 /WX` (static CRT, zero `.NET`/CLR dependencies):

```text
WinRingsSetup.exe (Single-File Installer)
  ├── WinRingsHost.exe   (Out-of-process supervisor, Tray UI, CrashGuard, CLI)
  ├── WinRingsShell.dll  (In-process Explorer shell extension: Ring + Top Bar)
  └── WinRingsTools.exe  (Out-of-process dark-mode Settings UI & media/file tools)
```

### Why Separate `WinRingsHost.exe`, `WinRingsShell.dll`, and `WinRingsTools.exe`?
1. **`explorer.exe` Isolation & Safety:** Heavy file I/O (image conversion, `ffmpeg` media trimming, `.zip` archiving, interactive image redaction, and the 4-tab Settings dialog) runs out-of-process inside `WinRingsTools.exe` so `explorer.exe` never blocks or loads multimedia codecs.
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

### 3.1 Dual-Swap-Chain Direct2D + DirectComposition + `Windows::UI::Composition` Ring (`RingOverlay.cpp`, `AcrylicBackdrop.cpp`, `RingThemeEngine.h`)
- Creates a top-level `WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP` overlay window (`WinRings.Overlay.v1`).
- **Live Wallpaper-Reactive Frosted Acrylic (`AcrylicBackdrop.cpp`):**
  - Enables `DWMWA_USE_HOSTBACKDROPBRUSH` (`17`), `DwmExtendFrameIntoClientArea({-1, -1, -1, -1})`, and `WCA_FORCE_ACTIVEWINDOW_APPEARANCE` (`15`).
  - Builds a strict-tree `Windows::UI::Composition` effect factory (`GaussianBlur (30 px) -> Saturation -> Blend(D2D1_BLEND_MODE_LUMINOSITY) -> Composite(SOURCE_OVER)`) reading `Compositor::CreateHostBackdropBrush()`.
  - Clips the desktop acrylic backdrop through `m_acrylicSwapChain` to the true ring annulus (`holeRadius = 60 DIP`, `outerRadius = 148 DIP`, `ringRadius = 112 DIP`), curved outer submenu arcs (`ComputeIconArcLayout`), window preview cards, drop pills, search pill, and center label pill, while rendering crisp icons, typography, tiled `256x256` frosted noise grain (`m_noiseBrush`), and razor-thin `<= 0.85 DIP` hairline rims on `m_fgSwapChain`.
- **Single Shared Material Engine (`RingThemeEngine.h`):**
  - `ComputeRingMaterial` and `GenerateAcrylicNoiseBgra256` drive `WinRingsShell.dll`, `WinRingsTools.exe` (`SettingsDialog.cpp` 1:1 GPU preview), and `WinRingsTests.exe` from one source of truth across the 3 visual presets (`A4S`, `DarkWood`, `Blue`), `glassLevel (0–100)`, and `lightDarkMode (0–100)`.
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

## 4. Unified Minimal Top Bar Internals

Windows 11 25H2 (`build 26200.8875`) splits the taskbar across two system modules in `explorer.exe`:
- `C:\Windows\System32\Taskbar.dll` (`TrayUI`, `CTray`, AppBar docking, work-area reservation)
- `C:\Windows\SystemApps\MicrosoftWindows.Client.Core_cw5n1h2txyewy\SystemTray.dll` (`SystemTrayFrame`, `NotifyIconStack`, `OverflowXamlIslandManager`)

### 4.1 Why Calling `TrayUI::SetStuckPlace` Directly Crashes Windows 11 25H2
Reverse-engineering `Taskbar.dll` (`TimeDateStamp = 0xA4EA993A`, `SizeOfImage = 0x002FC000`) revealed that `TrayUI::SetStuckPlace` (`0x0E1050`) invokes `0x0E436C` (`NotifyTaskbarEdgeChanged`) whenever `Feature_59213768` (`0x0E486C`) is enabled. On Windows 11 25H2, `0x0E436C` dereferences a null WinRT XAML island edge listener pointer (`[rax] = 0` at `Taskbar.dll + 0x0E1233`) when transitioning to `ABE_TOP (1)`.

### 4.2 The Zero-WinRT 4+1 Detour Architecture (`TopBarHook.cpp`)
Instead of hooking `Feature_59213768` or calling `TrayUI::SetStuckPlace`, `TopBarHook.cpp` verifies the exact PE headers and 10-byte prologues of `Taskbar.dll` and `SystemTray.dll`, allocates CFG-aligned near-trampoline pages within `±1.75 GB`, and installs **5 surgical detours**:

| # | Module | RVA | Function | Purpose |
| :- | :--- | :--- | :--- | :--- |
| 1 | `Taskbar.dll` | `0x045EA0` | `TrayUI::GetMinSize` (Primary) | Overrides primary taskbar height to `MulDiv(heightDip, dpi, 96)` (`30 px` at 96 DPI). |
| 2 | `Taskbar.dll` | `0x075430` | `TrayUI::GetMinSize` (Secondary) | Overrides secondary monitor taskbar height to `MulDiv(heightDip, dpi, 96)`. |
| 3 | `Taskbar.dll` | `0x0161A0` | `TrayUI::GetDockedRect` | Captures the live `ITrayUI*` instance pointer and original bottom-docked metrics. |
| 4 | `Taskbar.dll` | `0x016240` | `GetAutoHideFlags` | Suppresses bottom-edge auto-hide slide-off while docked at the top edge (`ABE_TOP`). |
| 5 | `SystemTray.dll` | `0x0B9EB0` | `OverflowXamlIslandManager::PositionWindow` | Repositions the system tray overflow popup (`TopLevelWindowForOverflowXamlIsland`) directly **below** the top taskbar (`Y = barBottomY + 4 DIP`). |

To move the taskbar between the top (`ABE_TOP = 1`) and bottom (`ABE_BOTTOM = 3`) edges cleanly:
1. `TopBarHook::ApplyOnTaskbarUiThread` updates `[pITrayUI + 0x32C]` (`m_primaryTaskbarHeightDip`) via `TrayUI::UpdateTaskbarSize` (`0x093540`).
2. Writes the target edge (`1` for Top, `3` for Bottom) directly to `[pITrayUI + 0x110]` (`m_uStuckPlace`) and clears `m_rcStuck[targetEdge]` (`[pITrayUI + 0x78 + targetEdge * 16]`).
3. Calls `ITrayUI::VerifySize(pITrayUI, true, true, nullptr)` (`vtable[0x68]`, RVA `0x0E1AD0`), which recomputes the top-docked rectangle `(0, 0, screenWidth, 30)`, moves `Shell_TrayWnd`, and updates the system work area `(0, 30, screenWidth, screenHeight)` without ever entering WinRT edge listeners.

### 4.3 Why the System Tray Overflow Popup (`^`) Needed Repositioning (`SystemTray.dll + 0x0B9EB0`)
Disassembly of `SystemTray.dll!OverflowXamlIslandManager::PositionWindow` (`0x0B9EB0`) showed that when `Feature_59213768` is disabled (the Windows 11 25H2 default), `SystemTray.dll` unconditionally computes:
```cpp
int Y = anchorY - heightPx;
int X = anchorX - widthPx / 2;
SetWindowPos(hwndOverflow, HWND_TOP, X, Y, widthPx, heightPx, SWP_SHOWWINDOW);
```
When the taskbar is at the top of the screen (`Y = 0..30`), `anchorY` is `~0`, so `Y = anchorY - heightPx` is **negative** (e.g., `-120`), placing `TopLevelWindowForOverflowXamlIsland` completely off-screen above the monitor.

WinRings fixes this in two complementary layers:
1. **`Detour_PositionOverflowWindow` (`TopBarHook.cpp`):** Immediately after `origPositionOverflow` computes the exact DPI-scaled `widthPx` and `heightPx`, `TopBarHook::RepositionTrayPopupIfNeeded` places `TopLevelWindowForOverflowXamlIsland` at `Y = barBottomY + MulDiv(4, dpi, 96)` (`34 px` at 96 DPI) below the top taskbar and clamps `X` within the monitor bounds.
2. **In-Process `WINEVENT_INCONTEXT` Hook (`XamlTaskbarCtrl.cpp`):** Watches `EVENT_OBJECT_SHOW` and `EVENT_OBJECT_LOCATIONCHANGE` for `TopLevelWindowForOverflowXamlIsland` and `NotifyIconOverflowWindow`, clamping any popup above `barBottomY` to open below the top taskbar even on future `SystemTray.dll` builds.

### 4.4 Live XAML Visual Diagnostics Controller (`XamlTaskbarCtrl.cpp`)
- Connects to `Windows.UI.Xaml.dll!InitializeXamlDiagnosticsEx` across `VisualDiagConnection1..60` until the taskbar's `Taskbar.TaskbarFrame` and `SystemTray.SystemTrayFrame` trees are matched.
- Tracks and compacts:
  - **Root Frames & Grids:** `HostRootGrid`, `TaskbarFrame`, `TaskbarRootGrid`, `SystemTrayFrame`, `SystemTrayFrameGrid` (`MinHeight = 0`, `MaxHeight = targetH`, `Height = targetH`).
  - **System Tray Stacks & Icons (`SystemTrayCompactItem`):** `NotifyIconStack`, `NonActivatableStack`, `MainStack`, `ShowDesktopStack`, `SystemTray.StackListView`, `SystemTray.ChevronIconView`, `SystemTray.NotifyIconView`, `SystemTray.IconView`, `ControlCenterButton`, `ClockButton`, `NotificationCenterButton` (`MinHeight = 0`, `MaxHeight = targetH`, `Height = NaN`, `VerticalAlignment = Center`, zeroed top/bottom `Margin` and `Padding`).
  - **Single-Line Clock:** Collapses `DateInnerTextBlock` and updates `TimeInnerTextBlock.Text` with `FormatClock(st, L"ddd d MMM  h:mm tt", ClockCasing::CompactLower)` -> `"Fri 20 Nov  4:02 pm"`, never modifying `FontSizeProperty`.
  - **Taskbar Chrome & App Buttons:** Collapses `TaskbarFrameRepeater` (or individual `AppTaskButton`, `StartButton`, `SearchBoxButton`, `TaskViewButton`, `WidgetsButton`) according to user config while guarding that elements inside `SystemTrayFrame` are never hidden.
