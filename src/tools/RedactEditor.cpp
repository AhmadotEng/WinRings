#include "RedactEditor.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "../shared/DropNaming.h"

namespace winrings::tools
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr int kToolbarH = 44;
        constexpr int kIdSolid  = 201;
        constexpr int kIdBlur   = 202;
        constexpr int kIdPixel  = 203;
        constexpr int kIdUndo   = 204;
        constexpr int kIdCopy   = 205;
        constexpr int kIdSave   = 206;

        enum class RedactMode
        {
            SolidBlack,
            Blur,
            Pixelate
        };

        struct RedactOp
        {
            RECT imgRect{}; // normalized in source image coordinates
            RedactMode mode = RedactMode::SolidBlack;
        };

        class RedactWindow
        {
        public:
            bool InitAndRun(HINSTANCE instance, const std::wstring& imagePath)
            {
                m_instance = instance;
                m_sourcePath = imagePath;

                if (!LoadSourceImage())
                {
                    MessageBoxW(nullptr, L"Could not open image for redaction.", L"WinRings Redact", MB_ICONWARNING | MB_OK);
                    return false;
                }

                D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.ReleaseAndGetAddressOf());

                WNDCLASSEXW wc{ sizeof(wc) };
                wc.style = CS_HREDRAW | CS_VREDRAW;
                wc.lpfnWndProc = WndProc;
                wc.hInstance = instance;
                wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
                wc.lpszClassName = L"WinRings.RedactEditor.v1";
                RegisterClassExW(&wc);

                RECT wa{};
                SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
                const int winW = (std::clamp)(static_cast<int>(m_imgW) + 40, 680, (std::max)(680, static_cast<int>(wa.right - wa.left - 80)));
                const int winH = (std::clamp)(static_cast<int>(m_imgH) + kToolbarH + 60, 480, (std::max)(480, static_cast<int>(wa.bottom - wa.top - 80)));

                m_hwnd = CreateWindowExW(0, wc.lpszClassName, L"WinRings — Redact Image (Ctrl+Z Undo)",
                    WS_OVERLAPPEDWINDOW,
                    wa.left + (wa.right - wa.left - winW) / 2,
                    wa.top + (wa.bottom - wa.top - winH) / 2,
                    winW, winH, nullptr, nullptr, instance, this);
                if (!m_hwnd)
                    return false;

                ShowWindow(m_hwnd, SW_SHOWNORMAL);

                MSG msg{};
                while (GetMessageW(&msg, nullptr, 0, 0) > 0)
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                return true;
            }

        private:
            static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
            {
                auto* self = reinterpret_cast<RedactWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (msg == WM_NCCREATE)
                {
                    self = static_cast<RedactWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                    self->m_hwnd = hwnd;
                }
                if (self)
                    return self->HandleMsg(msg, wp, lp);
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            bool LoadSourceImage()
            {
                if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(m_wicFactory.ReleaseAndGetAddressOf()))))
                    return false;

                ComPtr<IWICBitmapDecoder> decoder;
                if (FAILED(m_wicFactory->CreateDecoderFromFilename(m_sourcePath.c_str(), nullptr,
                    GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.ReleaseAndGetAddressOf())))
                    return false;

                ComPtr<IWICBitmapFrameDecode> frame;
                if (FAILED(decoder->GetFrame(0, frame.ReleaseAndGetAddressOf())))
                    return false;

                if (FAILED(frame->GetSize(&m_imgW, &m_imgH)) || m_imgW == 0 || m_imgH == 0)
                    return false;

                ComPtr<IWICFormatConverter> conv;
                if (FAILED(m_wicFactory->CreateFormatConverter(conv.ReleaseAndGetAddressOf())) ||
                    FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                        WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeMedianCut)))
                    return false;

                m_stride = m_imgW * 4;
                m_origPixels.resize(static_cast<size_t>(m_stride) * m_imgH);
                if (FAILED(conv->CopyPixels(nullptr, m_stride, static_cast<UINT>(m_origPixels.size()), m_origPixels.data())))
                    return false;

                m_workPixels = m_origPixels;
                return true;
            }

            void RebuildWorkPixels()
            {
                m_workPixels = m_origPixels;
                const int w = static_cast<int>(m_imgW);
                const int h = static_cast<int>(m_imgH);

                for (const auto& op : m_ops)
                {
                    const int x0 = (std::clamp)(static_cast<int>(op.imgRect.left), 0, w);
                    const int y0 = (std::clamp)(static_cast<int>(op.imgRect.top), 0, h);
                    const int x1 = (std::clamp)(static_cast<int>(op.imgRect.right), 0, w);
                    const int y1 = (std::clamp)(static_cast<int>(op.imgRect.bottom), 0, h);
                    if (x1 <= x0 || y1 <= y0)
                        continue;

                    if (op.mode == RedactMode::SolidBlack)
                    {
                        for (int y = y0; y < y1; ++y)
                        {
                            for (int x = x0; x < x1; ++x)
                            {
                                BYTE* px = &m_workPixels[static_cast<size_t>(y) * m_stride + static_cast<size_t>(x) * 4];
                                px[0] = 0;
                                px[1] = 0;
                                px[2] = 0;
                                px[3] = 255;
                            }
                        }
                    }
                    else if (op.mode == RedactMode::Pixelate)
                    {
                        const int block = 14;
                        for (int by = y0; by < y1; by += block)
                        {
                            for (int bx = x0; bx < x1; bx += block)
                            {
                                const int ex = (std::min)(bx + block, x1);
                                const int ey = (std::min)(by + block, y1);
                                uint64_t sb = 0, sg = 0, sr = 0, cnt = 0;
                                for (int y = by; y < ey; ++y)
                                {
                                    for (int x = bx; x < ex; ++x)
                                    {
                                        const BYTE* p = &m_workPixels[static_cast<size_t>(y) * m_stride + static_cast<size_t>(x) * 4];
                                        sb += p[0]; sg += p[1]; sr += p[2]; ++cnt;
                                    }
                                }
                                if (!cnt) continue;
                                const BYTE rb = static_cast<BYTE>(sb / cnt);
                                const BYTE rg = static_cast<BYTE>(sg / cnt);
                                const BYTE rr = static_cast<BYTE>(sr / cnt);
                                for (int y = by; y < ey; ++y)
                                {
                                    for (int x = bx; x < ex; ++x)
                                    {
                                        BYTE* p = &m_workPixels[static_cast<size_t>(y) * m_stride + static_cast<size_t>(x) * 4];
                                        p[0] = rb; p[1] = rg; p[2] = rr; p[3] = 255;
                                    }
                                }
                            }
                        }
                    }
                    else
                    {
                        // Multi-pass box blur over the selected region
                        const int rw = x1 - x0;
                        const int rh = y1 - y0;
                        std::vector<BYTE> temp(static_cast<size_t>(rw) * rh * 4);
                        const int rad = 10;
                        for (int pass = 0; pass < 2; ++pass)
                        {
                            for (int y = y0; y < y1; ++y)
                            {
                                for (int x = x0; x < x1; ++x)
                                {
                                    uint32_t sb = 0, sg = 0, sr = 0, sa = 0, cnt = 0;
                                    const int kx0 = (std::max)(0, x - rad);
                                    const int kx1 = (std::min)(w - 1, x + rad);
                                    for (int kx = kx0; kx <= kx1; ++kx)
                                    {
                                        const BYTE* p = &m_workPixels[static_cast<size_t>(y) * m_stride + static_cast<size_t>(kx) * 4];
                                        sb += p[0]; sg += p[1]; sr += p[2]; sa += p[3]; ++cnt;
                                    }
                                    BYTE* dst = &temp[(static_cast<size_t>(y - y0) * rw + (x - x0)) * 4];
                                    dst[0] = static_cast<BYTE>(sb / cnt);
                                    dst[1] = static_cast<BYTE>(sg / cnt);
                                    dst[2] = static_cast<BYTE>(sr / cnt);
                                    dst[3] = static_cast<BYTE>(sa / cnt);
                                }
                            }
                            for (int y = y0; y < y1; ++y)
                            {
                                for (int x = x0; x < x1; ++x)
                                {
                                    uint32_t sb = 0, sg = 0, sr = 0, sa = 0, cnt = 0;
                                    const int ky0 = (std::max)(y0, y - rad);
                                    const int ky1 = (std::min)(y1 - 1, y + rad);
                                    for (int ky = ky0; ky <= ky1; ++ky)
                                    {
                                        const BYTE* p = &temp[(static_cast<size_t>(ky - y0) * rw + (x - x0)) * 4];
                                        sb += p[0]; sg += p[1]; sr += p[2]; sa += p[3]; ++cnt;
                                    }
                                    BYTE* dst = &m_workPixels[static_cast<size_t>(y) * m_stride + static_cast<size_t>(x) * 4];
                                    dst[0] = static_cast<BYTE>(sb / cnt);
                                    dst[1] = static_cast<BYTE>(sg / cnt);
                                    dst[2] = static_cast<BYTE>(sr / cnt);
                                    dst[3] = static_cast<BYTE>(sa / cnt);
                                }
                            }
                        }
                    }
                }

                if (m_bitmap)
                {
                    D2D1_RECT_U rcU = D2D1::RectU(0, 0, m_imgW, m_imgH);
                    m_bitmap->CopyFromMemory(&rcU, m_workPixels.data(), m_stride);
                }
            }

            D2D1_RECT_F ComputeCanvasRect() const noexcept
            {
                RECT rc{};
                GetClientRect(m_hwnd, &rc);
                const float availW = (std::max)(32.0f, static_cast<float>(rc.right - rc.left - 24));
                const float availH = (std::max)(32.0f, static_cast<float>((rc.bottom - rc.top) - kToolbarH - 24));
                const float scaleX = availW / static_cast<float>(m_imgW);
                const float scaleY = availH / static_cast<float>(m_imgH);
                const float scale = (std::min)(scaleX, scaleY);
                const float drawW = static_cast<float>(m_imgW) * scale;
                const float drawH = static_cast<float>(m_imgH) * scale;
                const float left = (static_cast<float>(rc.right - rc.left) - drawW) * 0.5f;
                const float top = kToolbarH + 12.0f + (availH - drawH) * 0.5f;
                return D2D1::RectF(left, top, left + drawW, top + drawH);
            }

            POINT ClientToImagePoint(POINT pt) const noexcept
            {
                const D2D1_RECT_F c = ComputeCanvasRect();
                const float cw = (std::max)(1.0f, c.right - c.left);
                const float ch = (std::max)(1.0f, c.bottom - c.top);
                const int ix = static_cast<int>(((pt.x - c.left) / cw) * static_cast<float>(m_imgW));
                const int iy = static_cast<int>(((pt.y - c.top) / ch) * static_cast<float>(m_imgH));
                return POINT{
                    (std::clamp)(ix, 0, static_cast<int>(m_imgW)),
                    (std::clamp)(iy, 0, static_cast<int>(m_imgH))
                };
            }

            bool SaveRedactedPng()
            {
                const std::wstring outPath = BuildNonDestructiveOutputPath(m_sourcePath, L"redacted", L".png");
                ComPtr<IWICStream> stream;
                ComPtr<IWICBitmapEncoder> encoder;
                ComPtr<IWICBitmapFrameEncode> frame;
                if (FAILED(m_wicFactory->CreateStream(stream.ReleaseAndGetAddressOf())) ||
                    FAILED(stream->InitializeFromFilename(outPath.c_str(), GENERIC_WRITE)) ||
                    FAILED(m_wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.ReleaseAndGetAddressOf())) ||
                    FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
                    FAILED(encoder->CreateNewFrame(frame.ReleaseAndGetAddressOf(), nullptr)) ||
                    FAILED(frame->Initialize(nullptr)) ||
                    FAILED(frame->SetSize(m_imgW, m_imgH)))
                {
                    MessageBoxW(m_hwnd, L"Failed to save redacted image.", L"WinRings Redact", MB_ICONERROR | MB_OK);
                    return false;
                }

                WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
                frame->SetPixelFormat(&pf);
                if (FAILED(frame->WritePixels(m_imgH, m_stride, static_cast<UINT>(m_workPixels.size()), m_workPixels.data())) ||
                    FAILED(frame->Commit()) ||
                    FAILED(encoder->Commit()))
                {
                    DeleteFileW(outPath.c_str());
                    MessageBoxW(m_hwnd, L"Failed writing PNG pixels.", L"WinRings Redact", MB_ICONERROR | MB_OK);
                    return false;
                }

                const std::wstring msg = L"Saved redacted PNG:\n" + outPath;
                MessageBoxW(m_hwnd, msg.c_str(), L"WinRings Redact", MB_ICONINFORMATION | MB_OK);
                return true;
            }

            bool CopyRedactedToClipboard()
            {
                if (!OpenClipboard(m_hwnd))
                    return false;
                EmptyClipboard();

                const size_t pixelBytes = static_cast<size_t>(m_imgW) * m_imgH * 4;
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixelBytes);
                if (hMem)
                {
                    void* ptr = GlobalLock(hMem);
                    if (ptr)
                    {
                        auto* bih = static_cast<BITMAPINFOHEADER*>(ptr);
                        ZeroMemory(bih, sizeof(BITMAPINFOHEADER));
                        bih->biSize = sizeof(BITMAPINFOHEADER);
                        bih->biWidth = static_cast<LONG>(m_imgW);
                        bih->biHeight = -static_cast<LONG>(m_imgH);
                        bih->biPlanes = 1;
                        bih->biBitCount = 32;
                        bih->biCompression = BI_RGB;
                        std::memcpy(static_cast<BYTE*>(ptr) + sizeof(BITMAPINFOHEADER), m_workPixels.data(), pixelBytes);
                        GlobalUnlock(hMem);
                        SetClipboardData(CF_DIB, hMem);
                    }
                    else
                    {
                        GlobalFree(hMem);
                    }
                }
                CloseClipboard();
                return true;
            }

            void EnsureRenderTarget()
            {
                if (m_rt)
                    return;
                RECT rc{};
                GetClientRect(m_hwnd, &rc);
                D2D1_SIZE_U sz = D2D1::SizeU(
                    (std::max)(1u, static_cast<UINT>(rc.right - rc.left)),
                    (std::max)(1u, static_cast<UINT>(rc.bottom - rc.top)));
                D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties();
                rtp.dpiX = 96.0f;
                rtp.dpiY = 96.0f;
                if (SUCCEEDED(m_d2dFactory->CreateHwndRenderTarget(rtp,
                    D2D1::HwndRenderTargetProperties(m_hwnd, sz), m_rt.ReleaseAndGetAddressOf())))
                {
                    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                        96.0f, 96.0f);
                    m_rt->CreateBitmap(D2D1::SizeU(m_imgW, m_imgH), m_workPixels.data(), m_stride, bp,
                        m_bitmap.ReleaseAndGetAddressOf());
                }
            }

            void Render()
            {
                EnsureRenderTarget();
                if (!m_rt)
                    return;
                m_rt->BeginDraw();
                m_rt->Clear(D2D1::ColorF(28.0f / 255.0f, 28.0f / 255.0f, 30.0f / 255.0f, 1.0f));

                if (m_bitmap)
                {
                    const D2D1_RECT_F c = ComputeCanvasRect();
                    m_rt->DrawBitmap(m_bitmap.Get(), c, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

                    if (m_dragging)
                    {
                        ComPtr<ID2D1SolidColorBrush> br;
                        m_rt->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.72f, 1.0f, 0.9f), br.ReleaseAndGetAddressOf());
                        if (br)
                        {
                            const D2D1_RECT_F sel = D2D1::RectF(
                                static_cast<float>((std::min)(m_dragStartClient.x, m_dragCurClient.x)),
                                static_cast<float>((std::min)(m_dragStartClient.y, m_dragCurClient.y)),
                                static_cast<float>((std::max)(m_dragStartClient.x, m_dragCurClient.x)),
                                static_cast<float>((std::max)(m_dragStartClient.y, m_dragCurClient.y)));
                            m_rt->DrawRectangle(sel, br.Get(), 2.0f);
                        }
                    }
                }

                if (m_rt->EndDraw() == D2DERR_RECREATE_TARGET)
                {
                    m_bitmap.Reset();
                    m_rt.Reset();
                }
            }

            void UpdateModeChecks()
            {
                SendMessageW(GetDlgItem(m_hwnd, kIdSolid), BM_SETCHECK, m_mode == RedactMode::SolidBlack ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(GetDlgItem(m_hwnd, kIdBlur), BM_SETCHECK, m_mode == RedactMode::Blur ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(GetDlgItem(m_hwnd, kIdPixel), BM_SETCHECK, m_mode == RedactMode::Pixelate ? BST_CHECKED : BST_UNCHECKED, 0);
            }

            LRESULT HandleMsg(UINT msg, WPARAM wp, LPARAM lp)
            {
                switch (msg)
                {
                case WM_CREATE:
                {
                    if (HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
                    {
                        using SetPreferredAppModeFn = int (WINAPI*)(int);
                        if (auto setMode = reinterpret_cast<SetPreferredAppModeFn>(
                            GetProcAddress(ux, MAKEINTRESOURCEA(135))))
                            setMode(2);
                        FreeLibrary(ux);
                    }
                    const BOOL dark = TRUE;
                    DwmSetWindowAttribute(m_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
                    const int corner = 2; // DWMWCP_ROUND
                    DwmSetWindowAttribute(m_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
                    const COLORREF border = RGB(62, 62, 62);
                    DwmSetWindowAttribute(m_hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
                    m_font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
                    auto btn = [&](const wchar_t* text, DWORD style, int x, int w, int id) {
                        HWND c = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | style,
                            x, 8, w, 28, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance, nullptr);
                        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
                        SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
                    };
                    btn(L"Solid Black", BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP, 12, 92, kIdSolid);
                    btn(L"Blur", BS_AUTORADIOBUTTON | BS_PUSHLIKE, 110, 72, kIdBlur);
                    btn(L"Pixelate", BS_AUTORADIOBUTTON | BS_PUSHLIKE, 188, 82, kIdPixel);
                    btn(L"Undo (Ctrl+Z)", 0, 290, 102, kIdUndo);
                    btn(L"Copy DIB", 0, 400, 86, kIdCopy);
                    btn(L"Save PNG", BS_DEFPUSHBUTTON, 494, 96, kIdSave);
                    UpdateModeChecks();
                    return 0;
                }

                case WM_SIZE:
                    if (m_rt)
                        m_rt->Resize(D2D1::SizeU((std::max)(1, static_cast<int>(LOWORD(lp))),
                            (std::max)(1, static_cast<int>(HIWORD(lp)))));
                    InvalidateRect(m_hwnd, nullptr, FALSE);
                    return 0;

                case WM_PAINT:
                {
                    PAINTSTRUCT ps{};
                    BeginPaint(m_hwnd, &ps);
                    Render();
                    EndPaint(m_hwnd, &ps);
                    return 0;
                }

                case WM_LBUTTONDOWN:
                {
                    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                    if (pt.y > kToolbarH)
                    {
                        m_dragging = true;
                        m_dragStartClient = pt;
                        m_dragCurClient = pt;
                        SetCapture(m_hwnd);
                        SetFocus(m_hwnd);
                    }
                    return 0;
                }

                case WM_MOUSEMOVE:
                    if (m_dragging)
                    {
                        m_dragCurClient = POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                        InvalidateRect(m_hwnd, nullptr, FALSE);
                    }
                    return 0;

                case WM_LBUTTONUP:
                    if (m_dragging)
                    {
                        m_dragging = false;
                        ReleaseCapture();
                        const POINT p1 = ClientToImagePoint(m_dragStartClient);
                        const POINT p2 = ClientToImagePoint(POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
                        RECT ir{
                            (std::min)(p1.x, p2.x),
                            (std::min)(p1.y, p2.y),
                            (std::max)(p1.x, p2.x),
                            (std::max)(p1.y, p2.y)
                        };
                        if (ir.right - ir.left >= 2 && ir.bottom - ir.top >= 2)
                        {
                            m_ops.push_back(RedactOp{ ir, m_mode });
                            RebuildWorkPixels();
                        }
                        InvalidateRect(m_hwnd, nullptr, FALSE);
                    }
                    return 0;

                case WM_KEYDOWN:
                    if (wp == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000))
                    {
                        if (!m_ops.empty())
                        {
                            m_ops.pop_back();
                            RebuildWorkPixels();
                            InvalidateRect(m_hwnd, nullptr, FALSE);
                        }
                        return 0;
                    }
                    if (wp == VK_ESCAPE && m_dragging)
                    {
                        m_dragging = false;
                        ReleaseCapture();
                        InvalidateRect(m_hwnd, nullptr, FALSE);
                        return 0;
                    }
                    break;

                case WM_COMMAND:
                    switch (LOWORD(wp))
                    {
                    case kIdSolid: m_mode = RedactMode::SolidBlack; UpdateModeChecks(); break;
                    case kIdBlur:  m_mode = RedactMode::Blur; UpdateModeChecks(); break;
                    case kIdPixel: m_mode = RedactMode::Pixelate; UpdateModeChecks(); break;
                    case kIdUndo:
                        if (!m_ops.empty())
                        {
                            m_ops.pop_back();
                            RebuildWorkPixels();
                            InvalidateRect(m_hwnd, nullptr, FALSE);
                        }
                        break;
                    case kIdCopy:
                        CopyRedactedToClipboard();
                        break;
                    case kIdSave:
                        SaveRedactedPng();
                        break;
                    }
                    return 0;

                case WM_DESTROY:
                    m_bitmap.Reset();
                    m_rt.Reset();
                    if (m_font) { DeleteObject(m_font); m_font = nullptr; }
                    PostQuitMessage(0);
                    return 0;
                }
                return DefWindowProcW(m_hwnd, msg, wp, lp);
            }

            HINSTANCE m_instance = nullptr;
            HWND m_hwnd = nullptr;
            HFONT m_font = nullptr;
            std::wstring m_sourcePath;
            ComPtr<IWICImagingFactory> m_wicFactory;
            ComPtr<ID2D1Factory> m_d2dFactory;
            ComPtr<ID2D1HwndRenderTarget> m_rt;
            ComPtr<ID2D1Bitmap> m_bitmap;

            UINT m_imgW = 0;
            UINT m_imgH = 0;
            UINT m_stride = 0;
            std::vector<BYTE> m_origPixels;
            std::vector<BYTE> m_workPixels;
            std::vector<RedactOp> m_ops;

            RedactMode m_mode = RedactMode::SolidBlack;
            bool m_dragging = false;
            POINT m_dragStartClient{};
            POINT m_dragCurClient{};
        };
    } // namespace

    int RunRedactEditor(HINSTANCE instance, const std::wstring& imagePath)
    {
        if (imagePath.empty())
        {
            MessageBoxW(nullptr, L"No image path passed to --redact.", L"WinRings Redact", MB_ICONINFORMATION | MB_OK);
            return 1;
        }
        RedactWindow win;
        return win.InitAndRun(instance, imagePath) ? 0 : 1;
    }
} // namespace winrings::tools
