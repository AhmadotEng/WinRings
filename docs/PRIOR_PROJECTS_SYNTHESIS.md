# Prior Projects Technical Synthesis: A4S-Menu & ClearTaskbar

Before building **WinRings**, two predecessor Windows 11 shell customization projects by **Ahmad Mustafa (`AhmadotEng`)** were inspected and documented as architectural references:

1. **[A4S-Menu](https://github.com/AhmadotEng/A4S-Menu)** — Native C++17 Explorer-integrated Start Menu replacement (`A4SHost.exe`, `A4SShell.dll`, `A4SMenuSettings.exe`, `A4SMenuSetup.exe`).
2. **ClearTaskbar (`ClearTaskbar.exe`)** — In-process Windows 11 taskbar customization runtime using `InitializeXamlDiagnosticsEx`, `IVisualTreeServiceCallback2`, and `Taskbar.dll` docking/sizing hooks.

---

## 1. A4S-Menu (`A4SHost.exe` + `A4SShell.dll`) — Key Patterns Adopted in WinRings

### 1.1 Host Supervisor & Shell DLL Split
- **`A4SHost.exe`** runs out-of-process as a lightweight Win32 message-only supervisor while **`A4SShell.dll`** runs in-process inside `explorer.exe`.
- **Validated Explorer Discovery:** Verifies `Shell_TrayWnd` / `Progman` belongs to `%SystemRoot%\explorer.exe` in the same session, running under the same user SID and x64 architecture before attempting injection.
- **Cryptographic Attempt-Cookie Handshake:** Publishes a random 64-bit cookie via `RtlGenRandom` (`SystemFunction036`) on the host window (`SetPropW`). The injected DLL must read and echo that exact cookie back in `WINRINGS_DLL_STATUS_READY` before activation is committed.
- **Crash-Loop Circuit Breaker (`CrashGuard.h`):** Tracks abnormal `explorer.exe` process exits in `HKCU`. If 3 abnormal exits occur within a rolling 180-second window, injection is automatically disabled (`WINRINGS_HOST_STATUS_SAFETY_DISABLED`) until explicitly reset.
- **Session & Desktop Isolation:** Suspends input hooks on `WTS_SESSION_LOCK`, `WTS_REMOTE_DISCONNECT`, `PBT_APMSUSPEND`, and UAC/Secure Desktop transitions (`EVENT_SYSTEM_DESKTOPSWITCH`).
- **Single-File SHA-256 Verified Installer (`WinRingsSetup.exe`):** Embeds compiled binaries as `RT_RCDATA` resources and verifies their SHA-256 digests with `BCrypt` at extraction time before writing to `%LOCALAPPDATA%`.

---

## 2. ClearTaskbar (`ClearTaskbar.exe`) — Key Patterns Adopted & Evolved in WinRings

### 2.1 Live XAML Visual Diagnostics (`InitializeXamlDiagnosticsEx`)
- Windows 11 renders the taskbar and system tray inside XAML Islands (`Taskbar.TaskbarFrame` and `SystemTray.SystemTrayFrame`) hosted in `explorer.exe`.
- By calling `Windows.UI.Xaml.dll!InitializeXamlDiagnosticsEx` across `VisualDiagConnection1..N` and passing the in-process DLL path and `IClassFactory` CLSID, `explorer.exe` instantiates an `IObjectWithSite` endpoint and hands it an `IXamlDiagnostics` / `IVisualTreeService3` pointer.
- Calling `AdviseVisualTreeChange` registers an `IVisualTreeServiceCallback2` listener on the XAML UI thread, delivering live `InstanceHandle` notifications for every XAML element (`TaskbarFrame`, `SystemTrayFrame`, `BackgroundFill`, `BackgroundStroke`, `ScreenEdgeStroke`, `TaskbarFrameRepeater`, `TimeInnerTextBlock`, `DateInnerTextBlock`, `NotifyIconStack`, etc.).

### 2.2 Snapshot-and-Restore Property Management
- Every modified `DependencyProperty` is snapshotted via `ReadLocalValue` before modification and watched via `RegisterPropertyChangedCallback` so dynamic theme/layout changes by Explorer cannot overwrite active customizations, while `--exit` or disabling the top bar restores 100% of original XAML properties cleanly.

### 2.3 Evolution in WinRings Beyond ClearTaskbar
1. **Unified Single-DLL Runtime:** Instead of running two separate processes/DLLs fighting over `explorer.exe` hooks, `WinRingsShell.dll` hosts both the **Radial Switcher** and the **Unified Minimal Top Bar** inside a single coordinated Explorer extension.
2. **Zero-WinRT `Taskbar.dll` + `SystemTray.dll` Detours (`TopBarHook.cpp`):**
   - Avoids calling `TrayUI::SetStuckPlace` (`0x0E1050`) or hooking `Feature_59213768` (`0x0E486C`), which causes an access violation inside `NotifyTaskbarEdgeChanged` (`0x0E436C` -> `0x0E1233`) on Windows 11 25H2 (`build 26200.8875`).
   - Instead uses 4 surgical `Taskbar.dll` detours (`GetMinSize` primary/secondary, `GetDockedRect`, `GetAutoHideFlags`) + `ITrayUI::VerifySize`, and 1 `SystemTray.dll` detour (`OverflowXamlIslandManager::PositionWindow` at `0x0B9EB0`) + in-process `WINEVENT_INCONTEXT` guard so the system tray overflow popup (`^`) always opens **below** the top taskbar.
3. **30 DIP System Tray Compaction & Custom Clock Casing:**
   - Compacts `NotifyIconStack`, `SystemTray.ChevronIconView`, `SystemTray.NotifyIconView`, `ControlCenterButton`, `ClockButton`, and `NotificationCenterButton` to fit cleanly inside `30 DIP` without clipping.
   - Formats the single-line clock as `Fri 20 Nov  4:02 pm` (TitleCase weekday & month, lowercase `am`/`pm`) while preserving the native taskbar font size.
