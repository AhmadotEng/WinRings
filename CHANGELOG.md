# Changelog

All notable changes to **WinRings** are documented in this file.

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
