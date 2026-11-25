#pragma once

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace winrings
{
    struct WindowEntry
    {
        HWND hwnd = nullptr;
        DWORD pid = 0;
        std::wstring title;
        bool isMinimized = false;
    };

    struct AppWindowGroup
    {
        std::wstring normalizedKey;
        std::wstring exePath;
        std::wstring processName;
        Microsoft::WRL::ComPtr<IWICBitmapSource> iconBitmap;
        std::vector<WindowEntry> windows;
    };

    class WindowTracker
    {
    public:
        WindowTracker() = default;
        ~WindowTracker();

        WindowTracker(const WindowTracker&) = delete;
        WindowTracker& operator=(const WindowTracker&) = delete;

        bool Initialize(HWND eventWindow);
        void Shutdown();

        void OnWinEventForeground(HWND hwnd);
        void OnShellHookMessage(WPARAM eventCode, LPARAM lParam);
        void RecordForegroundBeforeOpen(HWND ignoreOverlayHwnd);

        HWND LastActiveWindow() const noexcept { return m_lastActiveWindow; }
        HWND PreviousActiveWindow() const noexcept { return m_previousActiveWindow; }
        HWND ResolvePreviousSwitchTarget(HWND ignoreOverlayHwnd) const;
        void SwapActiveHistory(HWND activated) noexcept;

        std::vector<AppWindowGroup> EnumerateGroupedWindows(HWND ignoreHwnd);
        Microsoft::WRL::ComPtr<IWICBitmapSource> GetShellIconBitmap(const std::wstring& path);
        Microsoft::WRL::ComPtr<IWICBitmapSource> GetStockIconBitmap(int stockIconId);
        IWICImagingFactory* WicFactory() const noexcept { return m_wicFactory.Get(); }

        void ActivateWindow(HWND target, bool centerOnActiveMonitor, bool pointerFollow);
        void CancelPointerGlide() noexcept;
        void OnUserMouseMove(POINT screenPt) noexcept;
        void OnGlideTimerTick() noexcept;

        static void MinimizeWindow(HWND hwnd) noexcept;
        static void CloseWindow(HWND hwnd) noexcept;
        static void EndTaskForWindows(const std::vector<WindowEntry>& windows) noexcept;
        static std::wstring NormalizeAppKey(const std::wstring& exePath, const std::wstring& fallbackName);
        static bool AppKeysMatch(const std::wstring& a, const std::wstring& b);
        static bool IsEligibleWindow(HWND hwnd, HWND shellWindow, HWND ignoreHwnd,
            DWORD selfPid, std::wstring& outTitle, DWORD& outPid, std::wstring& outExePath);

    private:
        static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
            LONG idObject, LONG idChild, DWORD eventThread, DWORD eventTime);

        Microsoft::WRL::ComPtr<IWICBitmapSource> ConvertHIconToWic(HICON hIcon);
        Microsoft::WRL::ComPtr<IWICBitmapSource> GetWindowIconBitmap(HWND hwnd, const std::wstring& exePath);
        void SeedMruFromEnumWindows();
        void PromoteInMru(HWND hwnd);
        void RemoveFromMru(HWND hwnd);

        HWND m_eventWindow = nullptr;
        HWINEVENTHOOK m_fgHook = nullptr;
        bool m_shellHookRegistered = false;
        Microsoft::WRL::ComPtr<IWICImagingFactory> m_wicFactory;
        std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<IWICBitmapSource>> m_iconCache;

        std::vector<HWND> m_mruOrder;
        HWND m_lastActiveWindow = nullptr;
        HWND m_previousActiveWindow = nullptr;

        // Non-blocking pointer glide state
        bool m_gliding = false;
        HWND m_glideTargetHwnd = nullptr;
        POINT m_glideStart{};
        POINT m_glideTarget{};
        POINT m_glideLastExpected{};
        POINT m_glideNextExpected{};
        int m_glideStep = 0;

        static WindowTracker* s_instance;
    };
} // namespace winrings
