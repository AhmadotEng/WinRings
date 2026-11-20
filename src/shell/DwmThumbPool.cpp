#include "DwmThumbPool.h"

#include <algorithm>
#include <cmath>
#include "../shared/WinRingsIdentity.h"

namespace winrings
{
    DwmThumbPool::~DwmThumbPool()
    {
        Shutdown();
    }

    bool DwmThumbPool::Initialize(HINSTANCE instance, HWND ownerHwnd)
    {
        m_instance = instance;
        m_ownerHwnd = ownerHwnd;
        if (!m_bgBrush)
            m_bgBrush = CreateSolidBrush(RGB(22, 25, 34));

        if (!m_classRegistered)
        {
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.lpfnWndProc = CardWndProc;
            wc.hInstance = m_instance;
            wc.hbrBackground = m_bgBrush;
            wc.lpszClassName = WINRINGS_THUMBCARD_CLASS_W;
            if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
                m_classRegistered = true;
        }
        return m_classRegistered;
    }

    void DwmThumbPool::Shutdown()
    {
        HideAll();
        for (auto& card : m_cards)
        {
            if (card.cardHwnd)
            {
                DestroyWindow(card.cardHwnd);
                card.cardHwnd = nullptr;
            }
        }
        if (m_bgBrush)
        {
            DeleteObject(m_bgBrush);
            m_bgBrush = nullptr;
        }
        m_ownerHwnd = nullptr;
    }

    LRESULT CALLBACK DwmThumbPool::CardWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_MOUSEACTIVATE)
            return MA_NOACTIVATE;
        if (msg == WM_NCHITTEST)
            return HTTRANSPARENT;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    bool DwmThumbPool::EnsureCardCreated(size_t slotIndex)
    {
        if (slotIndex >= kPoolSize || !m_classRegistered)
            return false;
        if (m_cards[slotIndex].cardHwnd && IsWindow(m_cards[slotIndex].cardHwnd))
            return true;

        HWND hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
            WINRINGS_THUMBCARD_CLASS_W,
            L"",
            WS_POPUP,
            0, 0, 1, 1,
            m_ownerHwnd,
            nullptr,
            m_instance,
            nullptr);
        if (!hwnd)
            return false;

        const DWORD cornerPref = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
        m_cards[slotIndex].cardHwnd = hwnd;
        return true;
    }

    void DwmThumbPool::ShowCard(
        size_t slotIndex,
        HWND sourceHwnd,
        int screenLeftPx,
        int screenTopPx,
        int widthPx,
        int heightPx,
        COLORREF borderColor,
        BYTE opacity)
    {
        if (slotIndex >= kPoolSize || !sourceHwnd || !IsWindow(sourceHwnd))
            return;
        if (!EnsureCardCreated(slotIndex))
            return;

        CardSlot& slot = m_cards[slotIndex];
        if (slot.thumb && slot.sourceHwnd != sourceHwnd)
        {
            DwmUnregisterThumbnail(slot.thumb);
            slot.thumb = nullptr;
            slot.sourceHwnd = nullptr;
        }

        const int w = (std::max)(1, widthPx);
        const int h = (std::max)(1, heightPx);
        slot.widthPx = w;
        slot.heightPx = h;

        DwmSetWindowAttribute(slot.cardHwnd, 34 /* DWMWA_BORDER_COLOR */, &borderColor, sizeof(borderColor));
        SetWindowPos(slot.cardHwnd, HWND_TOPMOST, screenLeftPx, screenTopPx, w, h,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        slot.visible = true;

        if (!slot.thumb)
        {
            if (FAILED(DwmRegisterThumbnail(slot.cardHwnd, sourceHwnd, &slot.thumb)) || !slot.thumb)
                return;
            slot.sourceHwnd = sourceHwnd;
        }

        UpdateCardAppearance(slotIndex, screenLeftPx, screenTopPx, w, h, borderColor, opacity);
    }

    void DwmThumbPool::UpdateCardAppearance(
        size_t slotIndex,
        int screenLeftPx,
        int screenTopPx,
        int widthPx,
        int heightPx,
        COLORREF borderColor,
        BYTE opacity)
    {
        if (slotIndex >= kPoolSize)
            return;
        CardSlot& slot = m_cards[slotIndex];
        if (!slot.cardHwnd || !slot.thumb)
            return;

        const int w = (std::max)(1, widthPx);
        const int h = (std::max)(1, heightPx);
        slot.widthPx = w;
        slot.heightPx = h;

        DwmSetWindowAttribute(slot.cardHwnd, 34 /* DWMWA_BORDER_COLOR */, &borderColor, sizeof(borderColor));
        SetWindowPos(slot.cardHwnd, HWND_TOPMOST, screenLeftPx, screenTopPx, w, h,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);

        int dstLeft = 0;
        int dstTop = 0;
        int dstRight = w;
        int dstBottom = h;
        SIZE srcSize{};
        if (SUCCEEDED(DwmQueryThumbnailSourceSize(slot.thumb, &srcSize)) && srcSize.cx > 0 && srcSize.cy > 0)
        {
            const double scale = (std::min)(
                static_cast<double>(w) / static_cast<double>(srcSize.cx),
                static_cast<double>(h) / static_cast<double>(srcSize.cy));
            const int fitW = (std::max)(1, static_cast<int>(std::llround(srcSize.cx * scale)));
            const int fitH = (std::max)(1, static_cast<int>(std::llround(srcSize.cy * scale)));
            dstLeft = (w - fitW) / 2;
            dstTop = (h - fitH) / 2;
            dstRight = dstLeft + fitW;
            dstBottom = dstTop + fitH;
        }

        DWM_THUMBNAIL_PROPERTIES props{};
        props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        props.fVisible = TRUE;
        props.fSourceClientAreaOnly = FALSE;
        props.opacity = opacity;
        props.rcDestination = RECT{ dstLeft, dstTop, dstRight, dstBottom };
        DwmUpdateThumbnailProperties(slot.thumb, &props);
    }

    void DwmThumbPool::HideCard(size_t slotIndex)
    {
        if (slotIndex >= kPoolSize)
            return;
        CardSlot& slot = m_cards[slotIndex];
        if (slot.thumb)
        {
            DwmUnregisterThumbnail(slot.thumb);
            slot.thumb = nullptr;
        }
        slot.sourceHwnd = nullptr;
        if (slot.visible && slot.cardHwnd && IsWindow(slot.cardHwnd))
        {
            ShowWindow(slot.cardHwnd, SW_HIDE);
        }
        slot.visible = false;
    }

    void DwmThumbPool::HideAll()
    {
        for (size_t i = 0; i < kPoolSize; ++i)
            HideCard(i);
    }
} // namespace winrings
