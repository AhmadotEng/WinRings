# WinRings — Configuration & Usage Guide

This guide covers all user-facing triggers, radial switcher workflows, Unified Minimal Top Bar settings, clock formatting options, and command-line flags.

---

## 1. Opening & Navigating the Radial Ring

### Visual Styles & Appearance (`WinRingsTools.exe --settings` -> Appearance)
Configured on the **Appearance** page (with a persistent 1:1 GPU preview shared across **Appearance**, **Pins & Folders**, and **Advanced**):
- **`A4S` (`A4S Menu`, Default):** Live wallpaper-reactive frosted acrylic (`RGB(32,32,32)` base tint at `128/255` alpha, 30 px Gaussian blur, luminosity adaptation, `256x256` frosted noise grain, `#3E3E3E` razor-thin `<= 0.85 DIP` hairline rims, and `#34638D` radial pointer reveal).
- **`DarkWood` (`Dark Wood`):** Matte `#202020` dark surface.
- **`Blue` (`Blue`):** Classic `#3186FF` sapphire glass look.
- **Glass Level (`0–100%`, `glassLevel`):** Controls acrylic opacity and blur balance (default `50%`).
- **Light / Dark Mode (`0–100%`, `lightDarkMode`):** Smoothly transitions material tone between dark (`0%`) and light (`100%`).

### Mouse Triggers
Configured via `mouseTrigger` in `%APPDATA%\WinRings\config.json` or the **Triggers & Input** page of `WinRingsTools.exe --settings`:
- **`SideBack` (Default):** Mouse Back button (`XButton1`).
- **`SideForward`:** Mouse Forward button (`XButton2`).
- **`Middle`:** Middle mouse wheel button.
- **`RightLeftChord`:** Hold Right mouse button and press Left mouse button.

### Activation Modes
1. **Quick-Tap Sticky Mode (`< 180 ms`):**
   - Tap and release the trigger button quickly.
   - The Ring stays open on screen.
   - Left-click any slot or outer submenu item to activate it, middle-click a window preview to close that window, type letters on the keyboard to search installed system apps (with a floating search pill above the Ring, live slot highlighting for pinned/running apps, and a curved top half-ring for unpinned system apps), press `1–9` or Arrow keys + `Enter`, or press `Esc` / click inside the center hub to close.
2. **Hold-Flick-Release Mode (`>= 180 ms`):**
   - Press and hold the trigger button, move the cursor outside the `18 DIP` deadzone into the ring annulus (or out onto an expanded outer-arc preview), and release the button to switch immediately.
3. **Double-Tap Last App (`< 280 ms`):**
   - Double-tap the mouse trigger button to immediately toggle back to the previously active application window.

### Keyboard Triggers
- **`Alt + Space` (Default Hotkey):** Opens/closes the Ring in Sticky Mode. Hold `Shift` while triggering (`Alt + Shift + Space`) when `shiftCenterOnActiveMonitor` is enabled to center the Ring on the active monitor and center the selected window upon activation.
- **Menu Key (`VK_APPS`):** Opens/closes the Ring in Sticky Mode.
- **Optional `Win + Tab` / `Alt + Tab` Replacement:** Enable `interceptWinTab` or `interceptAltTab` in Settings to intercept native task switchers.

---

## 2. Outer-Arc Submenus & Drag-and-Drop Workflows

- **Multi-Window Apps:** Hovering any app slot with 2+ open windows (or 1+ when `fanOutSingleWindow: true`) fans out live rounded DWM window thumbnails along the outer arc (`windowViewMode: "OuterArc"`) or displays a vertical title list (`"TitleColumn"`).
- **Downloads Slot Fan-Out:** Hovering the pinned `Downloads` folder slot fans out the 9 most recently modified files/folders in your `Downloads` directory. Click any item to open it or **drag it directly out of the Ring** (OLE `IDropSource`); the ring remains open during your drag so you can also drop or spring-load onto other ring slots.
- **Drag Icons to Reorder:** In Sticky Mode, press and drag any pinned icon around the ring annulus to reorder slots interactively; releasing saves the new order to `config.json`.
- **Dropping Files / URLs onto the Ring:** Start dragging files or links anywhere (even outside the ring, or press the ring trigger while dragging) and move onto the Ring:
  - Drop onto the **Center Hub** (`Pin to Ring`) to pin a file, folder, `.exe`, or `http(s)://` URL directly to the ring.
  - Drop onto an **App** or **Script** slot to launch it with the dropped files as arguments.
  - Drop onto a **Folder** slot (e.g., `Downloads`) to copy the dropped files into that directory.
  - Drop onto **Recycle Bin** to send the files to the Windows Recycle Bin.
  - **Spring-Loaded Window Switch:** Hold dragged files steady over any **App** or **Folder** icon for ~0.5 seconds; after a double-heartbeat scale pulse, WinRings brings that app or folder window forward while you continue holding the drag so you can drop directly inside the window.

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
