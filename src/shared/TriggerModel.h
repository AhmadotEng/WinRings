#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <string>

namespace winrings
{
    inline constexpr size_t kMaxTriggerBindings = 8;

    enum class MouseTriggerButton
    {
        SideBack,
        SideForward,
        Middle,
        RightLeftChord,
        Disabled
    };

    enum class TriggerKind : uint8_t
    {
        Disabled = 0,
        MouseButton = 1,
        RightLeftChord = 2,
        KeyChord = 3,
        SoloModifier = 4
    };

    struct TriggerBinding
    {
        TriggerKind kind = TriggerKind::Disabled;
        MouseTriggerButton mouseButton = MouseTriggerButton::Disabled;
        UINT modifiers = 0; // MOD_ALT, MOD_CONTROL, MOD_SHIFT, MOD_WIN
        UINT vk = 0;        // Normalized VK (for SoloModifier: VK_LWIN, VK_MENU, VK_CONTROL, VK_SHIFT)
        std::wstring profileId = L"default";

        bool IsActive() const noexcept
        {
            return kind != TriggerKind::Disabled;
        }
    };

    struct TriggerConflictInfo
    {
        bool hasWarning = false;
        std::wstring summary;
    };

    inline constexpr const wchar_t* ToString(MouseTriggerButton v) noexcept
    {
        switch (v)
        {
        case MouseTriggerButton::SideBack: return L"SideBack";
        case MouseTriggerButton::SideForward: return L"SideForward";
        case MouseTriggerButton::Middle: return L"Middle";
        case MouseTriggerButton::RightLeftChord: return L"RightLeftChord";
        case MouseTriggerButton::Disabled: return L"Disabled";
        }
        return L"SideBack";
    }

    inline constexpr const wchar_t* ToString(TriggerKind v) noexcept
    {
        switch (v)
        {
        case TriggerKind::Disabled: return L"Disabled";
        case TriggerKind::MouseButton: return L"MouseButton";
        case TriggerKind::RightLeftChord: return L"RightLeftChord";
        case TriggerKind::KeyChord: return L"KeyChord";
        case TriggerKind::SoloModifier: return L"SoloModifier";
        }
        return L"Disabled";
    }

    inline constexpr UINT NormalizeTriggerVk(UINT rawVk) noexcept
    {
        switch (rawVk)
        {
        case VK_RWIN:
        case VK_LWIN:
            return VK_LWIN;
        case VK_LMENU:
        case VK_RMENU:
        case VK_MENU:
            return VK_MENU;
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_CONTROL:
            return VK_CONTROL;
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_SHIFT:
            return VK_SHIFT;
        default:
            return rawVk;
        }
    }

    inline constexpr bool IsModifierVk(UINT vk) noexcept
    {
        switch (vk)
        {
        case VK_LWIN:
        case VK_RWIN:
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
            return true;
        default:
            return false;
        }
    }

    inline constexpr UINT ModifierFlagFromVk(UINT vk) noexcept
    {
        switch (NormalizeTriggerVk(vk))
        {
        case VK_LWIN: return MOD_WIN;
        case VK_MENU: return MOD_ALT;
        case VK_CONTROL: return MOD_CONTROL;
        case VK_SHIFT: return MOD_SHIFT;
        default: return 0;
        }
    }

    inline std::wstring FormatVirtualKeyName(UINT rawVk)
    {
        const UINT vk = NormalizeTriggerVk(rawVk);
        switch (vk)
        {
        case 0: return L"None";
        case VK_LWIN: return L"Win";
        case VK_MENU: return L"Alt";
        case VK_CONTROL: return L"Ctrl";
        case VK_SHIFT: return L"Shift";
        case VK_SPACE: return L"Space";
        case VK_TAB: return L"Tab";
        case VK_RETURN: return L"Enter";
        case VK_ESCAPE: return L"Esc";
        case VK_BACK: return L"Backspace";
        case VK_CAPITAL: return L"Caps Lock";
        case VK_APPS: return L"Menu Key";
        case VK_OEM_3: return L"`";
        case VK_OEM_MINUS: return L"-";
        case VK_OEM_PLUS: return L"=";
        case VK_OEM_4: return L"[";
        case VK_OEM_6: return L"]";
        case VK_OEM_5: return L"\\";
        case VK_OEM_1: return L";";
        case VK_OEM_7: return L"'";
        case VK_OEM_COMMA: return L",";
        case VK_OEM_PERIOD: return L".";
        case VK_OEM_2: return L"/";
        case VK_INSERT: return L"Insert";
        case VK_DELETE: return L"Delete";
        case VK_HOME: return L"Home";
        case VK_END: return L"End";
        case VK_PRIOR: return L"Page Up";
        case VK_NEXT: return L"Page Down";
        case VK_LEFT: return L"Left";
        case VK_RIGHT: return L"Right";
        case VK_UP: return L"Up";
        case VK_DOWN: return L"Down";
        default: break;
        }

        if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
            return std::wstring(1, static_cast<wchar_t>(vk));
        if (vk >= VK_F1 && vk <= VK_F24)
            return L"F" + std::to_wstring(vk - VK_F1 + 1);
        if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
            return L"Num " + std::to_wstring(vk - VK_NUMPAD0);

        const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        if (scan != 0)
        {
            wchar_t buf[64]{};
            if (GetKeyNameTextW(static_cast<LONG>(scan << 16), buf, _countof(buf)) > 0)
                return std::wstring(buf);
        }

        wchar_t hexBuf[16]{};
        swprintf_s(hexBuf, L"Key 0x%02X", vk);
        return hexBuf;
    }

