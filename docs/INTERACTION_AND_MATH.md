# WinRings — Interaction, Math & Search Reference (v3.0.0)

All core geometric, hit-testing, matching, and search formulas live as pure functions in `src/shared/` and are unit-tested before integration in `src/shell/`.

---

## 1. Trigger & Input State Machine (`src/shell/InputEngine.cpp`)

Installs `WH_MOUSE_LL` and `WH_KEYBOARD_LL` hooks while Explorer is active (suspended on session lock, RDP, or secure desktop):

- **Mouse Triggers (`MouseTriggerButton`):** `SideBack` (`XButton1`, default), `SideForward` (`XButton2`), `Middle`, `RightLeftChord` (non-blocking for normal right-click), or `Disabled`.
- **Dual Activation Modes (`EndTrigger`):**
  1. **Quick-Tap -> Sticky Mode:** `isTap == true` when `lbuttonHeld` OR (`elapsedMs < tapThresholdMs (180 ms)` AND `movedDip < deadzoneDip (18 DIP)`). Locks the ring open for click, hover, keyboard search, Arrows/`Enter`, `1–9`, `Tab`, `Esc`, and right-click context menu.
  2. **Hold-Flick-Release:** Holding `>= 180 ms` (or moving past deadzone) lets you flick onto any slot or fanned-out outer submenu card and release immediately to switch/launch.
  3. **Double-Tap Last App:** If `doubleTapLastApp == true` and two clean taps occur within `doubleTapWindowMs (280 ms)`, invokes `onDoubleTapLastApp` (instant `Alt+Tab` toggle with `WindowTracker::PreviousActiveWindow()`).
- **Click-vs-Drag Outside Dismissal:**
  - Clicking outside a sticky ring sets `m_outsideClickPending` and records `m_outsideClickOrigin`.
  - Moving `>= max(5, max(SM_CXDRAG, SM_CYDRAG))` clears `m_outsideClickPending` so dragging desktop/Explorer files toward the ring **never** dismisses it.
  - Only a clean `WM_LBUTTONUP` outside active bounds cancels the ring.
  - Additionally, `RingOverlay::HandleMessage(WM_NCHITTEST)` returns `HTTRANSPARENT` outside `IsPointInActiveBounds(pt)` so transparent corners of the `760x760` overlay never eat pointer events behind the ring.
- **Scroll Wheel & Keyboard Filtering:**
  - `WM_MOUSEWHEEL` while open calls `HandleScrollWheel` and swallows the wheel (`return 1`) so background windows never scroll.
  - **Keyboard Hotkey:** Default `Alt + Space` (`hotkeyModifiers = MOD_ALT`, `hotkeyVk = VK_SPACE`); optional `VK_APPS` (`menuKeyEnabled`), `interceptWinTab`, `interceptAltTab`.
  - **Shift-Center Modifier:** Holding `Shift` when committing/activating with `shiftCenterOnActiveMonitor` centers the ring/target window cleanly on the active monitor.
  - **Type-to-Search:** Typing `A–Z`, `0–9`, `-`, `.`, `Space`, or `Backspace` routes straight to `AppendFilterChar`/`BackspaceFilter`.

---

## 2. Geometry & Hit-Testing Math (`src/shared/RadialMath.h`)

- **Main Ring Geometry (`ComputeRingGeometry`):**
  - Default config: `ringRadiusDip = 115.0`, `iconSizeDip = 44.0`, `deadzoneDip = 18.0`, `maxSlots = 14`.
  - Adaptive scaling for dense rings: `slotCount >= 13` clamps `icon <= 40.0 DIP`, `>= 9` clamps `icon <= 46.0 DIP`, expands `ringRadius = max(88.0, configRingRadiusDip, ((icon + 14.0) * slotCount) / 2pi)`.
  - `outerRadius = ringRadius + iconSize * 0.5 + 12.0`.
  - `holeRadius = max(deadzoneDip + 8.0, ringRadius - iconSize * 0.5 - 10.0)`.
  - `runningDotRadius = max(holeRadius + 4.0, ringRadius - iconSize * 0.5 - 6.0)`.
- **Angular Sectors (`CalculateSectorIndex`):** Slot `0` anchored at 12 o'clock (`-pi / 2`), clockwise, `[-w/2, +w/2)` per slot; returns `-1` inside `deadzoneDip`.
- **Bounded Hover & Corridor Resolution (`ResolveRingHover`):**
  1. Inside `deadzoneDip` -> clears selection & collapses open submenus.
  2. Hovering **another main-ring slot badge** directly switches `selectedSlot` immediately (unless in top-search arc mode `expandedSlotIndex == -2`, where it keeps the top search arc open).
  3. Outside `mainRingOuterR`: checks expanded submenu cards (`HitTestRects`), then `IsPointInOuterSubmenuZone` (strictly `dist > mainRingOuterR`; combines tight annular card-band check + narrow bridge `IsPointInHoverCorridor` from `corridorOrigin` — never projects wraparound corridors into opposite ring sectors).
  4. **Outer Reach Cap:** Outside `mainRingOuterR + outerReach` (`12.0 DIP` sticky, `48.0 DIP` flick), main-ring hover returns `-1` so stray outer cursor motion never triggers unhovered app PiPs.

