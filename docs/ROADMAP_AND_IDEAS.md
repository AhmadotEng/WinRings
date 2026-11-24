# WinRings — Improvement Ideas & Architecture Roadmap (v3.1+)

Concrete feature ideas, UX polish opportunities, and structural refactors to explore next while preserving WinRings's native C++17 performance and visual craft.

---

## A. Interaction & Workflow Features

1. **Multi-Ring Workspaces / Profiles:**
   - Support named ring sets (e.g., `Default`, `Dev`, `Media`, `Design`) switchable via center double-click, left/right wheel tilt, `1–4` chord, or per-foreground-app auto-profile.
   - Reuse `PinnedSlotConfig` serialization with an array of named profiles while keeping single-ring config migration seamless.
2. **Direct Drop Onto Fanned-Out Window Previews:**
   - Currently file drops target main-ring slots or dwell on an `App`/`Folder` icon (`500 ms` spring-load) to switch windows. Extending `OnDragOverOverlay` / `OnDropOverlay` to open an app's fanned-out DWM preview strip (`DwmThumbPool`) and spring-load directly onto the exact target window HWND improves multi-window workflows (e.g., dropping onto a specific VS Code or Edge window).
3. **Quick Window Actions on Preview Cards:**
   - Right-click or compact hover action overlay on DWM window preview cards (`Snap Left`, `Snap Right`, `Maximize`, `Always on Top`, alongside existing middle-click `WM_CLOSE`).
4. **Precision Touchpad / Edge-Zone Triggering (Optional):**
   - Integrate Raw Input / WM_POINTER precision touchpad gesture detection alongside `InputEngine`'s low-level mouse triggers without external dependencies.
5. **Contextual Quick Actions Inside Pinned Folder Fan-Outs:**
   - Right-click menu on expanded `Downloads` / `Folder` arc items (`Reveal in Explorer`, `Copy Path`, `Delete to Recycle Bin`, `Pin Item to Main Ring`).

---

## B. Visual & Settings Polish

1. **Saved Custom Themes & Live Accent Swatches (`SettingsDialog.cpp`):**
   - Add visual color picker / preset swatch grid for `accentColor` (`#0078D4` + curated accents) and ability to save named `(theme, glassLevel, lightDarkMode, accentColor)` presets cleanly inside `RingThemeEngine.h`.
2. **System Light/Dark Auto-Sync:**
   - Watch `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize\AppsUseLightTheme` (and `WM_SETTINGCHANGE("ImmersiveColorSet")`) to automatically map `lightDarkMode` between dark (`0`) and light (`100`) when an `Auto` toggle is enabled.
3. **Custom Icon Override per Pinned Slot:**
   - Allow picking a `.ico`, `.png`, `.exe,index`, or folder icon per `PinnedSlotConfig` (persisted via `iconCacheFile` or explicit icon override field) on the **Pins & Folders** page and ring right-click menu.
4. **Smoother Physics for Slot Reordering & Animated Submenu Fan-Out:**
   - Add interpolated angular animation (`StepAnimations`) when surrounding slots slide apart during live drag-to-reorder instead of instant snap shifts, while keeping initial ring opening `disableOpeningAnimation` instant (`frame 0`) by default.
5. **Mixed-DPI Per-Monitor Live Transitions:**
   - Re-evaluate `UpdateDpiAndPosition` dynamically when dragging the ring or summoning via `Alt+Shift+Space` across monitors with differing scaling (`100%` vs `150%`).

---

## C. Architecture, Reliability & Tooling Refactors

1. **Decompose `RingOverlay.cpp` (`3,146` lines):**
   - Split cleanly into focused responsibilities inside `src/shell/`:
     - `WcaBackdropWindow.h/.cpp` — Back HWND lifecycle, WCA attributes, `1.0 px`-inset hash-gated `SetWindowRgn`.
     - `RingRenderer.h/.cpp` — Swap chains, D2D1 geometries, 3D glass bezel gradients, slot/search/pill drawing.
     - `SubmenuController` / `DragReorderController` — Outer arc layout, `StartFileDragOut`, spring-load state, and reorder state.
2. **Decompose `SettingsDialog.cpp` (`2,577` lines):**
   - Extract the shared 1:1 Direct2D preview control (`PreviewCanvas`), owner-drawn Fluent widgets (`FluentSlider`, `FluentCard`, `FluentButton`), and individual page controllers (**Appearance**, **Triggers**, **Pins**, **Advanced**).
3. **Incremental `AppIndex` Refresh (`ReadDirectoryChangesW`):**
   - Watch Start Menu directories (`FOLDERID_Programs`, `FOLDERID_CommonPrograms`) with `FindFirstChangeNotificationW` / `ReadDirectoryChangesW` so newly installed or uninstalled apps appear in `AppIndex` without waiting for manual refresh.
4. **Icon Cache Lifecycle & Favicon Cleanup (`%APPDATA%\WinRings\icons\`):**
   - Prune unreferenced cached favicons cleanly when a Website pin is removed.
5. **Automated Visual Sanity Check Script:**
   - Extend `tools/` with a headless `--preview` screenshot check (launching `--preview`, verifying `WinRings.Overlay.v1` + `WinRings.WcaBackdrop.v1` Z-order lock & `COMPLEXREGION`, capturing via `PrintWindow` across `A4S`, `DarkWood`, `Blue`).

---

## D. Scope Guardrails (Intentional Non-Goals)

- **No taskbar or Start Menu hooks:** Windows taskbar modification belongs in ClearTop Taskbar, and Start Menu replacement belongs in A4S-Menu. WinRings stays purely focused on the Radial Switcher & Gesture workflow.
- **Zero `.NET`/CLR & zero external converter binaries:** Keep the pure C++17 `/MT` static native build and lean 4-binary footprint intact.
