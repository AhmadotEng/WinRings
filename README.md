# WinRings — Native Windows 11 Radial Switcher & Unified Minimal Top Bar

[![Platform](https://img.shields.io/badge/Platform-Windows%2011%20x64-0078D4?logo=windows11)](https://github.com/AhmadotEng/WinRings)
[![Language](https://img.shields.io/badge/Language-C%2B%2B17%20%28Pure%20Native%20%2FMT%29-00599C?logo=cplusplus)](https://github.com/AhmadotEng/WinRings)
[![Runtime](https://img.shields.io/badge/.NET%20%2F%20CLR-Zero%20Dependencies-success)](https://github.com/AhmadotEng/WinRings)
[![Release](https://img.shields.io/github/v/release/AhmadotEng/WinRings?color=31B8FF)](https://github.com/AhmadotEng/WinRings/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**WinRings** is a **100% pure native C++17 (`x64`, static `/MT` CRT, zero `.NET`/CLR)** Windows 11 shell extension that integrates directly into `explorer.exe`. It combines two flagship capabilities into a single cohesive runtime:

1. **Radial Switcher & Gesture Engine** — An instant-opening Direct2D + DirectComposition radial app/window/file switcher with live rounded DWM window thumbnails, outer-arc submenus, `Downloads` folder fan-out with OLE drag-out, and drag-and-drop file actions (Convert, Compress/Trim, Archive, Redact).
2. **Unified Minimal Top Bar** — A zero-WinRT `Taskbar.dll` + `SystemTray.dll` native hook and live XAML Visual Diagnostics controller (`InitializeXamlDiagnosticsEx` + `IVisualTreeServiceCallback2`) that transforms the Windows 11 taskbar into a `30 DIP` clear or frosted minimal top bar (`rcWork.top = 30`), hides taskbar app/chrome clutter while preserving and compacting the live System Tray, repositions the system tray overflow flyout (`^`) directly **below** the top bar, and formats a single-line custom clock (`Fri 20 Nov  4:02 pm`) without altering the native font size.

---

## Table of Contents

- [Key Features](#key-features)
- [System Architecture](#system-architecture)
- [Quick Start & Installation](#quick-start--installation)
- [How to Trigger & Use the Ring](#how-to-trigger--use-the-ring)
- [Unified Minimal Top Bar](#unified-minimal-top-bar)
- [CLI Reference](#cli-reference)
- [Configuration (`config.json`)](#configuration-configjson)
- [Building & Testing from Source](#building--testing-from-source)
- [Documentation Index](#documentation-index)
- [Project Lineage](#project-lineage)
- [License](#license)

---

## Key Features

### 1. Radial Switcher (`RingOverlay`, `DwmThumbPool`, `InputEngine`)
- **Pure Direct2D + DirectComposition Rendering:** Per-pixel alpha composition (`WS_EX_NOREDIRECTIONBITMAP`) with 6 themes (`LiquidGlass`, `Glass`, `Kiwi`, `Aura`, `Orbit`, `Outline`) and custom hex accent color.
- **Frame-0 Instant Response:** Opens at full scale on frame 0 (`disableOpeningAnimation = true` by default) and respects Windows `SPI_GETCLIENTAREAANIMATION`.
- **Decoupled 18 DIP Deadzone:** Tight `18 DIP` inner deadzone (`ringDeadzoneDip = 18.0`) decoupled from the visual center hub for effortless flick selection.
- **Live Rounded DWM Window Previews:** Pools up to 9 non-layered top-level DWM thumbnail windows (`DWMWCP_ROUND`) around the outer arc for multi-window apps, or switches to a compact `TitleColumn` list view.
- **Multi-Trigger Input & Dual Activation Modes:**
  - **Mouse Triggers:** `SideBack` (`XButton1`, default), `SideForward` (`XButton2`), `Middle`, or `Right+Left Chord`.
  - **Quick-Tap vs. Hold-Flick-Release:** Tap `< 180 ms` locks the ring open in **Sticky Mode** (click, type to filter, arrow keys, or press `1–9`); hold `>= 180 ms`, flick toward a slot, and release to switch immediately.
  - **Double-Tap Last App:** Double-tapping the mouse trigger within `320 ms` jumps straight to your previous foreground application (`Alt+Tab` fast toggle).
  - **Scroll-Wheel Cycling:** Scroll while the ring is open to cycle windows or slots without leaking `WM_MOUSEWHEEL` to background apps.
  - **Keyboard Triggers:** Configurable global hotkey (`Alt + Space` default), Menu key (`VK_APPS`), and optional `Win + Tab` / `Alt + Tab` shell interception.
- **Outer-Arc Submenus & Drag-and-Drop Workflows:**
  - **Multi-Window Fan-Out:** Hover any running app with multiple windows to fan out live DWM previews along the outer arc; middle-click any preview to close that window (`WM_CLOSE`).
  - **Downloads Fan-Out & OLE Drag-Out:** Hover the pinned `Downloads` folder slot to inspect the 9 newest files/folders and drag any item directly out into another app.
  - **Drop Target Actions:** Drag files from Explorer onto Ring slots to launch with a target app, move/copy into a folder, send to `RecycleBin`, convert images/media (`Convert`), or compress/trim/zip/redact (`Actions`).

### 2. Unified Minimal Top Bar (`TopBarHook`, `XamlTaskbarCtrl`)
- **Top-Edge Docking & Work-Area Reservation:** Docks the native Windows 11 `Shell_TrayWnd` to the top edge of the monitor (`ABE_TOP = 1`) at a compact `28–40 DIP` height (`30 DIP` default) and updates `SPI_GETWORKAREA` (`rcWork.top = 30`) so maximized windows sit cleanly beneath the bar.
- **Zero-WinRT `Taskbar.dll` & `SystemTray.dll` Hooks:**
  - Hooks `TrayUI::GetMinSize` (primary & secondary), `TrayUI::GetDockedRect`, and `GetAutoHideFlags`, driving resize/redock through `ITrayUI::VerifySize` (`vtable[0x68]`) without ever touching fragile internal WinRT XAML island edge notifications.
  - Hooks `SystemTray.dll!OverflowXamlIslandManager::PositionWindow` (`0x0B9EB0`) plus an in-process `WINEVENT_INCONTEXT` guard so clicking the system tray overflow chevron (`^`) opens `TopLevelWindowForOverflowXamlIsland` directly **below** the top taskbar (`Y = topBarBottomPx + 4 DIP`) instead of off-screen above `Y = 0`.
- **Live XAML Visual Diagnostics Controller:**
  - Attaches in-process via `InitializeXamlDiagnosticsEx` (`VisualDiagConnection1..60`) and `IVisualTreeServiceCallback2`.
  - Snapshots every original `DependencyProperty` before modification and restores 100% of native taskbar properties on disable or exit.
  - **Single-Line Compact Clock:** Replaces the two-line Windows 11 date/time stack with a single-line clock (`ddd d MMM  h:mm tt` -> `Fri 20 Nov  4:02 pm`, TitleCase weekday/month + lowercase `am`/`pm`) while keeping the native taskbar `FontSize` unchanged.
  - **Selective Chrome Hiding & System Tray Compaction:** Hides taskbar app icons (`TaskbarFrameRepeater`), Start button, Search box, Task View, and Widgets while compacting and vertically centering `SystemTrayFrame`, `NotifyIconStack`, `SystemTray.ChevronIconView`, `SystemTray.NotifyIconView`, `ControlCenterButton`, `ClockButton`, and `NotificationCenterButton`.
  - **Clear / Frosted / Native Backdrops:** Supports 100% transparent (`Clear`), dark translucent (`Frosted`), or stock Windows 11 (`Native`) backgrounds.

---

## System Architecture

```mermaid
flowchart LR
    subgraph UserSpace["Out-of-Process Supervisor & Utilities"]
        Host["WinRingsHost.exe<br/>(Tray Host, CrashGuard, CLI)"]
        Tools["WinRingsTools.exe<br/>(4-Tab Settings, Convert, Trim, Redact)"]
        Setup["WinRingsSetup.exe<br/>(SHA-256 Single-File Installer)"]
    end

    subgraph ExplorerProc["explorer.exe (In-Process Shell Extension)"]
        Shell["WinRingsShell.dll<br/>(ShellController)"]
        Ring["RingOverlay + DwmThumbPool<br/>(Direct2D + DirectComposition + DWM)"]
        Input["InputEngine<br/>(WH_MOUSE_LL / WH_KEYBOARD_LL / Hotkeys)"]
        Hook["TopBarHook<br/>(Taskbar.dll + SystemTray.dll Detours)"]
        Xaml["XamlTaskbarCtrl<br/>(InitializeXamlDiagnosticsEx + IVisualTreeService3)"]
    end

    Setup -->|Installs & Verifies SHA-256| Host
    Setup -->|Installs & Verifies SHA-256| Shell
    Setup -->|Installs & Verifies SHA-256| Tools
    Host -->|CreateRemoteThread + Attempt Cookie| Shell
    Host <-->|Registered Control / Status Messages| Shell
    Tools -->|Saves config.json & triggers --reload| Host
    Shell --> Ring
    Shell --> Input
    Shell --> Hook
    Shell --> Xaml
```

| Binary | Size | Role |
| :--- | :--- | :--- |
| **`WinRingsHost.exe`** | `~280 KB` | Resident supervisor, custom cyan ring tray icon (auto-promoted in `HKCU\Control Panel\NotifyIconSettings`), Explorer injection handshake, 3-crash-in-180s safety circuit breaker, and CLI dispatcher. |
| **`WinRingsShell.dll`** | `~624 KB` | In-process Explorer module hosting `RingOverlay`, `DwmThumbPool`, `InputEngine`, `WindowTracker`, `SlotModel`, `DropTarget`, `TopBarHook`, and `XamlTaskbarCtrl`. |
| **`WinRingsTools.exe`** | `~401 KB` | Dark-mode native Win32/Direct2D/WIC 4-tab Settings UI, image/media converter, media compressor/trimmer, `.zip` archiver, and interactive image Redact editor. |
| **`WinRingsSetup.exe`** | `~1.46 MB` | Self-contained per-user installer embedding all 3 binaries as `RT_RCDATA` with `BCrypt` SHA-256 verification. |

---

## Quick Start & Installation

### Option 1: Single-File Installer (Recommended)
Download **`WinRingsSetup.exe`** from the [Releases](https://github.com/AhmadotEng/WinRings/releases) page and run:

```powershell
.\WinRingsSetup.exe
```

Or install silently from PowerShell:

```powershell
.\WinRingsSetup.exe --install --silent
```

This installs the verified binaries to:
```text
%LOCALAPPDATA%\WinRings\v1.0.0.0\
  ├── WinRingsHost.exe
  ├── WinRingsShell.dll
  └── WinRingsTools.exe
```
and registers `WinRingsHost.exe` under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` (`WinRings`).

### Option 2: Windows Startup Folder Shortcut
If you prefer managing startup items via `shell:startup` (`C:\Users\<User>\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup`), create a shortcut **`WinRings.lnk`** pointing to:

```text
%LOCALAPPDATA%\WinRings\v1.0.0.0\WinRingsHost.exe
```

---

## How to Trigger & Use the Ring

| Trigger / Action | Default Binding | Behavior |
| :--- | :--- | :--- |
| **Quick-Tap (Sticky Mode)** | Tap **Mouse Side Back (`XButton1`)** `< 180 ms` | Opens and locks the Ring on screen. Click any slot, hover outer submenus, type to filter by title, use Arrow keys / `1–9`, or press `Esc` to dismiss. |
| **Hold-Flick-Release** | Hold **Mouse Side Back (`XButton1`)** `>= 180 ms` | Opens the Ring at cursor; flick outside the `18 DIP` deadzone toward any slot or outer submenu item and release to activate immediately. |
| **Double-Tap Last App** | Double-tap **Mouse Side Back (`XButton1`)** `< 320 ms` | Instantly switches between your current and previous foreground window. |
| **Keyboard Hotkey** | **`Alt + Space`** | Toggles the Ring in Sticky Mode at the cursor (or monitor center if `Shift` is held). |
| **Menu Key** | **`VK_APPS`** | Toggles the Ring in Sticky Mode. |
| **Top-Bar Pill** | Click **`O` Pill** at top-left (`0, 0`) | Toggles the Ring via mouse click on the primary monitor's top bar. |
| **Tray Icon** | Left-click / Right-click **Cyan Ring Tray Icon** | Left-click opens the Ring; Right-click opens the WinRings menu below the top taskbar (`Open Ring Now`, `Toggle Top Minimal Bar`, `Settings...`, `Reload Config`, `Open Config Folder`, `Exit WinRings`). |

---

## Unified Minimal Top Bar

When `topBarEnabled: true` is active in `%APPDATA%\WinRings\config.json`:

1. **Top Docking & `30 DIP` Height:** `Shell_TrayWnd` docks to `(0, 0, screenWidth, 30)` and reserves `rcWork.top = 30`.
2. **Transparent / Frosted Backdrop:** Clears `BackgroundFill`, `BackgroundStroke`, and `ScreenEdgeStroke` in the taskbar XAML visual tree (`Clear` mode) or applies a dark translucent acrylic tint (`Frosted` mode).
3. **Single-Line Clock (`Fri 20 Nov  4:02 pm`):** Collapses `DateInnerTextBlock` and formats `TimeInnerTextBlock` in-place every second using TitleCase weekday/month and lowercase `am`/`pm` (`ClockCasing::CompactLower`) without modifying `FontSize`.
4. **System Tray Overflow Below Top Bar:** Clicking the `^` hidden-icons chevron on the top bar opens the tray overflow flyout directly **under** the top taskbar (`Y = topBarBottomPx + 4 DIP`).
5. **Instant Toggle & 100% Native Restoration:** Toggle the Minimal Top Bar on or off at any time from the tray menu (`Toggle Top Minimal Bar`) or CLI (`WinRingsHost.exe --toggle-topbar`). Disabling restores the taskbar to the bottom edge at `48 DIP` with all original XAML properties intact.

---

## CLI Reference

```powershell
# Check live host & shell attachment status
& "$env:LOCALAPPDATA\WinRings\v1.0.0.0\WinRingsHost.exe" --status

# Toggle the Radial Ring overlay
& "$env:LOCALAPPDATA\WinRings\v1.0.0.0\WinRingsHost.exe" --toggle

# Toggle the Unified Minimal Top Bar on/off
& "$env:LOCALAPPDATA\WinRings\v1.0.0.0\WinRingsHost.exe" --toggle-topbar

# Open the 4-tab Settings & Pins UI
& "$env:LOCALAPPDATA\WinRings\v1.0.0.0\WinRingsHost.exe" --settings

# Hot-reload %APPDATA%\WinRings\config.json into Explorer
& "$env:LOCALAPPDATA\WinRings\v1.0.0.0\WinRingsHost.exe" --reload

# Run the standalone Ring Preview Window (without Explorer injection)
& "$env:LOCALAPPDATA\WinRings\v1.0.0.0\WinRingsHost.exe" --preview

# Reset the 3-crash-in-180s safety circuit breaker
& "$env:LOCALAPPDATA\WinRings\v1.0.0.0\WinRingsHost.exe" --reset-safety

# Cleanly unhook, restore native taskbar state, and exit WinRings
& "$env:LOCALAPPDATA\WinRings\v1.0.0.0\WinRingsHost.exe" --exit
```

---

## Configuration (`config.json`)

WinRings stores its configuration at `%APPDATA%\WinRings\config.json` (`C:\Users\<User>\AppData\Roaming\WinRings\config.json`) and diagnostic logs at `%APPDATA%\WinRings\shell_diag.log`.

```json
{
  "mouseTrigger": "SideBack",
  "tapThresholdMs": 180,
  "doubleTapWindowMs": 320,
  "doubleTapLastApp": true,
  "scrollWheelMode": "CycleWindows",
  "hotkeyEnabled": true,
  "hotkeyModifiers": 1,
  "hotkeyVk": 32,
  "menuKeyEnabled": true,
  "interceptWinTab": false,
  "interceptAltTab": false,
  "topBarRingButton": true,
  "theme": "LiquidGlass",
  "accentColor": "#3186FF",
  "ringRadiusDip": 134.0,
  "ringDeadzoneDip": 18.0,
  "iconSizeDip": 28.0,
  "disableOpeningAnimation": true,
  "windowSubmenuView": "OuterArc",
  "fanOutSingleWindow": false,
  "thumbnailScale": 1.0,
  "pointerFollow": false,
  "shiftCenterOnActiveMonitor": true,
  "includeUnpinnedRunningApps": true,
  "maxSlots": 10,
  "topBarEnabled": true,
  "topBarDockTop": true,
  "topBarHeightDip": 30,
  "topBarKeepFontSize": true,
  "topBarClockSingleLine": true,
  "topBarClockFormat": "ddd d MMM  h:mm tt",
  "topBarClockCasing": "CompactLower",
  "topBarHideAppButtons": true,
  "topBarHideStart": true,
  "topBarHideSearch": true,
  "topBarHideTaskView": true,
  "topBarHideWidgets": true,
  "topBarBackdrop": "Clear",
  "topBarAllMonitors": true
}
```

---

## Building & Testing from Source

### Prerequisites
- **Windows 11 x64** (verified on Windows 11 25H2 Build `26200.8875`)
- **Visual Studio 2022 Build Tools** or Visual Studio 2022 (`MSVC v143` x64 C++ toolset + Windows 11 SDK)
- **PowerShell 5.1+**

### Build All Targets & Installer
```powershell
powershell -ExecutionPolicy Bypass -File .\tools\Build-WinRings.ps1
```
Compiles all 5 targets with `/std:c++17 /MT /O2 /EHsc /W4 /WX` (zero warnings, zero `.NET` dependencies) and stages deliverables under `build\stage\`.

### Run Automated Verification & Unit Tests
```powershell
powershell -ExecutionPolicy Bypass -File .\tools\Test-WinRingsProduct.ps1
```
Verifies x64 PE headers, zero CLR imports, DLL exports, ABI contract (`WinRingsAbiProbe.exe`), `--version` exit codes, and the unit test suite (`WinRingsTests.exe`).

---

## Documentation Index

- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — Deep-dive into the 4-binary architecture, Explorer injection protocol, Direct2D/DirectComposition rendering, DWM thumbnail pool, `Taskbar.dll` / `SystemTray.dll` zero-WinRT detours, and XAML Visual Diagnostics.
- **[docs/CONFIGURATION_AND_USAGE.md](docs/CONFIGURATION_AND_USAGE.md)** — Complete user & power-user guide covering triggers, themes, pins, collections, drag-and-drop tools, top-bar settings, clock formatting, and Startup setup.
- **[docs/BUILDING_AND_TESTING.md](docs/BUILDING_AND_TESTING.md)** — Build pipeline, verification gates, live lifecycle testing, and troubleshooting with `shell_diag.log`.
- **[docs/PRIOR_PROJECTS_SYNTHESIS.md](docs/PRIOR_PROJECTS_SYNTHESIS.md)** — Technical study of predecessor projects (`A4S-Menu` and `ClearTaskbar.exe`) and how WinRings unifies their shell integration patterns.
- **[docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md)** — Complete engineering specification and phase-by-phase implementation plan.
- **[docs/ENGINEERING_WALKTHROUGH.md](docs/ENGINEERING_WALKTHROUGH.md)** — Development, debugging, reverse-engineering (`Taskbar.dll` & `SystemTray.dll`), and verification walkthrough.
- **[docs/RADIAL_SWITCHER_SPEC.md](docs/RADIAL_SWITCHER_SPEC.md)** — Original radial switcher design plan and interaction specification.
- **[CHANGELOG.md](CHANGELOG.md)** — Release history and `v1.0.0` release notes.

---

## Project Lineage

WinRings builds upon lessons learned from two earlier Windows 11 shell customization projects:
- **[A4S-Menu](https://github.com/AhmadotEng/A4S-Menu)** — Native C++17 `A4SHost.exe` + `A4SShell.dll` Explorer-injected Start Menu replacement with crash-loop safety guards, cryptographic injection handshakes, and single-file SHA-256-verified installer packaging.
- **ClearTaskbar** — In-process Windows 11 XAML Visual Diagnostics (`InitializeXamlDiagnosticsEx` + `IVisualTreeServiceCallback2`) taskbar controller for transparent backdrops and single-line clock formatting.

---

## License

Licensed under the [MIT License](LICENSE). Copyright (c) 2026 Ahmad Mustafa ([AhmadotEng](https://github.com/AhmadotEng)).
