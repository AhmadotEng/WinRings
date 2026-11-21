# Changelog

All notable changes to **WinRings** are documented in this file.

## [v2.5.0] - 2026-11-21 (Release 2.5 — Approved A4S Look)

### Visual & Material Architecture (Approved Default Look)

- **Native Windows 11 WCA Acrylic Backdrop Matching A4S-Menu (`src/shell/RingOverlay.h`, `src/shell/RingOverlay.cpp`):**
  - Added a dedicated synchronized backdrop window (`WinRings.WcaBackdrop.v1`) immediately behind `WinRings.Overlay.v1` calling the exact same Windows 11 DWM backdrop pipeline as **A4S-Menu**:
    - `DwmSetWindowAttribute(20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */)`
    - `SetWindowCompositionAttribute(15 /* WCA_FORCE_ACTIVEWINDOW_APPEARANCE */, TRUE)`
    - `SetWindowCompositionAttribute(19 /* WCA_ACCENT_POLICY */, { state = 4 /* ACCENT_ENABLE_ACRYLICBLURBEHIND */, flags = 0x13, gradientColor = 0x80202020 })` at default **A4S Menu** (`glassLevel = 50`, `lightDarkMode = 0`), dynamically tracking `Glass Level`, `Light / Dark Mode`, `Blue`, and `Dark Wood`.
    - `BeginBufferedPaint(..., BPBF_TOPDOWNDIB, BPPF_ERASE)` transparent 32-bit surface so Windows 11 DWM renders its native acrylic blur, luminosity, glow, and micro-texture directly with zero synthetic noise painted over the live ring.
  - Clipped `WinRings.WcaBackdrop.v1` via `SetWindowRgn` (`CombineRgn` `RGN_DIFF` hollow ring, `CreatePolygonRgn` curved outer arcs, and `CreateRoundRectRgn` for window cards and search/center pills) inset by `1.0 px` with hash-based change gating.

- **Smooth Antialiased 3D Glass Bevel Edges (`src/shell/RingOverlay.cpp`, `src/tools/SettingsDialog.cpp`):**
  - Kept the native A4S-Menu WCA interior texture 100% untouched while bridging the `1.0 px` `SetWindowRgn` perimeter with a `2.0 DIP` antialiased Direct2D edge-bridge, a subtle top-lit 3D outer glass bezel gradient, an optically recessed 3D inner hole bevel gradient, and a soft elevation shadow lip.
  - Mirrored the same 3D glass bezel gradients and elevation lip in the persistent 1:1 Settings preview (`WinRingsTools.exe --settings`).

- **Calibrated `dwmcore` / Direct2D 1.1 Parity (`src/shell/AcrylicBackdrop.cpp`, `src/shared/RingThemeEngine.h`, `tests/WinRingsTests.cpp`):**
  - Fixed `CLSID_D2D1Flood` straight-RGBA handling and `D2D1_BLEND_MODE_COLOR` input semantics in `AcrylicBackdrop.cpp` (fallback path) so `dwmcore` and Direct2D 1.1 (`SettingsDialog.cpp` 1:1 preview) match within `±1/255` across dark, purple/blue wallpaper, and bright white backgrounds (`luminosity RGB(38,38,38) @ 0.90`, `tint RGB(32,32,32) @ 128/255`, `blurSigma = 30.0` DPI-scaled, triangular fine grain at `0.012`).

---

## [v2.0.0] - 2026-11-21 (Release 2)

### Added & Improved

- **A4S Menu Environment-Reactive Frosted Acrylic Engine (`src/shared/RingThemeEngine.h`, `src/shell/AcrylicBackdrop.cpp`):**
  - Unified material and frosted-grain specification (`RingMaterialSpec`, `ComputeRingMaterial`, `GenerateAcrylicNoiseBgra256`) shared across `WinRingsShell.dll`, `WinRingsTools.exe`, and `WinRingsTests.exe`.
  - Live Windows 11 desktop backdrop composition (`Windows::UI::Composition::Compositor::CreateHostBackdropBrush`) with `DWMWA_USE_HOSTBACKDROPBRUSH` (`17`), `DwmExtendFrameIntoClientArea({-1, -1, -1, -1})`, `WCA_FORCE_ACTIVEWINDOW_APPEARANCE` (`15`), and a strict-tree `GaussianBlur (30 px) -> Saturation -> Blend(D2D1_BLEND_MODE_LUMINOSITY) -> Composite(SOURCE_OVER)` effect pipeline paired with a tiled `256x256` frosted noise grain texture (`m_noiseBrush`).
  - Authoritative **A4S Menu** material profile (`RGB(32,32,32)` base tint, `128/255` alpha at `glassLevel = 50`, `RGB(62,62,62)` / `#3E3E3E` razor-thin outer & inner hairline borders `<= 0.85 DIP`, `#34638D` radial pointer reveal, `#515151` search border) alongside **Dark Wood** (matte `#202020`) and **Blue** (`#3186FF` sapphire glass) visual presets.
  - Continuous **Glass Level (`0–100%`)** and **Light / Dark Mode (`0–100%`)** sliders for real-time control over acrylic translucency, luminosity blend, and surface tone.

