#pragma once

#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <type_traits>

#include "WinRingsConfig.h"

namespace winrings
{
    inline constexpr ULONG_PTR WINRINGS_COPYDATA_DELTA_V1 = 0x57524431u; // 'WRD1'
    inline constexpr uint32_t kMaxTriggerSlots = static_cast<uint32_t>(kMaxTriggerBindings);

    enum class DeltaOp : uint32_t
    {
        SetLook = 1,
        SetGestures = 2,
        SetTriggers = 3,
        PreviewLiveRing = 4,
        FlushSaveToDisk = 5,
        SetRingPaused = 6,
        ReloadFromDisk = 7
    };

    struct TriggerBindingPod
    {
        uint32_t kind = 0;
        uint32_t mouseButton = 4; // Disabled
        uint32_t modifiers = 0;
        uint32_t vk = 0;
        wchar_t profileId[32]{};
    };

    struct WinRingsDeltaPacket
    {
        uint32_t version = 1;
        DeltaOp op = DeltaOp::SetLook;

        // Look fields
        uint32_t surfaceTexture = 0;
        uint8_t useCustomSurfaceColor = 0;
        uint8_t autoLightDark = 0;
        uint8_t autoWindowsAccent = 0;
        uint8_t reserved0 = 0;
        int32_t glassLevel = 50;
        int32_t lightDarkMode = 0;
        double ringRadiusDip = 115.0;
        double iconSizeDip = 44.0;
        wchar_t surfaceColor[12]{};
        wchar_t accentColor[12]{};

        // Gesture behavior fields
        uint8_t gestureTapOpenClose = 1;
        uint8_t gestureHoldReleaseSwitch = 1;
        uint8_t doubleTapLastApp = 1;
        uint8_t pointerFollow = 1;

        // Triggers array
        uint32_t triggerCount = 0;
        TriggerBindingPod triggers[kMaxTriggerSlots]{};

        // Generic int parameter (PreviewLiveRing: 0=hide tray preview, 1=show tray preview, 2=open persistent editable ring; SetRingPaused: 1=pause/0=resume)
        uint32_t intParam = 0;
        int32_t anchorScreenX = 0;
        int32_t anchorScreenY = 0;
        wchar_t previewProfileId[32]{};
    };

    static_assert(std::is_trivially_copyable_v<WinRingsDeltaPacket>, "Delta packet must be POD for WM_COPYDATA");

    inline void CopyWideTruncated(wchar_t* dst, size_t dstChars, const std::wstring& src) noexcept
    {
        if (!dst || dstChars == 0)
            return;
        wcsncpy_s(dst, dstChars, src.c_str(), _TRUNCATE);
    }

    inline WinRingsDeltaPacket MakeLookDelta(const SwitcherConfig& cfg) noexcept
    {
        WinRingsDeltaPacket pkt{};
        pkt.version = 1;
        pkt.op = DeltaOp::SetLook;
        pkt.surfaceTexture = static_cast<uint32_t>(cfg.surfaceTexture);
        pkt.useCustomSurfaceColor = cfg.useCustomSurfaceColor ? 1 : 0;
        pkt.autoLightDark = cfg.autoLightDark ? 1 : 0;
        pkt.autoWindowsAccent = cfg.autoWindowsAccent ? 1 : 0;
        pkt.glassLevel = std::clamp(cfg.glassLevel, 0, 100);
        pkt.lightDarkMode = std::clamp(cfg.lightDarkMode, 0, 100);
        pkt.ringRadiusDip = std::clamp(cfg.ringRadiusDip, 72.0, 240.0);
        pkt.iconSizeDip = std::clamp(cfg.iconSizeDip, 24.0, 72.0);
        CopyWideTruncated(pkt.surfaceColor, _countof(pkt.surfaceColor), cfg.surfaceColor);
        CopyWideTruncated(pkt.accentColor, _countof(pkt.accentColor), cfg.accentColor);
        return pkt;
    }

