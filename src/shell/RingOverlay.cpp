#include "RingOverlay.h"

#include <ShellScalingApi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../shared/WinRingsIdentity.h"

namespace winrings
{
    namespace
    {
        constexpr double kOverlayDip = 680.0;
        constexpr double kCenterDip = kOverlayDip * 0.5;
        constexpr UINT_PTR kAnimTimerId = 101;
        constexpr UINT kAnimIntervalMs = 16;
        constexpr double kOpenDurationMs = 120.0;

        const RectD kConvertBadgeDip{ 216.0, 610.0, 118.0, 42.0 };
        const RectD kActionsBadgeDip{ 346.0, 610.0, 118.0, 42.0 };

        D2D1_COLOR_F Argb(BYTE a, BYTE r, BYTE g, BYTE b) noexcept
        {
            return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
        }

        void ParseHexColor(const std::wstring& hex, BYTE& r, BYTE& g, BYTE& b) noexcept
        {
            r = 49; g = 134; b = 255;
            if (hex.empty())
                return;
            const wchar_t* p = (hex[0] == L'#') ? hex.c_str() + 1 : hex.c_str();
            unsigned int val = 0;
            if (wcslen(p) == 6 && swscanf_s(p, L"%06x", &val) == 1)
            {
                r = static_cast<BYTE>((val >> 16) & 0xFF);
                g = static_cast<BYTE>((val >> 8) & 0xFF);
                b = static_cast<BYTE>(val & 0xFF);
            }
            else if (wcslen(p) == 8 && swscanf_s(p, L"%08x", &val) == 1)
            {
                r = static_cast<BYTE>((val >> 16) & 0xFF);
                g = static_cast<BYTE>((val >> 8) & 0xFF);
                b = static_cast<BYTE>(val & 0xFF);
            }
        }

        bool IsClientAreaAnimationEnabled() noexcept
        {
            BOOL enabled = TRUE;
            SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
            return enabled != FALSE;
        }