- **True Annulus Ring Geometry, Curved Submenu Arcs & Refined Hit-Testing (`src/shell/RingOverlay.cpp`, `src/shared/RadialMath.h`):**
  - Dual-swap-chain composition (`m_acrylicSwapChain` clipped to the ring annulus, curved outer-arc bands, window cards, and floating pills + `m_fgSwapChain` for icons, typography, and hairline rims) with a completely transparent center opening (`holeRadius = 60 DIP`, `outerRadius = 148 DIP`, `ringRadius = 112 DIP`, `iconSize = 42 DIP`) and no shaded inner disc or blue wedge fill.
  - Strictly bounded main-ring hover band (`dist >= ringDeadzoneDip && dist <= outerRadius + 14 DIP`) so moving into empty outer space never accidentally selects an unexpanded slot, while active outer-arc submenus remain reachable through their angular hover corridor.
  - **Curved Half-Ring Outer Submenus:** `Downloads`, `Folder`, and `Collection` submenus now render inside a continuous curved frosted acrylic arc band (`ComputeIconArcLayout`) rather than isolated circles.

- **System-Wide App Search & Top Half-Ring (`src/shell/AppIndex.cpp`, `src/shared/SearchMatch.h`, `src/shell/RingOverlay.cpp`):**
  - Asynchronous Start Menu (`.lnk`) and `shell:AppsFolder` application indexer (`AppIndex`) with word-boundary and subsequence fuzzy ranking (`ScoreSearchCandidate`).
  - Dedicated floating search pill above the ring with magnifying-glass icon (`#515151` border).
  - Typing highlights matching pinned or open apps directly on the main ring (including pinned apps that are not currently running) and renders a curved top half-ring (`m_appSearchHits`, up to 6 system apps) to launch unpinned installed apps immediately.

- **Redesigned 5-Page Settings Suite with Persistent 1:1 GPU Preview (`src/tools/SettingsDialog.cpp`):**
  - `1040x660` native dark-mode Win32 + Direct2D 1.1 (`ID2D1DeviceContext` + `IDXGISwapChain1`) Settings window with 5 navigation pages: **Appearance**, **Triggers & Input**, **Top Bar & Clock**, **Pins & Folders**, and **Advanced**.
  - **True 1:1 Live GPU Preview** persisted across **Appearance**, **Pins & Folders**, and **Advanced**, running the exact same Direct2D effect graph (`BuildPreviewAcrylicEffectImage`), `ComputeRingMaterial`, `GenerateAcrylicNoiseBgra256` frosted texture, `RadialMath.h` slot layout, real extracted shell icons, and running-app indicators, with instant toggle between **Split Test** and **Live Desktop** backdrops.
  - Offscreen `ID2D1DCRenderTarget` + DIB double-buffered panel rendering (`WM_PAINT` + `WM_PRINTCLIENT`) for zero-flicker dark UI rendering.

- **Tray Menu & Tool Dialog Refinements (`src/host/WinRingsHost.cpp`, `src/tools/CompressTrimDlg.cpp`, `src/tools/RedactEditor.cpp`):**
  - Both left-click and right-click on the promoted system tray icon now open the WinRings tray context menu.
  - Dark DWM title bar and owner-drawn dark controls across the Compress/Trim and Redact tools.

---

## [v1.0.0] - 2026-11-20

### Added

