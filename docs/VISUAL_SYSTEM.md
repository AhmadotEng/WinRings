# WinRings — Visual System & Material Reference

WinRings renders a **hollow annulus ring** with authentic Windows 11 **A4S frosted acrylic** glass inside the ring band and smooth **3D antialiased glass bevel edges** around both the inner hole and outer rim.

---

## 1. Dual-HWND Rendering Architecture (`RingOverlay.cpp`)

Standard `SetWindowRgn` produces a 1-bit integer mask whose curved boundaries staircase when used alone. WinRings avoids this by locking two synchronized `WS_POPUP` windows together at `760x760 DIP`:

| Layer | Window Class & ExStyles | Responsibility |
| :--- | :--- | :--- |
| **Back HWND (Native WCA Acrylic)** | `WinRings.WcaBackdrop.v1`<br/>`WS_EX_TOPMOST \| WS_EX_TOOLWINDOW \| WS_EX_TRANSPARENT \| WS_EX_NOACTIVATE` | Supplies genuine Windows 11 DWM `ACCENT_ENABLE_ACRYLICBLURBEHIND` (`WM_NCHITTEST -> HTTRANSPARENT`, `WM_MOUSEACTIVATE -> MA_NOACTIVATE`, `DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_DONOTROUND`, `DWMWA_COLOR_NONE`). |
| **Front HWND (Direct2D 1.1 + DirectComposition)** | `WinRings.Overlay.v1`<br/>`WS_EX_NOREDIRECTIONBITMAP \| WS_EX_TOOLWINDOW \| WS_EX_NOACTIVATE \| WS_EX_TOPMOST` | Locked immediately above `wcaHwnd` (`SetWindowPos(wcaHwnd, m_hwnd, ..., SWP_NOACTIVATE)`). Renders the `2.0 DIP` antialiased edge-bridge, elevation shadow lips, 3D bezel gradients, icons, dots, glow, search pill, and labels. |

### 1.1 Back HWND Native WCA Pipeline (`CreateWcaBackdropWindow`, `UpdateWcaMaterial`)
1. Call `DwmSetWindowAttribute(wcaHwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark))`.
2. Call `SetWindowCompositionAttribute(15 /* WCA_FORCE_ACTIVEWINDOW_APPEARANCE */, &TRUE, 4)` so `WS_EX_NOACTIVATE` popups retain live acrylic blur/luminosity while unfocused.
3. Call `SetWindowCompositionAttribute(19 /* WCA_ACCENT_POLICY */, &policy, sizeof(policy))`:
   - `state = (tc.blurSigma > 1.0f) ? 4 (ACCENT_ENABLE_ACRYLICBLURBEHIND) : 2 (ACCENT_ENABLE_TRANSPARENTGRADIENT)`
   - `flags = 0x13`
   - `gradientColor = ((DWORD)alpha << 24) | RGB(r, g, b)` (`alpha` clamped `[2, 255]`; default **A4S Dark** = `0x80202020`, i.e., `alpha = 128`, `RGB(32,32,32)`)
4. In `WM_PAINT`, return `1` on `WM_ERASEBKGND` and clear the 32-bit DIB to `alpha = 0` via `BeginBufferedPaint(hdc, &ps.rcPaint, BPBF_TOPDOWNDIB, { sizeof(pp), BPPF_ERASE }, &memDc)` so DWM composites directly behind the entire surface.

### 1.2 `1.0 px`-Inset Hash-Gated Region Clipping (`UpdateWcaRegion`)
Every shape on `wcaHwnd` is inset **`1.0 px` inward** so jagged region pixels never peek past the antialiased front edges:
- **Outer circle:** `outerPx = max(2.0, outerR * scalePx - 1.0)`
- **Inner hole (`RGN_DIFF`):** `holePx = max(1.0, holeR * scalePx + 1.0)`
- **Curved arc tracks (`RGN_OR`):** `CreatePolygonRgn` (`kArcSteps = 44`, `kCapSteps = 14`, `halfW = trackWidth * 0.5 * scalePx - 1.0`)
- **Cards & Center/Search Pills (`RGN_OR`):** `CreateRoundRectRgn` inset `+1 / -1 px`
- All inputs are hashed at `0.25 px` precision (`HashMix`) to skip redundant `SetWindowRgn` calls.

