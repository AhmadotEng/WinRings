#pragma once

#include <Windows.h>
#include <string>
#include <vector>

#include "TriggerModel.h"

namespace winrings
{
    enum class ScrollWheelBehavior
    {
        CycleSlotsOrWindows,
        CycleSubmenuOnly,
        Disabled
    };

    enum class WindowViewMode
    {
        OuterArc,
        TitleColumn
    };

    enum class SurfaceTexture
    {
        A4SAcrylic,    // Authentic Windows 11 WCA ACCENT_ENABLE_ACRYLICBLURBEHIND
        TaskbarGlass,  // Windows 11 Taskbar / HostBackdrop wide diffusion glass
        PureMatte      // Clean zero-blur tinted surface
    };

    enum class RingTheme
    {
        A4S,       // Legacy alias -> A4SAcrylic
        DarkWood,  // Legacy alias -> PureMatte
        Blue       // Legacy alias -> TaskbarGlass + #142A52
    };

    enum class SlotKind
    {
        App,
        Website,
        File,
        Folder,
        Script,
        Collection,
        RecycleBin
    };

    struct PinnedSlotConfig
    {
        std::wstring id;
        SlotKind kind = SlotKind::App;
        std::wstring label;
        std::wstring pathOrUrl;
        std::wstring iconCacheFile;
        int maxFolderItems = 8;
        std::vector<PinnedSlotConfig> items;
        bool runAsAdmin = false;
        std::wstring arguments;
        bool hiddenFromRing = false;
    };

    struct RingProfileConfig
    {
        std::wstring id = L"default";
        std::wstring name = L"default";
        bool includeRunningApps = true;
        std::vector<PinnedSlotConfig> pinnedSlots;

        std::string ToJsonUtf8() const;
        static bool ParseJsonUtf8(const std::string& jsonUtf8, RingProfileConfig& outProfile, const std::wstring& fallbackId = L"profile");
        static bool LoadFromFile(const std::wstring& path, RingProfileConfig& outProfile, const std::wstring& fallbackId = L"");
        bool SaveToFile(const std::wstring& path) const;
    };

    struct SwitcherConfig
    {
        int version = 6;

        // Triggers & Input (Unified up to 8 triggers + clean gesture toggles)
        std::vector<TriggerBinding> triggers;
        bool gestureTapOpenClose = true;
        bool gestureHoldReleaseSwitch = true;
        bool doubleTapLastApp = true;
        ScrollWheelBehavior scrollWheelMode = ScrollWheelBehavior::CycleSlotsOrWindows;
        int tapThresholdMs = 180;
        int doubleTapWindowMs = 280;

        // Legacy trigger mirror fields (kept synced for backward compatibility)
        MouseTriggerButton mouseTrigger = MouseTriggerButton::SideBack;
        bool hotkeyEnabled = true;
        UINT hotkeyModifiers = MOD_ALT;
        UINT hotkeyVk = VK_SPACE;
        bool menuKeyEnabled = false;
        bool interceptWinTab = false;
        bool interceptAltTab = false;

        // Ring Geometry, Surface Texture, Color & Behavior
        double deadzoneDip = 18.0;
        double ringRadiusDip = 115.0;
        double iconSizeDip = 44.0;
        double thumbnailScale = 1.0;
        bool disableOpeningAnimation = true;
        WindowViewMode windowViewMode = WindowViewMode::OuterArc;
        bool fanOutSingleWindow = true;

        SurfaceTexture surfaceTexture = SurfaceTexture::A4SAcrylic;
        bool useCustomSurfaceColor = false;
        std::wstring surfaceColor = L"#202020";
        bool autoLightDark = false;
        bool autoWindowsAccent = false;

        RingTheme theme = RingTheme::A4S;
        int glassLevel = 50;
        int lightDarkMode = 0;
        std::wstring accentColor = L"#0078D4";
        bool pointerFollow = true;
        bool shiftCenterOnActiveMonitor = true;
        bool includeUnpinnedRunningApps = true;
        int maxSlots = 14;

        std::vector<PinnedSlotConfig> pinnedSlots;
        std::vector<RingProfileConfig> profiles;

        static std::wstring AppDataDirectory();
        static std::wstring ConfigPath();
        static std::wstring SanitizeProfileId(const std::wstring& rawName);
        static std::wstring ProfilesDirectory();
        static std::wstring ProfilesDirectoryForConfigPath(const std::wstring& configPath);
        static std::wstring ProfileFilePath(const std::wstring& profileId, const std::wstring& configPath = L"");
        static RingProfileConfig CreateBuiltInSMenuProfile();
        static SwitcherConfig CreateDefault();
        static SwitcherConfig LoadOrCreate();
        static bool LoadFromFile(const std::wstring& path, SwitcherConfig& outConfig);
        static bool ParseJsonUtf8(const std::string& jsonUtf8, SwitcherConfig& outConfig);

        RingProfileConfig* FindProfile(const std::wstring& profileId) noexcept;
        const RingProfileConfig* FindProfile(const std::wstring& profileId) const noexcept;
        const RingProfileConfig& ResolveProfile(const std::wstring& profileId) const noexcept;
        void SyncDefaultProfileMirror();
        void PullFromDefaultProfileMirror();
        void SyncLegacyTriggerMirrors() noexcept;
        std::string ToJsonUtf8(bool includeInlineProfiles = true) const;
        bool Save() const;
        bool SaveToFile(const std::wstring& path) const;
        bool SaveProfile(const std::wstring& profileId, const std::wstring& configPath = L"") const;
        static bool DeleteProfileFile(const std::wstring& profileId, const std::wstring& configPath = L"");
    };

    std::wstring Utf8ToWide(const std::string& utf8);
    std::string WideToUtf8(const std::wstring& wide);
    const wchar_t* ToString(ScrollWheelBehavior v) noexcept;
    const wchar_t* ToString(WindowViewMode v) noexcept;
    const wchar_t* ToString(SurfaceTexture v) noexcept;
    const wchar_t* ToString(RingTheme v) noexcept;
    const wchar_t* ToString(SlotKind v) noexcept;
} // namespace winrings
