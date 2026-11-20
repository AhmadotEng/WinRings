#pragma once

#include <Windows.h>
#include <dwmapi.h>
#include <array>

namespace winrings
{
    class DwmThumbPool
    {
    public:
        static constexpr size_t kPoolSize = 9;

        DwmThumbPool() = default;
        ~DwmThumbPool();

        DwmThumbPool(const DwmThumbPool&) = delete;
        DwmThumbPool& operator=(const DwmThumbPool&) = delete;

        bool Initialize(HINSTANCE instance, HWND ownerHwnd);
        void Shutdown();

        void ShowCard(
            size_t slotIndex,
            HWND sourceHwnd,
            int screenLeftPx,
            int screenTopPx,
            int widthPx,
            int heightPx,
            COLORREF borderColor,
            BYTE opacity = 255);

        void UpdateCardAppearance(
            size_t slotIndex,
            int screenLeftPx,
            int screenTopPx,
            int widthPx,
            int heightPx,
            COLORREF borderColor,
            BYTE opacity = 255);

        void HideCard(size_t slotIndex);
        void HideAll();

    private:
        struct CardSlot
        {
            HWND cardHwnd = nullptr;
            HWND sourceHwnd = nullptr;
            HTHUMBNAIL thumb = nullptr;
            int widthPx = 0;
            int heightPx = 0;
            bool visible = false;
        };

        static LRESULT CALLBACK CardWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
        bool EnsureCardCreated(size_t slotIndex);

        HINSTANCE m_instance = nullptr;
        HWND m_ownerHwnd = nullptr;
        HBRUSH m_bgBrush = nullptr;
        bool m_classRegistered = false;
        std::array<CardSlot, kPoolSize> m_cards{};
    };
} // namespace winrings
