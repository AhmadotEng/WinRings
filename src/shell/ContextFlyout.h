#pragma once

#include <Windows.h>
#include <string>
#include <vector>

namespace winrings
{
    enum class ContextIcon
    {
        None,
        WindowDot,
        NewWindow,
        Pin,
        Unpin,
        EndTask,
        CloseWindow,
        Folder,
        Trash,
        Settings,
        AdminShield,
        Search,
        Exit
    };

    struct ContextFlyoutItem
    {
        UINT id = 0;
        std::wstring label;
        ContextIcon icon = ContextIcon::None;
        bool isSeparator = false;
        bool isDanger = false;
    };

    /// Displays a crisp Direct2D dark popup flyout with Windows 11 taskbar-style icons
    /// positioned cleanly above the taskbar. Returns selected item id or 0 if dismissed.
    UINT ShowContextFlyout(
        HINSTANCE instance,
        HWND ownerHwnd,
        POINT anchorScreenPt,
        const std::vector<ContextFlyoutItem>& items);

    void DismissActiveContextFlyout() noexcept;
    bool IsContextFlyoutOpen() noexcept;
} // namespace winrings
