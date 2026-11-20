# WinRings — Engineering, Debugging & Reverse-Engineering Walkthrough

This document records the end-to-end engineering, reverse-engineering (`Taskbar.dll` and `SystemTray.dll` on Windows 11 25H2 Build `26200.8875`), bug diagnoses, and verification steps performed during the creation of **WinRings v1.0.0**.

---

## 1. Root-Cause Analyses & Fixes During Live Testing

### Bug 1: Downloads / Outer-Ring Submenu Hover Lock
- **Symptom:** Hovering over the `Downloads` slot expanded its 9-item outer-arc submenu, and subsequently moving the cursor onto a neighboring main-ring icon kept selection locked on `Downloads`.
- **Root Cause:** In `RingOverlay::UpdateHoverFromPoint` (`src/shell/RingOverlay.cpp`), when `m_expandedSlot >= 0`, the `IsPointInSubmenuCorridor` check executed *before* checking whether the cursor was inside the main ring annulus (`dist >= ringDeadzoneDip && dist <= outerRadius + 14.0f`) over a different main-ring slot (`ringSlot != m_expandedSlot`). Because the `±58°` corridor overlapped adjacent ring slots, the expanded submenu intercepted hover over neighboring main-ring icons.
- **Fix:** Updated `RingOverlay::UpdateHoverFromPoint` so that when `m_expandedSlot >= 0`, it first checks if the cursor is inside the main ring annulus (`dist >= cfg.ringDeadzoneDip && dist <= outerRadius + 14.0f`) and `ringSlot != m_expandedSlot`; if so, it immediately switches `m_selectedSlot` to `ringSlot` and collapses/updates the submenu. Added deterministic regression tests in `tests/WinRingsTests.cpp`.

---

### Bug 2: Windows 11 25H2 Top-Edge Docking Crash in `Taskbar.dll` & Zero-WinRT Fix
- **Symptom:** Hooking `Feature_59213768` (`0x0E486C`) in `Taskbar.dll` and calling `TrayUI::SetStuckPlace(pITrayUI, ABE_TOP)` (`0x0E1050`) caused a null-pointer dereference at `Taskbar.dll + 0x0E1233` (`mov rax, [rcx]; mov rbx, [rax]`) inside `0x0E436C` (`NotifyTaskbarEdgeChanged`), crashing `explorer.exe`.
- **Root Cause:** On Windows 11 25H2 (`build 26200.8875`), Microsoft's internal WinRT XAML island edge-change listener array at `[pITrayUI + 0x2F0]` contains an uninitialized pointer when `Feature_59213768` is forced on at runtime.
- **Fix (`src/shell/TopBarHook.cpp`):**
  1. Never hook `Feature_59213768` and never call `TrayUI::SetStuckPlace`.
  2. Install 4 zero-WinRT detours on `Taskbar.dll` (`TimeDateStamp == 0xA4EA993A`, `SizeOfImage == 0x002FC000`):
     - `0x045EA0` (`TrayUI::GetMinSize` Primary)
     - `0x075430` (`TrayUI::GetMinSize` Secondary)
     - `0x0161A0` (`TrayUI::GetDockedRect`)
     - `0x016240` (`GetAutoHideFlags`)
  3. In `TopBarHook::ApplyOnTaskbarUiThread`:
     - Update `[pITrayUI + 0x32C]` (`m_primaryTaskbarHeightDip = 30`) via `TrayUI::UpdateTaskbarSize` (`0x093540`).
     - Write `1` (`ABE_TOP`) or `3` (`ABE_BOTTOM`) directly to `[pITrayUI + 0x110]` (`m_uStuckPlace`) and zero `m_rcStuck[targetEdge]` (`[pITrayUI + 0x78 + targetEdge * 16]`).
     - Invoke `ITrayUI::VerifySize(pITrayUI, true, true, nullptr)` (`vtable[0x68]`, RVA `0x0E1AD0`), which cleanly moves `Shell_TrayWnd` to `(0, 0, 1920, 30)` and updates `rcWork` to `(0, 30, 1920, 1080)`.

