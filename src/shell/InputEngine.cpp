#include "InputEngine.h"

#include <algorithm>
#include <cmath>

namespace winrings
{
    namespace
    {
        constexpr ULONG_PTR kWinRingsInjectedExtra = 0x5752494Eu; // 'WRIN'
        constexpr WORD kMaskVirtualKey = 0xE8; // VK_NONAME harmless mask key
    }

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
        m_dismissedLockedOnPress = false;
        m_activeBinding = {};
        m_soloModCandidate = false;
        m_soloModVk = 0;
        m_soloModChorded = false;
        m_soloModProfileId.clear();
        m_winKeyDown = false;
        m_winKeyDownQpc = 0;
        m_rightButtonDown = false;
        m_chordActive = false;
        m_outsideClickPending = false;
        m_outsideClickOrigin = {};
        m_lastTapQpc = 0;
        m_lastTapProfileId.clear();
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

    bool InputEngine::AreModifiersExact(UINT requiredMods, UINT ignoreModVk) const noexcept
    {
        const UINT normIgnore = NormalizeTriggerVk(ignoreModVk);
        const bool wantAlt = (requiredMods & MOD_ALT) != 0;
        const bool wantCtrl = (requiredMods & MOD_CONTROL) != 0;
        const bool wantShift = (requiredMods & MOD_SHIFT) != 0;
        const bool wantWin = (requiredMods & MOD_WIN) != 0;

        const bool hasAlt = (normIgnore == VK_MENU) ? false : ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
        const bool hasCtrl = (normIgnore == VK_CONTROL) ? false : ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
        const bool hasShift = (normIgnore == VK_SHIFT) ? false : ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
        const bool hasWin = (normIgnore == VK_LWIN) ? false
            : (((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) || ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0));

        return wantAlt == hasAlt && wantCtrl == hasCtrl && wantShift == hasShift && wantWin == hasWin;
    }

    int InputEngine::FindMatchingKeyChord(DWORD rawVk, bool allowExtraShift) const noexcept
    {
        const UINT normVk = NormalizeTriggerVk(rawVk);
        if (IsModifierVk(normVk) || normVk == 0)
            return -1;

        for (size_t i = 0; i < m_config.triggers.size(); ++i)
        {
            const TriggerBinding& b = m_config.triggers[i];
            if (b.kind != TriggerKind::KeyChord)
                continue;
            if (NormalizeTriggerVk(b.vk) != normVk)
                continue;
            if (AreModifiersExact(b.modifiers))
                return static_cast<int>(i);
            if (allowExtraShift && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 &&
                AreModifiersExact(b.modifiers | MOD_SHIFT))
                return static_cast<int>(i);
        }
        return -1;
    }

    int InputEngine::FindMatchingSoloModifier(DWORD rawVk) const noexcept
    {
        const UINT normVk = NormalizeTriggerVk(rawVk);
        if (!IsModifierVk(normVk))
            return -1;
        if (!AreModifiersExact(0, normVk))
            return -1;

        for (size_t i = 0; i < m_config.triggers.size(); ++i)
        {
            const TriggerBinding& b = m_config.triggers[i];
            if (b.kind == TriggerKind::SoloModifier && NormalizeTriggerVk(b.vk) == normVk)
                return static_cast<int>(i);
        }
        return -1;
    }

    int InputEngine::FindMatchingMouseTrigger(UINT msg, DWORD mouseData, bool& outIsUp) const noexcept
    {
        outIsUp = false;
        MouseTriggerButton btn = MouseTriggerButton::Disabled;
        const WORD xButton = HIWORD(mouseData);

        if (msg == WM_MBUTTONDOWN) { btn = MouseTriggerButton::Middle; outIsUp = false; }
        else if (msg == WM_MBUTTONUP) { btn = MouseTriggerButton::Middle; outIsUp = true; }
        else if (msg == WM_XBUTTONDOWN && xButton == XBUTTON1) { btn = MouseTriggerButton::SideBack; outIsUp = false; }
        else if (msg == WM_XBUTTONUP && xButton == XBUTTON1) { btn = MouseTriggerButton::SideBack; outIsUp = true; }
        else if (msg == WM_XBUTTONDOWN && xButton == XBUTTON2) { btn = MouseTriggerButton::SideForward; outIsUp = false; }
        else if (msg == WM_XBUTTONUP && xButton == XBUTTON2) { btn = MouseTriggerButton::SideForward; outIsUp = true; }
        else
            return -1;

        for (size_t i = 0; i < m_config.triggers.size(); ++i)
        {
            const TriggerBinding& b = m_config.triggers[i];
            if (b.kind != TriggerKind::MouseButton || b.mouseButton != btn)
                continue;
            if (outIsUp || AreModifiersExact(b.modifiers))
                return static_cast<int>(i);
        }
        return -1;
    }

