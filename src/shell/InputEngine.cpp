#include "InputEngine.h"

#include <cmath>

namespace winrings
{
    InputEngine* InputEngine::s_instance = nullptr;

    InputEngine::~InputEngine()
    {
        Uninstall();
    }

    bool InputEngine::Install(const SwitcherConfig& config, Callbacks callbacks)
    {
        Uninstall();
        m_config = config;
        m_callbacks = std::move(callbacks);
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        m_qpcFreq = freq.QuadPart > 0 ? freq.QuadPart : 1;
        s_instance = this;

        HMODULE selfMod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&KeyboardHookProc), &selfMod);

        m_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc, selfMod, 0);
        m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, selfMod, 0);
        return m_keyboardHook != nullptr && m_mouseHook != nullptr;
    }

    void InputEngine::Uninstall()
    {
        ResetTransientState();
        if (m_keyboardHook)
        {
            UnhookWindowsHookEx(m_keyboardHook);
            m_keyboardHook = nullptr;
        }
        if (m_mouseHook)
        {
            UnhookWindowsHookEx(m_mouseHook);
            m_mouseHook = nullptr;
        }
        if (s_instance == this)
            s_instance = nullptr;
    }

    void InputEngine::UpdateConfig(const SwitcherConfig& config)
    {
        m_config = config;
    }

    void InputEngine::SetSuspended(bool suspended) noexcept
    {
        m_suspended = suspended;
        if (suspended)
            ResetTransientState();
    }

    void InputEngine::ResetTransientState() noexcept
    {
        m_triggerHeld = false;
        m_mouseTrigger = false;
        m_rightButtonDown = false;
        m_chordActive = false;
        m_lastTapQpc = 0;
    }

    LRESULT CALLBACK InputEngine::KeyboardHookProc(int code, WPARAM wp, LPARAM lp)
    {
        try
        {
            if (code >= 0 && s_instance)
                return s_instance->HandleKeyboard(code, wp, lp);
        }
        catch (...) {}
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    LRESULT CALLBACK InputEngine::MouseHookProc(int code, WPARAM wp, LPARAM lp)
    {
        try
        {
            if (code >= 0 && s_instance)
                return s_instance->HandleMouse(code, wp, lp);
        }
        catch (...) {}
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    bool InputEngine::IsHotkeyMatch(DWORD vkCode) const noexcept
    {
        if (!m_config.hotkeyEnabled || vkCode != m_config.hotkeyVk)
            return false;

        const bool wantAlt = (m_config.hotkeyModifiers & MOD_ALT) != 0;
        const bool wantCtrl = (m_config.hotkeyModifiers & MOD_CONTROL) != 0;
        const bool wantShift = (m_config.hotkeyModifiers & MOD_SHIFT) != 0;
        const bool wantWin = (m_config.hotkeyModifiers & MOD_WIN) != 0;

        const bool hasAlt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        const bool hasCtrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool hasShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool hasWin = ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) ||
            ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);

        return wantAlt == hasAlt && wantCtrl == hasCtrl &&
            wantShift == hasShift && wantWin == hasWin;
    }

    bool InputEngine::IsHotkeyModifierVk(DWORD vkCode) const noexcept
    {
        if ((m_config.hotkeyModifiers & MOD_ALT) &&
            (vkCode == VK_MENU || vkCode == VK_LMENU || vkCode == VK_RMENU))
            return true;
        if ((m_config.hotkeyModifiers & MOD_CONTROL) &&
            (vkCode == VK_CONTROL || vkCode == VK_LCONTROL || vkCode == VK_RCONTROL))
            return true;
        if ((m_config.hotkeyModifiers & MOD_SHIFT) &&
            (vkCode == VK_SHIFT || vkCode == VK_LSHIFT || vkCode == VK_RSHIFT))
            return true;
        if ((m_config.hotkeyModifiers & MOD_WIN) &&
            (vkCode == VK_LWIN || vkCode == VK_RWIN))
            return true;
        return false;
    }

    LRESULT InputEngine::HandleKeyboard(int code, WPARAM wp, LPARAM lp)
    {
        if (m_suspended)
            return CallNextHookEx(m_keyboardHook, code, wp, lp);

        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (!kb)
            return CallNextHookEx(m_keyboardHook, code, wp, lp);

        const bool isDown = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        const bool isUp = (wp == WM_KEYUP || wp == WM_SYSKEYUP);
        const bool ringOpen = m_callbacks.isRingOpen && m_callbacks.isRingOpen();

        if (isDown)
        {
            if (ringOpen && kb->vkCode == VK_ESCAPE)
            {
                m_triggerHeld = false;
                if (m_callbacks.onCancelRequested) m_callbacks.onCancelRequested();
                return 1;
            }

            if (IsHotkeyMatch(kb->vkCode) && !m_triggerHeld)
            {
                StartTrigger(false);
                return 1;
            }

            if (m_config.menuKeyEnabled && kb->vkCode == VK_APPS)
            {
                if (m_callbacks.onToggleRequested) m_callbacks.onToggleRequested();
                return 1;
            }

            if (m_config.interceptWinTab && kb->vkCode == VK_TAB &&
                (((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) || ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0)))
            {
                if (m_callbacks.onToggleRequested) m_callbacks.onToggleRequested();
                return 1;
            }

            if (m_config.interceptAltTab && kb->vkCode == VK_TAB &&
                ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0))
            {
                if (m_callbacks.onToggleRequested) m_callbacks.onToggleRequested();
                return 1;
            }

            // Sticky mode navigation & type-to-find without stealing foreground focus
            if (ringOpen && m_callbacks.isRingLockedOpen && m_callbacks.isRingLockedOpen())
            {
                if (kb->vkCode == VK_RETURN)
                {
                    if (m_callbacks.onCommitRequested) m_callbacks.onCommitRequested();
                    return 1;
                }
                if (kb->vkCode == VK_RIGHT || kb->vkCode == VK_DOWN || kb->vkCode == VK_TAB)
                {
                    if (m_callbacks.onArrowStep) m_callbacks.onArrowStep(-120);
                    return 1;
                }
                if (kb->vkCode == VK_LEFT || kb->vkCode == VK_UP)
                {
                    if (m_callbacks.onArrowStep) m_callbacks.onArrowStep(120);
                    return 1;
                }
                if (kb->vkCode == VK_BACK)
                {
                    if (m_callbacks.onFilterBackspace) m_callbacks.onFilterBackspace();
                    return 1;
                }
                const bool ctrlOrAlt = ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) ||
                    ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
                if (!ctrlOrAlt)
                {
                    if (kb->vkCode >= 'A' && kb->vkCode <= 'Z')
                    {
                        const wchar_t ch = static_cast<wchar_t>(L'a' + (kb->vkCode - 'A'));
                        if (m_callbacks.onFilterChar) m_callbacks.onFilterChar(ch);
                        return 1;
                    }
                    if (kb->vkCode >= '0' && kb->vkCode <= '9')
                    {
                        const wchar_t ch = static_cast<wchar_t>(L'0' + (kb->vkCode - '0'));
                        if (m_callbacks.onFilterChar) m_callbacks.onFilterChar(ch);
                        return 1;
                    }
                }
            }
        }
        else if (isUp)
        {
            if (m_config.menuKeyEnabled && kb->vkCode == VK_APPS)
                return 1;

            if (m_triggerHeld && !m_mouseTrigger &&
                (kb->vkCode == m_config.hotkeyVk || IsHotkeyModifierVk(kb->vkCode)))
            {
                EndTrigger();
            }
        }

        return CallNextHookEx(m_keyboardHook, code, wp, lp);
    }

    bool InputEngine::IsConfiguredMouseDown(UINT msg, DWORD mouseData) const noexcept
    {
        const WORD xButton = HIWORD(mouseData);
        switch (m_config.mouseTrigger)
        {
        case MouseTriggerButton::Middle: return msg == WM_MBUTTONDOWN;
        case MouseTriggerButton::SideBack: return msg == WM_XBUTTONDOWN && xButton == XBUTTON1;
        case MouseTriggerButton::SideForward: return msg == WM_XBUTTONDOWN && xButton == XBUTTON2;
        default: return false;
        }
    }

    bool InputEngine::IsConfiguredMouseUp(UINT msg, DWORD mouseData) const noexcept
    {
        const WORD xButton = HIWORD(mouseData);
        switch (m_config.mouseTrigger)
        {
        case MouseTriggerButton::Middle: return msg == WM_MBUTTONUP;
        case MouseTriggerButton::SideBack: return msg == WM_XBUTTONUP && xButton == XBUTTON1;
        case MouseTriggerButton::SideForward: return msg == WM_XBUTTONUP && xButton == XBUTTON2;
        default: return false;
        }
    }

    LRESULT InputEngine::HandleMouse(int code, WPARAM wp, LPARAM lp)
    {
        if (m_suspended)
            return CallNextHookEx(m_mouseHook, code, wp, lp);

        const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        if (!ms)
            return CallNextHookEx(m_mouseHook, code, wp, lp);

        const UINT msg = static_cast<UINT>(wp);
        const bool ringOpen = m_callbacks.isRingOpen && m_callbacks.isRingOpen();

        if (msg == WM_MOUSEMOVE)
        {
            if ((ms->flags & LLMHF_INJECTED) == 0 && m_callbacks.onUserPointerDelta)
                m_callbacks.onUserPointerDelta();
            if (ringOpen || m_triggerHeld)
            {
                if (m_callbacks.onMouseMoved)
                    m_callbacks.onMouseMoved(ms->pt.x, ms->pt.y);
            }
            return CallNextHookEx(m_mouseHook, code, wp, lp);
        }

        if (msg == WM_MOUSEWHEEL && ringOpen &&
            m_config.scrollWheelMode != ScrollWheelBehavior::Disabled)
        {
            const short delta = static_cast<short>(HIWORD(ms->mouseData));
            if (delta != 0)
            {
                if (m_callbacks.onMouseWheel)
                    m_callbacks.onMouseWheel(delta);
                return 1;
            }
        }

        // Right+Left chord trigger support (non-blocking for normal right-click)
        if (m_config.mouseTrigger == MouseTriggerButton::RightLeftChord)
        {
            if (msg == WM_RBUTTONDOWN)
            {
                m_rightButtonDown = true;
            }
            else if (msg == WM_LBUTTONDOWN && m_rightButtonDown && !m_triggerHeld)
            {
                m_chordActive = true;
                StartTrigger(true);
                return 1;
            }
            else if (msg == WM_LBUTTONUP && m_chordActive && m_triggerHeld)
            {
                m_chordActive = false;
                EndTrigger();
                return 1;
            }
            else if (msg == WM_RBUTTONUP)
            {
                m_rightButtonDown = false;
                if (m_chordActive)
                {
                    m_chordActive = false;
                    if (m_triggerHeld)
                        EndTrigger();
                    return 1;
                }
            }
        }
        else
        {
            if (IsConfiguredMouseDown(msg, ms->mouseData) && !m_triggerHeld)
            {
                StartTrigger(true);
                return 1;
            }
            if (IsConfiguredMouseUp(msg, ms->mouseData) && m_triggerHeld && m_mouseTrigger)
            {
                EndTrigger();
                return 1;
            }
        }

        // Click outside the overlay in sticky mode cancels cleanly
        if (msg == WM_LBUTTONDOWN && ringOpen &&
            m_callbacks.isRingLockedOpen && m_callbacks.isRingLockedOpen())
        {
            if (m_callbacks.isPointInOverlay && !m_callbacks.isPointInOverlay(ms->pt))
            {
                if (m_callbacks.onCancelRequested)
                    m_callbacks.onCancelRequested();
            }
        }

        return CallNextHookEx(m_mouseHook, code, wp, lp);
    }

    void InputEngine::StartTrigger(bool mouseTrigger)
    {
        m_triggerHeld = true;
        m_mouseTrigger = mouseTrigger;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        m_triggerDownQpc = now.QuadPart;
        GetCursorPos(&m_triggerOrigin);
        if (m_callbacks.onTriggerPressed)
            m_callbacks.onTriggerPressed(m_triggerOrigin.x, m_triggerOrigin.y);
    }

    void InputEngine::EndTrigger()
    {
        m_triggerHeld = false;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        POINT releasePt{};
        GetCursorPos(&releasePt);

        const double elapsedMs = static_cast<double>(now.QuadPart - m_triggerDownQpc) * 1000.0 /
            static_cast<double>(m_qpcFreq);
        const double dpiScale = m_callbacks.getOverlayDpiScale ? m_callbacks.getOverlayDpiScale() : 1.0;
        const double movedDip = std::hypot(
            static_cast<double>(releasePt.x - m_triggerOrigin.x),
            static_cast<double>(releasePt.y - m_triggerOrigin.y)) / (dpiScale > 0.0 ? dpiScale : 1.0);
        const bool isTap = (elapsedMs < static_cast<double>(m_config.tapThresholdMs)) &&
            (movedDip < m_config.deadzoneDip);

        if (isTap && m_config.doubleTapLastApp)
        {
            const double sinceLastMs = (m_lastTapQpc > 0)
                ? (static_cast<double>(now.QuadPart - m_lastTapQpc) * 1000.0 / static_cast<double>(m_qpcFreq))
                : 1e9;
            if (sinceLastMs <= static_cast<double>(m_config.doubleTapWindowMs))
            {
                m_lastTapQpc = 0;
                if (m_callbacks.onDoubleTapLastApp)
                    m_callbacks.onDoubleTapLastApp();
                return;
            }
            m_lastTapQpc = now.QuadPart;
        }
        else
        {
            m_lastTapQpc = 0;
        }

        if (m_callbacks.onTriggerReleased)
            m_callbacks.onTriggerReleased(releasePt.x, releasePt.y, isTap);
    }
} // namespace winrings