---

### Bug 3: System Tray Overflow Flyout (`^`) Opening Off-Screen Above Top Taskbar & Clock Casing
- **Symptom:**
  1. Clicking the system tray overflow chevron (`^`) on the top taskbar did not show the tray overflow flyout, and the WinRings tray icon was hidden by default.
  2. Weekday text in the single-line clock was lowercase (`fri 20 Nov  4:02 pm`) instead of TitleCase (`Fri 20 Nov  4:02 pm`).
- **Root Cause:**
  1. Disassembling `C:\Windows\SystemApps\MicrosoftWindows.Client.Core_cw5n1h2txyewy\SystemTray.dll` (`TimeDateStamp = 0x6A1A2032`, `SizeOfImage = 0x001FB000`) at `0x0B9EB0` (`OverflowXamlIslandManager::PositionWindow`) proved that when `Feature_59213768` is `false`, `SystemTray.dll` calculates `Y = anchorY - heightPx` (assuming a bottom taskbar). When the taskbar is at `Y = 0..30`, `anchorY ≈ 0`, so `Y = -heightPx` (off-screen above the top of the monitor).
  2. Windows 11 creates new tray icon entries under `HKCU\Control Panel\NotifyIconSettings\<Key>` with `IsPromoted = 0` by default, hiding them inside the overflow popup.
  3. `ClockCasing::CompactLower` in `ClockFormat.h` lowercased both `isWeekday` and `isMeridiem`.
- **Fix:**
  1. **`ClockFormat.h`:** Updated `ClockCasing::CompactLower` to only lowercase `isMeridiem` (`am`/`pm`), keeping weekday and month TitleCase (`Fri 20 Nov  4:02 pm`).
  2. **`TopBarHook.cpp` & `XamlTaskbarCtrl.cpp`:** Hooked `SystemTray.dll!OverflowXamlIslandManager::PositionWindow` (`0x0B9EB0`) and added an in-process `WINEVENT_INCONTEXT` listener (`TrayPopupWinEventProc`) for `TopLevelWindowForOverflowXamlIsland` and `NotifyIconOverflowWindow`, repositioning the popup to `Y = barBottomY + MulDiv(4, dpi, 96)` directly **below** the top taskbar.
  3. **`WinRingsHost.cpp`:** Added `EnsureTrayIconPromoted()` (`IsPromoted = 1` in `HKCU\Control Panel\NotifyIconSettings`), a custom 32×32 cyan ring tray icon, UIPI message filter allowances, and top-aligned tray context menu positioning below the top bar.
  4. **`XamlTaskbarCtrl.cpp`:** Added `ElementRole::SystemTrayCompactItem` to height-compact and vertically center `NotifyIconStack`, `SystemTray.ChevronIconView`, `SystemTray.NotifyIconView`, and `SystemTray.StackListView` inside the `30 DIP` top bar.

---

## 2. Final Verification Summary

| Verification Gate | Status | Result |
| :--- | :--- | :--- |
| `tools\Build-WinRings.ps1` | **PASSED** | All 5 targets compiled with `/std:c++17 /MT /O2 /W4 /WX` (0 warnings, 0 errors). |
| `tools\Test-WinRingsProduct.ps1` | **PASSED** | x64 PE headers verified, 0 `.NET`/CLR imports, DLL exports verified, `WinRingsAbiProbe: OK`, `WinRingsTests: ALL PASSED`. |
| Live Explorer Top Bar & Toggle | **PASSED** | `TopBarHook` 4/4 `Taskbar.dll` hooks + `SystemTray.dll` overflow hook active; `Shell_TrayWnd` at `(0, 0, 1920, 30)`, `rcWork` at `(0, 30, 1920, 1080)`; `--toggle-topbar` toggles `1 -> 0 -> 1` cleanly. |
