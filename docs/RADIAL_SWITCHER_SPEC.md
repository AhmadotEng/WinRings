# Windows Cursor-Centered Radial Switcher — 5-Phase Architecture Plan (Mouse & Keyboard Edition)

> **Approach:** Clean-room native Windows 10/11 implementation (`.NET 8 WPF` + Win32/DWM), focused on mouse side buttons, scroll-wheel navigation, and keyboard workflows ([Beckon](https://withbeckon.app/), [Docs](https://withbeckon.app/docs), [Story](https://withbeckon.app/story), [Privacy](https://withbeckon.app/privacy)).

---

## Core Architectural Principles

- **Full Radial Cone Click Area (`18 DIP` deadzone) with Icon Expansion & Glow:** Draw the inner glass ring at `visualHoleRadius` (`ringRadius - iconSize/2 - 10`) for breathing room, but run hit-testing across the entire angular wedge from `18 DIPs` outward in **both** Flick and Sticky modes (and during file drops). Entering any cone immediately highlights the full wedge, expands the slot icon (`1.24x` scale with circular accent glow), and fans out live window previews (even for single-window `1+` apps) with no outer distance gate. Clicking anywhere in the cone activates the app; clicking inside `< 18 DIPs` cancels cleanly.
- **Disable Opening Animation (Instant Static Mode):** Enabled by default (`DisableOpeningAnimation = true`, also respecting Windows `ClientAreaAnimation`) so the ring appears at `progress = 1.0`, full scale, and 100% opacity on Frame 0 with zero animation timer overhead. Turning it off enables a crisp `120 ms` cubic ease-out bloom and `90 ms` hover-scale animation.
- **Mouse-First Triggers & Scroll-Wheel Integration:** Support Middle Click, Side Back (`XBUTTON1`), Side Forward (`XBUTTON2`), and `Alt+Space`; intercept `WM_MOUSEWHEEL` while open so scrolling cycles windows or ring slots without scrolling background apps.
- **Double-Tap for Last App:** Quickly double-tapping the trigger (`< 280 ms`) switches straight back to the previous foreground `HWND` without spawning the ring.
- **Low Memory & Local Privacy:** Keep ~20 MB idle footprint by clearing overlay visuals and unregistering DWM thumbnails on dismiss; all thumbnails and file conversions run strictly locally.

---

## Phase 1 — Core Ring, Mouse Side Buttons, Scroll Wheel & Cone Hit-Testing
- **Global Low-Level Hooks (`GlobalInputHook.cs`):**
  - `WH_MOUSE_LL`: Trigger on configurable `Middle`, `SideBack (XBUTTON1)`, or `SideForward (XBUTTON2)` button (`WM_XBUTTONDOWN` / `WM_XBUTTONUP` high-word `mouseData`), plus `WM_MOUSEWHEEL` capture while the ring is open (suppressed from reaching background windows).
  - `WH_KEYBOARD_LL`: `Alt+Space` (or custom hotkey), `Esc` dismiss, and type-to-find filtering.
  - **Hold vs. Tap-Lock & Double-Tap Last App:** Hold + flick `>= 18 DIPs` commits on release; quick tap (`< 180 ms`) locks the ring open; double-tap (`< 280 ms`) activates the previous foreground window immediately.
- **Scroll-Wheel Behavior:**
  - When an outer window arc, title column, folder, or Collection is open: scroll wheel steps smoothly through sub-items (wrapping cleanly) and updates the live highlight/preview.
  - When on the main ring: scroll wheel steps clockwise/counter-clockwise through inner-ring slots so you can switch apps with zero cursor travel.
- **Polar Geometry & Window Activation:** 12 o'clock-anchored sector math (`RadialMath.cs`), `18 DIP` hit-test deadzone, Per-Monitor V2 DPI clamping, `EnumWindows` filtering cloaked/tool windows, icon caching, and `SetForegroundWindow` with focus-lock fallback.

## Phase 2 — Multi-Window Precision & Live DWM Thumbnails
- **Per-App Grouping:** Group open windows by executable path with count badges (`2`..`9+`), sorting non-minimized windows first in Z-order and minimized windows last (dimmed).
- **Outer Fan-Out Arc & Title Column:** Fan out up to 9 windows radially (most recent straight out along the app's angle) using hardware-composited `DwmRegisterThumbnail` previews, or switch to `TitleColumn` mode.
- **Hover-Corridor Persistence:** Safe triangular corridor (`RadialMath.IsPointInHoverCorridor`) prevents collapsing the submenu when moving toward it across adjacent icons.
- **Pointer Glide & Shift-Center:** Smooth ~160 ms eased cursor glide to the activated window (cancels on manual mouse movement); holding `Shift` on release centers the window on the active monitor (shrinking only if oversized).

## Phase 3 — Taskbar Replacement & Custom Ring Items
- **Persistent Pinned Slots (`%APPDATA%\RadialSwitcher\config.json`):** Pin apps (running or not), websites (cookie-free one-time favicon cache), files, folders/Downloads (newest-file fan-out + drag-out), scripts, Collections, and Recycle Bin (`SHFileOperation` with `FOF_ALLOWUNDO`).
- **In-Ring Management & Context Menus:** Drop files/URLs onto the `< 18 DIP` center pill to pin; right-click apps for open windows, `New Window`, `Minimize All`, `Quit`, or files for `Show in Explorer`; right-click center for `New Collection…` and `Settings…`.
- **Reversible Taskbar Auto-Hide (`TaskbarManager.cs`):** `SHAppBarMessage(ABM_SETSTATE)` with crash-safe original-state persistence in `taskbar_state.json`.

## Phase 4 — Local File Drop Actions
- **Dynamic OLE Drop Slots:** Dragging files onto the ring reveals **Convert** and **Actions** slots while keeping `18 DIP` cone targeting active across all slots.
- **Non-Destructive Local Processing (`DropActionService.cs`):** Native WIC image conversion (PNG/JPG/BMP/TIFF) + configurable local `ffmpeg` (WebP, AVIF, MP3, OGG, Opus, WMA, MP4, WebM, MKV, AVI, WMV), **Compress** (`Preset` 1080p/720p/480p, `File Size`, `Percentage`), **Trim**, interactive **Redact** (`Solid`, `Blur`, `Pixelate`), and `.zip` archiving.

## Phase 5 — Six Themes, Settings UI, Unit Tests & Installer
- **Six Looks (`ThemeEngine.cs`):** `LiquidGlass`, `Glass`, `Kiwi`, `Aura`, `Orbit`, and `Outline` with custom accent color.
- **Settings UI (`SettingsWindow.xaml`):** Configure trigger button (`Middle` / `SideBack` / `SideForward`), scroll-wheel mode, double-tap last app, instant static mode, window view mode, pointer glide, Shift-center, Taskbar auto-hide, theme, and `ffmpeg` path.
- **Tests & Installer:** `RadialSwitcher.Tests` (xUnit) covering `18 DIP` cone hit-testing, scroll-wheel stepping, outer-arc symmetry, hover corridor, and monitor clamping, plus Inno Setup script (`installer/RadialSwitcher.iss`).