    inline std::wstring FormatTriggerBinding(const TriggerBinding& b)
    {
        if (b.kind == TriggerKind::Disabled)
            return L"Disabled";

        std::wstring mods;
        auto appendMod = [&](UINT flag, const wchar_t* label) {
            if (b.modifiers & flag)
            {
                if (!mods.empty())
                    mods += L" + ";
                mods += label;
            }
        };
        appendMod(MOD_WIN, L"Win");
        appendMod(MOD_CONTROL, L"Ctrl");
        appendMod(MOD_ALT, L"Alt");
        appendMod(MOD_SHIFT, L"Shift");

        if (b.kind == TriggerKind::RightLeftChord)
            return mods.empty() ? L"Right + Left Click" : (mods + L" + Right + Left Click");

        if (b.kind == TriggerKind::MouseButton)
        {
            const wchar_t* btn = L"Disabled";
            switch (b.mouseButton)
            {
            case MouseTriggerButton::SideBack: btn = L"Mouse Back"; break;
            case MouseTriggerButton::SideForward: btn = L"Mouse Forward"; break;
            case MouseTriggerButton::Middle: btn = L"Middle Click"; break;
            case MouseTriggerButton::RightLeftChord: btn = L"Right + Left Click"; break;
            case MouseTriggerButton::Disabled: btn = L"Disabled"; break;
            }
            if (b.mouseButton == MouseTriggerButton::Disabled)
                return L"Disabled";
            return mods.empty() ? std::wstring(btn) : (mods + L" + " + btn);
        }

        if (b.kind == TriggerKind::SoloModifier)
            return FormatVirtualKeyName(b.vk);

        const std::wstring keyName = FormatVirtualKeyName(b.vk);
        return mods.empty() ? keyName : (mods + L" + " + keyName);
    }

    inline TriggerConflictInfo AnalyzeTriggerConflict(const TriggerBinding& b)
    {
        TriggerConflictInfo info;
        if (b.kind == TriggerKind::Disabled)
            return info;

        if (b.kind == TriggerKind::SoloModifier)
        {
            const UINT normVk = NormalizeTriggerVk(b.vk);
            if (normVk == VK_LWIN)
            {
                info.hasWarning = true;
                info.summary = L"Replaces Start Menu on single Win tap \u00B7 Win+Key shortcuts keep working";
            }
            else if (normVk == VK_MENU)
            {
                info.hasWarning = true;
                info.summary = L"Suppresses app menu bar on single Alt tap \u00B7 Alt+Key shortcuts keep working";
            }
            else
            {
                info.hasWarning = false;
                info.summary = L"Triggers only on clean tap release \u00B7 Shortcuts keep working";
            }
            return info;
        }

        if (b.kind == TriggerKind::RightLeftChord)
        {
            info.hasWarning = false;
            info.summary = L"Hold Right-click then Left-click \u00B7 Normal right-click stays untouched";
            return info;
        }

        if (b.kind == TriggerKind::MouseButton)
        {
            if (b.modifiers == 0)
            {
                if (b.mouseButton == MouseTriggerButton::SideBack || b.mouseButton == MouseTriggerButton::SideForward)
                {
                    info.hasWarning = true;
                    info.summary = L"Uses browser/app Back or Forward mouse button";
                }
                else if (b.mouseButton == MouseTriggerButton::Middle)
                {
                    info.hasWarning = true;
                    info.summary = L"Replaces middle-click (tab close / auto-scroll)";
                }
            }
            return info;
        }

        if (b.kind == TriggerKind::KeyChord)
        {
            const UINT normVk = NormalizeTriggerVk(b.vk);
            if (b.modifiers == MOD_ALT && normVk == VK_TAB)
            {
                info.hasWarning = true;
                info.summary = L"Replaces Windows Alt+Tab switcher while Ring is active";
            }
            else if (b.modifiers == MOD_WIN && normVk == VK_TAB)
            {
                info.hasWarning = true;
                info.summary = L"Replaces Windows Task View (Win+Tab) while Ring is active";
            }
            else if (b.modifiers == MOD_ALT && normVk == VK_SPACE)
            {
                info.hasWarning = true;
                info.summary = L"Replaces window title-bar menu (Alt+Space)";
            }
            else if (b.modifiers == 0 && normVk == VK_CAPITAL)
            {
                info.hasWarning = true;
                info.summary = L"Replaces Caps Lock toggle";
            }
            else if (b.modifiers == 0 && normVk == VK_APPS)
            {
                info.hasWarning = true;
                info.summary = L"Replaces Windows context menu key";
            }
        }
        return info;
    }

    inline bool AreTriggerBindingsEqual(const TriggerBinding& a, const TriggerBinding& b) noexcept
    {
        return a.kind == b.kind &&
            a.mouseButton == b.mouseButton &&
            a.modifiers == b.modifiers &&
            NormalizeTriggerVk(a.vk) == NormalizeTriggerVk(b.vk) &&
            a.profileId == b.profileId;
    }

    inline constexpr bool ShouldPassThroughWinSystemChord(bool winHeld, UINT rawVk) noexcept
    {
        if (!winHeld || rawVk == 0)
            return false;
        const UINT normVk = NormalizeTriggerVk(rawVk);
        return normVk != 0 && !IsModifierVk(normVk);
    }

    inline constexpr bool ResolveWinKeyHeld(
        bool asyncWinDown,
        bool hookWinDown,
        double msSinceHookWinDown,
        bool soloModWinActive) noexcept
    {
        if (asyncWinDown || soloModWinActive)
            return true;
        return hookWinDown && msSinceHookWinDown >= 0.0 && msSinceHookWinDown <= 120.0;
    }
} // namespace winrings