        bool ContainsInsensitive(const std::wstring& haystack, const std::wstring& needle)
        {
            if (needle.empty())
                return true;
            if (haystack.size() < needle.size())
                return false;
            auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                [](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); });
            return it != haystack.end();
        }
    } // namespace

    RingOverlay::~RingOverlay()
    {
        Shutdown();
    }

    bool RingOverlay::Initialize(HINSTANCE instance, HMODULE shellModule, WindowTracker* tracker, Callbacks callbacks)
    {
        m_instance = instance;
        m_shellModule = shellModule;
        m_tracker = tracker;
        m_callbacks = std::move(callbacks);

        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        m_qpcFreq = freq.QuadPart > 0 ? freq.QuadPart : 1;

        if (!m_classRegistered)
        {
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.style = CS_DBLCLKS;
            wc.lpfnWndProc = OverlayWndProc;
            wc.hInstance = m_instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = WINRINGS_OVERLAY_CLASS_W;
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                return false;
            m_classRegistered = true;
        }

        m_hwnd = CreateWindowExW(
            WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
            WINRINGS_OVERLAY_CLASS_W,
            L"WinRings Overlay",
            WS_POPUP,
            0, 0, 680, 680,
            nullptr, nullptr, m_instance, this);
        if (!m_hwnd)
            return false;

        if (!CreateGraphicsDevices())
            return false;

        m_thumbPool.Initialize(m_instance, m_hwnd);

        m_dropTarget = new RingDropTarget({
            [this]() { OnDragEnterOverlay(); },
            [this](POINT pt) { OnDragOverOverlay(pt); },
            [this]() { OnDragLeaveOverlay(); },
            [this](POINT pt, std::vector<std::wstring> files, std::wstring text) {
                OnDropOverlay(pt, std::move(files), std::move(text));
            }
        });
        RegisterDragDrop(m_hwnd, m_dropTarget);

        return true;
    }

    void RingOverlay::Shutdown()
    {
        Dismiss();
        if (m_hwnd)
        {
            RevokeDragDrop(m_hwnd);
        }
        if (m_dropTarget)
        {
            m_dropTarget->Release();
            m_dropTarget = nullptr;
        }
        m_thumbPool.Shutdown();
        ReleaseGraphicsDevices();
        if (m_hwnd)
        {
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
    }

    void RingOverlay::SetConfig(const SwitcherConfig& config)
    {
        m_config = config;
        if (m_isOpen)
            Render();
    }

    bool RingOverlay::CreateGraphicsDevices()
    {
        const UINT d3dFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_DRIVER_TYPE drivers[] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };
        HRESULT hr = E_FAIL;
        for (auto driver : drivers)
        {
            hr = D3D11CreateDevice(nullptr, driver, nullptr, d3dFlags, nullptr, 0,
                D3D11_SDK_VERSION, m_d3dDevice.ReleaseAndGetAddressOf(), nullptr, nullptr);
            if (SUCCEEDED(hr))
                break;
        }
        if (FAILED(hr) || !m_d3dDevice)
            return false;

        if (FAILED(m_d3dDevice.As(&m_dxgiDevice)) || !m_dxgiDevice)
            return false;

        D2D1_FACTORY_OPTIONS opts{};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
            &opts, reinterpret_cast<void**>(m_d2dFactory.ReleaseAndGetAddressOf()))))
            return false;

        if (FAILED(m_d2dFactory->CreateDevice(m_dxgiDevice.Get(), m_d2dDevice.ReleaseAndGetAddressOf())))
            return false;

        if (FAILED(m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            m_d2dContext.ReleaseAndGetAddressOf())))
            return false;

        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.ReleaseAndGetAddressOf()))))
            return false;

        m_dwriteFactory->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.0f, L"en-us", m_titleFormat.ReleaseAndGetAddressOf());
        if (m_titleFormat)
        {
            m_titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_titleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
            m_dwriteFactory->CreateEllipsisTrimmingSign(m_titleFormat.Get(), ellipsis.GetAddressOf());
            m_titleFormat->SetTrimming(&trim, ellipsis.Get());
            m_titleFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }

        m_dwriteFactory->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            10.0f, L"en-us", m_smallFormat.ReleaseAndGetAddressOf());
        if (m_smallFormat)
        {
            m_smallFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_smallFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
            m_dwriteFactory->CreateEllipsisTrimmingSign(m_smallFormat.Get(), ellipsis.GetAddressOf());
            m_smallFormat->SetTrimming(&trim, ellipsis.Get());
            m_smallFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }

        m_dwriteFactory->CreateTextFormat(L"Segoe UI Variable Display", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            16.0f, L"en-us", m_glyphFormat.ReleaseAndGetAddressOf());
        if (m_glyphFormat)
        {
            m_glyphFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_glyphFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        if (FAILED(DCompositionCreateDevice(m_dxgiDevice.Get(), IID_PPV_ARGS(m_dcompDevice.ReleaseAndGetAddressOf()))))
            return false;
        if (FAILED(m_dcompDevice->CreateTargetForHwnd(m_hwnd, TRUE, m_dcompTarget.ReleaseAndGetAddressOf())))
            return false;
        if (FAILED(m_dcompDevice->CreateVisual(m_dcompVisual.ReleaseAndGetAddressOf())))
            return false;

        return EnsureSwapChainSize(680, 680);
    }

    void RingOverlay::ReleaseGraphicsDevices()
    {
        m_d2dBitmapCache.clear();
        m_targetBitmap.Reset();
        m_swapChain.Reset();
        m_dcompVisual.Reset();
        m_dcompTarget.Reset();
        m_dcompDevice.Reset();
        m_glyphFormat.Reset();
        m_smallFormat.Reset();
        m_titleFormat.Reset();
        m_dwriteFactory.Reset();
        m_d2dContext.Reset();
        m_d2dDevice.Reset();
        m_d2dFactory.Reset();
        m_dxgiDevice.Reset();
        m_d3dDevice.Reset();
        m_swapWidthPx = 0;
        m_swapHeightPx = 0;
    }

    bool RingOverlay::EnsureSwapChainSize(int widthPx, int heightPx)
    {
        if (!m_dxgiDevice || !m_d2dContext || !m_dcompDevice || !m_dcompVisual || !m_dcompTarget)
            return false;

        widthPx = (std::max)(1, widthPx);
        heightPx = (std::max)(1, heightPx);
        if (m_swapChain && m_swapWidthPx == widthPx && m_swapHeightPx == heightPx && m_targetBitmap)
            return true;

        m_d2dContext->SetTarget(nullptr);
        m_targetBitmap.Reset();

        if (!m_swapChain)
        {
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
            if (FAILED(m_dxgiDevice->GetAdapter(adapter.GetAddressOf())))
                return false;
            Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
            if (FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()))))
                return false;

            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = static_cast<UINT>(widthPx);
            desc.Height = static_cast<UINT>(heightPx);
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

            if (FAILED(factory->CreateSwapChainForComposition(m_dxgiDevice.Get(), &desc, nullptr,
                m_swapChain.ReleaseAndGetAddressOf())))
                return false;

            if (FAILED(m_dcompVisual->SetContent(m_swapChain.Get())))
                return false;
            if (FAILED(m_dcompTarget->SetRoot(m_dcompVisual.Get())))
                return false;
        }
        else
        {
            if (FAILED(m_swapChain->ResizeBuffers(2, static_cast<UINT>(widthPx), static_cast<UINT>(heightPx),
                DXGI_FORMAT_B8G8R8A8_UNORM, 0)))
                return false;
        }

        Microsoft::WRL::ComPtr<IDXGISurface> surface;
        if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(surface.GetAddressOf()))))
            return false;

        D2D1_BITMAP_PROPERTIES1 bp{};
        bp.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
        bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        bp.dpiX = 96.0f;
        bp.dpiY = 96.0f;

        if (FAILED(m_d2dContext->CreateBitmapFromDxgiSurface(surface.Get(), &bp,
            m_targetBitmap.ReleaseAndGetAddressOf())))
            return false;

        m_swapWidthPx = widthPx;
        m_swapHeightPx = heightPx;
        return true;
    }

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> RingOverlay::GetOrCreateD2DBitmap(IWICBitmapSource* wicSource)
    {
        if (!wicSource || !m_d2dContext)
            return nullptr;
        if (auto it = m_d2dBitmapCache.find(wicSource); it != m_d2dBitmapCache.end())
            return it->second;

        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bmp;
        if (FAILED(m_d2dContext->CreateBitmapFromWicBitmap(wicSource, nullptr, bmp.GetAddressOf())) || !bmp)
            return nullptr;

        if (m_d2dBitmapCache.size() > 256)
            m_d2dBitmapCache.clear();
        m_d2dBitmapCache.emplace(wicSource, bmp);
        return bmp;
    }

    void RingOverlay::UpdateDpiAndPosition(int physicalX, int physicalY)
    {
        POINT pt{ physicalX, physicalY };
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);

        UINT dpiX = 96, dpiY = 96;
        using GetDpiForMonitorFn = HRESULT (WINAPI*)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
        static auto pGetDpiForMonitor = reinterpret_cast<GetDpiForMonitorFn>(
            GetProcAddress(LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32), "GetDpiForMonitor"));
        if (pGetDpiForMonitor && SUCCEEDED(pGetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX > 0)
            m_dpiScale = static_cast<double>(dpiX) / 96.0;
        else
            m_dpiScale = 1.0;

        MONITORINFO mi{ sizeof(mi) };
        RectD workAreaDip{ 0.0, 0.0, 1920.0, 1080.0 };
        if (GetMonitorInfoW(mon, &mi))
        {
            workAreaDip = RectD{
                mi.rcWork.left / m_dpiScale,
                mi.rcWork.top / m_dpiScale,
                (mi.rcWork.right - mi.rcWork.left) / m_dpiScale,
                (mi.rcWork.bottom - mi.rcWork.top) / m_dpiScale
            };
        }

        const double cursorDipX = physicalX / m_dpiScale;
        const double cursorDipY = physicalY / m_dpiScale;
        const PointD clampedDip = ClampRingCenter(cursorDipX, cursorDipY, m_config.ringRadiusDip + 50.0, workAreaDip);

        const int physSize = static_cast<int>(std::llround(kOverlayDip * m_dpiScale));
        const int leftPx = static_cast<int>(std::llround((clampedDip.x - kCenterDip) * m_dpiScale));
        const int topPx = static_cast<int>(std::llround((clampedDip.y - kCenterDip) * m_dpiScale));

        m_centerScreenPhysical = { clampedDip.x * m_dpiScale, clampedDip.y * m_dpiScale };

        EnsureSwapChainSize(physSize, physSize);
        SetWindowPos(m_hwnd, HWND_TOPMOST, leftPx, topPx, physSize, physSize, SWP_NOACTIVATE);
    }

    bool RingOverlay::IsPointInActiveBounds(POINT screenPt) const noexcept
    {
        if (!m_isOpen || !m_hwnd)
            return false;
        const double dx = (screenPt.x - m_centerScreenPhysical.x) / m_dpiScale;
        const double dy = (screenPt.y - m_centerScreenPhysical.y) / m_dpiScale;
        const double dist = std::hypot(dx, dy);
        if (dist <= MainRingOuterRadius(m_config.ringRadiusDip, m_config.iconSizeDip) + 10.0)
            return true;
        if (!m_submenuCardRects.empty())
        {
            const PointD local{ kCenterDip + dx, kCenterDip + dy };
            for (const auto& cardRect : m_submenuCardRects)
            {
                const RectD expanded{
                    cardRect.left - 12.0, cardRect.top - 12.0,
                    cardRect.width + 24.0, cardRect.height + 24.0
                };
                if (expanded.Contains(local))
                    return true;
            }
        }
        return false;
    }

    RingOverlay::ThemeColors RingOverlay::BuildThemeColors() const noexcept
    {
        BYTE ar = 49, ag = 134, ab = 255;
        ParseHexColor(m_config.accentColor, ar, ag, ab);

        ThemeColors c{};
        c.accent = Argb(255, ar, ag, ab);
        c.accentColorRef = RGB(ar, ag, ab);
        c.textColor = Argb(255, 245, 247, 250);

        switch (m_config.theme)
        {
        case RingTheme::LiquidGlass:
            c.ringFill = Argb(188, 22, 26, 36);
            c.ringStroke = Argb(120, ar, ag, ab);
            c.ringStrokeWidth = 1.5f;
            c.innerFill = Argb(28, 255, 255, 255);
            c.slotIdleFill = Argb(45, 255, 255, 255);
            c.slotSelectedFill = Argb(175, ar, ag, ab);
            c.sectorFill = Argb(76, ar, ag, ab);
            c.sectorStroke = Argb(220, ar, ag, ab);
            c.centerFill = Argb(218, 20, 23, 30);
            c.centerBorder = Argb(95, 255, 255, 255);
            c.borderColorRef = RGB(82, 90, 108);
            break;
        case RingTheme::Glass:
            c.ringFill = Argb(218, 30, 34, 42);
            c.ringStroke = Argb(85, 255, 255, 255);
            c.ringStrokeWidth = 1.2f;
            c.innerFill = Argb(48, 0, 0, 0);
            c.slotIdleFill = Argb(36, 255, 255, 255);
            c.slotSelectedFill = Argb(150, ar, ag, ab);
            c.sectorFill = Argb(62, ar, ag, ab);
            c.sectorStroke = Argb(190, ar, ag, ab);
            c.centerFill = Argb(228, 26, 29, 36);
            c.centerBorder = Argb(75, 255, 255, 255);
            c.borderColorRef = RGB(74, 80, 94);
            break;
        case RingTheme::Kiwi:
            c.ringFill = Argb(245, 245, 247, 250);
            c.ringStroke = Argb(255, 210, 216, 224);
            c.ringStrokeWidth = 1.5f;
            c.innerFill = Argb(232, 235, 239, 245);
            c.slotIdleFill = Argb(255, 255, 255, 255);
            c.slotSelectedFill = Argb(225, ar, ag, ab);
            c.sectorFill = Argb(58, ar, ag, ab);
            c.sectorStroke = Argb(225, ar, ag, ab);
            c.centerFill = Argb(250, 255, 255, 255);
            c.centerBorder = Argb(255, 200, 206, 214);
            c.textColor = Argb(255, 24, 27, 34);
            c.borderColorRef = RGB(200, 206, 214);
            break;
        case RingTheme::Aura:
            c.useAuraGradient = true;
            c.ringFill = Argb(220, ar, ag, ab);
            c.auraOuter = Argb(215, 14, 17, 24);
            c.ringStroke = Argb(155, ar, ag, ab);
            c.ringStrokeWidth = 1.8f;
            c.innerFill = Argb(128, 14, 17, 24);
            c.slotIdleFill = Argb(58, 255, 255, 255);
            c.slotSelectedFill = Argb(220, ar, ag, ab);
            c.sectorFill = Argb(96, ar, ag, ab);
            c.sectorStroke = Argb(245, 255, 255, 255);
            c.centerFill = Argb(232, 14, 17, 24);
            c.centerBorder = Argb(170, ar, ag, ab);
            c.borderColorRef = RGB(ar, ag, ab);
            break;
        case RingTheme::Orbit:
            c.ringFill = Argb(150, 16, 18, 24);
            c.ringStroke = Argb(175, ar, ag, ab);
            c.ringStrokeWidth = 2.0f;
            c.innerFill = Argb(0, 0, 0, 0);
            c.slotIdleFill = Argb(195, 26, 30, 38);
            c.slotSelectedFill = Argb(238, ar, ag, ab);
            c.sectorFill = Argb(48, ar, ag, ab);
            c.sectorStroke = Argb(228, ar, ag, ab);
            c.centerFill = Argb(215, 20, 23, 30);
            c.centerBorder = Argb(175, ar, ag, ab);
            c.borderColorRef = RGB(ar, ag, ab);
            break;
        case RingTheme::Outline:
            c.ringFill = Argb(236, 10, 12, 16);
            c.ringStroke = Argb(255, 255, 255, 255);
            c.ringStrokeWidth = 2.2f;
            c.innerFill = Argb(0, 0, 0, 0);
            c.slotIdleFill = Argb(255, 20, 22, 28);
            c.slotSelectedFill = Argb(255, ar, ag, ab);
            c.sectorFill = Argb(68, ar, ag, ab);
            c.sectorStroke = Argb(255, 255, 255, 255);
            c.centerFill = Argb(255, 10, 12, 16);
            c.centerBorder = Argb(255, 255, 255, 255);
            c.borderColorRef = RGB(255, 255, 255);
            break;
        }
        return c;
    }

    void RingOverlay::OpenAt(int physicalX, int physicalY, std::vector<RingSlot> slots, bool lockOpen)
    {
        if (!m_hwnd)
            return;

        m_allSlots = std::move(slots);
        m_visibleSlots = m_allSlots;
        m_slotScales.assign(m_visibleSlots.size(), 1.0);
        m_filterQuery.clear();
        m_selectedIndex = -1;
        ClearSubmenu();
        m_corridorOriginDip = { kCenterDip, kCenterDip };
        m_hasLastPointer = false;
        m_lastPointerPhysical = {};
        m_isLockedOpen = lockOpen;
        m_contextMenuOpen = false;
        m_dragActive = false;
        m_dragHoverBadge = 0;
        m_isOpen = true;

        UpdateDpiAndPosition(physicalX, physicalY);

        const bool staticMode = m_config.disableOpeningAnimation || !IsClientAreaAnimationEnabled();
        if (staticMode)
        {
            m_openProgress = 1.0;
        }
        else
        {
            m_openProgress = 0.0;
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            m_openStartQpc = now.QuadPart;
            if (!m_animTimerRunning)
            {
                SetTimer(m_hwnd, kAnimTimerId, kAnimIntervalMs, nullptr);
                m_animTimerRunning = true;
            }
        }

        Render();
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    }

    void RingOverlay::Dismiss()
    {
        if (!m_isOpen && !m_animTimerRunning)
            return;
        m_isOpen = false;
        m_isLockedOpen = false;
        m_contextMenuOpen = false;
        m_dragActive = false;
        m_selectedIndex = -1;
        m_filterQuery.clear();

        if (m_animTimerRunning && m_hwnd)
        {
            KillTimer(m_hwnd, kAnimTimerId);
            m_animTimerRunning = false;
        }
        ClearSubmenu();
        if (m_hwnd && IsWindow(m_hwnd))
            ShowWindow(m_hwnd, SW_HIDE);
        if (m_callbacks.onOverlayClosed)
            m_callbacks.onOverlayClosed();
    }

    bool RingOverlay::SlotHasSubmenuItems(const RingSlot& slot) const noexcept
    {
        const size_t minWin = m_config.fanOutSingleWindow ? 1 : 2;
        return slot.windows.size() >= minWin || !slot.subItems.empty();
    }

    void RingOverlay::ClearSubmenu()
    {
        m_thumbPool.HideAll();
        m_submenuCards.clear();
        m_submenuCardRects.clear();
        m_expandedSlotIndex = -1;
        m_selectedSubIndex = -1;
        m_submenuBoundsDip = {};
        m_submenuIsTitleColumn = false;
    }

    void RingOverlay::OpenSubmenuForSlot(int slotIndex, PointD originDip)
    {
        ClearSubmenu();
        if (slotIndex < 0 || slotIndex >= static_cast<int>(m_visibleSlots.size()))
            return;

        const RingSlot& slot = m_visibleSlots[static_cast<size_t>(slotIndex)];
        if (!SlotHasSubmenuItems(slot))
            return;

        m_expandedSlotIndex = slotIndex;
        const int slotCount = static_cast<int>(m_visibleSlots.size());
        const double originDist = std::hypot(originDip.x - kCenterDip, originDip.y - kCenterDip);
        const double mainRingOuterR = MainRingOuterRadius(m_config.ringRadiusDip, m_config.iconSizeDip);
        m_corridorOriginDip = (originDist <= mainRingOuterR)
            ? originDip
            : GetSlotCenter(kCenterDip, kCenterDip, m_config.ringRadiusDip, slotIndex, slotCount);
        const double baseAngle = GetSlotAngle(slotIndex, slotCount);
        const size_t minWin = m_config.fanOutSingleWindow ? 1 : 2;

        if (slot.windows.size() >= minWin)
        {
            const int count = (std::min)(static_cast<int>(slot.windows.size()), 9);
            if (m_config.windowViewMode == WindowViewMode::TitleColumn)
            {
                m_submenuIsTitleColumn = true;
                m_submenuCardRects = ComputeTitleColumnRects(
                    kCenterDip, kCenterDip, kOverlayDip, baseAngle,
                    m_config.ringRadiusDip, m_config.iconSizeDip, count);
            }
            else
            {
                m_submenuIsTitleColumn = false;
                m_submenuCardRects = ComputeWindowOuterArcRects(
                    kCenterDip, kCenterDip, baseAngle,
                    m_config.ringRadiusDip, m_config.iconSizeDip, m_config.thumbnailScale, count);
            }

            m_submenuCards.reserve(m_submenuCardRects.size());
            for (size_t i = 0; i < m_submenuCardRects.size(); ++i)
            {
                m_submenuCards.push_back(SubmenuCard{
                    m_submenuCardRects[i], &slot.windows[i], nullptr, 1.0, 1.0
                });
            }
        }
        else if (!slot.subItems.empty())
        {
            const int count = (std::min)(static_cast<int>(slot.subItems.size()), 9);
            m_submenuIsTitleColumn = false;
            m_submenuCardRects = ComputeFolderSubmenuRects(
                kCenterDip, kCenterDip, baseAngle,
                m_config.ringRadiusDip, m_config.iconSizeDip, count);

            m_submenuCards.reserve(m_submenuCardRects.size());
            for (size_t i = 0; i < m_submenuCardRects.size(); ++i)
            {
                m_submenuCards.push_back(SubmenuCard{
                    m_submenuCardRects[i], nullptr, &slot.subItems[i], 1.0, 1.0
                });
            }
        }

        if (m_submenuCards.empty())
        {
            ClearSubmenu();
            return;
        }

        m_submenuBoundsDip = ComputeUnionBounds(m_submenuCardRects);
        SyncDwmThumbnails();
    }

    void RingOverlay::SyncDwmThumbnails()
    {
        if (!m_isOpen || m_expandedSlotIndex < 0 ||
            m_config.windowViewMode != WindowViewMode::OuterArc)
        {
            m_thumbPool.HideAll();
            return;
        }

        const ThemeColors tc = BuildThemeColors();
        const int overlayLeftPx = static_cast<int>(std::llround(m_centerScreenPhysical.x - kCenterDip * m_dpiScale));
        const int overlayTopPx = static_cast<int>(std::llround(m_centerScreenPhysical.y - kCenterDip * m_dpiScale));

        for (size_t i = 0; i < DwmThumbPool::kPoolSize; ++i)
        {
            if (i >= m_submenuCards.size() || !m_submenuCards[i].window || m_submenuCards[i].window->isMinimized)
            {
                m_thumbPool.HideCard(i);
                continue;
            }
            const auto& card = m_submenuCards[i];
            const bool selected = (static_cast<int>(i) == m_selectedSubIndex);
            const double scale = card.currentScale;
            const double cx = card.boundsDip.left + card.boundsDip.width * 0.5;
            const double cy = card.boundsDip.top + card.boundsDip.height * 0.5;
            const double wDip = (card.boundsDip.width - 12.0) * scale;
            const double hDip = (card.boundsDip.height - 24.0) * scale;
            const double leftDip = cx - wDip * 0.5;
            const double topDip = cy - (card.boundsDip.height * 0.5 - 6.0) * scale;

            const int pxLeft = overlayLeftPx + static_cast<int>(std::llround(leftDip * m_dpiScale));
            const int pxTop = overlayTopPx + static_cast<int>(std::llround(topDip * m_dpiScale));
            const int pxW = static_cast<int>(std::llround(wDip * m_dpiScale));
            const int pxH = static_cast<int>(std::llround(hDip * m_dpiScale));

            m_thumbPool.ShowCard(i, card.window->hwnd, pxLeft, pxTop, pxW, pxH,
                selected ? tc.accentColorRef : tc.borderColorRef, 248);
        }
    }

    int RingOverlay::HitTestSubmenu(PointD localDip) const noexcept
    {
        return HitTestRects(localDip, m_submenuCardRects);
    }

    void RingOverlay::UpdatePointer(int physicalX, int physicalY)
    {
        if (!m_isOpen || m_visibleSlots.empty() || m_contextMenuOpen || m_dragActive)
            return;

        m_hasLastPointer = true;
        m_lastPointerPhysical = POINT{ physicalX, physicalY };

        const double dx = (physicalX - m_centerScreenPhysical.x) / m_dpiScale;
        const double dy = (physicalY - m_centerScreenPhysical.y) / m_dpiScale;
        const PointD localPt{ kCenterDip + dx, kCenterDip + dy };
        const PointD centerPt{ kCenterDip, kCenterDip };
        const int slotCount = static_cast<int>(m_visibleSlots.size());

        const RingHoverResult next = ResolveRingHover(
            localPt,
            centerPt,
            slotCount,
            m_config.deadzoneDip,
            m_config.ringRadiusDip,
            m_config.iconSizeDip,
            m_selectedIndex,
            m_expandedSlotIndex,
            m_corridorOriginDip,
            m_submenuCardRects,
            m_submenuBoundsDip,
            m_submenuIsTitleColumn);

        const bool staticAnim = m_config.disableOpeningAnimation || !IsClientAreaAnimationEnabled();
        auto ensureAnimRunning = [&]() {
            if (!staticAnim && !m_animTimerRunning && m_hwnd)
            {
                SetTimer(m_hwnd, kAnimTimerId, kAnimIntervalMs, nullptr);
                m_animTimerRunning = true;
            }
        };

        bool needsRender = false;
        bool needsThumbSync = false;

        if (next.selectedSlot != m_selectedIndex)
        {
            m_selectedIndex = next.selectedSlot;
            m_selectedSubIndex = -1;

            if (m_selectedIndex >= 0 && SlotHasSubmenuItems(m_visibleSlots[static_cast<size_t>(m_selectedIndex)]))
                OpenSubmenuForSlot(m_selectedIndex, localPt);
            else
                ClearSubmenu();

            if (staticAnim)
            {
                for (size_t i = 0; i < m_slotScales.size(); ++i)
                    m_slotScales[i] = (static_cast<int>(i) == m_selectedIndex) ? 1.24 : 1.0;
            }
            else
            {
                ensureAnimRunning();
            }
            needsRender = true;
        }
        else
        {
            if (m_selectedIndex >= 0)
            {
                if (!next.keepExistingSubmenu && m_expandedSlotIndex >= 0)
                {
                    ClearSubmenu();
                    needsRender = true;
                }
                else if (m_expandedSlotIndex != m_selectedIndex &&
                    SlotHasSubmenuItems(m_visibleSlots[static_cast<size_t>(m_selectedIndex)]))
                {
                    OpenSubmenuForSlot(m_selectedIndex, localPt);
                    needsRender = true;
                }
            }
            else if (m_expandedSlotIndex >= 0)
            {
                ClearSubmenu();
                needsRender = true;
            }

            if (next.updateCorridorOrigin)
                m_corridorOriginDip = localPt;

            if (next.selectedSubIndex != m_selectedSubIndex)
            {
                m_selectedSubIndex = next.selectedSubIndex;
                for (size_t i = 0; i < m_submenuCards.size(); ++i)
                    m_submenuCards[i].targetScale = (static_cast<int>(i) == m_selectedSubIndex) ? 1.12 : 1.0;
                if (staticAnim)
                {
                    for (auto& card : m_submenuCards)
                        card.currentScale = card.targetScale;
                }
                else
                {
                    ensureAnimRunning();
                }
                needsThumbSync = true;
                needsRender = true;
            }
        }

        if (needsThumbSync)
            SyncDwmThumbnails();
        if (needsRender)
            Render();
    }

    void RingOverlay::HandleScrollWheel(int wheelDelta)
    {
        if (!m_isOpen || m_visibleSlots.empty() || m_contextMenuOpen ||
            m_config.scrollWheelMode == ScrollWheelBehavior::Disabled)
            return;

        const int slotCount = static_cast<int>(m_visibleSlots.size());

        if (m_expandedSlotIndex >= 0 && !m_submenuCards.empty())
        {
            m_selectedSubIndex = StepIndex(m_selectedSubIndex, static_cast<int>(m_submenuCards.size()), wheelDelta);
            for (size_t i = 0; i < m_submenuCards.size(); ++i)
                m_submenuCards[i].currentScale = m_submenuCards[i].targetScale =
                    (static_cast<int>(i) == m_selectedSubIndex) ? 1.12 : 1.0;
            SyncDwmThumbnails();
            Render();
            return;
        }

        if (m_config.scrollWheelMode == ScrollWheelBehavior::CycleSubmenuOnly)
        {
            if (m_selectedIndex >= 0 && m_selectedIndex < slotCount &&
                SlotHasSubmenuItems(m_visibleSlots[static_cast<size_t>(m_selectedIndex)]))
            {
                const PointD slotOrigin = GetSlotCenter(kCenterDip, kCenterDip, m_config.ringRadiusDip, m_selectedIndex, slotCount);
                OpenSubmenuForSlot(m_selectedIndex, slotOrigin);
                if (!m_submenuCards.empty())
                {
                    m_selectedSubIndex = 0;
                    m_submenuCards[0].currentScale = m_submenuCards[0].targetScale = 1.12;
                    SyncDwmThumbnails();
                }
                Render();
            }
            return;
        }

        m_selectedIndex = StepIndex(m_selectedIndex, slotCount, wheelDelta);
        m_selectedSubIndex = -1;
        for (size_t i = 0; i < m_slotScales.size(); ++i)
            m_slotScales[i] = (static_cast<int>(i) == m_selectedIndex) ? 1.24 : 1.0;

        if (m_selectedIndex >= 0 && SlotHasSubmenuItems(m_visibleSlots[static_cast<size_t>(m_selectedIndex)]))
        {
            const PointD slotOrigin = GetSlotCenter(kCenterDip, kCenterDip, m_config.ringRadiusDip, m_selectedIndex, slotCount);
            OpenSubmenuForSlot(m_selectedIndex, slotOrigin);
        }
        else
            ClearSubmenu();

        Render();
    }

    void RingOverlay::CommitSelectionAndClose()
    {
        const bool shiftHeld = m_config.shiftCenterOnActiveMonitor &&
            ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);

        RingSlot chosenSlotCopy;
        WindowEntry chosenWindowCopy;
        bool hasSlot = false;
        bool hasWindow = false;

        if (m_expandedSlotIndex >= 0 && m_selectedSubIndex >= 0 &&
            m_selectedSubIndex < static_cast<int>(m_submenuCards.size()))
        {
            const auto& card = m_submenuCards[static_cast<size_t>(m_selectedSubIndex)];
            if (card.subSlot)
            {
                chosenSlotCopy = *card.subSlot;
                hasSlot = true;
            }
            else if (card.window)
            {
                chosenSlotCopy = m_visibleSlots[static_cast<size_t>(m_expandedSlotIndex)];
                chosenWindowCopy = *card.window;
                hasSlot = true;
                hasWindow = true;
            }
        }
        else if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_visibleSlots.size()))
        {
            chosenSlotCopy = m_visibleSlots[static_cast<size_t>(m_selectedIndex)];
            hasSlot = true;
            if (!chosenSlotCopy.windows.empty())
            {
                chosenWindowCopy = chosenSlotCopy.windows.front();
                hasWindow = true;
            }
        }

        Dismiss();
        if (hasSlot)
            ActivateSlotOrWindow(&chosenSlotCopy, hasWindow ? &chosenWindowCopy : nullptr, shiftHeld);
    }

    void RingOverlay::ActivateSlotOrWindow(const RingSlot* slot, const WindowEntry* win, bool shiftHeld)
    {
        if (win && win->hwnd && m_tracker)
        {
            m_tracker->ActivateWindow(win->hwnd, shiftHeld, m_config.pointerFollow);
            return;
        }
        if (!slot)
            return;
        if (slot->kind == SlotKind::RecycleBin)
        {
            SlotModel::LaunchPathOrUrl(L"shell:RecycleBinFolder");
            return;
        }
        if (!slot->pathOrUrl.empty())
            SlotModel::LaunchPathOrUrl(slot->pathOrUrl);
    }

    void RingOverlay::AppendFilterChar(wchar_t ch)
    {
        if (!m_isOpen)
            return;
        m_filterQuery.push_back(ch);
        RebuildLayoutAfterFilter();
    }

    void RingOverlay::BackspaceFilter()
    {
        if (!m_isOpen || m_filterQuery.empty())
            return;
        m_filterQuery.pop_back();
        RebuildLayoutAfterFilter();
    }

    void RingOverlay::RebuildLayoutAfterFilter()
    {
        ClearSubmenu();
        m_visibleSlots.clear();
        for (const auto& slot : m_allSlots)
        {
            bool match = ContainsInsensitive(slot.label, m_filterQuery);
            if (!match)
            {
                for (const auto& w : slot.windows)
                {
                    if (ContainsInsensitive(w.title, m_filterQuery))
                    {
                        match = true;
                        break;
                    }
                }
            }
            if (match)
                m_visibleSlots.push_back(slot);
        }
        m_slotScales.assign(m_visibleSlots.size(), 1.0);
        m_selectedIndex = (m_visibleSlots.size() == 1) ? 0 : -1;
        if (m_selectedIndex == 0)
            m_slotScales[0] = 1.24;
        Render();
    }

    void RingOverlay::StepAnimations()
    {
        if (!m_isOpen)
            return;

        bool anyActive = false;
        if (m_openProgress < 1.0)
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const double elapsedMs = static_cast<double>(now.QuadPart - m_openStartQpc) * 1000.0 /
                static_cast<double>(m_qpcFreq);
            m_openProgress = (std::min)(1.0, elapsedMs / kOpenDurationMs);
            if (m_openProgress < 1.0)
                anyActive = true;
        }

        for (size_t i = 0; i < m_slotScales.size(); ++i)
        {
            const double target = (static_cast<int>(i) == m_selectedIndex) ? 1.24 : 1.0;
            const double diff = target - m_slotScales[i];
            if (std::abs(diff) > 0.005)
            {
                m_slotScales[i] += diff * 0.35;
                anyActive = true;
            }
            else
            {
                m_slotScales[i] = target;
            }
        }

        bool subChanged = false;
        for (auto& card : m_submenuCards)
        {
            const double diff = card.targetScale - card.currentScale;
            if (std::abs(diff) > 0.005)
            {
                card.currentScale += diff * 0.35;
                anyActive = true;
                subChanged = true;
            }
            else
            {
                card.currentScale = card.targetScale;
            }
        }

        if (subChanged)
            SyncDwmThumbnails();
        Render();

        if (!anyActive && m_animTimerRunning)
        {
            KillTimer(m_hwnd, kAnimTimerId);
            m_animTimerRunning = false;
        }
    }

    std::wstring RingOverlay::BuildFilterSummary() const
    {
        if (m_selectedSubIndex >= 0 && m_selectedSubIndex < static_cast<int>(m_submenuCards.size()))
        {
            const auto& card = m_submenuCards[static_cast<size_t>(m_selectedSubIndex)];
            if (card.window) return card.window->title;
            if (card.subSlot) return card.subSlot->label;
        }
        if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_visibleSlots.size()))
            return m_visibleSlots[static_cast<size_t>(m_selectedIndex)].label;
        if (m_visibleSlots.empty())
            return L"No matches";
        return std::to_wstring(m_visibleSlots.size()) + L" items";
    }

    void RingOverlay::Render()
    {
        if (!m_d2dContext || !m_targetBitmap || !m_swapChain || !m_dcompDevice)
            return;

        const ThemeColors tc = BuildThemeColors();
        const float dpiScale = static_cast<float>(m_dpiScale);
        const double easedOpen = EaseOutCubic(m_openProgress);
        const float openScale = static_cast<float>(0.86 + 0.14 * easedOpen);

        m_d2dContext->SetTarget(m_targetBitmap.Get());
        m_d2dContext->BeginDraw();
        m_d2dContext->Clear(D2D1::ColorF(0, 0, 0, 0));

        D2D1::Matrix3x2F transform = D2D1::Matrix3x2F::Scale(dpiScale, dpiScale);
        if (openScale < 0.999f)
        {
            transform = D2D1::Matrix3x2F::Scale(openScale, openScale,
                D2D1::Point2F(static_cast<float>(kCenterDip), static_cast<float>(kCenterDip))) * transform;
        }
        m_d2dContext->SetTransform(transform);

        auto mkBrush = [&](D2D1_COLOR_F col) {
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> b;
            m_d2dContext->CreateSolidColorBrush(col, b.GetAddressOf());
            return b;
        };

        const float ringR = static_cast<float>(m_config.ringRadiusDip);
        const float outerR = ringR + 38.0f;
        const float innerR = (std::max)(static_cast<float>(m_config.deadzoneDip + 8.0), ringR - 36.0f);
        const D2D1_POINT_2F centerPt = D2D1::Point2F(static_cast<float>(kCenterDip), static_cast<float>(kCenterDip));

        // 1. Ring backdrop
        if (tc.useAuraGradient)
        {
            D2D1_GRADIENT_STOP stops[2] = {
                { 0.0f, tc.ringFill },
                { 1.0f, tc.auraOuter }
            };
            Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> sc;
            if (SUCCEEDED(m_d2dContext->CreateGradientStopCollection(stops, 2, sc.GetAddressOf())))
            {
                Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> rgb;
                D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES props = D2D1::RadialGradientBrushProperties(centerPt, D2D1::Point2F(0, 0), outerR, outerR);
                if (SUCCEEDED(m_d2dContext->CreateRadialGradientBrush(props, sc.Get(), rgb.GetAddressOf())))
                    m_d2dContext->FillEllipse(D2D1::Ellipse(centerPt, outerR, outerR), rgb.Get());
            }
        }
        else if (auto b = mkBrush(tc.ringFill))
        {
            m_d2dContext->FillEllipse(D2D1::Ellipse(centerPt, outerR, outerR), b.Get());
        }
        if (auto b = mkBrush(tc.ringStroke))
            m_d2dContext->DrawEllipse(D2D1::Ellipse(centerPt, outerR, outerR), b.Get(), tc.ringStrokeWidth);

        if (tc.innerFill.a > 0.001f)
        {
            if (auto b = mkBrush(tc.innerFill))
                m_d2dContext->FillEllipse(D2D1::Ellipse(centerPt, innerR, innerR), b.Get());
        }
        if (auto b = mkBrush(tc.centerBorder))
            m_d2dContext->DrawEllipse(D2D1::Ellipse(centerPt, innerR, innerR), b.Get(), 1.0f);

        // 2. Active 18 DIP cone wedge
        if (m_selectedIndex >= 0 && !m_visibleSlots.empty())
        {
            const float coneInnerR = static_cast<float>(m_config.deadzoneDip + 6.0);
            const size_t count = m_visibleSlots.size();
            Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
            if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(path.GetAddressOf())) && path)
            {
                Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(path->Open(sink.GetAddressOf())) && sink)
                {
                    if (count == 1)
                    {
                        sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
                        sink->BeginFigure(D2D1::Point2F(centerPt.x + outerR, centerPt.y), D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(centerPt.x - outerR, centerPt.y), D2D1::SizeF(outerR, outerR), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
                        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(centerPt.x + outerR, centerPt.y), D2D1::SizeF(outerR, outerR), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);

                        sink->BeginFigure(D2D1::Point2F(centerPt.x + coneInnerR, centerPt.y), D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(centerPt.x - coneInnerR, centerPt.y), D2D1::SizeF(coneInnerR, coneInnerR), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
                        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(centerPt.x + coneInnerR, centerPt.y), D2D1::SizeF(coneInnerR, coneInnerR), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    }
                    else
                    {
                        const double sectorW = kTwoPi / static_cast<double>(count);
                        const double centerA = -kPi * 0.5 + static_cast<double>(m_selectedIndex) * sectorW;
                        const double startA = centerA - sectorW * 0.5;
                        const double endA = centerA + sectorW * 0.5;
                        const D2D1_ARC_SIZE arcSize = (sectorW > kPi) ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;

                        const D2D1_POINT_2F p1{ centerPt.x + outerR * static_cast<float>(std::cos(startA)), centerPt.y + outerR * static_cast<float>(std::sin(startA)) };
                        const D2D1_POINT_2F p2{ centerPt.x + outerR * static_cast<float>(std::cos(endA)), centerPt.y + outerR * static_cast<float>(std::sin(endA)) };
                        const D2D1_POINT_2F p3{ centerPt.x + coneInnerR * static_cast<float>(std::cos(endA)), centerPt.y + coneInnerR * static_cast<float>(std::sin(endA)) };
                        const D2D1_POINT_2F p4{ centerPt.x + coneInnerR * static_cast<float>(std::cos(startA)), centerPt.y + coneInnerR * static_cast<float>(std::sin(startA)) };

                        sink->BeginFigure(p1, D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddArc(D2D1::ArcSegment(p2, D2D1::SizeF(outerR, outerR), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, arcSize));
                        sink->AddLine(p3);
                        sink->AddArc(D2D1::ArcSegment(p4, D2D1::SizeF(coneInnerR, coneInnerR), 0.0f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, arcSize));
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    }
                    sink->Close();
                }
                if (auto b = mkBrush(tc.sectorFill))
                    m_d2dContext->FillGeometry(path.Get(), b.Get());
                if (auto b = mkBrush(tc.sectorStroke))
                    m_d2dContext->DrawGeometry(path.Get(), b.Get(), 1.5f);
            }
        }

        // 3. Slot icons
        const int slotCount = static_cast<int>(m_visibleSlots.size());
        const double baseBadge = m_config.iconSizeDip + 14.0;
        for (int pass = 0; pass < 2; ++pass)
        {
            for (int i = 0; i < slotCount; ++i)
            {
                const bool selected = (i == m_selectedIndex);
                if ((pass == 0 && selected) || (pass == 1 && !selected))
                    continue;

                const RingSlot& slot = m_visibleSlots[static_cast<size_t>(i)];
                const PointD pos = GetSlotCenter(kCenterDip, kCenterDip, m_config.ringRadiusDip, i, slotCount);
                const float scale = static_cast<float>(i < static_cast<int>(m_slotScales.size()) ? m_slotScales[static_cast<size_t>(i)] : 1.0);
                const float radius = static_cast<float>(baseBadge * 0.5) * scale;
                const D2D1_POINT_2F sc{ static_cast<float>(pos.x), static_cast<float>(pos.y) };

                if (selected)
                {
                    D2D1_COLOR_F g0 = tc.accent; g0.a = 0.55f;
                    D2D1_COLOR_F g1 = tc.accent; g1.a = 0.0f;
                    D2D1_GRADIENT_STOP stops[2] = { { 0.0f, g0 }, { 1.0f, g1 } };
                    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> gsc;
                    if (SUCCEEDED(m_d2dContext->CreateGradientStopCollection(stops, 2, gsc.GetAddressOf())))
                    {
                        Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> glow;
                        const float gr = radius * 1.65f;
                        if (SUCCEEDED(m_d2dContext->CreateRadialGradientBrush(
                            D2D1::RadialGradientBrushProperties(sc, D2D1::Point2F(0, 0), gr, gr), gsc.Get(), glow.GetAddressOf())))
                        {
                            m_d2dContext->FillEllipse(D2D1::Ellipse(sc, gr, gr), glow.Get());
                        }
                    }
                }

                D2D1_COLOR_F fillCol = selected ? tc.slotSelectedFill : tc.slotIdleFill;
                const float slotAlpha = (slot.kind == SlotKind::App && !slot.IsRunning()) ? 0.65f : 1.0f;
                fillCol.a *= slotAlpha;
                if (auto b = mkBrush(fillCol))
                    m_d2dContext->FillEllipse(D2D1::Ellipse(sc, radius, radius), b.Get());
                if (selected)
                {
                    if (auto b = mkBrush(tc.sectorStroke))
                        m_d2dContext->DrawEllipse(D2D1::Ellipse(sc, radius, radius), b.Get(), 1.6f);
                }

                if (slot.iconBitmap)
                {
                    if (auto bmp = GetOrCreateD2DBitmap(slot.iconBitmap.Get()))
                    {
                        const float iconHalf = static_cast<float>(m_config.iconSizeDip * 0.39) * scale;
                        const D2D1_RECT_F dst = D2D1::RectF(sc.x - iconHalf, sc.y - iconHalf, sc.x + iconHalf, sc.y + iconHalf);
                        m_d2dContext->DrawBitmap(bmp.Get(), dst, slotAlpha);
                    }
                }
                else if (m_glyphFormat)
                {
                    std::wstring glyph = L"?";
                    if (slot.kind == SlotKind::RecycleBin) glyph = L"RB";
                    else if (slot.kind == SlotKind::Collection) glyph = L"COL";
                    else if (slot.kind == SlotKind::Website) glyph = L"WEB";
                    else if (!slot.label.empty()) glyph = std::wstring(1, static_cast<wchar_t>(towupper(slot.label[0])));

                    if (auto b = mkBrush(tc.textColor))
                    {
                        const D2D1_RECT_F tr = D2D1::RectF(sc.x - radius, sc.y - radius, sc.x + radius, sc.y + radius);
                        m_d2dContext->DrawTextW(glyph.c_str(), static_cast<UINT32>(glyph.size()), m_glyphFormat.Get(), tr, b.Get());
                    }
                }

                if (slot.windows.size() > 1 && m_smallFormat)
                {
                    const D2D1_POINT_2F bc{ sc.x + radius * 0.68f, sc.y - radius * 0.68f };
                    if (auto b = mkBrush(tc.sectorStroke))
                        m_d2dContext->FillEllipse(D2D1::Ellipse(bc, 9.0f, 9.0f), b.Get());
                    const std::wstring countTxt = slot.windows.size() > 9 ? L"9+" : std::to_wstring(slot.windows.size());
                    if (auto wb = mkBrush(Argb(255, 255, 255, 255)))
                    {
                        m_d2dContext->DrawTextW(countTxt.c_str(), static_cast<UINT32>(countTxt.size()),
                            m_smallFormat.Get(), D2D1::RectF(bc.x - 10, bc.y - 9, bc.x + 10, bc.y + 9), wb.Get());
                    }
                }
            }
        }

        // 4. Submenu cards (outer arc frames, title column rows, folder/collection badges)
        if (m_expandedSlotIndex >= 0)
        {
            for (size_t i = 0; i < m_submenuCards.size(); ++i)
            {
                const auto& card = m_submenuCards[i];
                const bool sel = (static_cast<int>(i) == m_selectedSubIndex);
                const float s = static_cast<float>(card.currentScale);
                const float cx = static_cast<float>(card.boundsDip.left + card.boundsDip.width * 0.5);
                const float cy = static_cast<float>(card.boundsDip.top + card.boundsDip.height * 0.5);
                const float hw = static_cast<float>(card.boundsDip.width * 0.5) * s;
                const float hh = static_cast<float>(card.boundsDip.height * 0.5) * s;
                const D2D1_RECT_F rect = D2D1::RectF(cx - hw, cy - hh, cx + hw, cy + hh);

                if (card.subSlot)
                {
                    const D2D1_POINT_2F cc{ cx, cy };
                    D2D1_COLOR_F fill = sel ? tc.slotSelectedFill : tc.centerFill;
                    if (auto b = mkBrush(fill))
                        m_d2dContext->FillEllipse(D2D1::Ellipse(cc, hw, hh), b.Get());
                    if (auto b = mkBrush(sel ? tc.sectorStroke : tc.centerBorder))
                        m_d2dContext->DrawEllipse(D2D1::Ellipse(cc, hw, hh), b.Get(), sel ? 2.0f : 1.2f);
                    if (card.subSlot->iconBitmap)
                    {
                        if (auto bmp = GetOrCreateD2DBitmap(card.subSlot->iconBitmap.Get()))
                        {
                            const float ih = 14.0f * s;
                            m_d2dContext->DrawBitmap(bmp.Get(), D2D1::RectF(cx - ih, cy - ih, cx + ih, cy + ih));
                        }
                    }
                    else if (m_titleFormat && !card.subSlot->label.empty())
                    {
                        const std::wstring ch(1, static_cast<wchar_t>(towupper(card.subSlot->label[0])));
                        if (auto b = mkBrush(tc.textColor))
                            m_d2dContext->DrawTextW(ch.c_str(), 1, m_titleFormat.Get(), rect, b.Get());
                    }
                }
                else if (card.window)
                {
                    D2D1_COLOR_F fill = tc.centerFill;
                    if (card.window->isMinimized)
                        fill.a *= 0.65f;
                    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect, 9.0f, 9.0f);
                    if (auto b = mkBrush(fill))
                        m_d2dContext->FillRoundedRectangle(rr, b.Get());
                    if (auto b = mkBrush(sel ? tc.sectorStroke : tc.centerBorder))
                        m_d2dContext->DrawRoundedRectangle(rr, b.Get(), sel ? 2.2f : 1.2f);

                    if (m_smallFormat)
                    {
                        D2D1_RECT_F textRect = (m_config.windowViewMode == WindowViewMode::TitleColumn)
                            ? D2D1::RectF(rect.left + 8, rect.top, rect.right - 8, rect.bottom)
                            : D2D1::RectF(rect.left + 6, rect.bottom - 20.0f, rect.right - 6, rect.bottom - 2.0f);
                        const std::wstring txt = (card.window->isMinimized && m_config.windowViewMode == WindowViewMode::TitleColumn)
                            ? (card.window->title + L" (minimized)")
                            : card.window->title;
                        D2D1_COLOR_F txtCol = tc.textColor;
                        if (card.window->isMinimized) txtCol.a *= 0.72f;
                        if (auto b = mkBrush(txtCol))
                            m_d2dContext->DrawTextW(txt.c_str(), static_cast<UINT32>(txt.size()), m_smallFormat.Get(), textRect, b.Get());
                    }
                }
            }
        }

        // 5. Drag-and-drop Convert / Actions bar
        if (m_dragActive)
        {
            auto drawDropPill = [&](const RectD& r, const wchar_t* label, bool hot) {
                const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
                    D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                        static_cast<float>(r.Right()), static_cast<float>(r.Bottom())), 21.0f, 21.0f);
                D2D1_COLOR_F fill = hot ? tc.slotSelectedFill : Argb(235, 30, 36, 48);
                if (auto b = mkBrush(fill)) m_d2dContext->FillRoundedRectangle(rr, b.Get());
                if (auto b = mkBrush(tc.accent)) m_d2dContext->DrawRoundedRectangle(rr, b.Get(), hot ? 2.2f : 1.5f);
                if (m_titleFormat)
                {
                    if (auto wb = mkBrush(Argb(255, 245, 247, 250)))
                        m_d2dContext->DrawTextW(label, static_cast<UINT32>(wcslen(label)), m_titleFormat.Get(), rr.rect, wb.Get());
                }
            };
            drawDropPill(kConvertBadgeDip, L"Convert", m_dragHoverBadge == 1);
            drawDropPill(kActionsBadgeDip, L"Actions", m_dragHoverBadge == 2);
        }

        // 6. Center pill badge
        {
            const float cw = 122.0f;
            const float ch = 46.0f;
            const D2D1_ROUNDED_RECT centerPill = D2D1::RoundedRect(
                D2D1::RectF(centerPt.x - cw * 0.5f, centerPt.y - ch * 0.5f,
                            centerPt.x + cw * 0.5f, centerPt.y + ch * 0.5f),
                ch * 0.5f, ch * 0.5f);

            if (auto b = mkBrush(tc.centerFill))
                m_d2dContext->FillRoundedRectangle(centerPill, b.Get());
            if (auto b = mkBrush(tc.centerBorder))
                m_d2dContext->DrawRoundedRectangle(centerPill, b.Get(), 1.1f);

            const std::wstring summary = BuildFilterSummary();
            if (m_titleFormat)
            {
                D2D1_RECT_F tr = m_filterQuery.empty()
                    ? D2D1::RectF(centerPill.rect.left + 8, centerPill.rect.top + 2, centerPill.rect.right - 8, centerPill.rect.bottom - 2)
                    : D2D1::RectF(centerPill.rect.left + 8, centerPill.rect.top + 4, centerPill.rect.right - 8, centerPt.y + 2);
                if (auto b = mkBrush(tc.textColor))
                    m_d2dContext->DrawTextW(summary.c_str(), static_cast<UINT32>(summary.size()), m_titleFormat.Get(), tr, b.Get());
            }
            if (!m_filterQuery.empty() && m_smallFormat)
            {
                const D2D1_RECT_F qr = D2D1::RectF(centerPill.rect.left + 8, centerPt.y, centerPill.rect.right - 8, centerPill.rect.bottom - 4);
                if (auto b = mkBrush(tc.accent))
                    m_d2dContext->DrawTextW(m_filterQuery.c_str(), static_cast<UINT32>(m_filterQuery.size()), m_smallFormat.Get(), qr, b.Get());
            }
        }

        const HRESULT endHr = m_d2dContext->EndDraw();
        if (endHr == D2DERR_RECREATE_TARGET)
        {
            ReleaseGraphicsDevices();
            CreateGraphicsDevices();
            return;
        }
        m_swapChain->Present(0, 0);
        m_dcompDevice->Commit();
    }

    void RingOverlay::ShowContextMenuAt(POINT screenPt)
    {
        if (!m_hwnd)
            return;
        ClearSubmenu();
        m_contextMenuOpen = true;

        HMENU menu = CreatePopupMenu();
        if (!menu)
        {
            m_contextMenuOpen = false;
            return;
        }

        enum MenuCmd
        {
            CmdWinBase = 100,
            CmdNewWindow = 200,
            CmdMinAll = 201,
            CmdCloseAll = 202,
            CmdOpenLocation = 203,
            CmdShowInExplorer = 204,
            CmdTogglePin = 205,
            CmdEmptyRecycleBin = 206,
            CmdNewCollection = 300,
            CmdToggleTopBar = 301,
            CmdSettings = 302,
            CmdExit = 303
        };

        RingSlot targetSlot;
        const bool hasSlot = (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_visibleSlots.size()));
        if (hasSlot)
        {
            targetSlot = m_visibleSlots[static_cast<size_t>(m_selectedIndex)];
            if (!targetSlot.windows.empty())
            {
                const size_t limit = (std::min)(targetSlot.windows.size(), size_t{ 9 });
                for (size_t i = 0; i < limit; ++i)
                    AppendMenuW(menu, MF_STRING, CmdWinBase + i, targetSlot.windows[i].title.c_str());
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            }
            if (targetSlot.kind == SlotKind::App)
            {
                AppendMenuW(menu, MF_STRING, CmdNewWindow, L"New Window");
                if (!targetSlot.windows.empty())
                {
                    AppendMenuW(menu, MF_STRING, CmdMinAll, L"Minimize All");
                    AppendMenuW(menu, MF_STRING, CmdCloseAll, L"Close All");
                }
                if (!targetSlot.pathOrUrl.empty())
                {
                    AppendMenuW(menu, MF_STRING, CmdOpenLocation, L"Open File Location");
                    AppendMenuW(menu, MF_STRING, CmdTogglePin,
                        targetSlot.isPinned ? L"Unpin from Ring" : L"Pin to Ring");
                }
            }
            else if (targetSlot.kind == SlotKind::File || targetSlot.kind == SlotKind::Folder || targetSlot.kind == SlotKind::Script)
            {
                AppendMenuW(menu, MF_STRING, CmdShowInExplorer, L"Show in Explorer");
                if (targetSlot.isPinned)
                    AppendMenuW(menu, MF_STRING, CmdTogglePin, L"Unpin from Ring");
            }
            else if (targetSlot.kind == SlotKind::RecycleBin)
            {
                AppendMenuW(menu, MF_STRING, CmdEmptyRecycleBin, L"Empty Recycle Bin");
            }
            else if (targetSlot.isPinned)
            {
                AppendMenuW(menu, MF_STRING, CmdTogglePin, L"Unpin from Ring");
            }
        }
        else
        {
            AppendMenuW(menu, MF_STRING, CmdNewCollection, L"New Collection...");
            AppendMenuW(menu, MF_STRING, CmdToggleTopBar, L"Toggle Top Minimal Bar");
            AppendMenuW(menu, MF_STRING, CmdSettings, L"Settings...");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, CmdExit, L"Exit WinRings");
        }

        SetForegroundWindow(m_hwnd);
        const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, m_hwnd, nullptr);
        DestroyMenu(menu);
        PostMessageW(m_hwnd, WM_NULL, 0, 0);
        m_contextMenuOpen = false;

        if (cmd == 0)
            return;

        if (hasSlot && cmd >= CmdWinBase && cmd < CmdWinBase + 9)
        {
            const size_t idx = cmd - CmdWinBase;
            if (idx < targetSlot.windows.size() && m_tracker)
            {
                const HWND winHwnd = targetSlot.windows[idx].hwnd;
                Dismiss();
                m_tracker->ActivateWindow(winHwnd, false, m_config.pointerFollow);
            }
            return;
        }

        switch (cmd)
        {
        case CmdNewWindow:
            Dismiss();
            SlotModel::LaunchPathOrUrl(targetSlot.pathOrUrl);
            break;
        case CmdMinAll:
            for (const auto& w : targetSlot.windows) WindowTracker::MinimizeWindow(w.hwnd);
            Dismiss();
            break;
        case CmdCloseAll:
            for (const auto& w : targetSlot.windows) WindowTracker::CloseWindow(w.hwnd);
            Dismiss();
            break;
        case CmdOpenLocation:
        case CmdShowInExplorer:
            Dismiss();
            SlotModel::RevealInExplorer(targetSlot.pathOrUrl);
            break;
        case CmdEmptyRecycleBin:
            Dismiss();
            SlotModel::EmptyRecycleBin(nullptr);
            break;
        case CmdTogglePin:
            if (targetSlot.isPinned)
            {
                m_config.pinnedSlots.erase(
                    std::remove_if(m_config.pinnedSlots.begin(), m_config.pinnedSlots.end(),
                        [&](const PinnedSlotConfig& p) { return p.id == targetSlot.id; }),
                    m_config.pinnedSlots.end());
            }
            else if (!targetSlot.pathOrUrl.empty())
            {
                PinnedSlotConfig p;
                p.id = L"pin-" + std::to_wstring(GetTickCount64());
                p.kind = SlotKind::App;
                p.label = targetSlot.label;
                p.pathOrUrl = targetSlot.pathOrUrl;
                m_config.pinnedSlots.push_back(std::move(p));
            }
            if (m_callbacks.saveAndReloadConfig) m_callbacks.saveAndReloadConfig(m_config);
            Dismiss();
            break;
        case CmdNewCollection:
        {
            PinnedSlotConfig col;
            col.id = L"col-" + std::to_wstring(GetTickCount64());
            col.kind = SlotKind::Collection;
            col.label = L"Collection " + std::to_wstring(m_config.pinnedSlots.size() + 1);
            m_config.pinnedSlots.push_back(std::move(col));
            if (m_callbacks.saveAndReloadConfig) m_callbacks.saveAndReloadConfig(m_config);
            Dismiss();
            break;
        }
        case CmdToggleTopBar:
            Dismiss();
            if (m_callbacks.onToggleTopBarRequested) m_callbacks.onToggleTopBarRequested();
            break;
        case CmdSettings:
            Dismiss();
            if (m_callbacks.launchTools) m_callbacks.launchTools(L"--settings");
            break;
        case CmdExit:
            Dismiss();
            if (m_callbacks.onExitRequested) m_callbacks.onExitRequested();
            break;
        }
    }

    void RingOverlay::ShowDropActionsMenu(const std::vector<std::wstring>& files, POINT screenPt)
    {
        if (files.empty() || !m_hwnd)
            return;

        HMENU menu = CreatePopupMenu();
        if (!menu)
            return;
        AppendMenuW(menu, MF_STRING, 1, L"Zip Archive");
        AppendMenuW(menu, MF_STRING, 2, L"Compress Media...");
        AppendMenuW(menu, MF_STRING, 3, L"Trim Media...");
        AppendMenuW(menu, MF_STRING, 4, L"Redact Image...");

        SetForegroundWindow(m_hwnd);
        const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, m_hwnd, nullptr);
        DestroyMenu(menu);
        PostMessageW(m_hwnd, WM_NULL, 0, 0);

        if (cmd == 0 || !m_callbacks.launchTools)
            return;

        auto quoteAll = [&]() {
            std::wstring s;
            for (const auto& f : files)
            {
                if (!s.empty()) s.push_back(L' ');
                s += L"\"" + f + L"\"";
            }
            return s;
        };

        if (cmd == 1) m_callbacks.launchTools(L"--zip " + quoteAll());
        else if (cmd == 2) m_callbacks.launchTools(L"--compress \"" + files[0] + L"\"");
        else if (cmd == 3) m_callbacks.launchTools(L"--trim \"" + files[0] + L"\"");
        else if (cmd == 4) m_callbacks.launchTools(L"--redact \"" + files[0] + L"\"");
    }

    void RingOverlay::OnDragEnterOverlay()
    {
        m_dragActive = true;
        m_dragHoverBadge = 0;
        ClearSubmenu();
        Render();
    }

    void RingOverlay::OnDragOverOverlay(POINT screenPt)
    {
        const double dx = (screenPt.x - m_centerScreenPhysical.x) / m_dpiScale;
        const double dy = (screenPt.y - m_centerScreenPhysical.y) / m_dpiScale;
        const PointD localPt{ kCenterDip + dx, kCenterDip + dy };

        int nextBadge = 0;
        if (kConvertBadgeDip.Contains(localPt)) nextBadge = 1;
        else if (kActionsBadgeDip.Contains(localPt)) nextBadge = 2;

        const int nextIndex = nextBadge ? -1
            : CalculateSectorIndex(dx, dy, static_cast<int>(m_visibleSlots.size()), m_config.deadzoneDip);

        if (nextBadge != m_dragHoverBadge || nextIndex != m_selectedIndex)
        {
            m_dragHoverBadge = nextBadge;
            m_selectedIndex = nextIndex;
            for (size_t i = 0; i < m_slotScales.size(); ++i)
                m_slotScales[i] = (static_cast<int>(i) == m_selectedIndex) ? 1.24 : 1.0;
            Render();
        }
    }

    void RingOverlay::OnDragLeaveOverlay()
    {
        m_dragActive = false;
        m_dragHoverBadge = 0;
        Render();
    }

    void RingOverlay::OnDropOverlay(POINT screenPt, std::vector<std::wstring> files, std::wstring text)
    {
        m_dragActive = false;
        m_dragHoverBadge = 0;

        const double dx = (screenPt.x - m_centerScreenPhysical.x) / m_dpiScale;
        const double dy = (screenPt.y - m_centerScreenPhysical.y) / m_dpiScale;
        const PointD localPt{ kCenterDip + dx, kCenterDip + dy };

        if (!files.empty())
        {
            if (kConvertBadgeDip.Contains(localPt))
            {
                Dismiss();
                if (m_callbacks.launchTools)
                {
                    std::wstring args = L"--convert";
                    for (const auto& f : files) args += L" \"" + f + L"\"";
                    m_callbacks.launchTools(args);
                }
                return;
            }
            if (kActionsBadgeDip.Contains(localPt))
            {
                ShowDropActionsMenu(files, screenPt);
                Dismiss();
                return;
            }
        }

        const double distFromCenter = std::hypot(dx, dy);
        if (distFromCenter <= m_config.deadzoneDip + 20.0)
        {
            if (!files.empty())
            {
                const std::wstring& first = files[0];
                const DWORD attrs = GetFileAttributesW(first.c_str());
                SlotKind kind = SlotKind::File;
                if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
                    kind = SlotKind::Folder;
                else if (first.size() >= 4 && _wcsicmp(first.c_str() + first.size() - 4, L".exe") == 0)
                    kind = SlotKind::App;

                const size_t slash = first.find_last_of(L"\\/");
                std::wstring label = (slash != std::wstring::npos) ? first.substr(slash + 1) : first;
                if (kind != SlotKind::Folder)
                {
                    const size_t dot = label.find_last_of(L'.');
                    if (dot != std::wstring::npos && dot > 0) label.resize(dot);
                }

                PinnedSlotConfig pin;
                pin.id = L"pin-" + std::to_wstring(GetTickCount64());
                pin.kind = kind;
                pin.label = label;
                pin.pathOrUrl = first;
                m_config.pinnedSlots.push_back(std::move(pin));
                if (m_callbacks.saveAndReloadConfig) m_callbacks.saveAndReloadConfig(m_config);
            }
            else if (!text.empty() && (text.rfind(L"http://", 0) == 0 || text.rfind(L"https://", 0) == 0))
            {
                const std::wstring pinId = L"site-" + std::to_wstring(GetTickCount64());
                PinnedSlotConfig pin;
                pin.id = pinId;
                pin.kind = SlotKind::Website;
                pin.label = text;
                pin.pathOrUrl = text;
                m_config.pinnedSlots.push_back(pin);
                if (m_callbacks.saveAndReloadConfig) m_callbacks.saveAndReloadConfig(m_config);

        const HWND overlayHwnd = m_hwnd;
        const std::wstring cfgPath = SwitcherConfig::ConfigPath();
        SlotModel::FetchFaviconAsync(text, [overlayHwnd, cfgPath, pinId](std::wstring relPath) {
            if (relPath.empty()) return;
            SwitcherConfig diskCfg;
            if (!SwitcherConfig::LoadFromFile(cfgPath, diskCfg))
                diskCfg = SwitcherConfig::CreateDefault();
            for (auto& s : diskCfg.pinnedSlots)
            {
                if (s.id == pinId) { s.iconCacheFile = relPath; break; }
            }
            diskCfg.SaveToFile(cfgPath);
            if (overlayHwnd && IsWindow(overlayHwnd))
                PostMessageW(overlayHwnd, WM_APP + 50, 0, 0);
        });
            }
            Dismiss();
            return;
        }

        if (!files.empty() && m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_visibleSlots.size()))
        {
            const RingSlot target = m_visibleSlots[static_cast<size_t>(m_selectedIndex)];
            Dismiss();
            if (target.kind == SlotKind::RecycleBin)
            {
                SlotModel::SendToRecycleBin(files);
            }
            else if ((target.kind == SlotKind::App || target.kind == SlotKind::Script) && !target.pathOrUrl.empty())
            {
                std::wstring args;
                for (const auto& f : files)
                {
                    if (!args.empty()) args.push_back(L' ');
                    args += L"\"" + f + L"\"";
                }
                SlotModel::LaunchPathOrUrl(target.pathOrUrl, args);
            }
            return;
        }

        Dismiss();
    }

    LRESULT CALLBACK RingOverlay::OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<RingOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE)
        {
            self = static_cast<RingOverlay*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->m_hwnd = hwnd;
        }
        if (self)
        {
            try { return self->HandleMessage(msg, wp, lp); }
            catch (...) {}
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT RingOverlay::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_TIMER:
            if (wp == kAnimTimerId)
            {
                StepAnimations();
                return 0;
            }
            break;

        case WM_MOUSEMOVE:
        {
            if (m_isOpen && !m_contextMenuOpen && !m_dragActive)
            {
                POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                ClientToScreen(m_hwnd, &pt);
                if (!m_hasLastPointer || pt.x != m_lastPointerPhysical.x || pt.y != m_lastPointerPhysical.y)
                    UpdatePointer(pt.x, pt.y);
            }
            return 0;
        }

        case WM_LBUTTONDOWN:
        {
            if (!m_isLockedOpen)
                return 0;
            POINT clientPt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            POINT screenPt = clientPt;
            ClientToScreen(m_hwnd, &screenPt);
            if (!m_hasLastPointer && m_selectedIndex < 0)
                UpdatePointer(screenPt.x, screenPt.y);
            const PointD localDip{ clientPt.x / m_dpiScale, clientPt.y / m_dpiScale };

            // Support drag-out from Folder/Downloads fan-out items only when clicking that card
            const int hitSub = HitTestSubmenu(localDip);
            if (m_expandedSlotIndex >= 0 && hitSub >= 0 && hitSub == m_selectedSubIndex &&
                m_selectedSubIndex < static_cast<int>(m_submenuCards.size()))
            {
                const auto& card = m_submenuCards[static_cast<size_t>(m_selectedSubIndex)];
                if (card.subSlot && (card.subSlot->kind == SlotKind::File || card.subSlot->kind == SlotKind::Folder) &&
                    !card.subSlot->pathOrUrl.empty())
                {
                    if (DragDetect(m_hwnd, screenPt))
                    {
                        const std::wstring path = card.subSlot->pathOrUrl;
                        Dismiss();
                        RingDropTarget::StartFileDragOut(m_hwnd, path);
                        return 0;
                    }
                }
            }

            const double dist = std::hypot(localDip.x - kCenterDip, localDip.y - kCenterDip);
            if (dist <= m_config.deadzoneDip)
            {
                Dismiss();
                return 0;
            }
            CommitSelectionAndClose();
            return 0;
        }

        case WM_RBUTTONUP:
        {
            POINT screenPt{};
            GetCursorPos(&screenPt);
            ShowContextMenuAt(screenPt);
            return 0;
        }

        case WM_APP + 50:
            if (m_callbacks.saveAndReloadConfig)
            {
                m_config = SwitcherConfig::LoadOrCreate();
                m_callbacks.saveAndReloadConfig(m_config);
            }
            return 0;

        case WM_CAPTURECHANGED:
            break;

        default:
            break;
        }
        return DefWindowProcW(m_hwnd, msg, wp, lp);
    }
} // namespace winrings