    int InputEngine::FindRightLeftChordTrigger() const noexcept
    {
        for (size_t i = 0; i < m_config.triggers.size(); ++i)
        {
            const TriggerBinding& b = m_config.triggers[i];
            if (b.kind == TriggerKind::RightLeftChord && AreModifiersExact(b.modifiers))
                return static_cast<int>(i);
        }
        return -1;
    }

    bool InputEngine::IsKeyPartOfActiveBinding(DWORD normVk) const noexcept
    {
        if (m_activeBinding.kind != TriggerKind::KeyChord)
            return false;

        const UINT activeVk = NormalizeTriggerVk(m_activeBinding.vk);
        const bool isTabChord = (activeVk == VK_TAB && m_activeBinding.modifiers != 0);
        if (!isTabChord && normVk == activeVk)
            return true;

        const UINT modFlag = ModifierFlagFromVk(normVk);
        return (modFlag != 0) && ((m_activeBinding.modifiers & modFlag) != 0);
    }

    bool InputEngine::IsSameOpenProfile(const std::wstring& triggerProf) const noexcept
    {
        std::wstring active = m_callbacks.getActiveProfileId ? m_callbacks.getActiveProfileId() : std::wstring(L"default");
        if (active.empty())
            active = L"default";
        const wchar_t* trig = triggerProf.empty() ? L"default" : triggerProf.c_str();
        return _wcsicmp(active.c_str(), trig) == 0;
    }

