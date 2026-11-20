#pragma once

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <functional>
#include <string>
#include <vector>

#include "../shared/WinRingsConfig.h"
#include "WindowTracker.h"

namespace winrings
{
    struct RingSlot
    {
        std::wstring id;
        SlotKind kind = SlotKind::App;
        std::wstring label;
        std::wstring pathOrUrl;
        Microsoft::WRL::ComPtr<IWICBitmapSource> iconBitmap;
        bool isPinned = false;
        int maxFolderItems = 8;
        std::vector<WindowEntry> windows;
        std::vector<RingSlot> subItems;

        bool IsRunning() const noexcept { return !windows.empty(); }
        bool HasSubmenu(bool fanOutSingleWindow) const noexcept
        {
            const size_t minWin = fanOutSingleWindow ? 1 : 2;
            return windows.size() >= minWin || kind == SlotKind::Folder || kind == SlotKind::Collection;
        }
    };

    class SlotModel
    {
    public:
        static std::vector<RingSlot> BuildRingSlots(
            HWND ignoreHwnd,
            const SwitcherConfig& config,
            WindowTracker& tracker,
            IWICImagingFactory* wicFactory);

        static void LaunchPathOrUrl(const std::wstring& pathOrUrl, const std::wstring& arguments = L"");
        static void RevealInExplorer(const std::wstring& path);
        static bool SendToRecycleBin(const std::vector<std::wstring>& paths);
        static bool EmptyRecycleBin(HWND ownerHwnd);
        static std::wstring ExpandEnv(const std::wstring& input);

        // Cookie-free background favicon fetch -> writes under %APPDATA%\WinRings\icons\ and saves relative path.
        static void FetchFaviconAsync(
            const std::wstring& url,
            std::function<void(std::wstring relativeIconPath)> onCompleted);
    };
} // namespace winrings
