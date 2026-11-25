#pragma once

#include <Windows.h>
#include <functional>

#include "../shared/WinRingsConfig.h"

namespace winrings
{
    class InputEngine
    {
    public:
        struct Callbacks
        {
            std::function<bool()> isRingOpen;
            std::function<bool()> isRingLockedOpen;
            std::function<bool()> isRingDragActive;
            std::function<bool(POINT screenPt)> isPointInOverlay;
            std::function<double()> getOverlayDpiScale;
            std::function<std::wstring()> getActiveProfileId;
            std::function<void(int screenX, int screenY, const std::wstring& profileId)> onTriggerPressed;
            std::function<void(int screenX, int screenY, bool isTap)> onTriggerReleased;
            std::function<void()> onDoubleTapLastApp;
            std::function<void(int screenX, int screenY)> onMouseMoved;
            std::function<void(int wheelDelta)> onMouseWheel;
            std::function<void()> onCancelRequested;
            std::function<void(const std::wstring& profileId)> onToggleRequested;
            std::function<void()> onCommitRequested;
            std::function<void(int stepDelta)> onArrowStep;
            std::function<void(wchar_t ch)> onFilterChar;
            std::function<void()> onFilterBackspace;
            std::function<void(POINT screenPt)> onUserPointerDelta;
        };

        InputEngine() = default;
        ~InputEngine();

        InputEngine(const InputEngine&) = delete;
        InputEngine& operator=(const InputEngine&) = delete;

        bool Install(const SwitcherConfig& config, Callbacks callbacks);
        void Uninstall();
        void UpdateConfig(const SwitcherConfig& config);
        void SetSuspended(bool suspended) noexcept;
        void ResetTransientState() noexcept;

    private:
        static LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wp, LPARAM lp);
        static LRESULT CALLBACK MouseHookProc(int code, WPARAM wp, LPARAM lp);

        LRESULT HandleKeyboard(int code, WPARAM wp, LPARAM lp);
        LRESULT HandleMouse(int code, WPARAM wp, LPARAM lp);

        bool AreModifiersExact(UINT requiredMods, UINT ignoreModVk = 0) const noexcept;
        int FindMatchingKeyChord(DWORD rawVk, bool allowExtraShift = false) const noexcept;
        int FindMatchingSoloModifier(DWORD rawVk) const noexcept;
        int FindMatchingMouseTrigger(UINT msg, DWORD mouseData, bool& outIsUp) const noexcept;
        int FindRightLeftChordTrigger() const noexcept;
        bool IsKeyPartOfActiveBinding(DWORD normVk) const noexcept;
        bool IsSameOpenProfile(const std::wstring& triggerProf) const noexcept;
        bool IsWinKeyHeld() const noexcept;
        void InjectModifierMaskTap() noexcept;
        bool TryConsumeDoubleTapOnPress(const std::wstring& profileId) noexcept;

        void StartTrigger(bool mouseTrigger, const TriggerBinding& binding);
        void EndTrigger();

        SwitcherConfig m_config;
        Callbacks m_callbacks;
        HHOOK m_keyboardHook = nullptr;
        HHOOK m_mouseHook = nullptr;

        bool m_suspended = false;
        bool m_triggerHeld = false;
        bool m_mouseTrigger = false;
        bool m_dismissedLockedOnPress = false;
        TriggerBinding m_activeBinding{};

        bool m_soloModCandidate = false;
        UINT m_soloModVk = 0;
        bool m_soloModChorded = false;
        std::wstring m_soloModProfileId;
        bool m_winKeyDown = false;
        LONGLONG m_winKeyDownQpc = 0;

        bool m_rightButtonDown = false;
        bool m_chordActive = false;
        bool m_outsideClickPending = false;
        LONGLONG m_triggerDownQpc = 0;
        LONGLONG m_lastTapQpc = 0;
        std::wstring m_lastTapProfileId;
        LONGLONG m_qpcFreq = 1;
        POINT m_triggerOrigin{};
        POINT m_outsideClickOrigin{};

        static InputEngine* s_instance;
    };
} // namespace winrings