    bool InputEngine::IsWinKeyHeld() const noexcept
    {
        const bool asyncWinDown =
            ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) ||
            ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);
        double msSinceHook = -1.0;
        if (m_winKeyDown && m_winKeyDownQpc > 0)
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            msSinceHook = static_cast<double>(now.QuadPart - m_winKeyDownQpc) * 1000.0 /
                static_cast<double>(m_qpcFreq);
        }
        const bool soloModWinActive = m_soloModCandidate && m_soloModVk == VK_LWIN;
        return ResolveWinKeyHeld(asyncWinDown, m_winKeyDown, msSinceHook, soloModWinActive);
    }

    void InputEngine::InjectModifierMaskTap() noexcept
    {
        INPUT inputs[2]{};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = kMaskVirtualKey;
        inputs[0].ki.dwExtraInfo = kWinRingsInjectedExtra;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wVk = kMaskVirtualKey;
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
        inputs[1].ki.dwExtraInfo = kWinRingsInjectedExtra;
        SendInput(2, inputs, sizeof(INPUT));
    }

    bool InputEngine::TryConsumeDoubleTapOnPress(const std::wstring& profileId) noexcept
    {
        const std::wstring normProf = profileId.empty() ? std::wstring(L"default") : profileId;
        if (!m_config.doubleTapLastApp || m_lastTapQpc <= 0 ||
            !m_config.ResolveProfile(normProf).includeRunningApps ||
            _wcsicmp(m_lastTapProfileId.c_str(), normProf.c_str()) != 0)
        {
            m_lastTapQpc = 0;
            m_lastTapProfileId.clear();
            return false;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double sinceLastMs = static_cast<double>(now.QuadPart - m_lastTapQpc) * 1000.0 /
            static_cast<double>(m_qpcFreq);
        m_lastTapQpc = 0;
        m_lastTapProfileId.clear();
        if (sinceLastMs <= static_cast<double>(m_config.doubleTapWindowMs))
        {
            if (m_callbacks.onDoubleTapLastApp)
                m_callbacks.onDoubleTapLastApp();
            m_dismissedLockedOnPress = true;
            return true;
        }
        return false;
    }

    LRESULT InputEngine::HandleKeyboard(int code, WPARAM wp, LPARAM lp)
    {
        if (m_suspended)
            return CallNextHookEx(m_keyboardHook, code, wp, lp);

        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (!kb)
            return CallNextHookEx(m_keyboardHook, code, wp, lp);

        if (kb->dwExtraInfo == kWinRingsInjectedExtra || kb->vkCode == kMaskVirtualKey)
            return CallNextHookEx(m_keyboardHook, code, wp, lp);

        const bool isDown = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        const bool isUp = (wp == WM_KEYUP || wp == WM_SYSKEYUP);
        const UINT normVk = NormalizeTriggerVk(kb->vkCode);
        const bool ringOpen = m_callbacks.isRingOpen && m_callbacks.isRingOpen();
        const bool sticky = m_callbacks.isRingLockedOpen && m_callbacks.isRingLockedOpen();
        const bool trayPreview = m_callbacks.isTrayPreviewOpen && m_callbacks.isTrayPreviewOpen();

        if (isDown)
        {
            if (normVk == VK_LWIN)
            {
                m_winKeyDown = true;
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                m_winKeyDownQpc = now.QuadPart;
            }
            const bool winHeld = IsWinKeyHeld();

            if (m_soloModCandidate && normVk != m_soloModVk)
                m_soloModChorded = true;

            if (ringOpen && !winHeld && kb->vkCode == VK_ESCAPE)
            {
                m_triggerHeld = false;
                m_dismissedLockedOnPress = false;
                if (m_callbacks.onCancelRequested) m_callbacks.onCancelRequested();
                return 1;
            }

            if (!m_triggerHeld && !m_dismissedLockedOnPress)
            {
                const int chordIdx = FindMatchingKeyChord(kb->vkCode, false);
                if (chordIdx >= 0)
                {
                    const TriggerBinding& matched = m_config.triggers[static_cast<size_t>(chordIdx)];
                    if (TryConsumeDoubleTapOnPress(matched.profileId))
                    {
                        if ((matched.modifiers & (MOD_WIN | MOD_ALT)) != 0)
                            InjectModifierMaskTap();
                        return 1;
                    }
                    if (ringOpen && sticky && m_config.gestureTapOpenClose && IsSameOpenProfile(matched.profileId))
                    {
                        if (m_callbacks.onCancelRequested) m_callbacks.onCancelRequested();
                        m_dismissedLockedOnPress = true;
                        return 1;
                    }
                    StartTrigger(false, matched);
                    return 1;
                }

                const int soloIdx = FindMatchingSoloModifier(kb->vkCode);
                if (!m_soloModCandidate && soloIdx >= 0)
                {
                    m_soloModCandidate = true;
                    m_soloModVk = normVk;
                    m_soloModChorded = false;
                    m_soloModProfileId = m_config.triggers[static_cast<size_t>(soloIdx)].profileId;
                    // Never swallow modifier keydown so Win+R, Alt+F4, Ctrl+C, etc. keep working.
                    return CallNextHookEx(m_keyboardHook, code, wp, lp);
                }
            }
            else if (m_triggerHeld && !m_mouseTrigger &&
                m_activeBinding.kind == TriggerKind::KeyChord &&
                NormalizeTriggerVk(m_activeBinding.vk) == normVk)
            {
                // Repeated Tab while holding Alt/Win steps slots while held!
                if (ringOpen && normVk == VK_TAB)
                {
                    const bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                    if (m_callbacks.onArrowStep) m_callbacks.onArrowStep(shiftHeld ? 120 : -120);
                }
                return 1;
            }

            // Never swallow Windows system chords (Win+I, Win+R, Win+Shift+S, Win+Arrows, Win+Esc, etc.)
            if (ShouldPassThroughWinSystemChord(winHeld, kb->vkCode))
            {
                if (ringOpen)
                {
                    m_triggerHeld = false;
                    m_dismissedLockedOnPress = false;
                    if (m_callbacks.onCancelRequested)
                        m_callbacks.onCancelRequested();
                }
                return CallNextHookEx(m_keyboardHook, code, wp, lp);
            }

            // Type-to-find & keyboard navigation whenever the ring is open (except when hosted over Settings UI)
            if (ringOpen && !trayPreview)
            {
                const bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                const bool ctrlOrAlt = ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) ||
                    ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);

                if (!ctrlOrAlt && kb->vkCode == VK_TAB)
                {
                    if (m_callbacks.onArrowStep) m_callbacks.onArrowStep(shiftHeld ? 120 : -120);
                    return 1;
                }
                if (sticky && !ctrlOrAlt)
                {
                    if (kb->vkCode == VK_RETURN)
                    {
                        if (m_callbacks.onCommitRequested) m_callbacks.onCommitRequested();
                        return 1;
                    }
                    if (kb->vkCode == VK_RIGHT || kb->vkCode == VK_DOWN)
                    {
                        if (m_callbacks.onArrowStep) m_callbacks.onArrowStep(-120);
                        return 1;
                    }
                    if (kb->vkCode == VK_LEFT || kb->vkCode == VK_UP)
                    {
                        if (m_callbacks.onArrowStep) m_callbacks.onArrowStep(120);
                        return 1;
                    }
                }
                if (!ctrlOrAlt && kb->vkCode == VK_BACK)
                {
                    if (m_callbacks.onFilterBackspace) m_callbacks.onFilterBackspace();
                    return 1;
                }
                if (!ctrlOrAlt && !m_triggerHeld)
                {
                    wchar_t typedChar = 0;
                    if (kb->vkCode >= 'A' && kb->vkCode <= 'Z')
                        typedChar = static_cast<wchar_t>(L'a' + (kb->vkCode - 'A'));
                    else if (kb->vkCode >= '0' && kb->vkCode <= '9')
                        typedChar = static_cast<wchar_t>(L'0' + (kb->vkCode - '0'));
                    else if (kb->vkCode >= VK_NUMPAD0 && kb->vkCode <= VK_NUMPAD9)
                        typedChar = static_cast<wchar_t>(L'0' + (kb->vkCode - VK_NUMPAD0));
                    else if (kb->vkCode == VK_SPACE)
                        typedChar = L' ';
                    else if (kb->vkCode == VK_OEM_MINUS || kb->vkCode == VK_SUBTRACT)
                        typedChar = L'-';
                    else if (kb->vkCode == VK_OEM_PERIOD || kb->vkCode == VK_DECIMAL)
                        typedChar = L'.';

                    if (typedChar != 0)
                    {
                        if (m_callbacks.onFilterChar) m_callbacks.onFilterChar(typedChar);
                        return 1;
                    }
                }
            }
        }
        else if (isUp)
        {
            if (normVk == VK_LWIN)
            {
                m_winKeyDown = false;
                m_winKeyDownQpc = 0;
            }
            if (m_dismissedLockedOnPress)
            {
                if (!IsModifierVk(normVk))
                {
                    m_dismissedLockedOnPress = false;
                    return 1;
                }
                return CallNextHookEx(m_keyboardHook, code, wp, lp);
            }

            if (m_soloModCandidate && normVk == m_soloModVk)
            {
                const bool wasChorded = m_soloModChorded;
                const std::wstring soloProf = m_soloModProfileId;
                m_soloModCandidate = false;
                m_soloModVk = 0;
                m_soloModChorded = false;
                m_soloModProfileId.clear();

                if (!wasChorded)
                {
                    if (normVk == VK_LWIN || normVk == VK_MENU)
                        InjectModifierMaskTap();

                    const std::wstring normProf = soloProf.empty() ? std::wstring(L"default") : soloProf;
                    const bool allowDoubleTap = m_config.doubleTapLastApp &&
                        m_config.ResolveProfile(normProf).includeRunningApps;
                    LARGE_INTEGER now{};
                    QueryPerformanceCounter(&now);
                    bool didDoubleTap = false;
                    if (allowDoubleTap && m_lastTapQpc > 0 &&
                        _wcsicmp(m_lastTapProfileId.c_str(), normProf.c_str()) == 0)
                    {
                        const double sinceLastMs = static_cast<double>(now.QuadPart - m_lastTapQpc) * 1000.0 /
                            static_cast<double>(m_qpcFreq);
                        if (sinceLastMs <= static_cast<double>(m_config.doubleTapWindowMs))
                        {
                            m_lastTapQpc = 0;
                            m_lastTapProfileId.clear();
                            didDoubleTap = true;
                            if (m_callbacks.onDoubleTapLastApp)
                                m_callbacks.onDoubleTapLastApp();
                        }
                    }
                    if (!didDoubleTap)
                    {
                        if (allowDoubleTap)
                        {
                            m_lastTapQpc = now.QuadPart;
                            m_lastTapProfileId = normProf;
                        }
                        else
                        {
                            m_lastTapQpc = 0;
                            m_lastTapProfileId.clear();
                        }
                        if (m_config.gestureTapOpenClose && m_callbacks.onToggleRequested)
                            m_callbacks.onToggleRequested(soloProf);
                    }
                }
                return CallNextHookEx(m_keyboardHook, code, wp, lp);
            }

            if (m_triggerHeld && !m_mouseTrigger && IsKeyPartOfActiveBinding(normVk))
            {
                EndTrigger();
                if (!IsModifierVk(normVk))
                    return 1;
            }
        }

        return CallNextHookEx(m_keyboardHook, code, wp, lp);
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
        const bool sticky = m_callbacks.isRingLockedOpen && m_callbacks.isRingLockedOpen();
        const bool trayPreview = m_callbacks.isTrayPreviewOpen && m_callbacks.isTrayPreviewOpen();

        if (msg == WM_MOUSEMOVE)
        {
            if (m_outsideClickPending)
            {
                const int dragThresh = (std::max)(5, (std::max)(GetSystemMetrics(SM_CXDRAG), GetSystemMetrics(SM_CYDRAG)));
                if (std::abs(ms->pt.x - m_outsideClickOrigin.x) >= dragThresh ||
                    std::abs(ms->pt.y - m_outsideClickOrigin.y) >= dragThresh)
                {
                    m_outsideClickPending = false;
                }
            }
            if ((ms->flags & LLMHF_INJECTED) == 0 && m_callbacks.onUserPointerDelta)
                m_callbacks.onUserPointerDelta(ms->pt);
            if (ringOpen || m_triggerHeld)
            {
                if (m_callbacks.onMouseMoved)
                    m_callbacks.onMouseMoved(ms->pt.x, ms->pt.y);
            }
            return CallNextHookEx(m_mouseHook, code, wp, lp);
        }

        if (m_soloModCandidate &&
            (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN ||
             msg == WM_XBUTTONDOWN || msg == WM_MOUSEWHEEL))
        {
            m_soloModChorded = true;
        }

        if (msg == WM_MOUSEWHEEL && ringOpen &&
            m_config.scrollWheelMode != ScrollWheelBehavior::Disabled &&
            (!trayPreview || (m_callbacks.isPointInOverlay && m_callbacks.isPointInOverlay(ms->pt))))
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
        const int rlIdx = FindRightLeftChordTrigger();
        if (rlIdx >= 0 || m_chordActive)
        {
            if (msg == WM_RBUTTONDOWN)
            {
                m_rightButtonDown = true;
            }
            else if (msg == WM_LBUTTONDOWN && m_rightButtonDown && !m_triggerHeld && rlIdx >= 0)
            {
                const TriggerBinding& matched = m_config.triggers[static_cast<size_t>(rlIdx)];
                if (TryConsumeDoubleTapOnPress(matched.profileId))
                    return 1;
                if (ringOpen && sticky && m_config.gestureTapOpenClose && IsSameOpenProfile(matched.profileId))
                {
                    if (m_callbacks.onCancelRequested) m_callbacks.onCancelRequested();
                    m_dismissedLockedOnPress = true;
                    return 1;
                }
                m_chordActive = true;
                StartTrigger(true, matched);
                return 1;
            }
            else if (msg == WM_LBUTTONUP && m_dismissedLockedOnPress)
            {
                m_dismissedLockedOnPress = false;
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

        // Standard mouse button triggers (with optional modifier filter)
        bool isMouseUp = false;
        const int mouseIdx = FindMatchingMouseTrigger(msg, ms->mouseData, isMouseUp);
        if (mouseIdx >= 0)
        {
            if (!isMouseUp && !m_triggerHeld)
            {
                const TriggerBinding& matched = m_config.triggers[static_cast<size_t>(mouseIdx)];
                if (TryConsumeDoubleTapOnPress(matched.profileId))
                    return 1;
                if (ringOpen && sticky && m_config.gestureTapOpenClose && IsSameOpenProfile(matched.profileId))
                {
                    if (m_callbacks.onCancelRequested) m_callbacks.onCancelRequested();
                    m_dismissedLockedOnPress = true;
                    return 1;
                }
                StartTrigger(true, matched);
                return 1;
            }
            if (isMouseUp && m_dismissedLockedOnPress)
            {
                m_dismissedLockedOnPress = false;
                return 1;
            }
            if (isMouseUp && m_triggerHeld && m_mouseTrigger &&
                m_activeBinding.kind == TriggerKind::MouseButton &&
                m_activeBinding.mouseButton == m_config.triggers[static_cast<size_t>(mouseIdx)].mouseButton)
            {
                EndTrigger();
                return 1;
            }
            return 1;
        }

        // Click outside the overlay in sticky mode cancels only on clean click-release (not when dragging or over Settings UI)
        if (msg == WM_LBUTTONDOWN && ringOpen && sticky && !trayPreview)
        {
            const bool dragActive = m_callbacks.isRingDragActive && m_callbacks.isRingDragActive();
            if (!dragActive && (!m_callbacks.isPointInOverlay || !m_callbacks.isPointInOverlay(ms->pt)))
            {
                m_outsideClickPending = true;
                m_outsideClickOrigin = ms->pt;
            }
            else
            {
                m_outsideClickPending = false;
            }
        }
        else if (msg == WM_LBUTTONUP && m_outsideClickPending)
        {
            m_outsideClickPending = false;
            const bool dragActive = m_callbacks.isRingDragActive && m_callbacks.isRingDragActive();
            if (ringOpen && sticky &&
                !dragActive && (!m_callbacks.isPointInOverlay || !m_callbacks.isPointInOverlay(ms->pt)))
            {
                if (m_callbacks.onCancelRequested)
                    m_callbacks.onCancelRequested();
            }
        }

        return CallNextHookEx(m_mouseHook, code, wp, lp);
    }

    void InputEngine::StartTrigger(bool mouseTrigger, const TriggerBinding& binding)
    {
        if ((binding.modifiers & (MOD_WIN | MOD_ALT)) != 0)
            InjectModifierMaskTap();
        m_triggerHeld = true;
        m_mouseTrigger = mouseTrigger;
        m_activeBinding = binding;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        m_triggerDownQpc = now.QuadPart;
        GetCursorPos(&m_triggerOrigin);
        if (m_callbacks.onTriggerPressed)
            m_callbacks.onTriggerPressed(m_triggerOrigin.x, m_triggerOrigin.y, m_activeBinding.profileId);
    }

    void InputEngine::EndTrigger()
    {
        const std::wstring normProf = m_activeBinding.profileId.empty() ? std::wstring(L"default") : m_activeBinding.profileId;
        m_triggerHeld = false;
        m_activeBinding = {};
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
        const bool lbuttonHeld = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const bool isTap = lbuttonHeld ||
            ((elapsedMs < static_cast<double>(m_config.tapThresholdMs)) &&
             (movedDip < m_config.deadzoneDip));

        const bool allowDoubleTap = !lbuttonHeld && isTap && m_config.doubleTapLastApp &&
            m_config.ResolveProfile(normProf).includeRunningApps;
        if (allowDoubleTap)
        {
            const double sinceLastMs = (m_lastTapQpc > 0 && _wcsicmp(m_lastTapProfileId.c_str(), normProf.c_str()) == 0)
                ? (static_cast<double>(now.QuadPart - m_lastTapQpc) * 1000.0 / static_cast<double>(m_qpcFreq))
                : 1e9;
            if (sinceLastMs <= static_cast<double>(m_config.doubleTapWindowMs))
            {
                m_lastTapQpc = 0;
                m_lastTapProfileId.clear();
                if (m_callbacks.onDoubleTapLastApp)
                    m_callbacks.onDoubleTapLastApp();
                return;
            }
            m_lastTapQpc = now.QuadPart;
            m_lastTapProfileId = normProf;
        }
        else
        {
            m_lastTapQpc = 0;
            m_lastTapProfileId.clear();
        }

        if (isTap)
        {
            if (m_config.gestureTapOpenClose)
            {
                if (m_callbacks.onTriggerReleased)
                    m_callbacks.onTriggerReleased(releasePt.x, releasePt.y, true);
            }
            else if (m_callbacks.onCancelRequested)
            {
                m_callbacks.onCancelRequested();
            }
            return;
        }

        if (m_config.gestureHoldReleaseSwitch)
        {
            if (m_callbacks.onTriggerReleased)
                m_callbacks.onTriggerReleased(releasePt.x, releasePt.y, false);
        }
        else if (m_config.gestureTapOpenClose)
        {
            if (m_callbacks.onTriggerReleased)
                m_callbacks.onTriggerReleased(releasePt.x, releasePt.y, true);
        }
        else if (m_callbacks.onCancelRequested)
        {
            m_callbacks.onCancelRequested();
        }
    }
} // namespace winrings