---

## 2. Front HWND Edge-Bridge, 3D Glass Bezels & Foreground (`RingOverlay::Render`)

> **Critical Invariant:** When `wcaActive == true`, synthetic D2D grain is disabled (`useNoise = false`). DWM WCA already provides Windows 11's native acrylic micro-texture; painting synthetic noise on top muddies the glass.

Four perimeter layers seamlessly wrap the `1.0 px`-inset WCA boundary:

1. **Antialiased `2.0 DIP` Edge-Bridge** (`2.2 DIP` on curved polygon arcs):
   - Drawn at `outerR - 0.9f` and `holeR + 0.9f`.
   - Dark: `RGBA(34, 34, 36, 0.90)` (`0.88` arcs) | Light: `RGBA(236, 236, 240, 0.90)` (`0.88` arcs) | Blue: `(tintR, tintG, tintB, 0.90)`.
2. **Soft Elevation Shadow Lip:**
   - Outer: radius `outerR + 0.65f`, stroke `1.3f` | Inner: radius `holeR - 0.55f`, stroke `1.1f`.
   - Opacity: `0.30f` (dark) / `0.14f` (light) black.
3. **Top-Lit 3D Outer Glass Bezel** (`1.05f` width at `outerR - 0.525f`, vertical linear gradient stops `0.0 / 0.52 / 1.0`):
   - **A4S Dark:** `(1, 1, 1, 0.25)` -> `(118/255, 118/255, 124/255, 0.42)` -> `(1, 1, 1, 0.09)`
   - **A4S Light:** `(1, 1, 1, 0.85)` -> `(170/255, 174/255, 182/255, 0.55)` -> `(120/255, 126/255, 138/255, 0.42)`
   - **Blue:** `(132/255, 198/255, 1, 0.62)` -> `ringStroke` -> `(52/255, 120/255, 230/255, 0.34)`
4. **Optically Recessed 3D Inner Bevel** (`0.95f` width at `holeR + 0.475f`, inverted catch-light stops `0.0 / 0.52 / 1.0`):
   - **A4S Dark:** `(1, 1, 1, 0.07)` -> `(108/255, 108/255, 114/255, 0.32)` -> `(1, 1, 1, 0.20)`
   - **A4S Light:** `(110/255, 116/255, 126/255, 0.34)` -> `(170/255, 174/255, 182/255, 0.45)` -> `(1, 1, 1, 0.72)`
   - **Blue:** `(48/255, 110/255, 210/255, 0.28)` -> `innerRimStroke` -> `(126/255, 190/255, 1, 0.48)`

### 2.1 Foreground Tokens (`RingThemeEngine.h` & `RingOverlay.cpp`)
- **True Open Annulus:** Center hub interior stays transparent (`innerFill = 0`, `slotIdleFill = 0`, `sectorFill = 0`); **zero** cone wedges or gray center discs.
- **Hairline Width:** `min(0.85f, 1.0f / dpiScale)` DIP (`>= 1.10f` around WCA pills).
- **Selected Slot Halo & Radial Glow:**
  - Halo fill (`slotSelectedFill`): Dark `RGBA(255,255,255, 37)` | Light `RGBA(0,0,0, 26)` | Blue `RGBA(accent, 68)`.
  - Hover rim: Dark `RGBA(255,255,255, 85)` | Light `RGBA(0,0,0, 55)`.
  - Fluent pointer radial glow (`hoverRevealColor`): Default A4S `RGB(52, 99, 141)` (`#34638D`) at center alpha `0.30f` fading to `0.0f` over `haloR * 1.48f`.
- **Running Dots:** `2.4f` radius @ `1.0` alpha for `isFrontmost`, `1.7f` radius @ `0.60` alpha for other running windows (`dotR` positioned along `geom.runningDotRadius`).
- **Search Pill:** `kSearchH = 26 DIP`, radius `13 DIP`, border `RGB(81,81,81)` (`#515151` dark) -> `#A5A5AC` (light), `#909090` vector magnifier, centered above `centerPill`.
- **Typography:** `"Segoe UI Variable Text"` (`13 DIP` primary & search, `11 DIP` card/badge labels, `"Segoe UI Variable Display"` `16 DIP` semi-bold fallback glyph), `#FFFFFF` / `#999999` (dark) and `#18181B` / `#66666E` (light).