- **Pure Native C++17 Explorer-Integrated Architecture (`x64`, static `/MT` CRT, zero `.NET`/CLR dependencies):**
  - **`WinRingsHost.exe`** — Lightweight out-of-process supervisor and tray host managing `explorer.exe` injection (`CreateRemoteThread` + `LoadLibraryW`), cryptographic attempt-cookie handshake, rolling crash-loop circuit breaker (3 abnormal Explorer exits within 180 seconds), WTS session lock/RDP/UAC suppression, and CLI control (`--toggle`, `--toggle-topbar`, `--reload`, `--settings`, `--preview`, `--status`, `--reset-safety`, `--exit`, `--version`).
  - **`WinRingsShell.dll`** — In-process Windows 11 Explorer shell extension hosting the Direct2D + DirectComposition Radial Switcher, DWM live window thumbnail pool, zero-WinRT `Taskbar.dll` / `SystemTray.dll` top-bar hooks, and live XAML Visual Diagnostics controller.
  - **`WinRingsTools.exe`** — Native dark-mode Win32/Direct2D/WIC utility hosting the 4-tab Settings & Pins editor (with live Direct2D Ring preview and live clock format preview), WIC/ffmpeg image & media conversion, media compression/trimming dialog, native `.zip` archiver, and interactive Solid/Blur/Pixelate image Redact editor.
  - **`WinRingsSetup.exe`** — Single-file per-user installer embedding SHA-256-verified payloads (`BCrypt`), configuring `%LOCALAPPDATA%\WinRings\v1.0.0.0\`, `HKCU\...\Run`, `HKCU\...\Uninstall\WinRings`, Start Menu shortcut, and opt-in `ClearTaskbar.exe` Startup migration/restore.

- **Radial Switcher & Gesture Engine:**
  - Direct2D + DirectComposition (`WS_EX_NOREDIRECTIONBITMAP`) radial overlay with 6 built-in themes (`LiquidGlass`, `Glass`, `Kiwi`, `Aura`, `Orbit`, `Outline`) and custom hex accent color.
  - Frame-0 instant opening mode (`disableOpeningAnimation = true`, honors `SPI_GETCLIENTAREAANIMATION`).
  - Decoupled `18 DIP` inner deadzone (`ringDeadzoneDip = 18.0`) with generous visual center hub.
  - Up to 9 pooled non-layered rounded (`DWMWCP_ROUND`) `DwmRegisterThumbnail` preview windows for multi-window apps, plus `TitleColumn` list view mode.
  - Multi-trigger input engine: mouse buttons (`SideBack` / `XButton1`, `SideForward` / `XButton2`, `Middle`, `Right+Left Chord`), quick-tap sticky mode vs hold-flick-release mode, double-tap last app switching, background-suppressed scroll-wheel cycling, configurable keyboard hotkey (default `Alt + Space`), Menu key (`VK_APPS`), optional `Win + Tab` / `Alt + Tab` interception, and optional Top-Bar Ring Toggle pill.
  - Outer-arc submenus for multi-window apps, Pinned Collections, newest-9 `Downloads` folder fan-out with OLE file drag-out, and COM `IDropTarget` drag-and-drop onto Ring slots (`App`, `Folder`, `Collection`, `RecycleBin`, `Convert`, `Actions`).
  - Submenu hover-corridor persistence with smooth angular switch-over when moving directly from an expanded outer submenu to a neighboring main ring slot.

- **Unified Minimal Top Bar (Windows 11 25H2 Build 26200.8875):**
  - 4 zero-WinRT `Taskbar.dll` detours (`TrayUI::GetMinSize` primary & secondary, `TrayUI::GetDockedRect`, `GetAutoHideFlags`) paired with `ITrayUI::VerifySize` (`vtable[0x68]`) to dock the native Windows 11 taskbar to the top monitor edge (`ABE_TOP = 1`), compact height to `28–40 DIP` (default `30 DIP`), and reserve the desktop work area (`rcWork.top = 30`).
  - `SystemTray.dll` detour on `OverflowXamlIslandManager::PositionWindow` (`0x0B9EB0`) plus an in-process `WINEVENT_INCONTEXT` hook so the system tray overflow popup (`TopLevelWindowForOverflowXamlIsland` / `NotifyIconOverflowWindow`) and tray context menu always open directly **below** the top taskbar (`Y = topBarBottomPx + 4 DIP`).
  - Automatic promotion of the WinRings tray icon in `HKCU\Control Panel\NotifyIconSettings` (`IsPromoted = 1`) with custom 32×32 cyan ring icon and UIPI message filter support.
  - Live XAML Visual Diagnostics (`InitializeXamlDiagnosticsEx` + `IVisualTreeServiceCallback2`) controller with full original `DependencyProperty` snapshotting and restoration:
    - Single-line custom clock (`ddd d MMM  h:mm tt`) with `CompactLower` casing (`Fri 20 Nov  4:02 pm` — TitleCase weekday/month + lowercase `am`/`pm`), preserving native taskbar `FontSize`.
    - Configurable hiding of taskbar app buttons, Start button, Search box, Task View, and Widgets while compacting and vertically centering all System Tray icon stacks (`NotifyIconStack`, `SystemTray.ChevronIconView`, `SystemTray.NotifyIconView`, `ControlCenterButton`, `ClockButton`, `NotificationCenterButton`).
    - Configurable `Clear` (100% transparent), `Frosted` (translucent acrylic dark tint), and `Native` backdrop modes.
