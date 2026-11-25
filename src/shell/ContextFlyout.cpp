#include "ContextFlyout.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace winrings
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr wchar_t kContextFlyoutClassW[] = L"WinRings.ContextFlyout.v1";
        constexpr float kRowH = 34.0f;
        constexpr float kSepH = 9.0f;
        constexpr float kPadX = 6.0f;
        constexpr float kPadY = 6.0f;

        D2D1_COLOR_F RgbF(BYTE r, BYTE g, BYTE b, float a = 1.0f) noexcept
        {
            return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, a);
        }

        struct FlyoutRowRect
        {
            float top = 0.0f;
            float bottom = 0.0f;
            size_t itemIndex = 0;
        };

        class ContextFlyoutPopup
        {
        public:
            UINT Show(HINSTANCE instance, HWND ownerHwnd, POINT anchorScreenPt, const std::vector<ContextFlyoutItem>& items)
            {
                if (items.empty())
                    return 0;

                m_instance = instance;
                m_items = items;

                HMONITOR mon = MonitorFromPoint(anchorScreenPt, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{ sizeof(mi) };
                if (!GetMonitorInfoW(mon, &mi))
                    return 0;

                UINT dpiX = 96, dpiY = 96;
                using GetDpiForMonitorFn = HRESULT (WINAPI*)(HMONITOR, int, UINT*, UINT*);
                if (HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
                {
                    if (auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor")))
                        fn(mon, 0, &dpiX, &dpiY);
                    FreeLibrary(shcore);
                }
                m_dpiScale = (dpiX > 0) ? (static_cast<float>(dpiX) / 96.0f) : 1.0f;

                WNDCLASSEXW wc{ sizeof(wc) };
                wc.lpfnWndProc = WndProc;
                wc.hInstance = instance;
                wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                wc.lpszClassName = kContextFlyoutClassW;
                if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                    return 0;

                size_t maxChars = 0;
                float totalH = kPadY * 2.0f;
                m_rows.clear();
                for (size_t i = 0; i < m_items.size(); ++i)
                {
                    if (m_items[i].isSeparator)
                    {
                        m_rows.push_back(FlyoutRowRect{ totalH, totalH + kSepH, i });
                        totalH += kSepH;
                    }
                    else
                    {
                        maxChars = (std::max)(maxChars, m_items[i].label.size());
                        m_rows.push_back(FlyoutRowRect{ totalH, totalH + kRowH, i });
                        totalH += kRowH;
                    }
                }

                m_widthDip = std::clamp(136.0f + static_cast<float>(maxChars) * 6.8f, 224.0f, 286.0f);
                m_heightDip = totalH;

                const int wPx = static_cast<int>(std::lround(m_widthDip * m_dpiScale));
                const int hPx = static_cast<int>(std::lround(m_heightDip * m_dpiScale));
                const int padPx = static_cast<int>(std::lround(8.0f * m_dpiScale));
                const int bottomClearPx = static_cast<int>(std::lround(62.0f * m_dpiScale));

                RECT safe = mi.rcWork;
                if (safe.bottom >= mi.rcMonitor.bottom - 8)
                    safe.bottom = (std::min)(safe.bottom, static_cast<LONG>(mi.rcMonitor.bottom - bottomClearPx));
                if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr))
                {
                    RECT tRc{};
                    if (IsWindowVisible(tray) && GetWindowRect(tray, &tRc) &&
                        tRc.top > mi.rcMonitor.top && tRc.bottom >= mi.rcMonitor.bottom - 8 &&
                        tRc.left < mi.rcMonitor.right && tRc.right > mi.rcMonitor.left)
                    {
                        safe.bottom = (std::min)(safe.bottom, static_cast<LONG>(tRc.top - padPx));
                    }
                }

                int x = anchorScreenPt.x;
                int y = anchorScreenPt.y;
                if (x + wPx > safe.right - padPx)
                    x = anchorScreenPt.x - wPx;
                x = std::clamp(x, static_cast<int>(safe.left + padPx),
                    (std::max)(static_cast<int>(safe.left + padPx), static_cast<int>(safe.right - wPx - padPx)));

                if (y + hPx > safe.bottom - padPx)
                    y = anchorScreenPt.y - hPx;
                y = std::clamp(y, static_cast<int>(safe.top + padPx),
                    (std::max)(static_cast<int>(safe.top + padPx), static_cast<int>(safe.bottom - hPx - padPx)));

                m_hwnd = CreateWindowExW(
                    WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                    kContextFlyoutClassW,
                    L"",
                    WS_POPUP,
                    x, y, wPx, hPx,
                    ownerHwnd, nullptr, instance, this);
                if (!m_hwnd)
                    return 0;

                const BOOL dark = TRUE;
                DwmSetWindowAttribute(m_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
                const int corner = 2; // DWMWCP_ROUND
                DwmSetWindowAttribute(m_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
                const COLORREF border = RGB(66, 66, 74);
                DwmSetWindowAttribute(m_hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));

                if (!InitGraphics(wPx, hPx))
                {
                    DestroyPopup();
                    return 0;
                }

                ShowWindow(m_hwnd, SW_SHOWNORMAL);
                SetForegroundWindow(m_hwnd);
                Render();

                MSG msg{};
                while (!m_done && m_hwnd && IsWindow(m_hwnd))
                {
                    const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
                    if (got <= 0)
                    {
                        if (got == 0)
                            PostQuitMessage(static_cast<int>(msg.wParam));
                        break;
                    }
                    if (msg.hwnd != m_hwnd)
                    {
                        if (msg.message == WM_LBUTTONDOWN || msg.message == WM_MBUTTONDOWN ||
                            msg.message == WM_NCLBUTTONDOWN || msg.message == WM_MOUSEWHEEL)
                        {
                            m_done = true;
                            if (ownerHwnd && msg.hwnd == ownerHwnd)
                                continue; // Consume click/wheel on non-activating Ring so dismissing menu doesn't launch a slot
                        }
                        else if (msg.message == WM_RBUTTONDOWN)
                        {
                            if (ownerHwnd && msg.hwnd == ownerHwnd)
                                continue; // Wait for WM_RBUTTONUP on Ring to reposition cleanly
                            m_done = true;
                        }
                    }
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }

                DestroyPopup();
                return m_selectedId;
            }

            void RequestDismiss() noexcept
            {
                m_done = true;
                if (m_hwnd && IsWindow(m_hwnd))
                    PostMessageW(m_hwnd, WM_NULL, 0, 0);
            }

            static ContextFlyoutPopup* s_activePopup;

        private:
            static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
            {
                auto* self = reinterpret_cast<ContextFlyoutPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (msg == WM_NCCREATE)
                {
                    self = static_cast<ContextFlyoutPopup*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                    self->m_hwnd = hwnd;
                }
                if (self)
                    return self->HandleMessage(hwnd, msg, wp, lp);
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            bool InitGraphics(int wPx, int hPx)
            {
                ReleaseGraphics();
                const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
                D3D_DRIVER_TYPE drivers[] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };
                HRESULT hr = E_FAIL;
                for (auto drv : drivers)
                {
                    hr = D3D11CreateDevice(nullptr, drv, nullptr, flags, nullptr, 0,
                        D3D11_SDK_VERSION, m_d3dDevice.ReleaseAndGetAddressOf(), nullptr, nullptr);
                    if (SUCCEEDED(hr)) break;
                }
                if (FAILED(hr) || !m_d3dDevice || FAILED(m_d3dDevice.As(&m_dxgiDevice)) || !m_dxgiDevice)
                    return false;

                D2D1_FACTORY_OPTIONS opts{};
                if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                    &opts, reinterpret_cast<void**>(m_d2dFactory.ReleaseAndGetAddressOf()))))
                    return false;
                if (FAILED(m_d2dFactory->CreateDevice(m_dxgiDevice.Get(), m_d2dDevice.ReleaseAndGetAddressOf())))
                    return false;
                if (FAILED(m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                    m_d2dCtx.ReleaseAndGetAddressOf())))
                    return false;

                if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                    reinterpret_cast<IUnknown**>(m_dwrite.ReleaseAndGetAddressOf()))))
                    return false;

                if (SUCCEEDED(m_dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", m_fmtText.ReleaseAndGetAddressOf())) && m_fmtText)
                {
                    m_fmtText->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    m_fmtText->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    m_fmtText->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                    ComPtr<IDWriteInlineObject> ell;
                    m_dwrite->CreateEllipsisTrimmingSign(m_fmtText.Get(), ell.GetAddressOf());
                    m_fmtText->SetTrimming(&trim, ell.Get());
                }

                ComPtr<IDXGIAdapter> adapter;
                ComPtr<IDXGIFactory2> factory;
                if (FAILED(m_dxgiDevice->GetAdapter(adapter.GetAddressOf())) ||
                    FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()))))
                    return false;

                DXGI_SWAP_CHAIN_DESC1 desc{};
                desc.Width = static_cast<UINT>((std::max)(1, wPx));
                desc.Height = static_cast<UINT>((std::max)(1, hPx));
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                desc.BufferCount = 2;
                desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
                desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

                if (FAILED(factory->CreateSwapChainForHwnd(m_d3dDevice.Get(), m_hwnd, &desc, nullptr, nullptr,
                    m_swapChain.ReleaseAndGetAddressOf())))
                    return false;

                ComPtr<IDXGISurface> surf;
                if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(surf.GetAddressOf()))))
                    return false;

                D2D1_BITMAP_PROPERTIES1 bp{};
                bp.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
                bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
                bp.dpiX = 96.0f;
                bp.dpiY = 96.0f;
                return SUCCEEDED(m_d2dCtx->CreateBitmapFromDxgiSurface(surf.Get(), &bp, m_targetBmp.ReleaseAndGetAddressOf()));
            }

            void ReleaseGraphics() noexcept
            {
                if (m_d2dCtx) m_d2dCtx->SetTarget(nullptr);
                m_targetBmp.Reset();
                m_swapChain.Reset();
                m_fmtText.Reset();
                m_dwrite.Reset();
                m_d2dCtx.Reset();
                m_d2dDevice.Reset();
                m_d2dFactory.Reset();
                m_dxgiDevice.Reset();
                m_d3dDevice.Reset();
            }

            void DestroyPopup() noexcept
            {
                ReleaseGraphics();
                if (m_hwnd && IsWindow(m_hwnd))
                {
                    DestroyWindow(m_hwnd);
                    m_hwnd = nullptr;
                }
            }

            int HitTestRow(float xDip, float yDip) const noexcept
            {
                if (xDip < kPadX || xDip > m_widthDip - kPadX)
                    return -1;
                for (size_t r = 0; r < m_rows.size(); ++r)
                {
                    if (m_items[m_rows[r].itemIndex].isSeparator)
                        continue;
                    if (yDip >= m_rows[r].top && yDip <= m_rows[r].bottom)
                        return static_cast<int>(r);
                }
                return -1;
            }

            void DrawIcon(ContextIcon icon, float cx, float cy, D2D1_COLOR_F col)
            {
                ComPtr<ID2D1SolidColorBrush> br;
                if (FAILED(m_d2dCtx->CreateSolidColorBrush(col, br.GetAddressOf())) || !br)
                    return;

                ComPtr<ID2D1StrokeStyle> roundSs;
                D2D1_STROKE_STYLE_PROPERTIES sp{};
                sp.startCap = D2D1_CAP_STYLE_ROUND;
                sp.endCap = D2D1_CAP_STYLE_ROUND;
                sp.lineJoin = D2D1_LINE_JOIN_ROUND;
                m_d2dFactory->CreateStrokeStyle(sp, nullptr, 0, roundSs.GetAddressOf());

                switch (icon)
                {
                case ContextIcon::WindowDot:
                    m_d2dCtx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 3.0f, 3.0f), br.Get());
                    break;
                case ContextIcon::NewWindow:
                    m_d2dCtx->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 6.0f, cy - 5.0f, cx + 6.0f, cy + 5.0f), 1.8f, 1.8f), br.Get(), 1.3f);
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx - 5.5f, cy - 2.0f), D2D1::Point2F(cx + 5.5f, cy - 2.0f), br.Get(), 1.1f);
                    break;
                case ContextIcon::Pin:
                case ContextIcon::Unpin:
                {
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx - 3.5f, cy - 5.5f), D2D1::Point2F(cx + 3.5f, cy - 5.5f), br.Get(), 1.3f, roundSs.Get());
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx - 2.5f, cy - 5.5f), D2D1::Point2F(cx - 3.0f, cy + 1.0f), br.Get(), 1.3f, roundSs.Get());
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx + 2.5f, cy - 5.5f), D2D1::Point2F(cx + 3.0f, cy + 1.0f), br.Get(), 1.3f, roundSs.Get());
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx - 4.8f, cy + 1.2f), D2D1::Point2F(cx + 4.8f, cy + 1.2f), br.Get(), 1.3f, roundSs.Get());
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx, cy + 1.2f), D2D1::Point2F(cx, cy + 6.0f), br.Get(), 1.35f, roundSs.Get());
                    if (icon == ContextIcon::Unpin)
                        m_d2dCtx->DrawLine(D2D1::Point2F(cx - 6.0f, cy - 5.5f), D2D1::Point2F(cx + 6.0f, cy + 5.5f), br.Get(), 1.45f, roundSs.Get());
                    break;
                }
                case ContextIcon::EndTask:
                    m_d2dCtx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 6.2f, 6.2f), br.Get(), 1.35f);
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx - 4.2f, cy + 4.2f), D2D1::Point2F(cx + 4.2f, cy - 4.2f), br.Get(), 1.35f, roundSs.Get());
                    break;
                case ContextIcon::CloseWindow:
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx - 5.0f, cy - 5.0f), D2D1::Point2F(cx + 5.0f, cy + 5.0f), br.Get(), 1.4f, roundSs.Get());
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx + 5.0f, cy - 5.0f), D2D1::Point2F(cx - 5.0f, cy + 5.0f), br.Get(), 1.4f, roundSs.Get());
                    break;
                case ContextIcon::Folder:
                    m_d2dCtx->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 6.5f, cy - 4.0f, cx + 6.5f, cy + 5.5f), 1.6f, 1.6f), br.Get(), 1.25f);
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx - 6.0f, cy - 4.0f), D2D1::Point2F(cx - 1.5f, cy - 6.0f), br.Get(), 1.25f, roundSs.Get());
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx - 1.5f, cy - 6.0f), D2D1::Point2F(cx + 1.0f, cy - 4.0f), br.Get(), 1.25f, roundSs.Get());
                    break;
                case ContextIcon::Trash:
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx - 5.5f, cy - 4.2f), D2D1::Point2F(cx + 5.5f, cy - 4.2f), br.Get(), 1.3f, roundSs.Get());
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx - 2.2f, cy - 6.0f), D2D1::Point2F(cx + 2.2f, cy - 6.0f), br.Get(), 1.3f, roundSs.Get());
                    m_d2dCtx->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 4.2f, cy - 4.2f, cx + 4.2f, cy + 5.8f), 1.2f, 1.2f), br.Get(), 1.25f);
                    break;
                case ContextIcon::Settings:
                    m_d2dCtx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 5.2f, 5.2f), br.Get(), 1.35f);
                    m_d2dCtx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 1.8f, 1.8f), br.Get());
                    break;
                case ContextIcon::AdminShield:
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx, cy - 6.0f), D2D1::Point2F(cx + 5.2f, cy - 3.8f), br.Get(), 1.3f, roundSs.Get());
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx + 5.2f, cy - 3.8f), D2D1::Point2F(cx + 4.4f, cy + 1.5f), br.Get(), 1.3f, roundSs.Get());
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx + 4.4f, cy + 1.5f), D2D1::Point2F(cx, cy + 6.0f), br.Get(), 1.3f, roundSs.Get());
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx, cy + 6.0f), D2D1::Point2F(cx - 4.4f, cy + 1.5f), br.Get(), 1.3f, roundSs.Get());
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx - 4.4f, cy + 1.5f), D2D1::Point2F(cx - 5.2f, cy - 3.8f), br.Get(), 1.3f, roundSs.Get());
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx - 5.2f, cy - 3.8f), D2D1::Point2F(cx, cy - 6.0f), br.Get(), 1.3f, roundSs.Get());
                    break;
                case ContextIcon::Search:
                    m_d2dCtx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx - 1.2f, cy - 1.2f), 4.4f, 4.4f), br.Get(), 1.35f);
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx + 2.0f, cy + 2.0f), D2D1::Point2F(cx + 5.6f, cy + 5.6f), br.Get(), 1.5f, roundSs.Get());
                    break;
                case ContextIcon::Exit:
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx - 4.0f, cy - 5.0f), D2D1::Point2F(cx + 4.0f, cy - 5.0f), br.Get(), 1.3f, roundSs.Get());
                    m_d2dCtx->DrawLine(D2D1::Point2F(cx, cy - 5.0f), D2D1::Point2F(cx, cy + 5.0f), br.Get(), 1.3f, roundSs.Get());
                    break;
                case ContextIcon::None:
                default:
                    break;
                }
            }

            void Render()
            {
                if (!m_d2dCtx || !m_targetBmp || !m_swapChain)
                    return;

                auto mk = [&](D2D1_COLOR_F c) {
                    ComPtr<ID2D1SolidColorBrush> br;
                    m_d2dCtx->CreateSolidColorBrush(c, br.GetAddressOf());
                    return br;
                };

                m_d2dCtx->SetTarget(m_targetBmp.Get());
                m_d2dCtx->BeginDraw();
                m_d2dCtx->SetTransform(D2D1::Matrix3x2F::Scale(m_dpiScale, m_dpiScale));
                m_d2dCtx->Clear(RgbF(32, 32, 37));

                for (size_t r = 0; r < m_rows.size(); ++r)
                {
                    const auto& row = m_rows[r];
                    const auto& item = m_items[row.itemIndex];
                    if (item.isSeparator)
                    {
                        const float midY = (row.top + row.bottom) * 0.5f;
                        if (auto b = mk(RgbF(56, 56, 64)))
                            m_d2dCtx->DrawLine(D2D1::Point2F(12.0f, midY), D2D1::Point2F(m_widthDip - 12.0f, midY), b.Get(), 1.0f);
                        continue;
                    }

                    const bool hovered = (static_cast<int>(r) == m_hoverRow);
                    if (hovered)
                    {
                        const D2D1_COLOR_F hl = item.isDanger ? RgbF(76, 32, 36) : RgbF(50, 50, 58);
                        if (auto b = mk(hl))
                            m_d2dCtx->FillRoundedRectangle(D2D1::RoundedRect(
                                D2D1::RectF(kPadX, row.top + 1.0f, m_widthDip - kPadX, row.bottom - 1.0f), 6.0f, 6.0f), b.Get());
                    }

                    const D2D1_COLOR_F fg = item.isDanger
                        ? (hovered ? RgbF(255, 165, 165) : RgbF(248, 132, 132))
                        : (hovered ? RgbF(250, 250, 252) : RgbF(232, 232, 238));
                    DrawIcon(item.icon, 24.0f, (row.top + row.bottom) * 0.5f, fg);

                    if (m_fmtText)
                    {
                        if (auto b = mk(fg))
                        {
                            m_d2dCtx->DrawTextW(item.label.c_str(), static_cast<UINT32>(item.label.size()),
                                m_fmtText.Get(), D2D1::RectF(42.0f, row.top, m_widthDip - 14.0f, row.bottom), b.Get());
                        }
                    }
                }

                m_d2dCtx->EndDraw();
                m_d2dCtx->SetTarget(nullptr);
                m_swapChain->Present(0, 0);
            }

            LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
            {
                switch (msg)
                {
                case WM_ERASEBKGND:
                    return 1;
                case WM_PAINT:
                {
                    PAINTSTRUCT ps{};
                    BeginPaint(hwnd, &ps);
                    Render();
                    EndPaint(hwnd, &ps);
                    return 0;
                }
                case WM_MOUSEMOVE:
                {
                    const float xDip = static_cast<float>(GET_X_LPARAM(lp)) / m_dpiScale;
                    const float yDip = static_cast<float>(GET_Y_LPARAM(lp)) / m_dpiScale;
                    const int nextHover = HitTestRow(xDip, yDip);
                    if (nextHover != m_hoverRow)
                    {
                        m_hoverRow = nextHover;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }
                case WM_LBUTTONDOWN:
                {
                    const float xDip = static_cast<float>(GET_X_LPARAM(lp)) / m_dpiScale;
                    const float yDip = static_cast<float>(GET_Y_LPARAM(lp)) / m_dpiScale;
                    m_pressedRow = HitTestRow(xDip, yDip);
                    return 0;
                }
                case WM_LBUTTONUP:
                {
                    const float xDip = static_cast<float>(GET_X_LPARAM(lp)) / m_dpiScale;
                    const float yDip = static_cast<float>(GET_Y_LPARAM(lp)) / m_dpiScale;
                    const int hit = HitTestRow(xDip, yDip);
                    if (hit >= 0 && (hit == m_pressedRow || m_pressedRow < 0))
                    {
                        m_selectedId = m_items[m_rows[static_cast<size_t>(hit)].itemIndex].id;
                        m_done = true;
                    }
                    m_pressedRow = -1;
                    return 0;
                }
                case WM_RBUTTONUP:
                    m_done = true;
                    return 0;
                case WM_KEYDOWN:
                    if (wp == VK_ESCAPE)
                    {
                        m_done = true;
                        return 0;
                    }
                    break;
                case WM_ACTIVATE:
                    if (LOWORD(wp) == WA_INACTIVE)
                    {
                        m_done = true;
                        return 0;
                    }
                    break;
                case WM_CLOSE:
                    m_done = true;
                    return 0;
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            HINSTANCE m_instance = nullptr;
            HWND m_hwnd = nullptr;
            float m_dpiScale = 1.0f;
            float m_widthDip = 236.0f;
            float m_heightDip = 200.0f;
            std::vector<ContextFlyoutItem> m_items;
            std::vector<FlyoutRowRect> m_rows;
            int m_hoverRow = -1;
            int m_pressedRow = -1;
            UINT m_selectedId = 0;
            bool m_done = false;

            ComPtr<ID3D11Device> m_d3dDevice;
            ComPtr<IDXGIDevice> m_dxgiDevice;
            ComPtr<ID2D1Factory1> m_d2dFactory;
            ComPtr<ID2D1Device> m_d2dDevice;
            ComPtr<ID2D1DeviceContext> m_d2dCtx;
            ComPtr<IDXGISwapChain1> m_swapChain;
            ComPtr<ID2D1Bitmap1> m_targetBmp;
            ComPtr<IDWriteFactory> m_dwrite;
            ComPtr<IDWriteTextFormat> m_fmtText;
        };
        ContextFlyoutPopup* ContextFlyoutPopup::s_activePopup = nullptr;
    } // namespace

    UINT ShowContextFlyout(
        HINSTANCE instance,
        HWND ownerHwnd,
        POINT anchorScreenPt,
        const std::vector<ContextFlyoutItem>& items)
    {
        if (ContextFlyoutPopup::s_activePopup)
        {
            ContextFlyoutPopup::s_activePopup->RequestDismiss();
            return 0;
        }
        ContextFlyoutPopup popup;
        ContextFlyoutPopup::s_activePopup = &popup;
        const UINT result = popup.Show(instance, ownerHwnd, anchorScreenPt, items);
        if (ContextFlyoutPopup::s_activePopup == &popup)
            ContextFlyoutPopup::s_activePopup = nullptr;
        return result;
    }

    void DismissActiveContextFlyout() noexcept
    {
        if (ContextFlyoutPopup::s_activePopup)
            ContextFlyoutPopup::s_activePopup->RequestDismiss();
    }

    bool IsContextFlyoutOpen() noexcept
    {
        return ContextFlyoutPopup::s_activePopup != nullptr;
    }
} // namespace winrings
