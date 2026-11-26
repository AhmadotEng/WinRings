#pragma once

#include <Windows.h>
#include <functional>
#include <memory>

#include "../shared/WinRingsApi.h"
#include "../shared/WinRingsDeltaIpc.h"

namespace winrings::host
{
    enum class EngineMode
    {
        Stopped,
        Starting,
        Active,
        Paused,
        SafetyLocked,
        PreviewOnly
    };

    struct SettingsHostBridge
    {
        std::function<EngineMode()> queryEngineState;
        std::function<bool(const WinRingsDeltaPacket&)> sendDelta;
        std::function<void(bool paused)> setPaused;
        std::function<bool()> toggleRingNow;
        std::function<bool()> resetSafetyAndStart;
        std::function<void()> restartHook;
        std::function<void()> requestExit;
    };

    class SettingsHostUi
    {
    public:
        SettingsHostUi();
        ~SettingsHostUi();

        SettingsHostUi(const SettingsHostUi&) = delete;
        SettingsHostUi& operator=(const SettingsHostUi&) = delete;

        bool InitializeTray(HINSTANCE instance, HWND hostWindow, UINT taskbarCreatedMsg, SettingsHostBridge bridge);
        void ShutdownTray() noexcept;

        void OpenWindow(bool activate = true);
        void CloseWindow();
        void NotifyHostStatusChanged();

        bool HandleHostWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& outResult);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace winrings::host