### Submenu Geometry (`RadialMath.h`)
- **Folder / Downloads / Collection Curved Half-Ring (`ComputeIconArcLayout`):** Arranges up to `9` (max `12`) `40x40 DIP` items centered around the slot's outward angle (`maxSpanRad = 4.01426`, `trackWidthDip = 56.0`, `clearanceDip = 12.0`).
- **Multi-Window Outer Arc (`ComputeWindowOuterArcRects`):** Up to `9` DWM thumbnail cards (`104x68 DIP * thumbnailScale`), fanned symmetrically `0, +1, -1, +2, -2...` via `GetOuterArcCenters`.
- **Title Column Mode (`ComputeTitleColumnRects`):** `130–190 DIP` wide × `30 DIP` high rows with `4 DIP` gap placed on the clearest side of `mainOuterRadius + 10 DIP`.

---

## 3. Drag-and-Drop Workflows (`DropTarget.cpp`, `SlotMatching.h`, `RingOverlay.cpp`)

| Workflow | Entry & Threshold | Behavior |
| :--- | :--- | :--- |
| **Summon While Dragging** | Start Explorer drag outside -> press trigger while `VK_LBUTTON` held | `InputEngine::EndTrigger` sees `lbuttonHeld`, treats summon as sticky Lock-Open so user can drop onto the ring. |
| **Pin to Ring (Center Drop)** | Drop files, folders, `.exe`, or `http(s)://` URL within `pinRadiusDip = max(deadzone + 20, holeRadius - 4)` | Draws accent ring highlight (`holeR - 4`); saves new `PinnedSlotConfig`, triggers `--reload`; `http(s)://` pins trigger async WinHTTP favicon download (`FetchFaviconAsync` -> `%APPDATA%\WinRings\icons\`). |
| **Drop Onto Ring Slot** | Drop files on sector `>= pinRadiusDip` | **App/Script:** launches target with quoted file paths as args. **Folder (e.g. Downloads):** copies files via `SlotModel::CopyFilesToFolder` (`FO_COPY` + `FOF_ALLOWUNDO \| FOF_RENAMEONCOLLISION`). **Recycle Bin:** deletes via `SlotModel::SendToRecycleBin` (`FOF_ALLOWUNDO`). |
| **Spring-Loaded Hover-to-Open** | `OnDragOverOverlay` on `App` or `Folder` slot (`CanSlotSpringLoad`) | `EvaluateSpringHover(elapsedMs)`: `500 ms` dwell -> `420 ms` double-heartbeat pulse (`+24% / -14%` sine beats) -> `TriggerSpringLoadForSlot` dismisses ring and calls `ActivateWindow(hwnd, false, false)` (or launches path) **without moving cursor** while OLE drag continues. |
| **Drag Icons to Reorder** | Sticky Mode `WM_LBUTTONDOWN` on pinned slot, move `>= 6.0 DIP` (`m_filterQuery` & `!m_searchArcActive`) | `ComputeReorderTargetSlotIndex` clamps to `[0, pinnedCount - 1]`; `ReorderVectorElement` rotates `m_visibleSlots`/`m_allSlots`/`m_slotScales` live; `WM_LBUTTONUP` saves `m_config.pinnedSlots` and reloads. |
| **Drag Out of Downloads/Folder Fan-Out** | Press submenu item, move `>= 5.0 DIP` | Calls `RingDropTarget::StartFileDragOut(nullptr, dragPath)` (`SHDoDragDrop`) while setting `m_draggingOutFromRing = true` so the ring stays open during drag and dismisses cleanly afterward. |

---

## 4. Explorer Window Matching (`src/shared/SlotMatching.h`) & Search (`src/shared/SearchMatch.h`)

- **File Explorer vs. Folder/Recycle Bin Separation:**
  - `WindowTracker` detects `IsExplorerExecutablePath(exePath)` and locks the App slot's label to `"File Explorer"` with the native Explorer shell icon (ignoring dynamic `WM_GETICON` folder icons like Downloads' green arrow or Start Menu `.lnk` names).
  - `SlotModel::BuildRingSlots` matches open Explorer windows against pinned `SlotKind::Folder` items via `ExplorerWindowMatchesFolder` (strips `" - File Explorer"`, matches normalized path or leaf name — safe against drive roots like `"C:"`) and against `SlotKind::RecycleBin` via `ExplorerWindowMatchesRecycleBin` (including localized shell display name).
  - `ShouldShowSlotWindowPreview(kind, count, fanOutSingleWindow)` permits DWM thumbnail fan-outs **exclusively** for `SlotKind::App`.
- **Window Activation & Pointer Glide (`WindowTracker.cpp`):**
  - Restores minimized windows, attaches input/unlocks foreground safely, optionally centers window on active monitor (`Shift`), and when `pointerFollow == true` glides the cursor over `160 ms` (cancelling immediately via `LLMHF_INJECTED == 0` check if the user moves the mouse manually).
- **System App Search (`SearchMatch.h` & `AppIndex.cpp`):**
  - `AppIndex` scans Start Menu `.lnk` and `shell:AppsFolder` asynchronously on a background thread.
  - `ScoreSearchMatch(candidate, query)` assigns tiers: `0` Exact Prefix, `1` Word-Boundary / CamelCase Prefix, `2` Sequential Acronym/Initials (`query.size() >= 2`, e.g., `"fe" -> "File Explorer"`), `3` Substring, `-1` No match.
  - While typing, `RingOverlay::RebuildLayoutAfterFilter` dims non-matching ring slots (`0.22` alpha) and displays up to `6` matching unpinned installed apps in a curved top half-ring (`expandedSlotIndex == -2`, `ComputeIconArcLayout` centered at `-pi / 2`).