---

## 3. Parametric 2-Slider Theme Engine (`src/shared/RingThemeEngine.h`)

`ComputeRingMaterial(cfg, dpiScale)` maps `g = glassLevel / 100.0f` (`50` default) and `t = lightDarkMode / 100.0f` (`0` default) + `accentColor` (`#0078D4` default):

| Theme Preset | `tint` Color & `tintAlpha` (`g=0.5, t=0`) | `blurSigma` & `saturation` | `luminosity` & `noiseOpacity` |
| :--- | :--- | :--- | :--- |
| **`A4S`** (Default) | `RGB(32..243)`, `0.08 -> 128/255 -> 0.96` (`0.502` @ `g=0.5`) | `18 -> 30 -> 0` (`30.0`), `1.30 -> 1.00` (`1.15`) | `RGB(38..246)`, `0.24 -> 0.90/0.88 -> 0`, `0.008 -> 0.012 -> 0` |
| **`DarkWood`** | `RGB(32..238, 32..236, 32..232)`, `0.08 + 0.88*g` | `max(0, (1-g)*14)`, `1.02` | `tint` color, `(1-g)*0.45`, `(1-g)*0.010` |
| **`Blue`** | `RGB(20..220, 42..236, 82..255)`, `0.10 -> 0.48 -> 0.94` (`g=0.45` knee) | `16 -> 28 -> 0`, `1.36 -> 1.06` | `RGB(26..228, 50..240, 96..255)`, `0.20 -> 0.72 -> 0`, `0.008 -> 0.012 -> 0` |

---

## 4. Fallback `dwmcore` Acrylic & Direct2D 1.1 1:1 Preview

Both fallback compositing (`AcrylicBackdrop.cpp`) and the Settings preview (`SettingsDialog.cpp::BuildPreviewAcrylicEffectImage`) run the calibrated linear pipeline `GaussianBlur(blurSigma) -> Saturation -> Blend(luminosity) -> Composite(SOURCE_OVER, tint)` plus `GenerateAcrylicNoiseBgra256()` (triangular-peaked `256x256` PBGRA bitmap, `alpha = 140`, drawn at `noiseOpacity = 0.012`).

**Two critical API quirks that MUST remain separated between the two paths:**

| API Detail | `AcrylicBackdrop.cpp` (`Windows::UI::Composition` / `dwmcore`) | `SettingsDialog.cpp` (`ID2D1DeviceContext` Direct2D 1.1) |
| :--- | :--- | :--- |
| **`CLSID_D2D1Flood` `Color`** | Pass **STRAIGHT RGBA** `{ r, g, b, a }` (`dwmcore` premultiplies internally) | Pass **PREMULTIPLIED RGBA** `{ r*a, g*a, b*a, a }` |
| **`CLSID_D2D1Blend` `Mode`** | **`D2D1_BLEND_MODE_COLOR`** (with `{ backdropStage, lumFlood }`) | **`D2D1_BLEND_MODE_LUMINOSITY`** (with `{ curStage, lumFlood }`) |
| **Graph constraint** | Strict tree (no shared DAG nodes; `HostBackdropBrush` needs `DWMWA_USE_HOSTBACKDROPBRUSH=17`, `DwmExtendFrameIntoClientArea(-1)`, `WCA_FORCE_ACTIVEWINDOW_APPEARANCE=15`) | Standard D2D1 effect DAG + `PushLayer` annulus mask |

**Verified Parity at Default `A4S` (`glassLevel=50`, `lightDarkMode=0`):** Pure black `(1,0,5)` lifts to `(33,33,36)` vs `(32,33,35)` live WCA; bright white `(200,200,200)` compresses to luma `43`; blue/violet wallpapers preserve vibrant hue bleed (`~RGB(24,45,82)`).