    inline WinRingsDeltaPacket MakeGesturesDelta(const SwitcherConfig& cfg) noexcept
    {
        WinRingsDeltaPacket pkt{};
        pkt.version = 1;
        pkt.op = DeltaOp::SetGestures;
        pkt.gestureTapOpenClose = cfg.gestureTapOpenClose ? 1 : 0;
        pkt.gestureHoldReleaseSwitch = cfg.gestureHoldReleaseSwitch ? 1 : 0;
        pkt.doubleTapLastApp = cfg.doubleTapLastApp ? 1 : 0;
        pkt.pointerFollow = cfg.pointerFollow ? 1 : 0;
        return pkt;
    }

    inline WinRingsDeltaPacket MakeTriggersDelta(const SwitcherConfig& cfg) noexcept
    {
        WinRingsDeltaPacket pkt{};
        pkt.version = 1;
        pkt.op = DeltaOp::SetTriggers;
        const size_t count = (std::min)(cfg.triggers.size(), static_cast<size_t>(kMaxTriggerSlots));
        pkt.triggerCount = static_cast<uint32_t>(count);
        for (size_t i = 0; i < count; ++i)
        {
            const TriggerBinding& src = cfg.triggers[i];
            TriggerBindingPod& dst = pkt.triggers[i];
            dst.kind = static_cast<uint32_t>(src.kind);
            dst.mouseButton = static_cast<uint32_t>(src.mouseButton);
            dst.modifiers = static_cast<uint32_t>(src.modifiers);
            dst.vk = static_cast<uint32_t>(NormalizeTriggerVk(src.vk));
            CopyWideTruncated(dst.profileId, _countof(dst.profileId), src.profileId.empty() ? L"default" : src.profileId);
        }
        return pkt;
    }

    inline WinRingsDeltaPacket MakeSimpleDelta(DeltaOp op, uint32_t param = 0, int32_t x = 0, int32_t y = 0) noexcept
    {
        WinRingsDeltaPacket pkt{};
        pkt.version = 1;
        pkt.op = op;
        pkt.intParam = param;
        pkt.anchorScreenX = x;
        pkt.anchorScreenY = y;
        return pkt;
    }

    inline WinRingsDeltaPacket MakePreviewDelta(bool show, int32_t x, int32_t y, const std::wstring& profileId = L"default") noexcept
    {
        WinRingsDeltaPacket pkt = MakeSimpleDelta(DeltaOp::PreviewLiveRing, show ? 1u : 0u, x, y);
        CopyWideTruncated(pkt.previewProfileId, _countof(pkt.previewProfileId), profileId.empty() ? L"default" : profileId);
        return pkt;
    }

    inline WinRingsDeltaPacket MakeOpenProfileForEditDelta(int32_t x, int32_t y, const std::wstring& profileId = L"default") noexcept
    {
        WinRingsDeltaPacket pkt = MakeSimpleDelta(DeltaOp::PreviewLiveRing, 2u, x, y);
        CopyWideTruncated(pkt.previewProfileId, _countof(pkt.previewProfileId), profileId.empty() ? L"default" : profileId);
        return pkt;
    }

    inline WinRingsDeltaPacket MakeReloadFromDiskDelta(const std::wstring& profileId = L"") noexcept
    {
        WinRingsDeltaPacket pkt = MakeSimpleDelta(DeltaOp::ReloadFromDisk, 0u, 0, 0);
        if (!profileId.empty())
            CopyWideTruncated(pkt.previewProfileId, _countof(pkt.previewProfileId), profileId);
        return pkt;
    }

