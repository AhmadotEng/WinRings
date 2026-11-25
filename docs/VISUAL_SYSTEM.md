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

> **Critical Invariant:** When `wcaActive == true` (`A4SAcrylic`), synthetic D2D grain is disabled (`useNoise = false`). DWM WCA already provides Windows 11's native acrylic micro-texture; painting synthetic noise on top muddies the glass.

Four perimeter layers seamlessly wrap the `1.0 px`-inset WCA boundary (driven cleanly by `RingMaterialSpec` tokens in `RingThemeEngine.h` for both neutral and custom surface colors):

1. **Antialiased `2.0 DIP` Edge-Bridge** (`2.2 DIP` on curved polygon arcs):
   - Drawn at `outerR - 0.9f` and `holeR + 0.9f` using `tc.edgeBridgeColor` (`RGBA(34,34,36,0.90)` dark, `RGBA(236,236,240,0.90)` light, or custom surface RGB `@ 0.90`).
2. **Soft Elevation Shadow Lip:**
   - Outer: radius `outerR + 0.65f`, stroke `1.3f` | Inner: radius `holeR - 0.55f`, stroke `1.1f`.
   - Opacity: `0.30f` (dark) / `0.14f` (light) black.
3. **Top-Lit 3D Outer Glass Bezel** (`1.05f` width at `outerR - 0.525f`, vertical linear gradient stops `0.0 / 0.52 / 1.0` from `tc.outerBezelTop/Mid/Bot`).
4. **Optically Recessed 3D Inner Bevel** (`0.95f` width at `holeR + 0.475f`, inverted catch-light stops `0.0 / 0.52 / 1.0` from `tc.innerBezelTop/Mid/Bot`).

---

## 3. Honest Multi-Texture Engine & Decoupled Color (`src/shared/RingThemeEngine.h`)

`ComputeRingMaterial(cfg, dpiScale)` decouples **Surface Texture** (`SurfaceTexture`) from **Surface Color** (`useCustomSurfaceColor`, `surfaceColor`, `lightDarkMode`, `autoLightDark`) and **Highlight Accent** (`accentColor`, `autoWindowsAccent`):

| Surface Texture | Runtime Backend (`SyncBackdropBackend`) | Optical Recipe (`g = glassLevel / 100.0f`) |
| :--- | :--- | :--- |
| **`A4SAcrylic`** (Default) | `WcaDualHwnd`: `WinRings.WcaBackdrop.v1` native `ACCENT_ENABLE_ACRYLICBLURBEHIND` (`state=4, flags=0x13`) + DComp foreground | `tintAlpha`: `0.08 -> 128/255 -> 0.96`, `blurSigma`: `18 -> 30 -> 0`, `saturation`: `1.30 -> 1.00`, `luminosityAlpha`: `0.24 -> 0.90 -> 0` (zero synthetic noise on WCA) |
| **`TaskbarGlass`** | `TaskbarComposition`: `AcrylicBackdrop` (`Windows::UI::Composition` `HostBackdropBrush` + `dwmcore` tree effect graph) | Wide silky diffusion (`blurSigma`: `28 -> 54 -> 0`), rich color bleed (`saturation`: `1.38 -> 1.12`), deep luminosity wash (`0.35 -> 0.84 -> 0`), `tintAlpha`: `0.12 + 0.76*g`, subtle `0.010` anti-banding frost |
| **`PureMatte`** | `MatteDComp`: Direct2D solid/translucent fill + DComp foreground (`m_wcaBackdropHwnd` hidden) | Zero blur (`blurSigma = 0`), zero grain (`noiseOpacity = 0`), clean `tintAlpha = 0.14 + 0.84*g` |

Any of the three textures can be combined freely with neutral Dark/Light, `Auto System` Light/Dark (`AppsUseLightTheme`), or any custom `#RRGGBB` surface color (`surfaceColor`), while `isLightText` adapts automatically via relative luminance (`0.2126*R + 0.7152*G + 0.0722*B < 142`).

---

## 4. `dwmcore` `TaskbarGlass` Pipeline Details (`AcrylicBackdrop.cpp`)

`AcrylicBackdrop.cpp` runs `GaussianBlur(blurSigma) -> Saturation -> Blend(luminosity) -> Composite(SOURCE_OVER, tint)` inside `Windows::UI::Composition`:
- **Strict Tree Graph:** Zero shared parent DAG nodes inside `dwmcore`.
- **`CLSID_D2D1Flood` `Color`:** Pass **STRAIGHT RGBA** `{ r, g, b, a }` (`dwmcore` premultiplies internally).
- **`CLSID_D2D1Blend` `Mode`:** Pass **`D2D1_BLEND_MODE_COLOR`** with `{ backdropStage, lumFlood }` to preserve wallpaper hue/saturation while applying `luminosityAlpha`.
- **Clean Backend Switching:** Switching away from `TaskbarGlass` calls `m_impl->target.Close()` and resets `DWMWA_USE_HOSTBACKDROPBRUSH` + `DwmExtendFrameIntoClientArea({0,0,0,0})` before attaching `IDCompositionTarget`.
