# WinRings — Configuration & Usage Guide

This guide covers all user-facing triggers, radial switcher workflows, Unified Minimal Top Bar settings, clock formatting options, and command-line flags.

---

## 1. Opening & Navigating the Radial Ring

### Mouse Triggers
Configured via `mouseTrigger` in `%APPDATA%\WinRings\config.json` or Tab 1 (**Triggers & Input**) of `WinRingsTools.exe --settings`:
- **`SideBack` (Default):** Mouse Back button (`XButton1`).
- **`SideForward`:** Mouse Forward button (`XButton2`).
- **`Middle`:** Middle mouse wheel button.
- **`RightLeftChord`:** Hold Right mouse button and press Left mouse button.

### Activation Modes
1. **Quick-Tap Sticky Mode (`< 180 ms`):**
   - Tap and release the trigger button quickly.
   - The Ring stays open on screen.
   - Left-click any slot or outer submenu item to activate it, middle-click a window preview to close that window, type letters on the keyboard to filter windows by title, press `1–9` or Arrow keys + `Enter`, or press `Esc` / click inside the center hub to close.
2. **Hold-Flick-Release Mode (`>= 180 ms`):**
   - Press and hold the trigger button, move the cursor outside the `18 DIP` deadzone toward a slot (or out onto an expanded outer-arc preview), and release the button to switch immediately.
3. **Double-Tap Last App (`< 320 ms`):**
   - Double-tap the mouse trigger button to immediately toggle back to the previously active application window.

### Keyboard Triggers
- **`Alt + Space` (Default Hotkey):** Opens/closes the Ring in Sticky Mode. Hold `Shift` while triggering (`Alt + Shift + Space`) when `shiftCenterOnActiveMonitor` is enabled to center the Ring on the active monitor and center the selected window upon activation.
- **Menu Key (`VK_APPS`):** Opens/closes the Ring in Sticky Mode.
- **Optional `Win + Tab` / `Alt + Tab` Replacement:** Enable `interceptWinTab` or `interceptAltTab` in Settings to intercept native task switchers.

---

## 2. Outer-Arc Submenus & Drag-and-Drop Tools

- **Multi-Window Apps:** Hovering any app slot with 2+ open windows (or 1+ when `fanOutSingleWindow: true`) fans out live rounded DWM window thumbnails along the outer arc (`windowSubmenuView: "OuterArc"`) or displays a vertical title list (`"TitleColumn"`).
- **Downloads Slot Fan-Out:** Hovering the pinned `Downloads` folder slot fans out the 9 most recently modified files/folders in your `Downloads` directory. You can click any item to open it or **drag it directly out of the Ring** into another application (OLE `IDropSource`).
- **Dropping Files onto the Ring:** Start dragging files in File Explorer and trigger the Ring:
  - Drop onto an **App** slot to open the files with that application.
  - Drop onto a **Folder** slot to move/copy the files into that directory (`Shift` forces Move, `Ctrl` forces Copy).
  - Drop onto **Recycle Bin** to send the files to the Windows Recycle Bin.
  - Drop onto **Convert** to convert images (`.png`, `.jpg`, `.webp`, `.bmp`, `.ico`) via WIC or video/audio via `ffmpeg`.
  - Drop onto **Actions** to open the Compress/Trim dialog, create a `.zip` archive, or open the interactive Direct2D/WIC **Redact Editor** (Solid, Blur, Pixelate).

---

## 3. Unified Minimal Top Bar & Clock Formatting

Configured on Tab 3 (**Top Bar & Clock**) of `WinRingsTools.exe --settings`:

| Setting | JSON Key | Default | Description |
| :--- | :--- | :--- | :--- |
| **Enable Minimal Top Bar** | `topBarEnabled` | `true` | Enables the Windows 11 minimal top bar and XAML controller. |
| **Dock to Top Edge** | `topBarDockTop` | `true` | Docks `Shell_TrayWnd` to the top monitor edge and reserves `rcWork.top`. |
| **Bar Height (DIP)** | `topBarHeightDip` | `30` | Taskbar thickness in DIPs (`28–40 DIP`). |
| **Keep Native Font Size** | `topBarKeepFontSize` | `true` | Preserves the stock Windows 11 taskbar clock/tray font size. |
| **Single-Line Clock** | `topBarClockSingleLine` | `true` | Replaces the two-line date/time stack with a single-line custom clock. |
| **Clock Format** | `topBarClockFormat` | `"ddd d MMM  h:mm tt"` | Custom date/time format pattern. |
| **Clock Casing** | `topBarClockCasing` | `"CompactLower"` | `"CompactLower"` (`Fri 20 Nov  4:02 pm`), `"Exact"` (`Fri 20 Nov  4:02 PM`), `"Lower"` (`fri 20 nov  4:02 pm`), or `"Upper"` (`FRI 20 NOV  4:02 PM`). |
| **Backdrop Style** | `topBarBackdrop` | `"Clear"` | `"Clear"` (100% transparent), `"Frosted"` (translucent dark tint), or `"Native"`. |
| **Hide App Buttons** | `topBarHideAppButtons` | `true` | Hides pinned/running app icons on the taskbar while keeping the System Tray. |
| **Hide Start / Search / Task View / Widgets** | `topBarHideStart`, etc. | `true` | Hides Windows 11 left/center chrome buttons. |

### Supported Clock Format Tokens (`ClockFormat.h`)
- `dddd` — Full weekday name (`Friday`)
- `ddd` — Abbreviated weekday name (`Fri`)
- `dd` — Two-digit day of month (`01–31`)
- `d` — Day of month without leading zero (`1–31`)
- `MMMM` — Full month name (`November`)
- `MMM` — Abbreviated month name (`Nov`)
- `MM` / `M` — Month number (`01–12` / `1–12`)
- `yyyy` / `yy` — 4-digit or 2-digit year (`2026` / `26`)
- `HH` / `H` — 24-hour clock (`00–23` / `0–23`)
- `hh` / `h` — 12-hour clock (`01–12` / `1–12`)
- `mm` / `m` — Minutes (`00–59` / `0–59`)
- `ss` / `s` — Seconds (`00–59` / `0–59`)
- `tt` / `t` — Meridiem (`pm` / `p` in `CompactLower` or `Lower`; `PM` / `P` in `Exact` or `Upper`)
- `'text'` — Literal quoted text