    inline bool ApplyDeltaToConfig(SwitcherConfig& cfg, const WinRingsDeltaPacket& pkt)
    {
        if (pkt.version != 1)
            return false;

        switch (pkt.op)
        {
        case DeltaOp::SetLook:
            if (pkt.surfaceTexture <= static_cast<uint32_t>(SurfaceTexture::PureMatte))
                cfg.surfaceTexture = static_cast<SurfaceTexture>(pkt.surfaceTexture);
            cfg.useCustomSurfaceColor = (pkt.useCustomSurfaceColor != 0);
            cfg.autoLightDark = (pkt.autoLightDark != 0);
            cfg.autoWindowsAccent = (pkt.autoWindowsAccent != 0);
            cfg.glassLevel = std::clamp(static_cast<int>(pkt.glassLevel), 0, 100);
            cfg.lightDarkMode = std::clamp(static_cast<int>(pkt.lightDarkMode), 0, 100);
            cfg.ringRadiusDip = std::clamp(pkt.ringRadiusDip, 72.0, 240.0);
            cfg.iconSizeDip = std::clamp(pkt.iconSizeDip, 24.0, 72.0);
            if (pkt.surfaceColor[0] != L'\0')
                cfg.surfaceColor = pkt.surfaceColor;
            if (pkt.accentColor[0] != L'\0')
                cfg.accentColor = pkt.accentColor;
            return true;

        case DeltaOp::SetGestures:
            cfg.gestureTapOpenClose = (pkt.gestureTapOpenClose != 0);
            cfg.gestureHoldReleaseSwitch = (pkt.gestureHoldReleaseSwitch != 0);
            cfg.doubleTapLastApp = (pkt.doubleTapLastApp != 0);
            cfg.pointerFollow = (pkt.pointerFollow != 0);
            return true;

        case DeltaOp::SetTriggers:
        {
            const uint32_t count = (std::min)(pkt.triggerCount, kMaxTriggerSlots);
            cfg.triggers.clear();
            cfg.triggers.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                const TriggerBindingPod& src = pkt.triggers[i];
                TriggerBinding b;
                b.kind = (src.kind <= static_cast<uint32_t>(TriggerKind::SoloModifier))
                    ? static_cast<TriggerKind>(src.kind)
                    : TriggerKind::Disabled;
                b.mouseButton = (src.mouseButton <= static_cast<uint32_t>(MouseTriggerButton::Disabled))
                    ? static_cast<MouseTriggerButton>(src.mouseButton)
                    : MouseTriggerButton::Disabled;
                b.modifiers = src.modifiers;
                b.vk = NormalizeTriggerVk(src.vk);
                b.profileId = (src.profileId[0] != L'\0') ? std::wstring(src.profileId) : std::wstring(L"default");
                if (b.IsActive())
                    cfg.triggers.push_back(std::move(b));
            }
            cfg.SyncLegacyTriggerMirrors();
            return true;
        }

        default:
            return false;
        }
    }

    inline bool QueryWindowsAppsUseLightTheme(bool fallbackIsLight = false) noexcept
    {
        DWORD val = fallbackIsLight ? 1u : 0u;
        DWORD size = sizeof(val);
        DWORD type = 0;
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                L"AppsUseLightTheme",
                RRF_RT_REG_DWORD,
                &type,
                &val,
                &size) == ERROR_SUCCESS)
        {
            return val != 0;
        }
        return fallbackIsLight;
    }

    inline std::wstring QueryWindowsAccentHex(const std::wstring& fallbackHex = L"#0078D4")
    {
        DWORD abgr = 0;
        DWORD size = sizeof(abgr);
        DWORD type = 0;
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\DWM",
                L"AccentColor",
                RRF_RT_REG_DWORD,
                &type,
                &abgr,
                &size) == ERROR_SUCCESS)
        {
            const unsigned int r = abgr & 0xFFu;
            const unsigned int g = (abgr >> 8) & 0xFFu;
            const unsigned int b = (abgr >> 16) & 0xFFu;
            wchar_t buf[16]{};
            swprintf_s(buf, L"#%02X%02X%02X", r, g, b);
            return std::wstring(buf);
        }
        return fallbackHex;
    }

    inline SwitcherConfig ResolveEffectiveConfigForSystem(const SwitcherConfig& raw)
    {
        SwitcherConfig eff = raw;
        if (eff.autoLightDark && !eff.useCustomSurfaceColor)
            eff.lightDarkMode = QueryWindowsAppsUseLightTheme(eff.lightDarkMode >= 50) ? 100 : 0;
        if (eff.autoWindowsAccent)
            eff.accentColor = QueryWindowsAccentHex(eff.accentColor);
        return eff;
    }
} // namespace winrings
