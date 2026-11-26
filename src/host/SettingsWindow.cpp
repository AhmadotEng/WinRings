#include "SettingsWindow.h"

#include <commdlg.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include "../shared/RingThemeEngine.h"
#include "../shared/SettingsLayoutMath.h"
#include "../shared/SlotMatching.h"
#include "../shared/TriggerModel.h"
#include "../shared/WinRingsConfig.h"
#include "../shared/WinRingsIdentity.h"

namespace winrings::host
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr UINT kTrayCallbackMsg = WM_APP + 100;
        constexpr UINT kTrayIconId = 1;
        constexpr UINT_PTR kTrayRetryTimerId = 101;
        constexpr UINT_PTR kTrayPromoteTimerId = 102;
        constexpr UINT_PTR kCaretBlinkTimerId = 201;

        constexpr wchar_t kSettingsWndClass[] = L"WinRings.SettingsWindow.v1";

        constexpr float kWinW = 840.0f;
        constexpr float kWinH = 560.0f;
        constexpr float kSidebarW = 164.0f;
        constexpr float kFooterTop = 520.0f;

        constexpr float kShortcutViewportTop = 48.0f;
        constexpr float kShortcutViewportBot = 272.0f;
        constexpr float kPinsViewportTop = 172.0f;
        constexpr float kPinsViewportBot = 498.0f;
        constexpr float kPinRowH = 34.0f;

        constexpr float kStageL = 480.0f;
        constexpr float kStageT = 16.0f;
        constexpr float kStageR = 826.0f;
        constexpr float kStageB = 506.0f;

        constexpr ULONG_PTR kTrayMaskExtra = 0x5752544Du; // 'WRTM'
        constexpr WORD kMaskVirtualKey = 0xE8;

        enum class ProfileInlineForm
        {
            None,
            AddLinkCmd,
            NewProfile,
            RenameProfile,
            EditSlot
        };

        enum TrayMenuCmd : UINT
        {
            CmdHeaderStatus = 1000,
            CmdTogglePauseOrStart = 1001,
            CmdOpenRing = 1002,
            CmdOpenSettings = 1003,
            CmdRestartHook = 1004,
            CmdExitAll = 1005
        };

        struct SurfaceSwatch
        {
            const wchar_t* hex;
            COLORREF rgb;
            bool isNeutralDark;
            bool isNeutralLight;
        };

        constexpr SurfaceSwatch kSurfaceSwatches[] = {
            { L"#202020", RGB(0x20, 0x20, 0x20), true,  false },
            { L"#F3F3F3", RGB(0xF3, 0xF3, 0xF3), false, true  },
            { L"#142A52", RGB(0x14, 0x2A, 0x52), false, false },
            { L"#0F3B2E", RGB(0x0F, 0x3B, 0x2E), false, false },
            { L"#2A1B4E", RGB(0x2A, 0x1B, 0x4E), false, false },
            { L"#4A2C11", RGB(0x4A, 0x2C, 0x11), false, false },
            { L"#4A1520", RGB(0x4A, 0x15, 0x20), false, false },
        };

        struct AccentSwatch
        {
            const wchar_t* hex;
            COLORREF rgb;
        };

        constexpr AccentSwatch kAccentSwatches[] = {
            { L"#34638D", RGB(0x34, 0x63, 0x8D) },
            { L"#0078D4", RGB(0x00, 0x78, 0xD4) },
            { L"#8B5CF6", RGB(0x8B, 0x5C, 0xF6) },
            { L"#10B981", RGB(0x10, 0xB9, 0x81) },
            { L"#F59E0B", RGB(0xF5, 0x9E, 0x0B) },
            { L"#EF4444", RGB(0xEF, 0x44, 0x44) },
        };

        D2D1_COLOR_F RgbF(BYTE r, BYTE g, BYTE b, float a = 1.0f) noexcept
        {
            return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, a);
        }

        D2D1_COLOR_F HexToD2D(const std::wstring& hex, float a = 1.0f) noexcept
        {
            BYTE r = 0, g = 120, b = 212;
            detail::ParseAccentHex(hex, r, g, b);
            return RgbF(r, g, b, a);
        }

        COLORREF HexToColorRef(const std::wstring& hex) noexcept
        {
            BYTE r = 0, g = 120, b = 212;
            detail::ParseAccentHex(hex, r, g, b);
            return RGB(r, g, b);
        }

        std::wstring ColorRefToHex(COLORREF c)
        {
            wchar_t buf[16]{};
            swprintf_s(buf, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
            return buf;
        }

        void EnableDarkWin32Menus() noexcept
        {
            if (HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            {
                using SetPreferredAppModeFn = int (WINAPI*)(int);
                using FlushMenuThemesFn = void (WINAPI*)();
                if (auto setMode = reinterpret_cast<SetPreferredAppModeFn>(
                    GetProcAddress(ux, MAKEINTRESOURCEA(135))))
                    setMode(2); // AllowDark
                if (auto flush = reinterpret_cast<FlushMenuThemesFn>(
                    GetProcAddress(ux, MAKEINTRESOURCEA(136))))
                    flush();
                FreeLibrary(ux);
            }
        }

        HICON CreateWinRingsTrayIcon() noexcept
        {
            constexpr int kSize = 32;
            BITMAPV5HEADER bi{};
            bi.bV5Size = sizeof(bi);
            bi.bV5Width = kSize;
            bi.bV5Height = -kSize;
            bi.bV5Planes = 1;
            bi.bV5BitCount = 32;
            bi.bV5Compression = BI_BITFIELDS;
            bi.bV5RedMask = 0x00FF0000;
            bi.bV5GreenMask = 0x0000FF00;
            bi.bV5BlueMask = 0x000000FF;
            bi.bV5AlphaMask = 0xFF000000;

            void* bits = nullptr;
            HDC screenDc = GetDC(nullptr);
            HBITMAP colorBmp = CreateDIBSection(screenDc, reinterpret_cast<BITMAPINFO*>(&bi),
                DIB_RGB_COLORS, &bits, nullptr, 0);
            ReleaseDC(nullptr, screenDc);
            if (!colorBmp || !bits)
            {
                if (colorBmp) DeleteObject(colorBmp);
                return LoadIconW(nullptr, IDI_APPLICATION);
            }

            auto* px = static_cast<uint32_t*>(bits);
            constexpr float cx = (kSize - 1) * 0.5f;
            constexpr float cy = (kSize - 1) * 0.5f;
            constexpr float offset = 5.1f;
            constexpr float ringR = 8.4f;
            constexpr float halfThick = 2.15f;
            for (int y = 0; y < kSize; ++y)
            {
                for (int x = 0; x < kSize; ++x)
                {
                    const float fx = static_cast<float>(x);
                    const float fy = static_cast<float>(y);
                    const float d1 = std::fabs(std::hypot(fx - (cx - offset), fy - cy) - ringR);
                    const float d2 = std::fabs(std::hypot(fx - (cx + offset), fy - cy) - ringR);
                    const float a1 = (d1 < halfThick) ? (std::clamp)(halfThick - d1, 0.0f, 1.0f) : 0.0f;
                    const float a2 = (d2 < halfThick) ? (std::clamp)(halfThick - d2, 0.0f, 1.0f) : 0.0f;
                    const float alpha = (std::max)(a1, a2);

                    if (alpha > 0.01f)
                    {
                        const uint32_t a = static_cast<uint32_t>(alpha * 255.0f + 0.5f);
                        const float t = fy / static_cast<float>(kSize);
                        const bool ring1Top = (a1 >= a2 && fy < cy) || (a2 < 0.15f);
                        const float rBase = ring1Top ? (56.0f * (1.0f - t) + 14.0f * t) : (32.0f * (1.0f - t) + 0.0f * t);
                        const float gBase = ring1Top ? (196.0f * (1.0f - t) + 142.0f * t) : (164.0f * (1.0f - t) + 104.0f * t);
                        const float bBase = ring1Top ? (255.0f * (1.0f - t) + 242.0f * t) : (255.0f * (1.0f - t) + 218.0f * t);
                        const uint32_t r = static_cast<uint32_t>(rBase * alpha);
                        const uint32_t g = static_cast<uint32_t>(gBase * alpha);
                        const uint32_t b = static_cast<uint32_t>(bBase * alpha);
                        px[y * kSize + x] = (a << 24) | (r << 16) | (g << 8) | b;
                    }
                    else
                    {
                        px[y * kSize + x] = 0;
                    }
                }
            }

            HBITMAP maskBmp = CreateBitmap(kSize, kSize, 1, 1, nullptr);
            ICONINFO ii{};
            ii.fIcon = TRUE;
            ii.hbmMask = maskBmp;
            ii.hbmColor = colorBmp;
            HICON icon = CreateIconIndirect(&ii);
            DeleteObject(colorBmp);
            if (maskBmp) DeleteObject(maskBmp);
            return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
        }

        void EnsureTrayIconPromoted() noexcept
        {
            HKEY hRoot = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings",
                0, KEY_READ | KEY_WRITE, &hRoot) != ERROR_SUCCESS || !hRoot)
                return;

            wchar_t subKeyName[128]{};
            for (DWORD idx = 0;; ++idx)
            {
                DWORD nameLen = _countof(subKeyName);
                if (RegEnumKeyExW(hRoot, idx, subKeyName, &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                    break;

                HKEY hSub = nullptr;
                if (RegOpenKeyExW(hRoot, subKeyName, 0, KEY_READ | KEY_WRITE, &hSub) != ERROR_SUCCESS || !hSub)
                    continue;

                wchar_t exePath[512]{};
                DWORD cbExe = sizeof(exePath);
                DWORD type = 0;
                const bool hasExe = (RegQueryValueExW(hSub, L"ExecutablePath", nullptr, &type,
                    reinterpret_cast<LPBYTE>(exePath), &cbExe) == ERROR_SUCCESS &&
                    (type == REG_SZ || type == REG_EXPAND_SZ));

                wchar_t tip[256]{};
                DWORD cbTip = sizeof(tip);
                const bool hasTip = (RegQueryValueExW(hSub, L"InitialTooltip", nullptr, &type,
                    reinterpret_cast<LPBYTE>(tip), &cbTip) == ERROR_SUCCESS &&
                    (type == REG_SZ || type == REG_EXPAND_SZ));

                if ((hasExe && wcsstr(exePath, WINRINGS_HOST_FILENAME_W) != nullptr) ||
                    (hasTip && wcsstr(tip, L"WinRings") != nullptr))
                {
                    DWORD promoted = 0;
                    DWORD cbPromoted = sizeof(promoted);
                    DWORD valType = 0;
                    const LONG qRes = RegQueryValueExW(hSub, L"IsPromoted", nullptr, &valType,
                        reinterpret_cast<LPBYTE>(&promoted), &cbPromoted);
                    if (qRes != ERROR_SUCCESS || promoted != 1u)
                    {
                        const DWORD one = 1u;
                        RegSetValueExW(hSub, L"IsPromoted", 0, REG_DWORD,
                            reinterpret_cast<const BYTE*>(&one), sizeof(one));
                    }
                }
                RegCloseKey(hSub);
            }
            RegCloseKey(hRoot);
        }

        float QueryMonitorDpiScale(HMONITOR mon) noexcept
        {
            UINT dpiX = 96, dpiY = 96;
            using GetDpiForMonitorFn = HRESULT (WINAPI*)(HMONITOR, int, UINT*, UINT*);
            if (HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            {
                if (auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor")))
                    fn(mon, 0, &dpiX, &dpiY);
                FreeLibrary(shcore);
            }
            return (dpiX > 0) ? (static_cast<float>(dpiX) / 96.0f) : 1.0f;
        }

        std::wstring TrimWide(std::wstring s)
        {
            while (!s.empty() && iswspace(s.back())) s.pop_back();
            size_t start = 0;
            while (start < s.size() && iswspace(s[start])) ++start;
            return (start > 0) ? s.substr(start) : s;
        }

        std::wstring InferProfileItemLabel(SlotKind kind, const std::wstring& pathOrUrl)
        {
            if (kind == SlotKind::Website)
            {
                std::wstring stripped = pathOrUrl;
                const size_t scheme = stripped.find(L"://");
                if (scheme != std::wstring::npos)
                    stripped = stripped.substr(scheme + 3);
                if (stripped.rfind(L"www.", 0) == 0)
                    stripped = stripped.substr(4);
                const size_t slash = stripped.find(L'/');
                if (slash != std::wstring::npos)
                    stripped.resize(slash);
                return stripped.empty() ? pathOrUrl : stripped;
            }
            const size_t slash = pathOrUrl.find_last_of(L"\\/");
            std::wstring name = (slash != std::wstring::npos) ? pathOrUrl.substr(slash + 1) : pathOrUrl;
            if (kind != SlotKind::Folder)
            {
                const size_t dot = name.find_last_of(L'.');
                if (dot != std::wstring::npos && dot > 0)
                    name.resize(dot);
            }
            return name.empty() ? pathOrUrl : name;
        }

        bool PickFileOrFolderDialog(HWND owner, bool folderOnly, std::wstring& outPath)
        {
            ComPtr<IFileOpenDialog> dlg;
            if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(dlg.GetAddressOf()))) || !dlg)
                return false;

            DWORD opts = FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST;
            opts |= folderOnly ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
            dlg->SetOptions(opts);
            dlg->SetTitle(folderOnly ? L"Add Folder to Profile" : L"Add App or File to Profile");
            if (!folderOnly)
            {
                const COMDLG_FILTERSPEC filters[] = {
                    { L"Apps & Shortcuts (*.exe;*.lnk;*.bat;*.cmd;*.ps1)", L"*.exe;*.lnk;*.bat;*.cmd;*.ps1" },
                    { L"All Files (*.*)", L"*.*" }
                };
                dlg->SetFileTypes(2, filters);
            }
            if (FAILED(dlg->Show(owner)))
                return false;
            ComPtr<IShellItem> item;
            if (FAILED(dlg->GetResult(item.GetAddressOf())) || !item)
                return false;
            PWSTR raw = nullptr;
            if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw)
                return false;
            outPath = raw;
            CoTaskMemFree(raw);
            return !outPath.empty();
        }

        bool PickCustomIconDialog(HWND owner, std::wstring& outPath)
        {
            ComPtr<IFileOpenDialog> dlg;
            if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(dlg.GetAddressOf()))) || !dlg)
                return false;

            const COMDLG_FILTERSPEC filters[] = {
                { L"Icons & Images (*.ico;*.png;*.jpg;*.exe;*.dll)", L"*.ico;*.png;*.jpg;*.jpeg;*.bmp;*.exe;*.dll;*.lnk" },
                { L"All Files (*.*)", L"*.*" }
            };
            dlg->SetFileTypes(2, filters);
            dlg->SetOptions(FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_FILEMUSTEXIST);
            dlg->SetTitle(L"Choose Custom Ring Icon");

            if (FAILED(dlg->Show(owner)))
                return false;
            ComPtr<IShellItem> item;
            if (FAILED(dlg->GetResult(item.GetAddressOf())) || !item)
                return false;
            PWSTR raw = nullptr;
            if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw)
                return false;
            outPath = raw;
            CoTaskMemFree(raw);
            return !outPath.empty();
        }

        std::wstring ReadClipboardTextSingleLine(HWND owner)
        {
            if (!OpenClipboard(owner))
                return {};
            std::wstring result;
            if (HANDLE hData = GetClipboardData(CF_UNICODETEXT))
            {
                if (const auto* raw = static_cast<const wchar_t*>(GlobalLock(hData)))
                {
                    result = raw;
                    GlobalUnlock(hData);
                }
            }
            CloseClipboard();
            for (wchar_t& ch : result)
            {
                if (ch == L'\r' || ch == L'\n' || ch == L'\t')
                    ch = L' ';
            }
            return TrimWide(result);
        }

        struct HitRect
        {
            float l = 0, t = 0, r = 0, b = 0;
            bool Contains(float x, float y) const noexcept
            {
                return x >= l && x <= r && y >= t && y <= b;
            }
            D2D1_RECT_F ToD2D() const noexcept
            {
                return D2D1::RectF(l, t, r, b);
            }
        };

        struct WindowLayout
        {
            HitRect navItem[3] = {
                { 12.0f, 64.0f,  152.0f, 100.0f },
                { 12.0f, 106.0f, 152.0f, 142.0f },
                { 12.0f, 148.0f, 152.0f, 184.0f },
            };
            HitRect sidebarProfilesCard{ 12.0f, 148.0f, 152.0f, 184.0f };
            HitRect sidebarProfileAddBtn{ 126.0f, 155.0f, 148.0f, 177.0f };
            std::vector<HitRect> sidebarProfileRow;
            std::vector<HitRect> sidebarProfileRenameBtn;

            HitRect footerStatusPill{ 10.0f, 526.0f, 134.0f, 554.0f };

            // Tab 0: General
            HitRect addTriggerBtn{ 758.0f, 14.0f, 822.0f, 36.0f };
            HitRect triggerPill[kMaxTriggerBindings]{};
            HitRect triggerProfileBadge[kMaxTriggerBindings]{};
            HitRect triggerRemove[kMaxTriggerBindings]{};
            float triggerContentH = 0.0f;
            HitRect behaviorRow[4]{};

            // Tab 1: Appearance (left column 178..466)
            HitRect texCard[3]{};
            HitRect modeChip[3]{};
            HitRect surfaceDot[8]{};
            HitRect accentAutoChip{ 178.0f, 214.0f, 232.0f, 240.0f };
            HitRect accentDot[7]{};
            HitRect sliderTrack[3]{};

            // Tab 2: Profiles (left column 178..466)
            HitRect openConfigBtn{ 378.0f, 16.0f, 434.0f, 40.0f };
            HitRect profileResetBtn{ 440.0f, 16.0f, 466.0f, 40.0f };
            HitRect profileDeleteBtn{};
            HitRect profileRunningRow{ 178.0f, 48.0f, 466.0f, 88.0f };
            HitRect addAppBtn{ 188.0f, 132.0f, 274.0f, 158.0f };
            HitRect addFolderBtn{ 279.0f, 132.0f, 365.0f, 158.0f };
            HitRect addLinkBtn{ 370.0f, 132.0f, 456.0f, 158.0f };

            // Inline form inside Profiles card (178..466, y = 98..506)
            HitRect inlineField1Rect{ 190.0f, 146.0f, 454.0f, 174.0f };
            HitRect inlineField2Rect{ 190.0f, 200.0f, 454.0f, 228.0f };
            HitRect inlineAdminRow{ 190.0f, 238.0f, 454.0f, 266.0f };
            HitRect editNameRect{ 190.0f, 134.0f, 454.0f, 160.0f };
            HitRect editIconStatusRect{ 190.0f, 184.0f, 330.0f, 210.0f };
            HitRect editIconBrowseBtn{ 336.0f, 184.0f, 400.0f, 210.0f };
            HitRect editIconResetBtn{ 404.0f, 184.0f, 454.0f, 210.0f };
            HitRect editHideRow{ 190.0f, 220.0f, 454.0f, 248.0f };
            HitRect editAdminRow{ 190.0f, 252.0f, 454.0f, 280.0f };
            HitRect inlineCancelBtn{ 282.0f, 462.0f, 364.0f, 492.0f };
            HitRect inlineSubmitBtn{ 372.0f, 462.0f, 454.0f, 492.0f };

            std::vector<HitRect> profilePinRowRect;
            std::vector<HitRect> profilePinEditBtn;
            std::vector<HitRect> profilePinRemoveBtn;
            float profilePinsContentH = 0.0f;
        };
    } // namespace

    struct SettingsHostUi::Impl
    {
        static Impl* s_instance;

        HINSTANCE instance = nullptr;
        HWND hostHwnd = nullptr;
        HWND uiHwnd = nullptr;
        HICON trayIcon = nullptr;
        UINT taskbarCreatedMsg = 0;
        bool trayAdded = false;
        unsigned trayRetryCount = 0;
        unsigned trayPromoteTicks = 0;

        SettingsHostBridge bridge;
        SwitcherConfig config;
        int activeTab = 0; // 0 = General, 1 = Appearance, 2 = Profiles
        std::wstring selectedProfileId = L"default";

        float dpiScale = 1.0f;
        bool windowActive = true;
        bool minimized = false;
        bool dirty = false;
        bool livePreviewSent = false;
        bool syncingRing = false;
        bool modalOpen = false;
        int hoverNav = -1;
        bool hoverProfilesCard = false;
        bool hoverSidebarAdd = false;
        int hoverSidebarProf = -1;
        int hoverSidebarRename = -1;
        bool hoverStatusPill = false;
        bool hoverConfigBtn = false;
        bool hoverProfileResetBtn = false;
        bool hoverProfileDeleteBtn = false;
        bool trackingMouseLeave = false;
        int dragSlider = -1;
        float triggerScrollDip = 0.0f;
        float pinsScrollDip = 0.0f;

        // Shortcut capture hook state
        int recordingIndex = -1;
        HHOOK capKeyHook = nullptr;
        HHOOK capMouseHook = nullptr;
        UINT capModDown = 0;
        bool capSoloDisqualified = false;
        bool capRightDown = false;
        UINT swallowNextKeyUpVk = 0;

        // Inline form state inside Profiles tab
        ProfileInlineForm inlineForm = ProfileInlineForm::None;
        int inlineFocusField = 0;
        std::wstring inlineField1;
        std::wstring inlineField2;
        bool inlineAdmin = false;
        bool inlineCaretOn = true;
        std::wstring renameProfileId;
        std::wstring editSlotId;
        bool editHiddenFromRing = false;
        std::wstring editIconPath;
        SlotKind editSlotKind = SlotKind::App;
        std::wstring editSlotTarget;

        // On-demand Direct3D / Direct2D / DirectWrite resources (only alive while uiHwnd exists)
        ComPtr<ID3D11Device> d3dDevice;
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<ID2D1Factory1> d2dFactory;
        ComPtr<ID2D1Device> d2dDevice;
        ComPtr<ID2D1DeviceContext> d2dCtx;
        ComPtr<IDXGISwapChain1> swapChain;
        ComPtr<ID2D1Bitmap1> targetBmp;
        ComPtr<IDWriteFactory> dwrite;
        ComPtr<IDWriteTextFormat> fmtBrand;
        ComPtr<IDWriteTextFormat> fmtLogo;
        ComPtr<ID2D1Bitmap> logoBmp;
        ComPtr<IDWriteTextFormat> fmtBold;
        ComPtr<IDWriteTextFormat> fmtBody;
        ComPtr<IDWriteTextFormat> fmtCenter;
        ComPtr<IDWriteTextFormat> fmtCaption;
        ComPtr<IDWriteTextFormat> fmtSmallCenter;
        ComPtr<IDWriteTextFormat> fmtWrap;
        int swapW = 0;
        int swapH = 0;

        static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (msg == WM_NCCREATE)
            {
                self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                if (self)
                    self->uiHwnd = hwnd;
            }
            if (self)
                return self->HandleWindowMessage(hwnd, msg, wp, lp);
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        static LRESULT CALLBACK CaptureKeyboardProc(int code, WPARAM wp, LPARAM lp)
        {
            if (code >= 0 && s_instance && s_instance->recordingIndex >= 0)
                return s_instance->HandleCaptureKeyboard(code, wp, lp);
            return CallNextHookEx(nullptr, code, wp, lp);
        }

        static LRESULT CALLBACK CaptureMouseProc(int code, WPARAM wp, LPARAM lp)
        {
            if (code >= 0 && s_instance && s_instance->recordingIndex >= 0)
                return s_instance->HandleCaptureMouse(code, wp, lp);
            return CallNextHookEx(nullptr, code, wp, lp);
        }

        EngineMode CurrentEngineMode() const noexcept
        {
            return bridge.queryEngineState ? bridge.queryEngineState() : EngineMode::Stopped;
        }

        bool SendDelta(const WinRingsDeltaPacket& pkt) const noexcept
        {
            return bridge.sendDelta ? bridge.sendDelta(pkt) : false;
        }

        void AddTrayIcon()
        {
            if (!hostHwnd || !IsWindow(hostHwnd))
                return;
            if (!trayIcon)
                trayIcon = CreateWinRingsTrayIcon();

            NOTIFYICONDATAW nid{ sizeof(nid) };
            nid.hWnd = hostHwnd;
            nid.uID = kTrayIconId;
            nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
            nid.uCallbackMessage = kTrayCallbackMsg;
            nid.hIcon = trayIcon;
            wcscpy_s(nid.szTip, L"WinRings \u2014 Radial Switcher");
            if (Shell_NotifyIconW(NIM_ADD, &nid) || Shell_NotifyIconW(NIM_MODIFY, &nid))
            {
                trayAdded = true;
                KillTimer(hostHwnd, kTrayRetryTimerId);
                EnsureTrayIconPromoted();
                trayPromoteTicks = 0;
                SetTimer(hostHwnd, kTrayPromoteTimerId, 700, nullptr);
            }
            else if (trayRetryCount < 30)
            {
                ++trayRetryCount;
                SetTimer(hostHwnd, kTrayRetryTimerId, 1000, nullptr);
            }
        }

        void RemoveTrayIcon() noexcept
        {
            if (hostHwnd && IsWindow(hostHwnd))
            {
                KillTimer(hostHwnd, kTrayRetryTimerId);
                KillTimer(hostHwnd, kTrayPromoteTimerId);
            }
            if (trayAdded && hostHwnd)
            {
                NOTIFYICONDATAW nid{ sizeof(nid) };
                nid.hWnd = hostHwnd;
                nid.uID = kTrayIconId;
                Shell_NotifyIconW(NIM_DELETE, &nid);
                trayAdded = false;
            }
            if (trayIcon)
            {
                DestroyIcon(trayIcon);
                trayIcon = nullptr;
            }
        }

        void ShowTrayContextMenu()
        {
            if (!hostHwnd)
                return;
            POINT pt{};
            GetCursorPos(&pt);
            const EngineMode mode = CurrentEngineMode();

            HMENU menu = CreatePopupMenu();
            if (!menu)
                return;

            const wchar_t* statusStr = L"Ring Engine: Stopped";
            if (mode == EngineMode::Active) statusStr = L"Ring Engine: Active";
            else if (mode == EngineMode::Paused) statusStr = L"Ring Engine: Paused";
            else if (mode == EngineMode::Starting) statusStr = L"Ring Engine: Connecting...";
            else if (mode == EngineMode::SafetyLocked) statusStr = L"Ring Engine: Safety Locked";
            else if (mode == EngineMode::PreviewOnly) statusStr = L"Ring Engine: Preview Mode";

            AppendMenuW(menu, MF_STRING | MF_GRAYED, CmdHeaderStatus, statusStr);
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

            if (mode == EngineMode::Active)
                AppendMenuW(menu, MF_STRING, CmdTogglePauseOrStart, L"Pause Ring");
            else if (mode == EngineMode::Paused)
                AppendMenuW(menu, MF_STRING, CmdTogglePauseOrStart, L"Resume Ring");
            else if (mode == EngineMode::SafetyLocked)
                AppendMenuW(menu, MF_STRING, CmdTogglePauseOrStart, L"Reset Safety && Start");

            AppendMenuW(menu, MF_STRING | (mode == EngineMode::Active ? 0u : MF_GRAYED), CmdOpenRing, L"Open Ring Now");
            AppendMenuW(menu, MF_STRING, CmdOpenSettings, L"Open WinRings Settings");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, CmdRestartHook, L"Reload Config && Ring Hook");
            AppendMenuW(menu, MF_STRING, CmdExitAll, L"Exit WinRings");

            SetMenuDefaultItem(menu, CmdOpenSettings, FALSE);

            HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(mon, &mi);
            UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON;
            flags |= (pt.y > (mi.rcWork.top + mi.rcWork.bottom) / 2) ? TPM_BOTTOMALIGN : TPM_TOPALIGN;

            SetForegroundWindow(hostHwnd);
            const UINT cmd = TrackPopupMenuEx(menu, flags, pt.x, pt.y, hostHwnd, nullptr);
            DestroyMenu(menu);
            PostMessageW(hostHwnd, WM_NULL, 0, 0);

            switch (cmd)
            {
            case CmdTogglePauseOrStart:
                if (mode == EngineMode::Active && bridge.setPaused)
                    bridge.setPaused(true);
                else if (mode == EngineMode::Paused && bridge.setPaused)
                    bridge.setPaused(false);
                else if (mode == EngineMode::SafetyLocked && bridge.resetSafetyAndStart)
                    bridge.resetSafetyAndStart();
                NotifyStatus();
                break;
            case CmdOpenRing:
                if (bridge.toggleRingNow)
                    bridge.toggleRingNow();
                break;
            case CmdOpenSettings:
                OpenWindow(true);
                break;
            case CmdRestartHook:
                if (bridge.restartHook)
                    bridge.restartHook();
                NotifyStatus();
                break;
            case CmdExitAll:
                CloseWindow();
                if (bridge.requestExit)
                    bridge.requestExit();
                break;
            default:
                break;
            }
        }

        void SyncLiveRingPreview(bool sendLook = false)
        {
            if (syncingRing)
                return;
            syncingRing = true;

            const EngineMode mode = CurrentEngineMode();
            const bool isPaused = (mode != EngineMode::Active && mode != EngineMode::PreviewOnly);
            const bool hasWindow = (uiHwnd != nullptr && IsWindow(uiHwnd) && IsWindowVisible(uiHwnd));
            const bool wantShow = hasWindow && ShouldShowLiveRingForTab(activeTab, minimized, windowActive, isPaused);

            if (wantShow)
            {
                POINT clientOrigin{ 0, 0 };
                ClientToScreen(uiHwnd, &clientOrigin);
                const POINT anchor = ComputeRingStageCenterPhysical(
                    clientOrigin, dpiScale, kStageL, kStageT, kStageR, kStageB);
                if (!livePreviewSent || sendLook)
                    SendDelta(MakeLookDelta(config));
                if (SendDelta(MakePreviewDelta(true, anchor.x, anchor.y, selectedProfileId)))
                    livePreviewSent = true;
            }
            else if (livePreviewSent)
            {
                SendDelta(MakePreviewDelta(false, 0, 0, selectedProfileId));
                livePreviewSent = false;
            }

            syncingRing = false;
        }

        void OpenWindow(bool activate)
        {
            if (uiHwnd && IsWindow(uiHwnd))
            {
                if (IsIconic(uiHwnd))
                    ShowWindow(uiHwnd, SW_RESTORE);
                minimized = false;
                windowActive = true;
                if (activate)
                    SetForegroundWindow(uiHwnd);
                SyncLiveRingPreview();
                Invalidate();
                return;
            }

            WNDCLASSEXW wc{ sizeof(wc) };
            wc.lpfnWndProc = SettingsWndProc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = trayIcon;
            wc.hIconSm = trayIcon;
            wc.lpszClassName = kSettingsWndClass;
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                return;

            config = SwitcherConfig::LoadOrCreate();
            if (!config.FindProfile(selectedProfileId))
                selectedProfileId = L"default";

            dirty = false;
            livePreviewSent = false;
            windowActive = true;
            minimized = false;
            modalOpen = false;
            hoverNav = -1;
            hoverProfilesCard = false;
            hoverSidebarAdd = false;
            hoverSidebarProf = -1;
            hoverSidebarRename = -1;
            hoverStatusPill = false;
            hoverConfigBtn = false;
            hoverProfileResetBtn = false;
            hoverProfileDeleteBtn = false;
            trackingMouseLeave = false;
            dragSlider = -1;
            recordingIndex = -1;
            triggerScrollDip = 0.0f;
            pinsScrollDip = 0.0f;
            inlineForm = ProfileInlineForm::None;
            inlineFocusField = 0;
            inlineField1.clear();
            inlineField2.clear();
            inlineAdmin = false;
            renameProfileId.clear();
            editSlotId.clear();
            editHiddenFromRing = false;
            editIconPath.clear();

            POINT cursor{};
            GetCursorPos(&cursor);
            HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(mon, &mi);
            dpiScale = QueryMonitorDpiScale(mon);

            const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
            RECT rc{ 0, 0, static_cast<LONG>(std::lround(kWinW * dpiScale)), static_cast<LONG>(std::lround(kWinH * dpiScale)) };
            AdjustWindowRectEx(&rc, style, FALSE, 0);
            const int winW = rc.right - rc.left;
            const int winH = rc.bottom - rc.top;
            const int x = mi.rcWork.left + (std::max)(0, static_cast<int>(mi.rcWork.right - mi.rcWork.left - winW) / 2);
            const int y = mi.rcWork.top + (std::max)(0, static_cast<int>(mi.rcWork.bottom - mi.rcWork.top - winH) / 2);

            uiHwnd = CreateWindowExW(
                0,
                kSettingsWndClass,
                L"WinRings",
                style,
                x, y, winW, winH,
                nullptr, nullptr, instance, this);
            if (!uiHwnd)
                return;

            const BOOL dark = TRUE;
            DwmSetWindowAttribute(uiHwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
            const COLORREF capCol = RGB(24, 24, 27);
            DwmSetWindowAttribute(uiHwnd, 35 /* DWMWA_CAPTION_COLOR */, &capCol, sizeof(capCol));
            const COLORREF borderCol = RGB(48, 48, 56);
            DwmSetWindowAttribute(uiHwnd, 34 /* DWMWA_BORDER_COLOR */, &borderCol, sizeof(borderCol));

            RECT clientRc{};
            GetClientRect(uiHwnd, &clientRc);
            if (!InitGraphics(clientRc.right - clientRc.left, clientRc.bottom - clientRc.top))
            {
                CloseWindow();
                return;
            }

            ShowWindow(uiHwnd, activate ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
            if (activate)
                SetForegroundWindow(uiHwnd);
            Render();
            SyncLiveRingPreview();
        }

        void CloseWindow()
        {
            CancelCapture();
            inlineForm = ProfileInlineForm::None;
            windowActive = false;
            dragSlider = -1;

            if (uiHwnd && IsWindow(uiHwnd))
                KillTimer(uiHwnd, kCaretBlinkTimerId);

            if (livePreviewSent)
            {
                SendDelta(MakePreviewDelta(false, 0, 0, selectedProfileId));
                livePreviewSent = false;
            }

            if (dirty)
            {
                config.PullFromDefaultProfileMirror();
                config.Save();
                SendDelta(MakeSimpleDelta(DeltaOp::FlushSaveToDisk));
                dirty = false;
            }

            ReleaseGraphics();
            if (uiHwnd && IsWindow(uiHwnd))
            {
                HWND doomed = uiHwnd;
                uiHwnd = nullptr;
                DestroyWindow(doomed);
            }
            uiHwnd = nullptr;

            // Trim idle working set so hidden tray host uses minimal memory and 0% GPU.
            SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
        }

        void NotifyStatus()
        {
            if (uiHwnd && IsWindow(uiHwnd))
            {
                SyncLiveRingPreview();
                Invalidate();
            }
        }

        bool InitGraphics(int wPx, int hPx)
        {
            ReleaseGraphics();

            const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            const D3D_DRIVER_TYPE drivers[] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };
            HRESULT hr = E_FAIL;
            for (D3D_DRIVER_TYPE drv : drivers)
            {
                hr = D3D11CreateDevice(nullptr, drv, nullptr, flags, nullptr, 0,
                    D3D11_SDK_VERSION, d3dDevice.ReleaseAndGetAddressOf(), nullptr, nullptr);
                if (SUCCEEDED(hr))
                    break;
            }
            if (FAILED(hr) || !d3dDevice || FAILED(d3dDevice.As(&dxgiDevice)) || !dxgiDevice)
                return false;

            D2D1_FACTORY_OPTIONS opts{};
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                &opts, reinterpret_cast<void**>(d2dFactory.ReleaseAndGetAddressOf()))))
                return false;
            if (FAILED(d2dFactory->CreateDevice(dxgiDevice.Get(), d2dDevice.ReleaseAndGetAddressOf())))
                return false;
            if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2dCtx.ReleaseAndGetAddressOf())))
                return false;
            if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(dwrite.ReleaseAndGetAddressOf()))))
                return false;

            auto mkFmt = [&](const wchar_t* face, float sz, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT align,
                ComPtr<IDWriteTextFormat>& out) {
                dwrite->CreateTextFormat(face, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    sz, L"en-us", out.ReleaseAndGetAddressOf());
                if (out)
                {
                    out->SetTextAlignment(align);
                    out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                    ComPtr<IDWriteInlineObject> ell;
                    dwrite->CreateEllipsisTrimmingSign(out.Get(), ell.GetAddressOf());
                    out->SetTrimming(&trim, ell.Get());
                }
            };
            mkFmt(L"Segoe UI Variable Display", 17.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, fmtBrand);
            mkFmt(L"Segoe UI Variable Display", 19.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, fmtLogo);
            mkFmt(L"Segoe UI Variable Text", 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, fmtBold);
            mkFmt(L"Segoe UI Variable Text", 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, fmtBody);
            mkFmt(L"Segoe UI Variable Text", 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, fmtCenter);
            mkFmt(L"Segoe UI Variable Text", 10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, fmtCaption);
            mkFmt(L"Segoe UI Variable Text", 10.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, fmtSmallCenter);

            dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.2f, L"en-us", fmtWrap.ReleaseAndGetAddressOf());
            if (fmtWrap)
            {
                fmtWrap->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                fmtWrap->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                fmtWrap->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            }

            LoadLogoBitmap();
            return ResizeSwapChain(wPx, hPx);
        }

        void LoadLogoBitmap()
        {
            logoBmp.Reset();
            if (!d2dCtx || !instance)
                return;
            HRSRC hRes = FindResourceW(instance, MAKEINTRESOURCEW(201), RT_RCDATA);
            if (!hRes)
                return;
            const DWORD resSize = SizeofResource(instance, hRes);
            HGLOBAL hData = LoadResource(instance, hRes);
            const void* resBytes = hData ? LockResource(hData) : nullptr;
            if (!resBytes || resSize == 0)
                return;

            ComPtr<IWICImagingFactory> wic;
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(wic.GetAddressOf()))) || !wic)
                return;

            ComPtr<IWICStream> stream;
            if (FAILED(wic->CreateStream(stream.GetAddressOf())) ||
                FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(resBytes)), resSize)))
                return;

            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICFormatConverter> converter;
            if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf())) ||
                !decoder || FAILED(decoder->GetFrame(0, frame.GetAddressOf())) || !frame ||
                FAILED(wic->CreateFormatConverter(converter.GetAddressOf())) || !converter ||
                FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
                return;

            d2dCtx->CreateBitmapFromWicBitmap(converter.Get(), nullptr, logoBmp.ReleaseAndGetAddressOf());
        }

        bool ResizeSwapChain(int wPx, int hPx)
        {
            if (!dxgiDevice || !d2dCtx || !uiHwnd)
                return false;
            wPx = (std::max)(1, wPx);
            hPx = (std::max)(1, hPx);
            if (swapChain && swapW == wPx && swapH == hPx && targetBmp)
                return true;

            d2dCtx->SetTarget(nullptr);
            targetBmp.Reset();

            if (!swapChain)
            {
                ComPtr<IDXGIAdapter> adapter;
                ComPtr<IDXGIFactory2> factory;
                if (FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf())) ||
                    FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()))))
                    return false;

                DXGI_SWAP_CHAIN_DESC1 desc{};
                desc.Width = static_cast<UINT>(wPx);
                desc.Height = static_cast<UINT>(hPx);
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                desc.BufferCount = 2;
                desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
                desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

                if (FAILED(factory->CreateSwapChainForHwnd(d3dDevice.Get(), uiHwnd, &desc, nullptr, nullptr,
                    swapChain.ReleaseAndGetAddressOf())))
                    return false;
            }
            else
            {
                if (FAILED(swapChain->ResizeBuffers(2, static_cast<UINT>(wPx), static_cast<UINT>(hPx),
                    DXGI_FORMAT_B8G8R8A8_UNORM, 0)))
                    return false;
            }

            ComPtr<IDXGISurface> surf;
            if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(surf.GetAddressOf()))))
                return false;
            D2D1_BITMAP_PROPERTIES1 bp{};
            bp.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
            bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
            bp.dpiX = 96.0f;
            bp.dpiY = 96.0f;
            if (FAILED(d2dCtx->CreateBitmapFromDxgiSurface(surf.Get(), &bp, targetBmp.ReleaseAndGetAddressOf())))
                return false;

            swapW = wPx;
            swapH = hPx;
            return true;
        }

        void ReleaseGraphics() noexcept
        {
            if (d2dCtx) d2dCtx->SetTarget(nullptr);
            targetBmp.Reset();
            swapChain.Reset();
            fmtWrap.Reset();
            fmtSmallCenter.Reset();
            fmtCaption.Reset();
            fmtCenter.Reset();
            fmtBody.Reset();
            fmtBold.Reset();
            fmtBrand.Reset();
            fmtLogo.Reset();
            logoBmp.Reset();
            dwrite.Reset();
            d2dCtx.Reset();
            d2dDevice.Reset();
            d2dFactory.Reset();
            dxgiDevice.Reset();
            d3dDevice.Reset();
            swapW = 0;
            swapH = 0;
        }

        void Invalidate() noexcept
        {
            if (uiHwnd && IsWindow(uiHwnd))
                InvalidateRect(uiHwnd, nullptr, FALSE);
        }

        float ComputeTriggersContentHeight() const noexcept
        {
            const size_t tCount = (std::min)(config.triggers.size(), kMaxTriggerBindings);
            float y = 0.0f;
            for (size_t i = 0; i < tCount; ++i)
            {
                const TriggerConflictInfo ci = AnalyzeTriggerConflict(config.triggers[i]);
                y += ci.summary.empty() ? 42.0f : 58.0f;
            }
            return y;
        }

        void ClampTriggerScroll() noexcept
        {
            const float viewH = kShortcutViewportBot - kShortcutViewportTop;
            const float maxScroll = (std::max)(0.0f, ComputeTriggersContentHeight() - viewH);
            triggerScrollDip = std::clamp(triggerScrollDip, 0.0f, maxScroll);
        }

        float ComputePinsContentHeight() const noexcept
        {
            const RingProfileConfig& prof = config.ResolveProfile(selectedProfileId);
            return static_cast<float>(prof.pinnedSlots.size()) * kPinRowH;
        }

        void ClampPinsScroll() noexcept
        {
            const float viewH = kPinsViewportBot - kPinsViewportTop;
            const float maxScroll = (std::max)(0.0f, ComputePinsContentHeight() - viewH);
            pinsScrollDip = std::clamp(pinsScrollDip, 0.0f, maxScroll);
        }

        void PruneDisabledPlaceholders()
        {
            const auto it = std::remove_if(config.triggers.begin(), config.triggers.end(),
                [](const TriggerBinding& t) { return !t.IsActive(); });
            config.triggers.erase(it, config.triggers.end());
        }

        void StartCapture(int index)
        {
            StopCapture();
            if (index < 0 || index >= static_cast<int>(config.triggers.size()))
                return;

            recordingIndex = index;
            capModDown = 0;
            capSoloDisqualified = false;
            capRightDown = false;
            swallowNextKeyUpVk = 0;

            HMODULE selfMod = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&CaptureKeyboardProc), &selfMod);
            capKeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, CaptureKeyboardProc, selfMod, 0);
            capMouseHook = SetWindowsHookExW(WH_MOUSE_LL, CaptureMouseProc, selfMod, 0);
            Invalidate();
        }

        void StopCapture() noexcept
        {
            recordingIndex = -1;
            capModDown = 0;
            capSoloDisqualified = false;
            capRightDown = false;
            swallowNextKeyUpVk = 0;
            if (capKeyHook) { UnhookWindowsHookEx(capKeyHook); capKeyHook = nullptr; }
            if (capMouseHook) { UnhookWindowsHookEx(capMouseHook); capMouseHook = nullptr; }
        }

        void CancelCapture()
        {
            StopCapture();
            PruneDisabledPlaceholders();
            ClampTriggerScroll();
        }

        void InjectMaskTap() noexcept
        {
            INPUT inputs[2]{};
            inputs[0].type = INPUT_KEYBOARD;
            inputs[0].ki.wVk = kMaskVirtualKey;
            inputs[0].ki.dwExtraInfo = kTrayMaskExtra;
            inputs[1].type = INPUT_KEYBOARD;
            inputs[1].ki.wVk = kMaskVirtualKey;
            inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
            inputs[1].ki.dwExtraInfo = kTrayMaskExtra;
            SendInput(2, inputs, sizeof(INPUT));
        }

        UINT CurrentHeldModifiers(UINT excludeNormVk = 0) const noexcept
        {
            UINT mods = 0;
            if (excludeNormVk != VK_MENU && (GetAsyncKeyState(VK_MENU) & 0x8000)) mods |= MOD_ALT;
            if (excludeNormVk != VK_CONTROL && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) mods |= MOD_CONTROL;
            if (excludeNormVk != VK_SHIFT && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) mods |= MOD_SHIFT;
            if (excludeNormVk != VK_LWIN &&
                ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)))
                mods |= MOD_WIN;
            return mods;
        }

        void FinishCapturedBinding(const TriggerBinding& captured)
        {
            const int idx = recordingIndex;
            StopCapture();
            if ((captured.modifiers & (MOD_WIN | MOD_ALT)) != 0)
                InjectMaskTap();
            if (idx >= 0 && idx < static_cast<int>(config.triggers.size()))
            {
                TriggerBinding next = captured;
                if (!config.triggers[static_cast<size_t>(idx)].profileId.empty())
                    next.profileId = config.triggers[static_cast<size_t>(idx)].profileId;
                config.triggers[static_cast<size_t>(idx)] = std::move(next);
                PruneDisabledPlaceholders();
                ClampTriggerScroll();
                CommitTriggersDelta();
            }
            else
            {
                PruneDisabledPlaceholders();
                ClampTriggerScroll();
                Invalidate();
            }
        }

        LRESULT HandleCaptureKeyboard(int code, WPARAM wp, LPARAM lp)
        {
            const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
            if (!kb || kb->dwExtraInfo == kTrayMaskExtra || kb->vkCode == kMaskVirtualKey)
                return CallNextHookEx(capKeyHook, code, wp, lp);

            const bool isDown = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
            const bool isUp = (wp == WM_KEYUP || wp == WM_SYSKEYUP);
            const UINT normVk = NormalizeTriggerVk(kb->vkCode);

            if (isUp && swallowNextKeyUpVk != 0 && normVk == swallowNextKeyUpVk)
            {
                swallowNextKeyUpVk = 0;
                return 1;
            }

            if (IsModifierVk(normVk))
            {
                const UINT flag = ModifierFlagFromVk(normVk);
                if (isDown)
                {
                    if ((capModDown & ~flag) != 0)
                        capSoloDisqualified = true;
                    capModDown |= flag;
                    return CallNextHookEx(capKeyHook, code, wp, lp);
                }
                if (isUp)
                {
                    const bool cleanSolo = ((capModDown & flag) != 0) &&
                        ((capModDown & ~flag) == 0) && !capSoloDisqualified;
                    capModDown &= ~flag;
                    if (cleanSolo)
                    {
                        if (normVk == VK_LWIN || normVk == VK_MENU)
                            InjectMaskTap();
                        TriggerBinding b;
                        b.kind = TriggerKind::SoloModifier;
                        b.vk = normVk;
                        FinishCapturedBinding(b);
                    }
                    return CallNextHookEx(capKeyHook, code, wp, lp);
                }
            }

            if (isDown)
            {
                capSoloDisqualified = true;
                const UINT mods = CurrentHeldModifiers();
                if (normVk == VK_ESCAPE && mods == 0)
                {
                    CancelCapture();
                    Invalidate();
                    return 1;
                }
                TriggerBinding b;
                b.kind = TriggerKind::KeyChord;
                b.modifiers = mods;
                b.vk = normVk;
                swallowNextKeyUpVk = normVk;
                FinishCapturedBinding(b);
                return 1;
            }

            return CallNextHookEx(capKeyHook, code, wp, lp);
        }

        LRESULT HandleCaptureMouse(int code, WPARAM wp, LPARAM lp)
        {
            const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
            if (!ms)
                return CallNextHookEx(capMouseHook, code, wp, lp);

            const UINT msg = static_cast<UINT>(wp);
            if (msg == WM_MOUSEMOVE)
                return CallNextHookEx(capMouseHook, code, wp, lp);

            if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN)
                capSoloDisqualified = true;

            bool insideUi = false;
            if (uiHwnd && IsWindow(uiHwnd))
            {
                RECT rc{};
                GetWindowRect(uiHwnd, &rc);
                insideUi = PtInRect(&rc, ms->pt) != FALSE;
            }

            if (msg == WM_RBUTTONDOWN)
            {
                capRightDown = true;
                return CallNextHookEx(capMouseHook, code, wp, lp);
            }
            if (msg == WM_RBUTTONUP)
            {
                capRightDown = false;
                return CallNextHookEx(capMouseHook, code, wp, lp);
            }
            if (msg == WM_LBUTTONDOWN)
            {
                if (capRightDown)
                {
                    capRightDown = false;
                    TriggerBinding b;
                    b.kind = TriggerKind::RightLeftChord;
                    b.mouseButton = MouseTriggerButton::RightLeftChord;
                    b.modifiers = CurrentHeldModifiers();
                    FinishCapturedBinding(b);
                    return 1;
                }
                if (!insideUi)
                {
                    CancelCapture();
                    Invalidate();
                }
                return CallNextHookEx(capMouseHook, code, wp, lp);
            }

            MouseTriggerButton btn = MouseTriggerButton::Disabled;
            if (msg == WM_MBUTTONDOWN) btn = MouseTriggerButton::Middle;
            else if (msg == WM_XBUTTONDOWN && HIWORD(ms->mouseData) == XBUTTON1) btn = MouseTriggerButton::SideBack;
            else if (msg == WM_XBUTTONDOWN && HIWORD(ms->mouseData) == XBUTTON2) btn = MouseTriggerButton::SideForward;

            if (btn != MouseTriggerButton::Disabled)
            {
                TriggerBinding b;
                b.kind = TriggerKind::MouseButton;
                b.mouseButton = btn;
                b.modifiers = CurrentHeldModifiers();
                FinishCapturedBinding(b);
                return 1;
            }

            return CallNextHookEx(capMouseHook, code, wp, lp);
        }

        void CommitTriggersDelta()
        {
            config.SyncLegacyTriggerMirrors();
            dirty = true;
            SendDelta(MakeTriggersDelta(config));
            Invalidate();
        }

        void CommitGesturesDelta()
        {
            dirty = true;
            SendDelta(MakeGesturesDelta(config));
            Invalidate();
        }

        void CommitLookDelta()
        {
            dirty = true;
            SendDelta(MakeLookDelta(config));
            if (!livePreviewSent)
                SyncLiveRingPreview();
            Invalidate();
        }

        void CommitProfileEdit()
        {
            config.PullFromDefaultProfileMirror();
            config.Save();
            dirty = false;
            ClampPinsScroll();
            SendDelta(MakeReloadFromDiskDelta(selectedProfileId));
            SyncLiveRingPreview();
            Invalidate();
        }

        WindowLayout ComputeLayout() const
        {
            WindowLayout lo;

            // Unified sidebar Profiles container wrapping "Profiles" + all profiles under it
            {
                float py = 188.0f;
                for (size_t i = 0; i < config.profiles.size(); ++i)
                {
                    if (py + 26.0f > 508.0f)
                        break;
                    lo.sidebarProfileRow.push_back(HitRect{ 18.0f, py, 148.0f, py + 26.0f });
                    lo.sidebarProfileRenameBtn.push_back(HitRect{ 126.0f, py + 3.0f, 146.0f, py + 23.0f });
                    py += 28.0f;
                }
                lo.sidebarProfilesCard = HitRect{
                    12.0f, 148.0f, 152.0f,
                    ComputeUnifiedProfilesCardBottomDip(184.0f, lo.sidebarProfileRow.size(), 28.0f, 4.0f, 512.0f)
                };
            }

            // Footer clickable status pill
            {
                const EngineMode mode = CurrentEngineMode();
                const float pillR = (mode == EngineMode::SafetyLocked) ? 248.0f : 134.0f;
                lo.footerStatusPill = HitRect{ 10.0f, 526.0f, pillR, 554.0f };
            }

            // General shortcuts layout (x = 178..822, viewport 48..272)
            lo.triggerContentH = ComputeTriggersContentHeight();
            const float sViewH = kShortcutViewportBot - kShortcutViewportTop;
            const float sMax = (std::max)(0.0f, lo.triggerContentH - sViewH);
            const float sScroll = std::clamp(triggerScrollDip, 0.0f, sMax);
            const float pillR = (lo.triggerContentH > sViewH) ? 772.0f : 778.0f;
            float sy = kShortcutViewportTop - sScroll;
            const size_t tCount = (std::min)(config.triggers.size(), kMaxTriggerBindings);
            for (size_t i = 0; i < tCount; ++i)
            {
                lo.triggerPill[i] = HitRect{ 192.0f, sy, pillR, sy + 34.0f };
                lo.triggerProfileBadge[i] = HitRect{ pillR - 92.0f, sy + 5.0f, pillR - 6.0f, sy + 29.0f };
                lo.triggerRemove[i] = HitRect{ pillR + 6.0f, sy + 2.0f, pillR + 30.0f, sy + 32.0f };
                const TriggerConflictInfo ci = AnalyzeTriggerConflict(config.triggers[i]);
                sy += ci.summary.empty() ? 42.0f : 58.0f;
            }

            for (int i = 0; i < 4; ++i)
            {
                const float by = 312.0f + static_cast<float>(i) * 48.0f;
                lo.behaviorRow[i] = HitRect{ 178.0f, by, 822.0f, by + 48.0f };
            }

            // Appearance left column (178..466)
            for (int i = 0; i < 3; ++i)
            {
                const float x0 = 178.0f + static_cast<float>(i) * 98.0f;
                lo.texCard[i] = HitRect{ x0, 36.0f, x0 + 92.0f, 88.0f };
                lo.modeChip[i] = HitRect{ x0, 118.0f, x0 + 92.0f, 144.0f };
            }
            for (int i = 0; i < 8; ++i)
            {
                const float cx = 192.0f + static_cast<float>(i) * 36.5f;
                lo.surfaceDot[i] = HitRect{ cx - 12.0f, 154.0f, cx + 12.0f, 178.0f };
            }
            for (int i = 0; i < 7; ++i)
            {
                const float cx = 250.0f + static_cast<float>(i) * 33.5f;
                lo.accentDot[i] = HitRect{ cx - 11.5f, 215.0f, cx + 11.5f, 239.0f };
            }
            for (int i = 0; i < 3; ++i)
            {
                const float ty = 304.0f + static_cast<float>(i) * 72.0f;
                lo.sliderTrack[i] = HitRect{ 192.0f, ty, 452.0f, ty + 24.0f };
            }

            // Profiles right pane header + pinned list (178..466)
            {
                const RingProfileConfig& activeProf = config.ResolveProfile(selectedProfileId);
                const bool isDefault = (_wcsicmp(activeProf.id.c_str(), L"default") == 0);
                const bool isSMenu = (_wcsicmp(activeProf.id.c_str(), L"SMenu") == 0);
                if (ShouldShowProfileDeleteAction(isDefault, isSMenu))
                {
                    lo.openConfigBtn = HitRect{ 346.0f, 16.0f, 402.0f, 40.0f };
                    lo.profileResetBtn = HitRect{ 408.0f, 16.0f, 434.0f, 40.0f };
                    lo.profileDeleteBtn = HitRect{ 440.0f, 16.0f, 466.0f, 40.0f };
                }
                else
                {
                    lo.openConfigBtn = HitRect{ 378.0f, 16.0f, 434.0f, 40.0f };
                    lo.profileResetBtn = HitRect{ 440.0f, 16.0f, 466.0f, 40.0f };
                    lo.profileDeleteBtn = HitRect{};
                }

                lo.profilePinsContentH = static_cast<float>(activeProf.pinnedSlots.size()) * kPinRowH;
                const float pViewH = kPinsViewportBot - kPinsViewportTop;
                const float pMax = (std::max)(0.0f, lo.profilePinsContentH - pViewH);
                const float pScroll = std::clamp(pinsScrollDip, 0.0f, pMax);
                const float rowR = (lo.profilePinsContentH > pViewH) ? 450.0f : 458.0f;

                float py = kPinsViewportTop - pScroll;
                for (size_t i = 0; i < activeProf.pinnedSlots.size(); ++i)
                {
                    lo.profilePinRowRect.push_back(HitRect{ 186.0f, py + 2.0f, rowR, py + kPinRowH - 2.0f });
                    lo.profilePinRemoveBtn.push_back(HitRect{ rowR - 25.0f, py + 5.0f, rowR - 4.0f, py + kPinRowH - 5.0f });
                    lo.profilePinEditBtn.push_back(HitRect{ rowR - 64.0f, py + 5.0f, rowR - 29.0f, py + kPinRowH - 5.0f });
                    py += kPinRowH;
                }
            }

            return lo;
        }

        void DrawTogglePill(float x, float y, bool on, D2D1_COLOR_F accent, bool enabled = true)
        {
            auto mk = [&](D2D1_COLOR_F c) {
                ComPtr<ID2D1SolidColorBrush> br;
                d2dCtx->CreateSolidColorBrush(c, br.GetAddressOf());
                return br;
            };
            constexpr float w = 36.0f;
            constexpr float h = 20.0f;
            const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), 10.0f, 10.0f);
            D2D1_COLOR_F fill = on ? accent : RgbF(60, 60, 68);
            if (!enabled) fill.a *= 0.45f;
            if (auto b = mk(fill)) d2dCtx->FillRoundedRectangle(rr, b.Get());
            if (!on)
            {
                if (auto rb = mk(RgbF(86, 86, 96))) d2dCtx->DrawRoundedRectangle(rr, rb.Get(), 1.0f);
            }
            const float kx = on ? (x + w - 10.0f) : (x + 10.0f);
            if (auto kb = mk(RgbF(250, 250, 252, enabled ? 1.0f : 0.5f)))
                d2dCtx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, y + 10.0f), 6.5f, 6.5f), kb.Get());
        }

        static D2D1_COLOR_F LerpColorF(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) noexcept
        {
            const float u = std::clamp(t, 0.0f, 1.0f);
            return D2D1::ColorF(
                a.r + (b.r - a.r) * u,
                a.g + (b.g - a.g) * u,
                a.b + (b.b - a.b) * u,
                a.a + (b.a - a.a) * u);
        }

        void DrawPlusIcon(float cx, float cy, float halfSpan, D2D1_COLOR_F col, float strokeW = 1.5f)
        {
            ComPtr<ID2D1SolidColorBrush> br;
            if (FAILED(d2dCtx->CreateSolidColorBrush(col, br.GetAddressOf())) || !br)
                return;
            d2dCtx->DrawLine(D2D1::Point2F(cx - halfSpan, cy), D2D1::Point2F(cx + halfSpan, cy), br.Get(), strokeW);
            d2dCtx->DrawLine(D2D1::Point2F(cx, cy - halfSpan), D2D1::Point2F(cx, cy + halfSpan), br.Get(), strokeW);
        }

        void DrawRenameBoxPenIcon(float cx, float cy, D2D1_COLOR_F col)
        {
            ComPtr<ID2D1SolidColorBrush> br;
            if (FAILED(d2dCtx->CreateSolidColorBrush(col, br.GetAddressOf())) || !br)
                return;
            // Open square box (top-right corner open for the pen)
            const float l = cx - 4.5f;
            const float r = cx + 3.5f;
            const float t = cy - 3.5f;
            const float b = cy + 4.5f;
            d2dCtx->DrawLine(D2D1::Point2F(l + 4.2f, t), D2D1::Point2F(l, t), br.Get(), 1.25f);
            d2dCtx->DrawLine(D2D1::Point2F(l, t), D2D1::Point2F(l, b), br.Get(), 1.25f);
            d2dCtx->DrawLine(D2D1::Point2F(l, b), D2D1::Point2F(r, b), br.Get(), 1.25f);
            d2dCtx->DrawLine(D2D1::Point2F(r, b), D2D1::Point2F(r, b - 4.2f), br.Get(), 1.25f);
            // Diagonal pen stroke entering the box
            d2dCtx->DrawLine(D2D1::Point2F(cx - 0.8f, cy + 0.8f), D2D1::Point2F(cx + 4.8f, cy - 4.8f), br.Get(), 1.5f);
            d2dCtx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - 1.2f, cy + 1.2f), 0.7f, 0.7f), br.Get());
        }

        void DrawResetArrowIcon(float cx, float cy, D2D1_COLOR_F col)
        {
            ComPtr<ID2D1SolidColorBrush> br;
            if (FAILED(d2dCtx->CreateSolidColorBrush(col, br.GetAddressOf())) || !br)
                return;
            // Circular counter-clockwise reset arc with top-left arrowhead
            constexpr float r = 4.6f;
            ComPtr<ID2D1PathGeometry> arc;
            ComPtr<ID2D1GeometrySink> sink;
            if (d2dFactory && SUCCEEDED(d2dFactory->CreatePathGeometry(arc.GetAddressOf())) &&
                SUCCEEDED(arc->Open(sink.GetAddressOf())))
            {
                sink->BeginFigure(D2D1::Point2F(cx - 3.6f, cy - 2.8f), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(cx + 3.2f, cy + 3.3f),
                    D2D1::SizeF(r, r), 0.0f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(cx + 1.5f, cy - 4.3f),
                    D2D1::SizeF(r, r), 0.0f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                d2dCtx->DrawGeometry(arc.Get(), br.Get(), 1.35f);
            }
            else
            {
                d2dCtx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), br.Get(), 1.35f);
            }
            d2dCtx->DrawLine(D2D1::Point2F(cx - 4.4f, cy - 5.2f), D2D1::Point2F(cx - 3.8f, cy - 2.2f), br.Get(), 1.35f);
            d2dCtx->DrawLine(D2D1::Point2F(cx - 1.1f, cy - 2.4f), D2D1::Point2F(cx - 3.8f, cy - 2.2f), br.Get(), 1.35f);
        }

        void DrawTrashIcon(float cx, float cy, D2D1_COLOR_F col)
        {
            ComPtr<ID2D1SolidColorBrush> br;
            if (FAILED(d2dCtx->CreateSolidColorBrush(col, br.GetAddressOf())) || !br)
                return;
            d2dCtx->DrawLine(D2D1::Point2F(cx - 4.8f, cy - 3.2f), D2D1::Point2F(cx + 4.8f, cy - 3.2f), br.Get(), 1.3f);
            d2dCtx->DrawLine(D2D1::Point2F(cx - 1.8f, cy - 4.8f), D2D1::Point2F(cx + 1.8f, cy - 4.8f), br.Get(), 1.3f);
            d2dCtx->DrawLine(D2D1::Point2F(cx - 3.6f, cy - 3.0f), D2D1::Point2F(cx - 3.0f, cy + 4.6f), br.Get(), 1.3f);
            d2dCtx->DrawLine(D2D1::Point2F(cx + 3.6f, cy - 3.0f), D2D1::Point2F(cx + 3.0f, cy + 4.6f), br.Get(), 1.3f);
            d2dCtx->DrawLine(D2D1::Point2F(cx - 3.0f, cy + 4.6f), D2D1::Point2F(cx + 3.0f, cy + 4.6f), br.Get(), 1.3f);
            d2dCtx->DrawLine(D2D1::Point2F(cx - 1.2f, cy - 1.2f), D2D1::Point2F(cx - 1.0f, cy + 2.8f), br.Get(), 1.1f);
            d2dCtx->DrawLine(D2D1::Point2F(cx + 1.2f, cy - 1.2f), D2D1::Point2F(cx + 1.0f, cy + 2.8f), br.Get(), 1.1f);
        }

        void DrawWinRingsLogo()
        {
            if (logoBmp)
            {
                const D2D1_SIZE_F sz = logoBmp->GetSize();
                if (sz.width > 1.0f && sz.height > 1.0f)
                {
                    constexpr float boxL = 12.0f;
                    constexpr float boxT = 9.0f;
                    constexpr float boxW = 140.0f;
                    constexpr float boxH = 47.0f;
                    const float scale = (std::min)(boxW / sz.width, boxH / sz.height);
                    const float dw = sz.width * scale;
                    const float dh = sz.height * scale;
                    const float dx = boxL + (boxW - dw) * 0.5f;
                    const float dy = boxT + (boxH - dh) * 0.5f;
                    d2dCtx->DrawBitmap(
                        logoBmp.Get(),
                        D2D1::RectF(dx, dy, dx + dw, dy + dh),
                        1.0f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    return;
                }
            }

            ComPtr<ID2D1SolidColorBrush> whiteBr;
            ComPtr<ID2D1SolidColorBrush> blueBr;
            d2dCtx->CreateSolidColorBrush(RgbF(244, 245, 250), whiteBr.GetAddressOf());
            d2dCtx->CreateSolidColorBrush(RgbF(78, 168, 255), blueBr.GetAddressOf());

            const D2D1_GRADIENT_STOP stops[2] = {
                { 0.0f, RgbF(64, 204, 255) },
                { 1.0f, RgbF(10, 126, 232) }
            };
            ComPtr<ID2D1GradientStopCollection> stopCol;
            ComPtr<ID2D1LinearGradientBrush> ringGrad;
            if (SUCCEEDED(d2dCtx->CreateGradientStopCollection(stops, 2, stopCol.GetAddressOf())) && stopCol)
            {
                d2dCtx->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(15.0f, 22.0f), D2D1::Point2F(39.0f, 40.0f)),
                    stopCol.Get(), ringGrad.GetAddressOf());
            }

            if (ComPtr<ID2D1SolidColorBrush> glowBr;
                SUCCEEDED(d2dCtx->CreateSolidColorBrush(RgbF(38, 152, 255, 0.14f), glowBr.GetAddressOf())) && glowBr)
            {
                d2dCtx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(26.5f, 31.0f), 14.5f, 11.0f), glowBr.Get());
            }

            ID2D1Brush* ringStroke = ringGrad ? static_cast<ID2D1Brush*>(ringGrad.Get()) : static_cast<ID2D1Brush*>(blueBr.Get());
            if (ringStroke)
            {
                d2dCtx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(22.5f, 31.0f), 6.8f, 6.8f), ringStroke, 2.1f);
                d2dCtx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(30.5f, 31.0f), 6.8f, 6.8f), ringStroke, 2.1f);
            }

            if (dwrite && fmtLogo && whiteBr && blueBr)
            {
                ComPtr<IDWriteTextLayout> layout;
                if (SUCCEEDED(dwrite->CreateTextLayout(L"WinRings", 8, fmtLogo.Get(), 106.0f, 34.0f, layout.GetAddressOf())) && layout)
                {
                    layout->SetDrawingEffect(blueBr.Get(), DWRITE_TEXT_RANGE{ 3u, 5u });
                    d2dCtx->DrawTextLayout(D2D1::Point2F(44.0f, 14.0f), layout.Get(), whiteBr.Get());
                }
            }
        }

        void DrawSidebarNavIcon(int idx, float cx, float cy, D2D1_COLOR_F col)
        {
            ComPtr<ID2D1SolidColorBrush> br;
            if (FAILED(d2dCtx->CreateSolidColorBrush(col, br.GetAddressOf())) || !br)
                return;

            if (idx == 0)
            {
                // General: clean horizontal control sliders
                d2dCtx->DrawLine(D2D1::Point2F(cx - 7.0f, cy - 3.5f), D2D1::Point2F(cx + 7.0f, cy - 3.5f), br.Get(), 1.5f);
                d2dCtx->DrawLine(D2D1::Point2F(cx - 7.0f, cy + 3.5f), D2D1::Point2F(cx + 7.0f, cy + 3.5f), br.Get(), 1.5f);
                auto drawKnob = [&](float hx, float hy) {
                    ComPtr<ID2D1SolidColorBrush> bg;
                    d2dCtx->CreateSolidColorBrush(RgbF(30, 30, 34), bg.GetAddressOf());
                    if (bg) d2dCtx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(hx, hy), 2.7f, 2.7f), bg.Get());
                    d2dCtx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(hx, hy), 2.7f, 2.7f), br.Get(), 1.5f);
                };
                drawKnob(cx - 2.0f, cy - 3.5f);
                drawKnob(cx + 2.5f, cy + 3.5f);
            }
            else if (idx == 1)
            {
                // Appearance: standard artist color palette icon
                ComPtr<ID2D1PathGeometry> pal;
                ComPtr<ID2D1GeometrySink> sink;
                if (d2dFactory && SUCCEEDED(d2dFactory->CreatePathGeometry(pal.GetAddressOf())) &&
                    SUCCEEDED(pal->Open(sink.GetAddressOf())))
                {
                    sink->BeginFigure(D2D1::Point2F(cx - 6.8f, cy + 1.0f), D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddBezier(D2D1::BezierSegment(
                        D2D1::Point2F(cx - 7.2f, cy - 4.8f),
                        D2D1::Point2F(cx - 1.8f, cy - 7.2f),
                        D2D1::Point2F(cx + 3.2f, cy - 6.2f)));
                    sink->AddBezier(D2D1::BezierSegment(
                        D2D1::Point2F(cx + 7.6f, cy - 5.2f),
                        D2D1::Point2F(cx + 7.4f, cy - 0.4f),
                        D2D1::Point2F(cx + 4.4f, cy + 0.6f)));
                    sink->AddBezier(D2D1::BezierSegment(
                        D2D1::Point2F(cx + 1.8f, cy + 1.4f),
                        D2D1::Point2F(cx + 4.4f, cy + 5.6f),
                        D2D1::Point2F(cx + 0.8f, cy + 6.4f)));
                    sink->AddBezier(D2D1::BezierSegment(
                        D2D1::Point2F(cx - 4.0f, cy + 7.2f),
                        D2D1::Point2F(cx - 6.5f, cy + 4.2f),
                        D2D1::Point2F(cx - 6.8f, cy + 1.0f)));
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();
                    d2dCtx->DrawGeometry(pal.Get(), br.Get(), 1.45f);
                }
                else
                {
                    d2dCtx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 6.5f, 5.5f), br.Get(), 1.45f);
                }
                d2dCtx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - 3.2f, cy - 1.8f), 1.15f, 1.15f), br.Get());
                d2dCtx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 0.2f, cy - 3.2f), 1.15f, 1.15f), br.Get());
                d2dCtx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - 2.6f, cy + 2.2f), 1.15f, 1.15f), br.Get());
            }
            else
            {
                // Profiles: stacked profile cards
                const D2D1_ROUNDED_RECT back = D2D1::RoundedRect(D2D1::RectF(cx - 6.0f, cy - 6.5f, cx + 3.5f, cy + 2.5f), 2.0f, 2.0f);
                const D2D1_ROUNDED_RECT front = D2D1::RoundedRect(D2D1::RectF(cx - 3.0f, cy - 2.5f, cx + 6.5f, cy + 6.5f), 2.0f, 2.0f);
                ComPtr<ID2D1SolidColorBrush> bg;
                d2dCtx->CreateSolidColorBrush(RgbF(30, 30, 34), bg.GetAddressOf());
                d2dCtx->DrawRoundedRectangle(back, br.Get(), 1.4f);
                if (bg) d2dCtx->FillRoundedRectangle(front, bg.Get());
                d2dCtx->DrawRoundedRectangle(front, br.Get(), 1.5f);
            }
        }

        void RenderSidebarAndFooter(const WindowLayout& lo, const SwitcherConfig& eff, D2D1_COLOR_F accentCol)
        {
            auto mk = [&](D2D1_COLOR_F c) {
                ComPtr<ID2D1SolidColorBrush> br;
                d2dCtx->CreateSolidColorBrush(c, br.GetAddressOf());
                return br;
            };

            // Sidebar surface
            if (auto b = mk(RgbF(27, 27, 31)))
                d2dCtx->FillRectangle(D2D1::RectF(0.0f, 0.0f, kSidebarW, kFooterTop), b.Get());
            if (auto b = mk(RgbF(44, 44, 52)))
                d2dCtx->DrawLine(D2D1::Point2F(kSidebarW, 0.0f), D2D1::Point2F(kSidebarW, kFooterTop), b.Get(), 1.0f);

            DrawWinRingsLogo();

            const wchar_t* navLabels[3] = { L"General", L"Appearance", L"Profiles" };

            // Draw ONE unified container covering Profiles + all profiles beneath it
            // Hidden when inactive & unhovered; soft hover fill on hover; active card + border when selected.
            const bool profActive = (activeTab == 2);
            const bool profHovered = (!profActive && hoverProfilesCard);
            if (profActive || profHovered)
            {
                const D2D1_ROUNDED_RECT cr = D2D1::RoundedRect(lo.sidebarProfilesCard.ToD2D(), 7.0f, 7.0f);
                if (profActive)
                {
                    if (auto b = mk(RgbF(46, 46, 53))) d2dCtx->FillRoundedRectangle(cr, b.Get());
                    if (auto b = mk(RgbF(66, 66, 76))) d2dCtx->DrawRoundedRectangle(cr, b.Get(), 1.0f);
                }
                else
                {
                    if (auto b = mk(RgbF(35, 35, 41))) d2dCtx->FillRoundedRectangle(cr, b.Get());
                }
            }

            for (int i = 0; i < 3; ++i)
            {
                const bool active = (activeTab == i);
                const bool hovered = (!active && ((i == 2) ? profHovered : (hoverNav == i)));
                const HitRect& nr = lo.navItem[i];
                if (i < 2)
                {
                    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(nr.ToD2D(), 7.0f, 7.0f);
                    if (active)
                    {
                        if (auto b = mk(RgbF(46, 46, 53))) d2dCtx->FillRoundedRectangle(rr, b.Get());
                        if (auto b = mk(RgbF(66, 66, 76))) d2dCtx->DrawRoundedRectangle(rr, b.Get(), 1.0f);
                    }
                    else if (hovered)
                    {
                        if (auto b = mk(RgbF(35, 35, 41))) d2dCtx->FillRoundedRectangle(rr, b.Get());
                    }
                }

                const D2D1_COLOR_F fg = active ? RgbF(246, 246, 250) : (hovered ? RgbF(214, 214, 222) : RgbF(164, 164, 176));
                DrawSidebarNavIcon(i, nr.l + 18.0f, (nr.t + nr.b) * 0.5f, fg);
                const float labelR = (i == 2) ? (lo.sidebarProfileAddBtn.l - 4.0f) : (nr.r - 8.0f);
                if (fmtBold)
                {
                    if (auto b = mk(fg))
                        d2dCtx->DrawTextW(navLabels[i], static_cast<UINT32>(wcslen(navLabels[i])), fmtBold.Get(),
                            D2D1::RectF(nr.l + 36.0f, nr.t, labelR, nr.b), b.Get());
                }
            }

            // Dull-until-near '+' icon beside Profiles tab
            {
                const float alpha = (inlineForm == ProfileInlineForm::NewProfile)
                    ? 1.0f
                    : ResolveSidebarIconHoverAlpha(hoverSidebarAdd, hoverProfilesCard);
                if (hoverSidebarAdd || inlineForm == ProfileInlineForm::NewProfile)
                {
                    if (auto b = mk(RgbF(54, 54, 64)))
                        d2dCtx->FillRoundedRectangle(D2D1::RoundedRect(lo.sidebarProfileAddBtn.ToD2D(), 5.0f, 5.0f), b.Get());
                }
                const D2D1_COLOR_F dullBase = profActive ? RgbF(64, 64, 74) : (profHovered ? RgbF(52, 52, 60) : RgbF(44, 44, 52));
                const D2D1_COLOR_F plusCol = LerpColorF(dullBase, RgbF(236, 236, 244), alpha);
                DrawPlusIcon(
                    (lo.sidebarProfileAddBtn.l + lo.sidebarProfileAddBtn.r) * 0.5f,
                    (lo.sidebarProfileAddBtn.t + lo.sidebarProfileAddBtn.b) * 0.5f,
                    4.2f, plusCol, 1.5f);
            }

            // Profiles inside the unified container (clean rows, no nested stadiums)
            {
                const RingProfileConfig& activeProf = config.ResolveProfile(selectedProfileId);
                const size_t pCount = (std::min)(config.profiles.size(), lo.sidebarProfileRow.size());
                if (pCount > 0 && (profActive || profHovered))
                {
                    const D2D1_COLOR_F divCol = profActive ? RgbF(60, 60, 70) : RgbF(48, 48, 56);
                    if (auto b = mk(divCol))
                        d2dCtx->DrawLine(D2D1::Point2F(22.0f, 185.0f), D2D1::Point2F(142.0f, 185.0f), b.Get(), 1.0f);
                }
                const D2D1_COLOR_F dullRenBase = profActive ? RgbF(62, 62, 72) : (profHovered ? RgbF(50, 50, 58) : RgbF(44, 44, 52));
                for (size_t i = 0; i < pCount; ++i)
                {
                    const auto& p = config.profiles[i];
                    const bool isSel = (_wcsicmp(p.id.c_str(), activeProf.id.c_str()) == 0);
                    const bool rowHov = (hoverSidebarProf == static_cast<int>(i));
                    const bool renHov = (hoverSidebarRename == static_cast<int>(i));
                    const bool isRenaming = (inlineForm == ProfileInlineForm::RenameProfile &&
                        _wcsicmp(p.id.c_str(), renameProfileId.c_str()) == 0);
                    const HitRect& pr = lo.sidebarProfileRow[i];

                    if (isSel)
                    {
                        D2D1_COLOR_F barCol = accentCol;
                        if (activeTab != 2) barCol.a *= 0.6f;
                        if (auto b = mk(barCol))
                        {
                            d2dCtx->FillRoundedRectangle(
                                D2D1::RoundedRect(D2D1::RectF(pr.l + 2.0f, pr.t + 6.0f, pr.l + 4.5f, pr.b - 6.0f), 1.25f, 1.25f),
                                b.Get());
                        }
                    }

                    const HitRect& rb = lo.sidebarProfileRenameBtn[i];
                    if (fmtBody)
                    {
                        const D2D1_COLOR_F txtCol = isSel
                            ? ((activeTab == 2 || profHovered) ? RgbF(246, 246, 252) : RgbF(214, 214, 224))
                            : ((rowHov || profHovered) ? RgbF(216, 216, 228) : RgbF(158, 158, 172));
                        if (auto b = mk(txtCol))
                            d2dCtx->DrawTextW(p.name.c_str(), static_cast<UINT32>(p.name.size()), fmtBody.Get(),
                                D2D1::RectF(pr.l + 12.0f, pr.t, rb.l - 4.0f, pr.b), b.Get());
                    }

                    if (renHov || isRenaming)
                    {
                        if (auto b = mk(RgbF(54, 54, 64)))
                            d2dCtx->FillRoundedRectangle(D2D1::RoundedRect(rb.ToD2D(), 4.5f, 4.5f), b.Get());
                    }
                    const float renAlpha = isRenaming ? 1.0f : ResolveSidebarIconHoverAlpha(renHov, hoverProfilesCard);
                    const D2D1_COLOR_F renCol = LerpColorF(dullRenBase, RgbF(232, 232, 242), renAlpha);
                    DrawRenameBoxPenIcon((rb.l + rb.r) * 0.5f, (rb.t + rb.b) * 0.5f, renCol);
                }
            }

            // Bottom Footer Bar
            if (auto b = mk(RgbF(22, 22, 26)))
                d2dCtx->FillRectangle(D2D1::RectF(0.0f, kFooterTop, kWinW, kWinH), b.Get());
            if (auto b = mk(RgbF(44, 44, 52)))
                d2dCtx->DrawLine(D2D1::Point2F(0.0f, kFooterTop), D2D1::Point2F(kWinW, kFooterTop), b.Get(), 1.0f);

            const EngineMode mode = CurrentEngineMode();
            D2D1_COLOR_F dotCol = RgbF(160, 160, 170);
            const wchar_t* stText = L"Stopped";
            if (mode == EngineMode::Active) { dotCol = RgbF(52, 211, 153); stText = L"Active"; }
            else if (mode == EngineMode::Paused) { dotCol = RgbF(251, 191, 36); stText = L"Paused"; }
            else if (mode == EngineMode::Starting) { dotCol = RgbF(251, 191, 36); stText = L"Connecting..."; }
            else if (mode == EngineMode::SafetyLocked) { dotCol = RgbF(248, 113, 113); stText = L"Safety Locked (Click to Reset)"; }
            else if (mode == EngineMode::PreviewOnly) { dotCol = accentCol; stText = L"Preview Mode"; }

            const bool statusClickable = ShouldFooterStatusClickTogglePause(
                mode == EngineMode::Active, mode == EngineMode::Paused) || (mode == EngineMode::SafetyLocked);
            if (hoverStatusPill && statusClickable)
            {
                const D2D1_ROUNDED_RECT spr = D2D1::RoundedRect(lo.footerStatusPill.ToD2D(), 7.0f, 7.0f);
                if (auto b = mk(RgbF(36, 36, 44))) d2dCtx->FillRoundedRectangle(spr, b.Get());
                if (auto b = mk(RgbF(58, 58, 68))) d2dCtx->DrawRoundedRectangle(spr, b.Get(), 1.0f);
            }

            if (auto b = mk(dotCol))
            {
                d2dCtx->FillEllipse(D2D1::Ellipse(
                    D2D1::Point2F(22.0f, (lo.footerStatusPill.t + lo.footerStatusPill.b) * 0.5f), 4.0f, 4.0f), b.Get());
            }
            if (fmtBody)
            {
                if (auto b = mk(RgbF(214, 214, 224)))
                    d2dCtx->DrawTextW(stText, static_cast<UINT32>(wcslen(stText)), fmtBody.Get(),
                        D2D1::RectF(34.0f, lo.footerStatusPill.t, lo.footerStatusPill.r - 6.0f, lo.footerStatusPill.b), b.Get());
            }

            (void)eff;
            if (fmtCaption)
            {
                const wchar_t* ver = L"v4.0.0";
                if (auto b = mk(RgbF(132, 132, 144)))
                    d2dCtx->DrawTextW(ver, static_cast<UINT32>(wcslen(ver)), fmtCaption.Get(),
                        D2D1::RectF(kWinW - 76.0f, kFooterTop + 12.0f, kWinW - 18.0f, kWinH - 12.0f), b.Get());
            }
        }

        void RenderGeneralTab(const WindowLayout& lo, D2D1_COLOR_F accentCol)
        {
            auto mk = [&](D2D1_COLOR_F c) {
                ComPtr<ID2D1SolidColorBrush> br;
                d2dCtx->CreateSolidColorBrush(c, br.GetAddressOf());
                return br;
            };

            if (fmtCaption)
            {
                if (auto b = mk(RgbF(146, 146, 158)))
                    d2dCtx->DrawTextW(L"SHORTCUTS", 9, fmtCaption.Get(), D2D1::RectF(182.0f, 16.0f, 400.0f, 34.0f), b.Get());
            }

            {
                const D2D1_ROUNDED_RECT addR = D2D1::RoundedRect(lo.addTriggerBtn.ToD2D(), 6.0f, 6.0f);
                const bool full = config.triggers.size() >= kMaxTriggerBindings;
                if (auto b = mk(RgbF(40, 40, 46))) d2dCtx->FillRoundedRectangle(addR, b.Get());
                if (auto b = mk(RgbF(64, 64, 74))) d2dCtx->DrawRoundedRectangle(addR, b.Get(), 1.0f);
                if (fmtSmallCenter)
                {
                    if (auto b = mk(full ? RgbF(110, 110, 122) : RgbF(236, 236, 242)))
                        d2dCtx->DrawTextW(L"+ Add", 5, fmtSmallCenter.Get(), lo.addTriggerBtn.ToD2D(), b.Get());
                }
            }

            const D2D1_ROUNDED_RECT card1 = D2D1::RoundedRect(D2D1::RectF(178.0f, 40.0f, 822.0f, 278.0f), 10.0f, 10.0f);
            if (auto b = mk(RgbF(33, 33, 38))) d2dCtx->FillRoundedRectangle(card1, b.Get());
            if (auto b = mk(RgbF(50, 50, 58))) d2dCtx->DrawRoundedRectangle(card1, b.Get(), 1.0f);

            const size_t tCount = (std::min)(config.triggers.size(), kMaxTriggerBindings);
            if (tCount == 0 && fmtBody)
            {
                const wchar_t* empty = L"No triggers bound yet. Click + Add above to record a shortcut or mouse button.";
                if (auto b = mk(RgbF(146, 146, 158)))
                    d2dCtx->DrawTextW(empty, static_cast<UINT32>(wcslen(empty)), fmtBody.Get(),
                        D2D1::RectF(196.0f, 54.0f, 804.0f, 90.0f), b.Get());
            }
            else
            {
                d2dCtx->PushAxisAlignedClip(D2D1::RectF(180.0f, kShortcutViewportTop, 820.0f, kShortcutViewportBot),
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

                for (size_t i = 0; i < tCount; ++i)
                {
                    const auto& pr = lo.triggerPill[i];
                    if (pr.b + 20.0f < kShortcutViewportTop || pr.t > kShortcutViewportBot)
                        continue;

                    const bool isRec = (recordingIndex == static_cast<int>(i));
                    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(pr.ToD2D(), 7.0f, 7.0f);
                    if (auto b = mk(isRec ? RgbF(42, 46, 56) : RgbF(42, 42, 48))) d2dCtx->FillRoundedRectangle(rr, b.Get());
                    if (auto b = mk(isRec ? accentCol : RgbF(58, 58, 66))) d2dCtx->DrawRoundedRectangle(rr, b.Get(), isRec ? 1.4f : 1.0f);

                    const std::wstring label = isRec
                        ? L"Recording\u2026 Press key combo, solo Win/Alt, or mouse button"
                        : FormatTriggerBinding(config.triggers[i]);
                    if (fmtBold)
                    {
                        if (auto b = mk(isRec ? RgbF(248, 250, 255) : RgbF(240, 240, 246)))
                            d2dCtx->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), fmtBold.Get(),
                                D2D1::RectF(pr.l + 12.0f, pr.t, lo.triggerProfileBadge[i].l - 8.0f, pr.b), b.Get());
                    }

                    if (!isRec)
                    {
                        const auto& pb = lo.triggerProfileBadge[i];
                        const D2D1_ROUNDED_RECT pbr = D2D1::RoundedRect(pb.ToD2D(), 6.0f, 6.0f);
                        const std::wstring profId = config.triggers[i].profileId.empty() ? L"default" : config.triggers[i].profileId;
                        const RingProfileConfig& prof = config.ResolveProfile(profId);
                        if (auto b = mk(RgbF(52, 52, 60))) d2dCtx->FillRoundedRectangle(pbr, b.Get());
                        if (auto b = mk(RgbF(74, 74, 84))) d2dCtx->DrawRoundedRectangle(pbr, b.Get(), 1.0f);
                        const std::wstring pTxt = prof.name + L" \u25BE";
                        if (fmtSmallCenter)
                        {
                            if (auto b = mk(RgbF(228, 228, 236)))
                                d2dCtx->DrawTextW(pTxt.c_str(), static_cast<UINT32>(pTxt.size()), fmtSmallCenter.Get(), pb.ToD2D(), b.Get());
                        }
                    }

                    const HitRect& rm = lo.triggerRemove[i];
                    if (auto b = mk(RgbF(44, 44, 50)))
                        d2dCtx->FillRoundedRectangle(D2D1::RoundedRect(rm.ToD2D(), 6.0f, 6.0f), b.Get());
                    if (auto b = mk(RgbF(190, 190, 200)))
                    {
                        const float cx = (rm.l + rm.r) * 0.5f;
                        const float cy = (rm.t + rm.b) * 0.5f;
                        d2dCtx->DrawLine(D2D1::Point2F(cx - 3.5f, cy - 3.5f), D2D1::Point2F(cx + 3.5f, cy + 3.5f), b.Get(), 1.3f);
                        d2dCtx->DrawLine(D2D1::Point2F(cx + 3.5f, cy - 3.5f), D2D1::Point2F(cx - 3.5f, cy + 3.5f), b.Get(), 1.3f);
                    }

                    const TriggerConflictInfo ci = AnalyzeTriggerConflict(config.triggers[i]);
                    if (!ci.summary.empty() && fmtCaption)
                    {
                        if (auto b = mk(ci.hasWarning ? RgbF(214, 166, 74) : RgbF(138, 138, 150)))
                            d2dCtx->DrawTextW(ci.summary.c_str(), static_cast<UINT32>(ci.summary.size()), fmtCaption.Get(),
                                D2D1::RectF(pr.l + 4.0f, pr.b + 2.0f, 804.0f, pr.b + 18.0f), b.Get());
                    }
                }

                const float sViewH = kShortcutViewportBot - kShortcutViewportTop;
                if (lo.triggerContentH > sViewH)
                {
                    if (auto b = mk(RgbF(42, 42, 48)))
                        d2dCtx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(812.0f, kShortcutViewportTop, 815.0f, kShortcutViewportBot), 1.5f, 1.5f), b.Get());
                    const float thumbH = (std::max)(26.0f, sViewH * (sViewH / lo.triggerContentH));
                    const float maxScroll = lo.triggerContentH - sViewH;
                    const float frac = (maxScroll > 0.0f) ? std::clamp(triggerScrollDip / maxScroll, 0.0f, 1.0f) : 0.0f;
                    const float thumbY = kShortcutViewportTop + frac * (sViewH - thumbH);
                    if (auto b = mk(RgbF(126, 126, 138)))
                        d2dCtx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(812.0f, thumbY, 815.0f, thumbY + thumbH), 1.5f, 1.5f), b.Get());
                }

                d2dCtx->PopAxisAlignedClip();
            }

            if (fmtCaption)
            {
                if (auto b = mk(RgbF(146, 146, 158)))
                    d2dCtx->DrawTextW(L"BEHAVIOR", 8, fmtCaption.Get(), D2D1::RectF(182.0f, 290.0f, 400.0f, 308.0f), b.Get());
            }

            const D2D1_ROUNDED_RECT card2 = D2D1::RoundedRect(D2D1::RectF(178.0f, 312.0f, 822.0f, 504.0f), 10.0f, 10.0f);
            if (auto b = mk(RgbF(33, 33, 38))) d2dCtx->FillRoundedRectangle(card2, b.Get());
            if (auto b = mk(RgbF(50, 50, 58))) d2dCtx->DrawRoundedRectangle(card2, b.Get(), 1.0f);

            const wchar_t* bLabels[4] = {
                L"Tap to Open / Close",
                L"Hold, Point & Release to Switch",
                L"Double-Tap for Last 2 Apps",
                L"Glide Pointer to Window"
            };
            const bool bVals[4] = {
                config.gestureTapOpenClose,
                config.gestureHoldReleaseSwitch,
                config.doubleTapLastApp,
                config.pointerFollow
            };
            for (int i = 0; i < 4; ++i)
            {
                const HitRect& br = lo.behaviorRow[i];
                if (i > 0)
                {
                    if (auto b = mk(RgbF(44, 44, 52)))
                        d2dCtx->DrawLine(D2D1::Point2F(br.l + 14.0f, br.t), D2D1::Point2F(br.r - 14.0f, br.t), b.Get(), 1.0f);
                }
                if (fmtBody)
                {
                    if (auto b = mk(RgbF(236, 236, 242)))
                        d2dCtx->DrawTextW(bLabels[i], static_cast<UINT32>(wcslen(bLabels[i])), fmtBody.Get(),
                            D2D1::RectF(br.l + 16.0f, br.t, br.r - 72.0f, br.b), b.Get());
                }
                DrawTogglePill(br.r - 54.0f, (br.t + br.b) * 0.5f - 10.0f, bVals[i], accentCol);
            }
        }

        void DrawStageBackdrop(const RingProfileConfig& prof)
        {
            auto mk = [&](D2D1_COLOR_F c) {
                ComPtr<ID2D1SolidColorBrush> br;
                d2dCtx->CreateSolidColorBrush(c, br.GetAddressOf());
                return br;
            };
            const D2D1_ROUNDED_RECT sr = D2D1::RoundedRect(D2D1::RectF(kStageL, kStageT, kStageR, kStageB), 12.0f, 12.0f);
            if (auto b = mk(RgbF(19, 19, 23))) d2dCtx->FillRoundedRectangle(sr, b.Get());
            if (auto b = mk(RgbF(46, 46, 54))) d2dCtx->DrawRoundedRectangle(sr, b.Get(), 1.0f);

            const std::wstring cap = L"LIVE RING  \u00B7  " + prof.name;
            if (fmtCaption)
            {
                if (auto b = mk(RgbF(124, 124, 136)))
                    d2dCtx->DrawTextW(cap.c_str(), static_cast<UINT32>(cap.size()), fmtCaption.Get(),
                        D2D1::RectF(kStageL + 14.0f, kStageT + 10.0f, kStageR - 14.0f, kStageT + 28.0f), b.Get());
            }

            const EngineMode mode = CurrentEngineMode();
            if (mode == EngineMode::Paused || mode == EngineMode::Stopped || mode == EngineMode::SafetyLocked)
            {
                const wchar_t* msg = (mode == EngineMode::Paused)
                    ? L"Ring is paused. Click Paused in the bottom-left corner to resume."
                    : (mode == EngineMode::SafetyLocked
                        ? L"Ring hook is safety locked. Reset in status bar below."
                        : L"Connecting to Ring engine...");
                if (fmtSmallCenter)
                {
                    if (auto b = mk(RgbF(136, 136, 148)))
                        d2dCtx->DrawTextW(msg, static_cast<UINT32>(wcslen(msg)), fmtSmallCenter.Get(),
                            D2D1::RectF(kStageL + 24.0f, kStageT + 210.0f, kStageR - 24.0f, kStageT + 270.0f), b.Get());
                }
            }
        }

        void DrawInlineInput(const HitRect& rect, const std::wstring& val, const wchar_t* placeholder, bool focused)
        {
            auto mk = [&](D2D1_COLOR_F c) {
                ComPtr<ID2D1SolidColorBrush> br;
                d2dCtx->CreateSolidColorBrush(c, br.GetAddressOf());
                return br;
            };
            const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect.ToD2D(), 6.0f, 6.0f);
            if (auto b = mk(RgbF(22, 22, 27))) d2dCtx->FillRoundedRectangle(rr, b.Get());
            if (auto b = mk(focused ? RgbF(132, 132, 146) : RgbF(56, 56, 66)))
                d2dCtx->DrawRoundedRectangle(rr, b.Get(), focused ? 1.3f : 1.0f);

            std::wstring shown = val;
            if (focused && inlineCaretOn)
                shown.push_back(L'|');
            if (fmtBody)
            {
                const bool showPlaceholder = val.empty() && !focused;
                const wchar_t* text = showPlaceholder ? placeholder : shown.c_str();
                if (auto b = mk(showPlaceholder ? RgbF(118, 118, 130) : RgbF(240, 240, 248)))
                {
                    d2dCtx->DrawTextW(text, static_cast<UINT32>(wcslen(text)), fmtBody.Get(),
                        D2D1::RectF(rect.l + 9.0f, rect.t, rect.r - 9.0f, rect.b), b.Get());
                }
            }
        }

        void RenderInlineForm(const WindowLayout& lo, D2D1_COLOR_F accentCol)
        {
            auto mk = [&](D2D1_COLOR_F c) {
                ComPtr<ID2D1SolidColorBrush> br;
                d2dCtx->CreateSolidColorBrush(c, br.GetAddressOf());
                return br;
            };

            if (inlineForm == ProfileInlineForm::NewProfile || inlineForm == ProfileInlineForm::RenameProfile)
            {
                const bool isRename = (inlineForm == ProfileInlineForm::RenameProfile);
                if (fmtCaption)
                {
                    const wchar_t* hdr = isRename ? L"RENAME PROFILE" : L"NEW PROFILE NAME";
                    if (auto b = mk(RgbF(168, 168, 182)))
                        d2dCtx->DrawTextW(hdr, static_cast<UINT32>(wcslen(hdr)), fmtCaption.Get(),
                            D2D1::RectF(190.0f, 118.0f, 454.0f, 138.0f), b.Get());
                }
                DrawInlineInput(lo.inlineField1Rect, inlineField1,
                    isRename ? L"Profile display name..." : L"e.g. Work, Media, Design", true);
                if (fmtWrap)
                {
                    const wchar_t* note = isRename
                        ? L"Updates this profile's display name in the sidebar, ring title, and shortcut badges."
                        : L"Creates an empty ring profile under profiles\\<name>.json so you can bind it to any trigger.";
                    if (auto b = mk(RgbF(150, 150, 164)))
                        d2dCtx->DrawTextW(note, static_cast<UINT32>(wcslen(note)), fmtWrap.Get(),
                            D2D1::RectF(190.0f, 190.0f, 454.0f, 260.0f), b.Get());
                }
            }
            else if (inlineForm == ProfileInlineForm::AddLinkCmd)
            {
                if (fmtCaption)
                {
                    if (auto b = mk(RgbF(168, 168, 182)))
                    {
                        d2dCtx->DrawTextW(L"URL, PATH, OR COMMAND", 21, fmtCaption.Get(), D2D1::RectF(190.0f, 118.0f, 454.0f, 138.0f), b.Get());
                        d2dCtx->DrawTextW(L"DISPLAY LABEL (OPTIONAL)", 24, fmtCaption.Get(), D2D1::RectF(190.0f, 180.0f, 454.0f, 196.0f), b.Get());
                    }
                }
                DrawInlineInput(lo.inlineField1Rect, inlineField1, L"https://..., cmd.exe, or C:\\...", inlineFocusField == 0);
                DrawInlineInput(lo.inlineField2Rect, inlineField2, L"Auto-named if empty", inlineFocusField == 1);
                if (fmtBody)
                {
                    if (auto b = mk(RgbF(228, 228, 238)))
                        d2dCtx->DrawTextW(L"Run as Administrator", 20, fmtBody.Get(),
                            D2D1::RectF(lo.inlineAdminRow.l, lo.inlineAdminRow.t, lo.inlineAdminRow.r - 48.0f, lo.inlineAdminRow.b), b.Get());
                }
                DrawTogglePill(lo.inlineAdminRow.r - 38.0f, (lo.inlineAdminRow.t + lo.inlineAdminRow.b) * 0.5f - 10.0f, inlineAdmin, accentCol);
            }
            else if (inlineForm == ProfileInlineForm::EditSlot)
            {
                if (fmtCaption)
                {
                    if (auto b = mk(RgbF(168, 168, 182)))
                    {
                        d2dCtx->DrawTextW(L"DISPLAY NAME", 12, fmtCaption.Get(), D2D1::RectF(190.0f, 114.0f, 454.0f, 130.0f), b.Get());
                        d2dCtx->DrawTextW(L"CUSTOM ICON", 11, fmtCaption.Get(), D2D1::RectF(190.0f, 166.0f, 454.0f, 182.0f), b.Get());
                    }
                }
                DrawInlineInput(lo.editNameRect, inlineField1, L"Pin display label...", true);

                const D2D1_ROUNDED_RECT sr = D2D1::RoundedRect(lo.editIconStatusRect.ToD2D(), 6.0f, 6.0f);
                if (auto b = mk(RgbF(24, 24, 30))) d2dCtx->FillRoundedRectangle(sr, b.Get());
                if (auto b = mk(RgbF(56, 56, 66))) d2dCtx->DrawRoundedRectangle(sr, b.Get(), 1.0f);
                const std::wstring iconName = FormatCustomIconDisplayName(editIconPath);
                if (fmtBody)
                {
                    if (auto b = mk(editIconPath.empty() ? RgbF(150, 150, 164) : RgbF(232, 232, 242)))
                        d2dCtx->DrawTextW(iconName.c_str(), static_cast<UINT32>(iconName.size()), fmtBody.Get(),
                            D2D1::RectF(lo.editIconStatusRect.l + 8.0f, lo.editIconStatusRect.t, lo.editIconStatusRect.r - 6.0f, lo.editIconStatusRect.b), b.Get());
                }

                const D2D1_ROUNDED_RECT br = D2D1::RoundedRect(lo.editIconBrowseBtn.ToD2D(), 6.0f, 6.0f);
                if (auto b = mk(RgbF(42, 42, 50))) d2dCtx->FillRoundedRectangle(br, b.Get());
                if (auto b = mk(RgbF(68, 68, 78))) d2dCtx->DrawRoundedRectangle(br, b.Get(), 1.0f);
                if (fmtSmallCenter)
                {
                    if (auto b = mk(RgbF(228, 228, 238)))
                        d2dCtx->DrawTextW(L"Browse...", 9, fmtSmallCenter.Get(), lo.editIconBrowseBtn.ToD2D(), b.Get());
                }

                const bool hasIcon = !editIconPath.empty();
                const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(lo.editIconResetBtn.ToD2D(), 6.0f, 6.0f);
                if (auto b = mk(RgbF(38, 38, 44))) d2dCtx->FillRoundedRectangle(rr, b.Get());
                if (auto b = mk(hasIcon ? RgbF(74, 74, 84) : RgbF(52, 52, 60))) d2dCtx->DrawRoundedRectangle(rr, b.Get(), 1.0f);
                if (fmtSmallCenter)
                {
                    if (auto b = mk(hasIcon ? RgbF(228, 228, 238) : RgbF(110, 110, 122)))
                        d2dCtx->DrawTextW(L"Reset", 5, fmtSmallCenter.Get(), lo.editIconResetBtn.ToD2D(), b.Get());
                }

                const bool canHide = (editSlotKind != SlotKind::Collection && editSlotKind != SlotKind::RecycleBin);
                if (canHide)
                {
                    if (fmtBody)
                    {
                        if (auto b = mk(RgbF(230, 230, 240)))
                            d2dCtx->DrawTextW(L"Hide from Ring (Search Only)", 28, fmtBody.Get(),
                                D2D1::RectF(lo.editHideRow.l, lo.editHideRow.t, lo.editHideRow.r - 44.0f, lo.editHideRow.b), b.Get());
                    }
                    DrawTogglePill(lo.editHideRow.r - 36.0f, (lo.editHideRow.t + lo.editHideRow.b) * 0.5f - 10.0f, editHiddenFromRing, accentCol);
                }

                if (CanSlotToggleRunAsAdmin(editSlotKind, editSlotTarget))
                {
                    if (fmtBody)
                    {
                        if (auto b = mk(RgbF(230, 230, 240)))
                            d2dCtx->DrawTextW(L"Run as Administrator", 20, fmtBody.Get(),
                                D2D1::RectF(lo.editAdminRow.l, lo.editAdminRow.t, lo.editAdminRow.r - 44.0f, lo.editAdminRow.b), b.Get());
                    }
                    DrawTogglePill(lo.editAdminRow.r - 36.0f, (lo.editAdminRow.t + lo.editAdminRow.b) * 0.5f - 10.0f, inlineAdmin, accentCol);
                }
                else if (fmtWrap)
                {
                    const wchar_t* note = canHide
                        ? L"Hidden pins stay searchable without occupying a Ring slot."
                        : L"Changes apply immediately to this Ring item after Save.";
                    if (auto b = mk(RgbF(146, 146, 160)))
                        d2dCtx->DrawTextW(note, static_cast<UINT32>(wcslen(note)), fmtWrap.Get(), lo.editAdminRow.ToD2D(), b.Get());
                }
            }

            const D2D1_ROUNDED_RECT cancelR = D2D1::RoundedRect(lo.inlineCancelBtn.ToD2D(), 7.0f, 7.0f);
            if (auto b = mk(RgbF(40, 40, 48))) d2dCtx->FillRoundedRectangle(cancelR, b.Get());
            if (auto b = mk(RgbF(68, 68, 78))) d2dCtx->DrawRoundedRectangle(cancelR, b.Get(), 1.0f);
            if (fmtSmallCenter)
            {
                if (auto b = mk(RgbF(214, 214, 226)))
                    d2dCtx->DrawTextW(L"Cancel", 6, fmtSmallCenter.Get(), lo.inlineCancelBtn.ToD2D(), b.Get());
            }

            const bool canSubmit = !TrimWide(inlineField1).empty();
            D2D1_COLOR_F svFill = accentCol;
            svFill.a = canSubmit ? 0.28f : 0.12f;
            const D2D1_ROUNDED_RECT subR = D2D1::RoundedRect(lo.inlineSubmitBtn.ToD2D(), 7.0f, 7.0f);
            if (auto b = mk(svFill)) d2dCtx->FillRoundedRectangle(subR, b.Get());
            if (auto b = mk(canSubmit ? accentCol : RgbF(68, 68, 78))) d2dCtx->DrawRoundedRectangle(subR, b.Get(), 1.2f);
            const wchar_t* okText = (inlineForm == ProfileInlineForm::NewProfile) ? L"Create"
                : ((inlineForm == ProfileInlineForm::AddLinkCmd) ? L"Pin" : L"Save");
            if (fmtSmallCenter)
            {
                if (auto b = mk(canSubmit ? RgbF(248, 250, 255) : RgbF(130, 130, 144)))
                    d2dCtx->DrawTextW(okText, static_cast<UINT32>(wcslen(okText)), fmtSmallCenter.Get(), lo.inlineSubmitBtn.ToD2D(), b.Get());
            }
        }

        void RenderProfilesTab(const WindowLayout& lo, D2D1_COLOR_F accentCol)
        {
            auto mk = [&](D2D1_COLOR_F c) {
                ComPtr<ID2D1SolidColorBrush> br;
                d2dCtx->CreateSolidColorBrush(c, br.GetAddressOf());
                return br;
            };

            const RingProfileConfig& prof = config.ResolveProfile(selectedProfileId);
            DrawStageBackdrop(prof);

            const bool isDefault = (_wcsicmp(prof.id.c_str(), L"default") == 0);
            const bool isSMenu = (_wcsicmp(prof.id.c_str(), L"SMenu") == 0);

            if (fmtBold)
            {
                const float nameR = isDefault ? (lo.openConfigBtn.l - 8.0f) : (lo.openConfigBtn.l - 8.0f);
                if (auto b = mk(RgbF(242, 242, 248)))
                    d2dCtx->DrawTextW(prof.name.c_str(), static_cast<UINT32>(prof.name.size()), fmtBold.Get(),
                        D2D1::RectF(180.0f, 16.0f, nameR, 40.0f), b.Get());
            }

            auto drawBtn = [&](const HitRect& r, const wchar_t* text, bool hovered = false) {
                const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r.ToD2D(), 6.5f, 6.5f);
                if (auto b = mk(hovered ? RgbF(42, 42, 50) : RgbF(33, 33, 38))) d2dCtx->FillRoundedRectangle(rr, b.Get());
                if (auto b = mk(hovered ? RgbF(72, 72, 84) : RgbF(52, 52, 60))) d2dCtx->DrawRoundedRectangle(rr, b.Get(), 1.0f);
                if (fmtSmallCenter)
                {
                    if (auto b = mk(hovered ? RgbF(246, 246, 252) : RgbF(228, 228, 236)))
                        d2dCtx->DrawTextW(text, static_cast<UINT32>(wcslen(text)), fmtSmallCenter.Get(), r.ToD2D(), b.Get());
                }
            };
            drawBtn(lo.openConfigBtn, L"Config", hoverConfigBtn);

            auto drawRedIconBtn = [&](const HitRect& r, bool hovered, bool isResetIcon) {
                const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r.ToD2D(), 6.5f, 6.5f);
                if (auto b = mk(hovered ? RgbF(68, 30, 36) : RgbF(33, 33, 38))) d2dCtx->FillRoundedRectangle(rr, b.Get());
                if (auto b = mk(hovered ? RgbF(168, 66, 76) : RgbF(52, 52, 60))) d2dCtx->DrawRoundedRectangle(rr, b.Get(), 1.0f);
                const D2D1_COLOR_F iconCol = hovered ? RgbF(248, 113, 122) : RgbF(164, 164, 176);
                const float icx = (r.l + r.r) * 0.5f;
                const float icy = (r.t + r.b) * 0.5f;
                if (isResetIcon)
                    DrawResetArrowIcon(icx, icy, iconCol);
                else
                    DrawTrashIcon(icx, icy, iconCol);
            };
            drawRedIconBtn(lo.profileResetBtn, hoverProfileResetBtn, true);
            if (ShouldShowProfileDeleteAction(isDefault, isSMenu))
                drawRedIconBtn(lo.profileDeleteBtn, hoverProfileDeleteBtn, false);

            {
                const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(lo.profileRunningRow.ToD2D(), 8.0f, 8.0f);
                if (auto b = mk(RgbF(33, 33, 38))) d2dCtx->FillRoundedRectangle(rr, b.Get());
                if (auto b = mk(RgbF(50, 50, 58))) d2dCtx->DrawRoundedRectangle(rr, b.Get(), 1.0f);
                if (fmtBody)
                {
                    if (auto b = mk(RgbF(232, 232, 240)))
                        d2dCtx->DrawTextW(L"Include Running Apps", 20, fmtBody.Get(),
                            D2D1::RectF(lo.profileRunningRow.l + 12.0f, lo.profileRunningRow.t, lo.profileRunningRow.r - 50.0f, lo.profileRunningRow.b), b.Get());
                }
                DrawTogglePill(lo.profileRunningRow.r - 46.0f, (lo.profileRunningRow.t + lo.profileRunningRow.b) * 0.5f - 10.0f,
                    prof.includeRunningApps, accentCol);
            }

            // Pinned Items Card (178..466, y = 98..506)
            const D2D1_ROUNDED_RECT card = D2D1::RoundedRect(D2D1::RectF(178.0f, 98.0f, 466.0f, 506.0f), 10.0f, 10.0f);
            if (auto b = mk(RgbF(30, 30, 35))) d2dCtx->FillRoundedRectangle(card, b.Get());
            if (auto b = mk(inlineForm != ProfileInlineForm::None ? accentCol : RgbF(50, 50, 58)))
                d2dCtx->DrawRoundedRectangle(card, b.Get(), 1.0f);

            if (inlineForm != ProfileInlineForm::None)
            {
                RenderInlineForm(lo, accentCol);
            }
            else
            {
                if (fmtCaption)
                {
                    const std::wstring hdr = L"PINNED ITEMS (" + std::to_wstring(prof.pinnedSlots.size()) + L")";
                    if (auto b = mk(RgbF(146, 146, 158)))
                        d2dCtx->DrawTextW(hdr.c_str(), static_cast<UINT32>(hdr.size()), fmtCaption.Get(),
                            D2D1::RectF(190.0f, 107.0f, 454.0f, 125.0f), b.Get());
                }

                drawBtn(lo.addAppBtn, L"+ App / File");
                drawBtn(lo.addFolderBtn, L"+ Folder");
                drawBtn(lo.addLinkBtn, L"+ Link / Cmd");

                if (auto b = mk(RgbF(42, 42, 50)))
                    d2dCtx->DrawLine(D2D1::Point2F(188.0f, 166.0f), D2D1::Point2F(456.0f, 166.0f), b.Get(), 1.0f);

                if (prof.pinnedSlots.empty() && fmtWrap)
                {
                    const wchar_t* emptyMsg = L"No pinned icons yet. Use + App / File, + Folder, or + Link / Cmd above.";
                    if (auto b = mk(RgbF(146, 146, 160)))
                        d2dCtx->DrawTextW(emptyMsg, static_cast<UINT32>(wcslen(emptyMsg)), fmtWrap.Get(),
                            D2D1::RectF(192.0f, 186.0f, 452.0f, 260.0f), b.Get());
                }
                else
                {
                    d2dCtx->PushAxisAlignedClip(D2D1::RectF(180.0f, kPinsViewportTop, 464.0f, kPinsViewportBot),
                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

                    const size_t count = (std::min)(prof.pinnedSlots.size(), lo.profilePinRowRect.size());
                    for (size_t i = 0; i < count; ++i)
                    {
                        const auto& slot = prof.pinnedSlots[i];
                        const HitRect& rr = lo.profilePinRowRect[i];
                        if (rr.b < kPinsViewportTop || rr.t > kPinsViewportBot)
                            continue;

                        const D2D1_ROUNDED_RECT rowR = D2D1::RoundedRect(rr.ToD2D(), 6.0f, 6.0f);
                        if (auto b = mk(slot.hiddenFromRing ? RgbF(28, 28, 34) : RgbF(38, 38, 45)))
                            d2dCtx->FillRoundedRectangle(rowR, b.Get());
                        if (auto b = mk(slot.hiddenFromRing ? RgbF(46, 46, 56) : RgbF(54, 54, 64)))
                            d2dCtx->DrawRoundedRectangle(rowR, b.Get(), 1.0f);

                        std::wstring title = slot.label.empty() ? InferProfileItemLabel(slot.kind, slot.pathOrUrl) : slot.label;
                        if (slot.hiddenFromRing) title += L" \u00B7 Hidden";
                        if (slot.runAsAdmin) title += L" \u00B7 Admin";
                        if (!slot.iconCacheFile.empty() && slot.iconCacheFile.rfind(L"builtin:", 0) != 0)
                            title += L" \u00B7 Icon";

                        if (fmtBody)
                        {
                            if (auto b = mk(slot.hiddenFromRing ? RgbF(180, 180, 194) : RgbF(238, 238, 246)))
                                d2dCtx->DrawTextW(title.c_str(), static_cast<UINT32>(title.size()), fmtBody.Get(),
                                    D2D1::RectF(rr.l + 10.0f, rr.t, lo.profilePinEditBtn[i].l - 4.0f, rr.b), b.Get());
                        }

                        const D2D1_ROUNDED_RECT er = D2D1::RoundedRect(lo.profilePinEditBtn[i].ToD2D(), 5.0f, 5.0f);
                        if (auto b = mk(RgbF(48, 48, 56))) d2dCtx->FillRoundedRectangle(er, b.Get());
                        if (auto b = mk(RgbF(68, 68, 78))) d2dCtx->DrawRoundedRectangle(er, b.Get(), 1.0f);
                        if (fmtSmallCenter)
                        {
                            if (auto b = mk(RgbF(224, 224, 236)))
                                d2dCtx->DrawTextW(L"Edit", 4, fmtSmallCenter.Get(), lo.profilePinEditBtn[i].ToD2D(), b.Get());
                        }

                        const HitRect& rm = lo.profilePinRemoveBtn[i];
                        const D2D1_ROUNDED_RECT rmR = D2D1::RoundedRect(rm.ToD2D(), 5.0f, 5.0f);
                        if (auto b = mk(RgbF(46, 36, 40))) d2dCtx->FillRoundedRectangle(rmR, b.Get());
                        if (auto b = mk(RgbF(204, 152, 156)))
                        {
                            const float cx = (rm.l + rm.r) * 0.5f;
                            const float cy = (rm.t + rm.b) * 0.5f;
                            d2dCtx->DrawLine(D2D1::Point2F(cx - 3.0f, cy - 3.0f), D2D1::Point2F(cx + 3.0f, cy + 3.0f), b.Get(), 1.3f);
                            d2dCtx->DrawLine(D2D1::Point2F(cx + 3.0f, cy - 3.0f), D2D1::Point2F(cx - 3.0f, cy + 3.0f), b.Get(), 1.3f);
                        }
                    }

                    const float viewH = kPinsViewportBot - kPinsViewportTop;
                    if (lo.profilePinsContentH > viewH)
                    {
                        if (auto b = mk(RgbF(38, 38, 46)))
                            d2dCtx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(457.0f, kPinsViewportTop, 460.0f, kPinsViewportBot), 1.5f, 1.5f), b.Get());
                        const float thumbH = (std::max)(26.0f, viewH * (viewH / lo.profilePinsContentH));
                        const float maxScroll = lo.profilePinsContentH - viewH;
                        const float frac = (maxScroll > 0.0f) ? std::clamp(pinsScrollDip / maxScroll, 0.0f, 1.0f) : 0.0f;
                        const float thumbY = kPinsViewportTop + frac * (viewH - thumbH);
                        if (auto b = mk(RgbF(124, 124, 138)))
                            d2dCtx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(457.0f, thumbY, 460.0f, thumbY + thumbH), 1.5f, 1.5f), b.Get());
                    }

                    d2dCtx->PopAxisAlignedClip();
                }
            }
        }

        void RenderAppearanceTab(const WindowLayout& lo, const SwitcherConfig& eff, D2D1_COLOR_F accentCol)
        {
            auto mk = [&](D2D1_COLOR_F c) {
                ComPtr<ID2D1SolidColorBrush> br;
                d2dCtx->CreateSolidColorBrush(c, br.GetAddressOf());
                return br;
            };

            DrawStageBackdrop(config.ResolveProfile(selectedProfileId));

            if (fmtCaption)
            {
                if (auto b = mk(RgbF(146, 146, 158)))
                    d2dCtx->DrawTextW(L"SURFACE TEXTURE", 15, fmtCaption.Get(), D2D1::RectF(180.0f, 16.0f, 400.0f, 32.0f), b.Get());
            }

            struct TexOpt { const wchar_t* title; const wchar_t* sub; SurfaceTexture tex; };
            const TexOpt texOpts[3] = {
                { L"A4S Acrylic", L"Win11 WCA", SurfaceTexture::A4SAcrylic },
                { L"Taskbar Glass", L"Host Blur", SurfaceTexture::TaskbarGlass },
                { L"Matte Tint", L"Zero Blur", SurfaceTexture::PureMatte },
            };
            for (int i = 0; i < 3; ++i)
            {
                const bool active = (config.surfaceTexture == texOpts[i].tex);
                const HitRect& cr = lo.texCard[i];
                const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(cr.ToD2D(), 8.0f, 8.0f);
                if (auto b = mk(active ? RgbF(44, 46, 54) : RgbF(33, 33, 38))) d2dCtx->FillRoundedRectangle(rr, b.Get());
                if (auto b = mk(active ? accentCol : RgbF(52, 52, 60))) d2dCtx->DrawRoundedRectangle(rr, b.Get(), active ? 1.5f : 1.0f);
                if (fmtCenter)
                {
                    if (auto b = mk(active ? RgbF(248, 248, 252) : RgbF(214, 214, 222)))
                        d2dCtx->DrawTextW(texOpts[i].title, static_cast<UINT32>(wcslen(texOpts[i].title)), fmtCenter.Get(),
                            D2D1::RectF(cr.l + 4.0f, cr.t + 6.0f, cr.r - 4.0f, cr.t + 26.0f), b.Get());
                }
                if (fmtSmallCenter)
                {
                    if (auto b = mk(RgbF(146, 146, 158)))
                        d2dCtx->DrawTextW(texOpts[i].sub, static_cast<UINT32>(wcslen(texOpts[i].sub)), fmtSmallCenter.Get(),
                            D2D1::RectF(cr.l + 4.0f, cr.t + 28.0f, cr.r - 4.0f, cr.b - 6.0f), b.Get());
                }
            }

            if (fmtCaption)
            {
                if (auto b = mk(RgbF(146, 146, 158)))
                    d2dCtx->DrawTextW(L"SURFACE COLOR", 13, fmtCaption.Get(), D2D1::RectF(180.0f, 98.0f, 400.0f, 114.0f), b.Get());
            }

            const bool isDarkMode = !config.useCustomSurfaceColor && !config.autoLightDark && config.lightDarkMode < 50;
            const bool isLightMode = !config.useCustomSurfaceColor && !config.autoLightDark && config.lightDarkMode >= 50;
            const bool isAutoMode = !config.useCustomSurfaceColor && config.autoLightDark;
            const wchar_t* modeNames[3] = { L"Dark", L"Light", L"Auto System" };
            const bool modeActive[3] = { isDarkMode, isLightMode, isAutoMode };
            for (int i = 0; i < 3; ++i)
            {
                const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(lo.modeChip[i].ToD2D(), 7.0f, 7.0f);
                if (auto b = mk(modeActive[i] ? RgbF(44, 46, 54) : RgbF(33, 33, 38))) d2dCtx->FillRoundedRectangle(rr, b.Get());
                if (auto b = mk(modeActive[i] ? accentCol : RgbF(52, 52, 60))) d2dCtx->DrawRoundedRectangle(rr, b.Get(), modeActive[i] ? 1.4f : 1.0f);
                if (fmtSmallCenter)
                {
                    if (auto b = mk(modeActive[i] ? RgbF(248, 248, 252) : RgbF(184, 184, 196)))
                        d2dCtx->DrawTextW(modeNames[i], static_cast<UINT32>(wcslen(modeNames[i])), fmtSmallCenter.Get(),
                            lo.modeChip[i].ToD2D(), b.Get());
                }
            }

            for (int i = 0; i < 8; ++i)
            {
                const HitRect& sr = lo.surfaceDot[i];
                const D2D1_POINT_2F cc{ (sr.l + sr.r) * 0.5f, (sr.t + sr.b) * 0.5f };
                bool selected = false;
                if (i < 7)
                {
                    const auto& sw = kSurfaceSwatches[i];
                    if (sw.isNeutralDark) selected = isDarkMode;
                    else if (sw.isNeutralLight) selected = isLightMode;
                    else selected = config.useCustomSurfaceColor && _wcsicmp(config.surfaceColor.c_str(), sw.hex) == 0;
                    if (auto b = mk(RgbF(GetRValue(sw.rgb), GetGValue(sw.rgb), GetBValue(sw.rgb))))
                        d2dCtx->FillEllipse(D2D1::Ellipse(cc, 9.5f, 9.5f), b.Get());
                }
                else
                {
                    bool matchesBuiltIn = false;
                    for (const auto& sw : kSurfaceSwatches)
                    {
                        if (_wcsicmp(config.surfaceColor.c_str(), sw.hex) == 0)
                            matchesBuiltIn = true;
                    }
                    selected = config.useCustomSurfaceColor && !matchesBuiltIn;
                    if (auto b = mk(selected ? HexToD2D(config.surfaceColor) : RgbF(46, 46, 54)))
                        d2dCtx->FillEllipse(D2D1::Ellipse(cc, 9.5f, 9.5f), b.Get());
                    if (auto b = mk(RgbF(228, 228, 236)))
                    {
                        d2dCtx->DrawLine(D2D1::Point2F(cc.x - 3.6f, cc.y), D2D1::Point2F(cc.x + 3.6f, cc.y), b.Get(), 1.3f);
                        d2dCtx->DrawLine(D2D1::Point2F(cc.x, cc.y - 3.6f), D2D1::Point2F(cc.x, cc.y + 3.6f), b.Get(), 1.3f);
                    }
                }
                if (selected)
                {
                    if (auto b = mk(RgbF(248, 248, 252)))
                        d2dCtx->DrawEllipse(D2D1::Ellipse(cc, 11.6f, 11.6f), b.Get(), 1.5f);
                }
                else if (auto b = mk(RgbF(72, 72, 82)))
                {
                    d2dCtx->DrawEllipse(D2D1::Ellipse(cc, 9.5f, 9.5f), b.Get(), 1.0f);
                }
            }

            if (fmtCaption)
            {
                if (auto b = mk(RgbF(146, 146, 158)))
                    d2dCtx->DrawTextW(L"GLOW & HIGHLIGHT", 16, fmtCaption.Get(), D2D1::RectF(180.0f, 190.0f, 400.0f, 206.0f), b.Get());
            }

            {
                const D2D1_ROUNDED_RECT autoR = D2D1::RoundedRect(lo.accentAutoChip.ToD2D(), 7.0f, 7.0f);
                if (auto b = mk(config.autoWindowsAccent ? RgbF(44, 46, 54) : RgbF(33, 33, 38))) d2dCtx->FillRoundedRectangle(autoR, b.Get());
                if (auto b = mk(config.autoWindowsAccent ? accentCol : RgbF(52, 52, 60))) d2dCtx->DrawRoundedRectangle(autoR, b.Get(), config.autoWindowsAccent ? 1.4f : 1.0f);
                if (fmtSmallCenter)
                {
                    if (auto b = mk(config.autoWindowsAccent ? RgbF(248, 248, 252) : RgbF(184, 184, 196)))
                        d2dCtx->DrawTextW(L"Auto", 4, fmtSmallCenter.Get(), lo.accentAutoChip.ToD2D(), b.Get());
                }

                for (int i = 0; i < 7; ++i)
                {
                    const HitRect& ar = lo.accentDot[i];
                    const D2D1_POINT_2F cc{ (ar.l + ar.r) * 0.5f, (ar.t + ar.b) * 0.5f };
                    bool selected = false;
                    if (i < 6)
                    {
                        const auto& aw = kAccentSwatches[i];
                        selected = !config.autoWindowsAccent && _wcsicmp(config.accentColor.c_str(), aw.hex) == 0;
                        if (auto b = mk(RgbF(GetRValue(aw.rgb), GetGValue(aw.rgb), GetBValue(aw.rgb))))
                            d2dCtx->FillEllipse(D2D1::Ellipse(cc, 9.0f, 9.0f), b.Get());
                    }
                    else
                    {
                        bool matchesBuiltIn = false;
                        for (const auto& aw : kAccentSwatches)
                        {
                            if (_wcsicmp(config.accentColor.c_str(), aw.hex) == 0)
                                matchesBuiltIn = true;
                        }
                        selected = !config.autoWindowsAccent && !matchesBuiltIn;
                        if (auto b = mk(selected ? HexToD2D(config.accentColor) : RgbF(46, 46, 54)))
                            d2dCtx->FillEllipse(D2D1::Ellipse(cc, 9.0f, 9.0f), b.Get());
                        if (auto b = mk(RgbF(228, 228, 236)))
                        {
                            d2dCtx->DrawLine(D2D1::Point2F(cc.x - 3.4f, cc.y), D2D1::Point2F(cc.x + 3.4f, cc.y), b.Get(), 1.3f);
                            d2dCtx->DrawLine(D2D1::Point2F(cc.x, cc.y - 3.4f), D2D1::Point2F(cc.x, cc.y + 3.4f), b.Get(), 1.3f);
                        }
                    }
                    if (selected)
                    {
                        if (auto b = mk(RgbF(248, 248, 252)))
                            d2dCtx->DrawEllipse(D2D1::Ellipse(cc, 11.2f, 11.2f), b.Get(), 1.5f);
                    }
                }
            }

            if (fmtCaption)
            {
                if (auto b = mk(RgbF(146, 146, 158)))
                    d2dCtx->DrawTextW(L"TUNING", 6, fmtCaption.Get(), D2D1::RectF(180.0f, 252.0f, 400.0f, 268.0f), b.Get());
            }

            const D2D1_ROUNDED_RECT tuneCard = D2D1::RoundedRect(D2D1::RectF(178.0f, 272.0f, 466.0f, 506.0f), 10.0f, 10.0f);
            if (auto b = mk(RgbF(33, 33, 38))) d2dCtx->FillRoundedRectangle(tuneCard, b.Get());
            if (auto b = mk(RgbF(50, 50, 58))) d2dCtx->DrawRoundedRectangle(tuneCard, b.Get(), 1.0f);

            const int sizePct = std::clamp(static_cast<int>(std::lround((config.ringRadiusDip - 90.0) / 76.0 * 100.0)), 0, 100);
            const bool brightDisabled = (config.autoLightDark && !config.useCustomSurfaceColor);
            struct SliderSpec { const wchar_t* label; int val; bool disabled; };
            const SliderSpec sliders[3] = {
                { L"Glass / Opacity", config.glassLevel, false },
                { L"Brightness", eff.lightDarkMode, brightDisabled },
                { L"Ring Size", sizePct, false }
            };

            for (int i = 0; i < 3; ++i)
            {
                const HitRect& tr = lo.sliderTrack[i];
                const float topY = tr.t - 18.0f;
                const float midY = (tr.t + tr.b) * 0.5f;
                const float frac = static_cast<float>(std::clamp(sliders[i].val, 0, 100)) / 100.0f;
                const float kx = tr.l + (tr.r - tr.l) * frac;

                if (i > 0)
                {
                    if (auto b = mk(RgbF(44, 44, 52)))
                        d2dCtx->DrawLine(D2D1::Point2F(192.0f, topY - 11.0f), D2D1::Point2F(452.0f, topY - 11.0f), b.Get(), 1.0f);
                }

                if (fmtBody)
                {
                    if (auto b = mk(RgbF(236, 236, 242, sliders[i].disabled ? 0.45f : 1.0f)))
                        d2dCtx->DrawTextW(sliders[i].label, static_cast<UINT32>(wcslen(sliders[i].label)), fmtBody.Get(),
                            D2D1::RectF(tr.l, topY, tr.r - 48.0f, topY + 18.0f), b.Get());
                }
                wchar_t pctTxt[16]{};
                swprintf_s(pctTxt, L"%d%%", sliders[i].val);
                if (fmtCaption)
                {
                    if (auto b = mk(RgbF(168, 168, 180, sliders[i].disabled ? 0.45f : 1.0f)))
                        d2dCtx->DrawTextW(pctTxt, static_cast<UINT32>(wcslen(pctTxt)), fmtCaption.Get(),
                            D2D1::RectF(tr.r - 44.0f, topY, tr.r, topY + 18.0f), b.Get());
                }

                if (auto b = mk(RgbF(60, 60, 68, sliders[i].disabled ? 0.45f : 1.0f)))
                    d2dCtx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tr.l, midY - 2.0f, tr.r, midY + 2.0f), 2.0f, 2.0f), b.Get());
                D2D1_COLOR_F fillC = accentCol;
                if (sliders[i].disabled) fillC.a *= 0.45f;
                if (auto b = mk(fillC))
                    d2dCtx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tr.l, midY - 2.0f, kx, midY + 2.0f), 2.0f, 2.0f), b.Get());
                if (auto b = mk(RgbF(248, 248, 252, sliders[i].disabled ? 0.45f : 1.0f)))
                    d2dCtx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, midY), 6.5f, 6.5f), b.Get());
            }
        }

        void Render()
        {
            if (!d2dCtx || !targetBmp || !swapChain || !uiHwnd)
                return;

            const SwitcherConfig eff = ResolveEffectiveConfigForSystem(config);
            const D2D1_COLOR_F accentCol = HexToD2D(eff.accentColor);
            const WindowLayout lo = ComputeLayout();

            d2dCtx->SetTarget(targetBmp.Get());
            d2dCtx->BeginDraw();
            d2dCtx->SetTransform(D2D1::Matrix3x2F::Scale(dpiScale, dpiScale));
            d2dCtx->Clear(RgbF(24, 24, 27));

            RenderSidebarAndFooter(lo, eff, accentCol);

            if (activeTab == 0)
                RenderGeneralTab(lo, accentCol);
            else if (activeTab == 1)
                RenderAppearanceTab(lo, eff, accentCol);
            else
                RenderProfilesTab(lo, accentCol);

            const HRESULT hr = d2dCtx->EndDraw();
            d2dCtx->SetTarget(nullptr);
            if (hr == D2DERR_RECREATE_TARGET)
            {
                InitGraphics(swapW, swapH);
                return;
            }
            swapChain->Present(1, 0);
        }

        void ApplySliderAtX(int sliderIdx, float xDip)
        {
            const WindowLayout lo = ComputeLayout();
            const HitRect& tr = lo.sliderTrack[sliderIdx];
            const float frac = std::clamp((xDip - tr.l) / (tr.r - tr.l), 0.0f, 1.0f);
            const int pct = static_cast<int>(std::lround(frac * 100.0f));

            if (sliderIdx == 0)
            {
                if (config.glassLevel != pct)
                {
                    config.glassLevel = pct;
                    CommitLookDelta();
                }
            }
            else if (sliderIdx == 1)
            {
                if (config.autoLightDark && !config.useCustomSurfaceColor)
                    config.autoLightDark = false;
                if (config.lightDarkMode != pct)
                {
                    config.lightDarkMode = pct;
                    CommitLookDelta();
                }
            }
            else if (sliderIdx == 2)
            {
                const double nextRadius = 90.0 + static_cast<double>(pct) / 100.0 * 76.0;
                const double nextIcon = 34.0 + static_cast<double>(pct) / 100.0 * 22.0;
                if (std::abs(config.ringRadiusDip - nextRadius) > 0.25)
                {
                    config.ringRadiusDip = nextRadius;
                    config.iconSizeDip = nextIcon;
                    CommitLookDelta();
                }
            }
        }

        bool PickNativeColor(COLORREF initialColor, std::wstring& outHex)
        {
            static std::array<COLORREF, 16> s_customColors = {
                RGB(32,32,32), RGB(20,42,82), RGB(15,59,46), RGB(42,27,78),
                RGB(74,44,17), RGB(74,21,32), RGB(0,120,212), RGB(52,99,141),
                RGB(139,92,246), RGB(16,185,129), RGB(245,158,11), RGB(239,68,68),
                RGB(243,243,243), RGB(24,24,27), RGB(56,189,248), RGB(236,72,153)
            };
            CHOOSECOLORW cc{};
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = uiHwnd;
            cc.rgbResult = initialColor;
            cc.lpCustColors = s_customColors.data();
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;

            modalOpen = true;
            const BOOL ok = ChooseColorW(&cc);
            modalOpen = false;
            if (uiHwnd && IsWindow(uiHwnd))
                SetForegroundWindow(uiHwnd);
            if (!ok)
                return false;
            outHex = ColorRefToHex(cc.rgbResult);
            return true;
        }

        static PinnedSlotConfig BuildPickedSlotConfig(bool folderOnly, const std::wstring& pickedPath)
        {
            SlotKind kind = folderOnly ? SlotKind::Folder : SlotKind::File;
            if (!folderOnly)
            {
                const size_t dot = pickedPath.find_last_of(L'.');
                const std::wstring ext = (dot != std::wstring::npos) ? pickedPath.substr(dot) : L"";
                if (_wcsicmp(ext.c_str(), L".exe") == 0 || _wcsicmp(ext.c_str(), L".lnk") == 0)
                    kind = SlotKind::App;
                else if (_wcsicmp(ext.c_str(), L".bat") == 0 || _wcsicmp(ext.c_str(), L".cmd") == 0 || _wcsicmp(ext.c_str(), L".ps1") == 0)
                    kind = SlotKind::Script;
            }
            PinnedSlotConfig item;
            item.id = L"pin-" + std::to_wstring(GetTickCount64());
            item.kind = kind;
            item.label = InferProfileItemLabel(kind, pickedPath);
            item.pathOrUrl = pickedPath;
            return item;
        }

        void OpenInlineForm(ProfileInlineForm form)
        {
            inlineForm = form;
            inlineFocusField = 0;
            inlineField1.clear();
            inlineField2.clear();
            inlineAdmin = false;
            renameProfileId.clear();
            editSlotId.clear();
            editHiddenFromRing = false;
            editIconPath.clear();
            inlineCaretOn = true;
            if (uiHwnd)
                SetTimer(uiHwnd, kCaretBlinkTimerId, 530, nullptr);
            Invalidate();
        }

        void OpenRenameProfileForm(const RingProfileConfig& prof)
        {
            inlineForm = ProfileInlineForm::RenameProfile;
            inlineFocusField = 0;
            inlineField1 = prof.name;
            inlineField2.clear();
            inlineAdmin = false;
            renameProfileId = prof.id;
            editSlotId.clear();
            editHiddenFromRing = false;
            editIconPath.clear();
            inlineCaretOn = true;
            if (uiHwnd)
                SetTimer(uiHwnd, kCaretBlinkTimerId, 530, nullptr);
            Invalidate();
        }

        void OpenEditSlotForm(const PinnedSlotConfig& slot)
        {
            inlineForm = ProfileInlineForm::EditSlot;
            inlineFocusField = 0;
            inlineField1 = slot.label.empty() ? InferProfileItemLabel(slot.kind, slot.pathOrUrl) : slot.label;
            inlineField2.clear();
            inlineAdmin = slot.runAsAdmin;
            renameProfileId.clear();
            editSlotId = slot.id;
            editHiddenFromRing = slot.hiddenFromRing;
            editIconPath = slot.iconCacheFile;
            editSlotKind = slot.kind;
            editSlotTarget = slot.pathOrUrl;
            inlineCaretOn = true;
            if (uiHwnd)
                SetTimer(uiHwnd, kCaretBlinkTimerId, 530, nullptr);
            Invalidate();
        }

        void CloseInlineForm()
        {
            if (inlineForm == ProfileInlineForm::None)
                return;
            inlineForm = ProfileInlineForm::None;
            inlineField1.clear();
            inlineField2.clear();
            inlineAdmin = false;
            renameProfileId.clear();
            editSlotId.clear();
            editHiddenFromRing = false;
            editIconPath.clear();
            if (uiHwnd)
                KillTimer(uiHwnd, kCaretBlinkTimerId);
            Invalidate();
        }

        void HandleInlineFormClick(const WindowLayout& lo, float xDip, float yDip)
        {
            if (inlineForm == ProfileInlineForm::EditSlot)
            {
                if (lo.editNameRect.Contains(xDip, yDip))
                {
                    inlineFocusField = 0;
                    inlineCaretOn = true;
                    Invalidate();
                    return;
                }
                if (lo.editIconBrowseBtn.Contains(xDip, yDip))
                {
                    std::wstring picked;
                    modalOpen = true;
                    const bool ok = PickCustomIconDialog(uiHwnd, picked);
                    modalOpen = false;
                    if (uiHwnd && IsWindow(uiHwnd))
                        SetForegroundWindow(uiHwnd);
                    if (ok && !picked.empty())
                    {
                        editIconPath = std::move(picked);
                        Invalidate();
                    }
                    return;
                }
                if (lo.editIconResetBtn.Contains(xDip, yDip))
                {
                    if (!editIconPath.empty())
                    {
                        editIconPath.clear();
                        Invalidate();
                    }
                    return;
                }
                if (editSlotKind != SlotKind::Collection && editSlotKind != SlotKind::RecycleBin &&
                    lo.editHideRow.Contains(xDip, yDip))
                {
                    editHiddenFromRing = !editHiddenFromRing;
                    Invalidate();
                    return;
                }
                if (CanSlotToggleRunAsAdmin(editSlotKind, editSlotTarget) && lo.editAdminRow.Contains(xDip, yDip))
                {
                    inlineAdmin = !inlineAdmin;
                    Invalidate();
                    return;
                }
            }
            else
            {
                if (lo.inlineField1Rect.Contains(xDip, yDip))
                {
                    inlineFocusField = 0;
                    inlineCaretOn = true;
                    Invalidate();
                    return;
                }
                if (inlineForm == ProfileInlineForm::AddLinkCmd && lo.inlineField2Rect.Contains(xDip, yDip))
                {
                    inlineFocusField = 1;
                    inlineCaretOn = true;
                    Invalidate();
                    return;
                }
                if (inlineForm == ProfileInlineForm::AddLinkCmd && lo.inlineAdminRow.Contains(xDip, yDip))
                {
                    inlineAdmin = !inlineAdmin;
                    Invalidate();
                    return;
                }
            }

            if (lo.inlineCancelBtn.Contains(xDip, yDip))
            {
                CloseInlineForm();
                return;
            }
            if (lo.inlineSubmitBtn.Contains(xDip, yDip))
            {
                SubmitInlineForm();
                return;
            }
        }

        void SubmitInlineForm()
        {
            if (inlineForm == ProfileInlineForm::EditSlot)
            {
                const std::wstring newLabel = TrimWide(inlineField1);
                if (newLabel.empty())
                    return;

                RingProfileConfig* curProf = config.FindProfile(selectedProfileId);
                if (!curProf && !config.profiles.empty())
                    curProf = &config.profiles.front();
                if (curProf)
                {
                    for (auto& slot : curProf->pinnedSlots)
                    {
                        if (slot.id == editSlotId)
                        {
                            slot.label = newLabel;
                            slot.iconCacheFile = editIconPath;
                            slot.runAsAdmin = CanSlotToggleRunAsAdmin(slot.kind, slot.pathOrUrl) && inlineAdmin;
                            slot.hiddenFromRing = (slot.kind != SlotKind::Collection && slot.kind != SlotKind::RecycleBin) && editHiddenFromRing;
                            CloseInlineForm();
                            CommitProfileEdit();
                            return;
                        }
                    }
                }
                CloseInlineForm();
                return;
            }

            if (inlineForm == ProfileInlineForm::RenameProfile)
            {
                const std::wstring name = TrimWide(inlineField1);
                if (name.empty())
                    return;
                if (RingProfileConfig* prof = config.FindProfile(renameProfileId))
                {
                    prof->name = name;
                    selectedProfileId = prof->id;
                    CloseInlineForm();
                    CommitProfileEdit();
                    return;
                }
                CloseInlineForm();
                return;
            }

            if (inlineForm == ProfileInlineForm::NewProfile)
            {
                const std::wstring name = TrimWide(inlineField1);
                if (name.empty())
                    return;
                std::wstring newId = SwitcherConfig::SanitizeProfileId(name);
                if (config.FindProfile(newId))
                {
                    const std::wstring suffix = L"-" + std::to_wstring(GetTickCount64() % 10000);
                    if (newId.size() + suffix.size() > 28)
                        newId.resize(28 - suffix.size());
                    newId += suffix;
                }
                RingProfileConfig np;
                np.id = newId;
                np.name = name;
                np.includeRunningApps = false;
                config.profiles.push_back(std::move(np));
                selectedProfileId = newId;
                CloseInlineForm();
                CommitProfileEdit();
                return;
            }

            if (inlineForm == ProfileInlineForm::AddLinkCmd)
            {
                const std::wstring tgt = TrimWide(inlineField1);
                const std::wstring lbl = TrimWide(inlineField2);
                if (tgt.empty())
                    return;

                const bool isWeb = (tgt.rfind(L"http://", 0) == 0 || tgt.rfind(L"https://", 0) == 0);
                const bool isUri = (tgt.rfind(L"ms-settings:", 0) == 0 || tgt.rfind(L"shell:", 0) == 0 || tgt.rfind(L"winrings://", 0) == 0);
                SlotKind kind = isWeb ? SlotKind::Website : (isUri ? SlotKind::File : SlotKind::App);
                if (!isWeb && !isUri)
                {
                    const DWORD attrs = GetFileAttributesW(tgt.c_str());
                    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
                    {
                        kind = SlotKind::Folder;
                    }
                    else
                    {
                        const size_t dot = tgt.find_last_of(L'.');
                        const std::wstring ext = (dot != std::wstring::npos) ? tgt.substr(dot) : L"";
                        if (_wcsicmp(ext.c_str(), L".bat") == 0 || _wcsicmp(ext.c_str(), L".cmd") == 0 || _wcsicmp(ext.c_str(), L".ps1") == 0)
                            kind = SlotKind::Script;
                    }
                }

                PinnedSlotConfig item;
                item.id = L"pin-" + std::to_wstring(GetTickCount64());
                item.kind = kind;
                item.label = lbl.empty() ? InferProfileItemLabel(kind, tgt) : lbl;
                item.pathOrUrl = tgt;
                item.runAsAdmin = !isWeb && !isUri && (kind != SlotKind::Folder) && inlineAdmin;

                RingProfileConfig* curProf = config.FindProfile(selectedProfileId);
                if (!curProf && !config.profiles.empty())
                    curProf = &config.profiles.front();
                if (!curProf)
                    return;
                curProf->pinnedSlots.push_back(std::move(item));
                CloseInlineForm();
                CommitProfileEdit();
            }
        }

        void HandleClick(float xDip, float yDip)
        {
            const WindowLayout lo = ComputeLayout();

            // Sidebar add-profile '+' icon (inside Profiles nav item)
            if (lo.sidebarProfileAddBtn.Contains(xDip, yDip))
            {
                CancelCapture();
                const bool alreadyNew = (activeTab == 2 && inlineForm == ProfileInlineForm::NewProfile);
                activeTab = 2;
                SyncLiveRingPreview();
                if (alreadyNew)
                    CloseInlineForm();
                else
                    OpenInlineForm(ProfileInlineForm::NewProfile);
                return;
            }

            // Sidebar profile rows & rename box-and-pen icons
            {
                const size_t pCount = (std::min)(config.profiles.size(), lo.sidebarProfileRow.size());
                for (size_t i = 0; i < pCount; ++i)
                {
                    if (lo.sidebarProfileRenameBtn[i].Contains(xDip, yDip))
                    {
                        CancelCapture();
                        const bool alreadyThis = (activeTab == 2 &&
                            inlineForm == ProfileInlineForm::RenameProfile &&
                            _wcsicmp(config.profiles[i].id.c_str(), renameProfileId.c_str()) == 0);
                        selectedProfileId = config.profiles[i].id;
                        activeTab = 2;
                        pinsScrollDip = 0.0f;
                        if (alreadyThis)
                            CloseInlineForm();
                        else
                            OpenRenameProfileForm(config.profiles[i]);
                        SyncLiveRingPreview();
                        Invalidate();
                        return;
                    }
                    if (lo.sidebarProfileRow[i].Contains(xDip, yDip))
                    {
                        CancelCapture();
                        CloseInlineForm();
                        selectedProfileId = config.profiles[i].id;
                        activeTab = 2;
                        pinsScrollDip = 0.0f;
                        SyncLiveRingPreview();
                        Invalidate();
                        return;
                    }
                }
            }

            // Sidebar nav
            for (int i = 0; i < 3; ++i)
            {
                if (lo.navItem[i].Contains(xDip, yDip))
                {
                    if (activeTab != i)
                    {
                        CancelCapture();
                        CloseInlineForm();
                        activeTab = i;
                        SyncLiveRingPreview();
                        Invalidate();
                    }
                    return;
                }
            }

            // Footer clickable status pill (Active <-> Paused, or SafetyLocked -> Reset)
            const EngineMode mode = CurrentEngineMode();
            if (lo.footerStatusPill.Contains(xDip, yDip))
            {
                if (mode == EngineMode::SafetyLocked)
                {
                    if (bridge.resetSafetyAndStart)
                        bridge.resetSafetyAndStart();
                    NotifyStatus();
                    return;
                }
                if (ShouldFooterStatusClickTogglePause(mode == EngineMode::Active, mode == EngineMode::Paused))
                {
                    if (bridge.setPaused)
                        bridge.setPaused(mode == EngineMode::Active);
                    SyncLiveRingPreview();
                    Invalidate();
                    return;
                }
            }

            if (activeTab == 0)
            {
                if (lo.addTriggerBtn.Contains(xDip, yDip))
                {
                    CancelCapture();
                    if (config.triggers.size() < kMaxTriggerBindings)
                    {
                        config.triggers.push_back(TriggerBinding{ TriggerKind::Disabled, MouseTriggerButton::Disabled, 0, 0, L"default" });
                        const float viewH = kShortcutViewportBot - kShortcutViewportTop;
                        triggerScrollDip = (std::max)(0.0f, ComputeTriggersContentHeight() - viewH);
                        StartCapture(static_cast<int>(config.triggers.size()) - 1);
                    }
                    return;
                }
                if (yDip >= kShortcutViewportTop && yDip <= kShortcutViewportBot)
                {
                    const size_t tCount = (std::min)(config.triggers.size(), kMaxTriggerBindings);
                    for (size_t i = 0; i < tCount; ++i)
                    {
                        if (lo.triggerRemove[i].Contains(xDip, yDip))
                        {
                            StopCapture();
                            config.triggers.erase(config.triggers.begin() + static_cast<ptrdiff_t>(i));
                            PruneDisabledPlaceholders();
                            ClampTriggerScroll();
                            CommitTriggersDelta();
                            return;
                        }
                        if (lo.triggerProfileBadge[i].Contains(xDip, yDip) &&
                            recordingIndex != static_cast<int>(i))
                        {
                            CancelCapture();
                            HMENU pMenu = CreatePopupMenu();
                            if (pMenu)
                            {
                                for (size_t pi = 0; pi < config.profiles.size(); ++pi)
                                {
                                    const auto& p = config.profiles[pi];
                                    const std::wstring curId = config.triggers[i].profileId.empty() ? L"default" : config.triggers[i].profileId;
                                    const UINT flags = MF_STRING | ((_wcsicmp(p.id.c_str(), curId.c_str()) == 0) ? MF_CHECKED : 0u);
                                    AppendMenuW(pMenu, flags, 5000u + static_cast<UINT>(pi), p.name.c_str());
                                }
                                POINT menuPt{
                                    static_cast<LONG>(std::lround(lo.triggerProfileBadge[i].l * dpiScale)),
                                    static_cast<LONG>(std::lround(lo.triggerProfileBadge[i].b * dpiScale))
                                };
                                ClientToScreen(uiHwnd, &menuPt);
                                modalOpen = true;
                                const UINT picked = TrackPopupMenuEx(pMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                    menuPt.x, menuPt.y, uiHwnd, nullptr);
                                DestroyMenu(pMenu);
                                modalOpen = false;
                                if (picked >= 5000u)
                                {
                                    const size_t pIdx = static_cast<size_t>(picked - 5000u);
                                    if (pIdx < config.profiles.size())
                                    {
                                        config.triggers[i].profileId = config.profiles[pIdx].id;
                                        CommitTriggersDelta();
                                    }
                                }
                            }
                            return;
                        }
                        if (lo.triggerPill[i].Contains(xDip, yDip))
                        {
                            if (recordingIndex == static_cast<int>(i))
                            {
                                CancelCapture();
                                Invalidate();
                            }
                            else
                            {
                                if (recordingIndex >= 0 &&
                                    recordingIndex < static_cast<int>(config.triggers.size()) &&
                                    !config.triggers[static_cast<size_t>(recordingIndex)].IsActive())
                                {
                                    const size_t remIdx = static_cast<size_t>(recordingIndex);
                                    StopCapture();
                                    config.triggers.erase(config.triggers.begin() + static_cast<ptrdiff_t>(remIdx));
                                    if (remIdx < i)
                                        --i;
                                    ClampTriggerScroll();
                                }
                                StartCapture(static_cast<int>(i));
                            }
                            return;
                        }
                    }
                }
                for (int i = 0; i < 4; ++i)
                {
                    if (lo.behaviorRow[i].Contains(xDip, yDip))
                    {
                        CancelCapture();
                        if (i == 0) config.gestureTapOpenClose = !config.gestureTapOpenClose;
                        else if (i == 1) config.gestureHoldReleaseSwitch = !config.gestureHoldReleaseSwitch;
                        else if (i == 2) config.doubleTapLastApp = !config.doubleTapLastApp;
                        else config.pointerFollow = !config.pointerFollow;
                        CommitGesturesDelta();
                        return;
                    }
                }
            }
            else if (activeTab == 1)
            {
                const SurfaceTexture texValues[3] = {
                    SurfaceTexture::A4SAcrylic, SurfaceTexture::TaskbarGlass, SurfaceTexture::PureMatte
                };
                for (int i = 0; i < 3; ++i)
                {
                    if (lo.texCard[i].Contains(xDip, yDip))
                    {
                        config.surfaceTexture = texValues[i];
                        CommitLookDelta();
                        return;
                    }
                }
                for (int i = 0; i < 3; ++i)
                {
                    if (lo.modeChip[i].Contains(xDip, yDip))
                    {
                        config.useCustomSurfaceColor = false;
                        if (i == 0) { config.autoLightDark = false; config.lightDarkMode = 0; }
                        else if (i == 1) { config.autoLightDark = false; config.lightDarkMode = 100; }
                        else { config.autoLightDark = true; }
                        CommitLookDelta();
                        return;
                    }
                }
                for (int i = 0; i < 8; ++i)
                {
                    if (lo.surfaceDot[i].Contains(xDip, yDip))
                    {
                        if (i < 7)
                        {
                            const auto& sw = kSurfaceSwatches[i];
                            if (sw.isNeutralDark)
                            {
                                config.useCustomSurfaceColor = false;
                                config.autoLightDark = false;
                                config.lightDarkMode = 0;
                            }
                            else if (sw.isNeutralLight)
                            {
                                config.useCustomSurfaceColor = false;
                                config.autoLightDark = false;
                                config.lightDarkMode = 100;
                            }
                            else
                            {
                                config.useCustomSurfaceColor = true;
                                config.autoLightDark = false;
                                config.surfaceColor = sw.hex;
                                config.lightDarkMode = 0;
                            }
                            CommitLookDelta();
                        }
                        else
                        {
                            std::wstring picked;
                            if (PickNativeColor(HexToColorRef(config.surfaceColor), picked))
                            {
                                config.useCustomSurfaceColor = true;
                                config.autoLightDark = false;
                                config.surfaceColor = picked;
                                config.lightDarkMode = 0;
                                CommitLookDelta();
                            }
                        }
                        return;
                    }
                }
                if (lo.accentAutoChip.Contains(xDip, yDip))
                {
                    config.autoWindowsAccent = !config.autoWindowsAccent;
                    CommitLookDelta();
                    return;
                }
                for (int i = 0; i < 7; ++i)
                {
                    if (lo.accentDot[i].Contains(xDip, yDip))
                    {
                        if (i < 6)
                        {
                            config.autoWindowsAccent = false;
                            config.accentColor = kAccentSwatches[i].hex;
                            CommitLookDelta();
                        }
                        else
                        {
                            std::wstring picked;
                            if (PickNativeColor(HexToColorRef(config.accentColor), picked))
                            {
                                config.autoWindowsAccent = false;
                                config.accentColor = picked;
                                CommitLookDelta();
                            }
                        }
                        return;
                    }
                }
                for (int i = 0; i < 3; ++i)
                {
                    if (lo.sliderTrack[i].Contains(xDip, yDip))
                    {
                        dragSlider = i;
                        SetCapture(uiHwnd);
                        ApplySliderAtX(i, xDip);
                        return;
                    }
                }
            }
            else
            {
                RingProfileConfig* curProf = config.FindProfile(selectedProfileId);
                if (!curProf && !config.profiles.empty())
                    curProf = &config.profiles.front();

                if (curProf && lo.profileResetBtn.Contains(xDip, yDip))
                {
                    CloseInlineForm();
                    if (_wcsicmp(curProf->id.c_str(), L"default") == 0)
                    {
                        *curProf = SwitcherConfig::CreateDefault().ResolveProfile(L"default");
                    }
                    else if (_wcsicmp(curProf->id.c_str(), L"SMenu") == 0)
                    {
                        *curProf = SwitcherConfig::CreateBuiltInSMenuProfile();
                    }
                    else
                    {
                        curProf->pinnedSlots.clear();
                        curProf->includeRunningApps = false;
                    }
                    CommitProfileEdit();
                    return;
                }

                if (curProf &&
                    ShouldShowProfileDeleteAction(
                        _wcsicmp(curProf->id.c_str(), L"default") == 0,
                        _wcsicmp(curProf->id.c_str(), L"SMenu") == 0) &&
                    lo.profileDeleteBtn.Contains(xDip, yDip))
                {
                    CloseInlineForm();
                    const std::wstring removedId = curProf->id;
                    config.profiles.erase(
                        std::remove_if(config.profiles.begin(), config.profiles.end(),
                            [&](const RingProfileConfig& p) { return _wcsicmp(p.id.c_str(), removedId.c_str()) == 0; }),
                        config.profiles.end());
                    SwitcherConfig::DeleteProfileFile(removedId);
                    bool trigChanged = false;
                    for (auto& t : config.triggers)
                    {
                        if (_wcsicmp(t.profileId.c_str(), removedId.c_str()) == 0)
                        {
                            t.profileId = L"default";
                            trigChanged = true;
                        }
                    }
                    if (trigChanged)
                    {
                        config.SyncLegacyTriggerMirrors();
                        SendDelta(MakeTriggersDelta(config));
                    }
                    selectedProfileId = L"default";
                    CommitProfileEdit();
                    return;
                }

                if (!curProf)
                    return;

                if (lo.openConfigBtn.Contains(xDip, yDip))
                {
                    config.SaveProfile(curProf->id);
                    const std::wstring cfgFile = SwitcherConfig::ProfileFilePath(curProf->id);
                    const std::wstring params = L"/select,\"" + cfgFile + L"\"";
                    ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
                    return;
                }

                if (lo.profileRunningRow.Contains(xDip, yDip))
                {
                    curProf->includeRunningApps = !curProf->includeRunningApps;
                    CommitProfileEdit();
                    return;
                }

                if (inlineForm != ProfileInlineForm::None)
                {
                    HandleInlineFormClick(lo, xDip, yDip);
                    return;
                }

                if (lo.addAppBtn.Contains(xDip, yDip) || lo.addFolderBtn.Contains(xDip, yDip))
                {
                    CloseInlineForm();
                    const bool folderOnly = lo.addFolderBtn.Contains(xDip, yDip);
                    std::wstring pickedPath;
                    modalOpen = true;
                    const bool ok = PickFileOrFolderDialog(uiHwnd, folderOnly, pickedPath);
                    modalOpen = false;
                    if (uiHwnd && IsWindow(uiHwnd))
                        SetForegroundWindow(uiHwnd);
                    if (ok && !pickedPath.empty())
                    {
                        curProf->pinnedSlots.push_back(BuildPickedSlotConfig(folderOnly, pickedPath));
                        CommitProfileEdit();
                    }
                    return;
                }

                if (lo.addLinkBtn.Contains(xDip, yDip))
                {
                    OpenInlineForm(ProfileInlineForm::AddLinkCmd);
                    return;
                }

                if (yDip >= kPinsViewportTop && yDip <= kPinsViewportBot)
                {
                    const size_t count = (std::min)(curProf->pinnedSlots.size(), lo.profilePinRowRect.size());
                    for (size_t i = 0; i < count; ++i)
                    {
                        if (lo.profilePinRemoveBtn[i].Contains(xDip, yDip))
                        {
                            curProf->pinnedSlots.erase(curProf->pinnedSlots.begin() + static_cast<ptrdiff_t>(i));
                            CommitProfileEdit();
                            return;
                        }
                        if (lo.profilePinEditBtn[i].Contains(xDip, yDip) || lo.profilePinRowRect[i].Contains(xDip, yDip))
                        {
                            OpenEditSlotForm(curProf->pinnedSlots[i]);
                            return;
                        }
                    }
                }
            }
        }

        LRESULT HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            switch (msg)
            {
            case WM_ERASEBKGND:
                return 1;

            case WM_PAINT:
            {
                PAINTSTRUCT ps{};
                BeginPaint(hwnd, &ps);
                Render();
                EndPaint(hwnd, &ps);
                return 0;
            }

            case WM_SIZE:
                minimized = (wp == SIZE_MINIMIZED);
                if (!minimized)
                {
                    const int cw = LOWORD(lp);
                    const int ch = HIWORD(lp);
                    if (cw > 0 && ch > 0)
                        ResizeSwapChain(cw, ch);
                }
                SyncLiveRingPreview();
                return 0;

            case WM_MOVE:
                SyncLiveRingPreview();
                return 0;

            case WM_WINDOWPOSCHANGED:
                if (IsWindowVisible(hwnd) && !IsIconic(hwnd))
                    SyncLiveRingPreview();
                break;

            case WM_DPICHANGED:
            {
                const UINT newDpi = HIWORD(wp);
                if (newDpi > 0)
                    dpiScale = static_cast<float>(newDpi) / 96.0f;
                const RECT* suggested = reinterpret_cast<const RECT*>(lp);
                if (suggested)
                {
                    SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                        suggested->right - suggested->left, suggested->bottom - suggested->top,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                }
                RECT crc{};
                GetClientRect(hwnd, &crc);
                ResizeSwapChain(crc.right - crc.left, crc.bottom - crc.top);
                SyncLiveRingPreview();
                Invalidate();
                return 0;
            }

            case WM_ACTIVATE:
            {
                const bool activating = (LOWORD(wp) != WA_INACTIVE);
                if (!activating)
                {
                    if (modalOpen || recordingIndex >= 0)
                        return 0;
                    HWND fg = GetForegroundWindow();
                    wchar_t cls[64]{};
                    if (fg && GetClassNameW(fg, cls, _countof(cls)) > 0 &&
                        (wcscmp(cls, WINRINGS_OVERLAY_CLASS_W) == 0 ||
                         wcscmp(cls, L"WinRings.WcaBackdrop.v1") == 0 ||
                         wcscmp(cls, L"WinRings.ContextFlyout.v1") == 0 ||
                         wcscmp(cls, WINRINGS_THUMBCARD_CLASS_W) == 0))
                    {
                        return 0;
                    }
                    windowActive = false;
                }
                else
                {
                    windowActive = true;
                }
                SyncLiveRingPreview();
                return 0;
            }

            case WM_TIMER:
                if (wp == kCaretBlinkTimerId)
                {
                    if (activeTab == 2 && inlineForm != ProfileInlineForm::None)
                    {
                        inlineCaretOn = !inlineCaretOn;
                        Invalidate();
                    }
                    else
                    {
                        KillTimer(hwnd, kCaretBlinkTimerId);
                    }
                    return 0;
                }
                break;

            case WM_LBUTTONDOWN:
            {
                const float xDip = static_cast<float>(GET_X_LPARAM(lp)) / dpiScale;
                const float yDip = static_cast<float>(GET_Y_LPARAM(lp)) / dpiScale;
                HandleClick(xDip, yDip);
                return 0;
            }

            case WM_MOUSEMOVE:
            {
                if (!trackingMouseLeave)
                {
                    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                    if (TrackMouseEvent(&tme))
                        trackingMouseLeave = true;
                }
                const float xDip = static_cast<float>(GET_X_LPARAM(lp)) / dpiScale;
                const float yDip = static_cast<float>(GET_Y_LPARAM(lp)) / dpiScale;
                if (dragSlider >= 0)
                {
                    ApplySliderAtX(dragSlider, xDip);
                    return 0;
                }
                const WindowLayout lo = ComputeLayout();
                int nextHoverNav = -1;
                for (int i = 0; i < 3; ++i)
                {
                    if (lo.navItem[i].Contains(xDip, yDip))
                    {
                        nextHoverNav = i;
                        break;
                    }
                }
                const bool nextHoverProfilesCard = lo.sidebarProfilesCard.Contains(xDip, yDip);
                const bool nextHoverAdd = lo.sidebarProfileAddBtn.Contains(xDip, yDip);
                int nextHoverProf = -1;
                int nextHoverRename = -1;
                for (size_t i = 0; i < lo.sidebarProfileRow.size(); ++i)
                {
                    if (lo.sidebarProfileRow[i].Contains(xDip, yDip))
                    {
                        nextHoverProf = static_cast<int>(i);
                        if (i < lo.sidebarProfileRenameBtn.size() &&
                            lo.sidebarProfileRenameBtn[i].Contains(xDip, yDip))
                            nextHoverRename = static_cast<int>(i);
                        break;
                    }
                }
                const bool nextHoverStatus = lo.footerStatusPill.Contains(xDip, yDip);
                const bool nextHoverConfigBtn = (activeTab == 2) && lo.openConfigBtn.Contains(xDip, yDip);
                const bool nextHoverProfileResetBtn = (activeTab == 2) && lo.profileResetBtn.Contains(xDip, yDip);
                const bool nextHoverProfileDeleteBtn = (activeTab == 2) &&
                    lo.profileDeleteBtn.r > lo.profileDeleteBtn.l &&
                    lo.profileDeleteBtn.Contains(xDip, yDip);

                if (nextHoverNav != hoverNav || nextHoverProfilesCard != hoverProfilesCard ||
                    nextHoverAdd != hoverSidebarAdd || nextHoverProf != hoverSidebarProf ||
                    nextHoverRename != hoverSidebarRename || nextHoverStatus != hoverStatusPill ||
                    nextHoverConfigBtn != hoverConfigBtn ||
                    nextHoverProfileResetBtn != hoverProfileResetBtn ||
                    nextHoverProfileDeleteBtn != hoverProfileDeleteBtn)
                {
                    hoverNav = nextHoverNav;
                    hoverProfilesCard = nextHoverProfilesCard;
                    hoverSidebarAdd = nextHoverAdd;
                    hoverSidebarProf = nextHoverProf;
                    hoverSidebarRename = nextHoverRename;
                    hoverStatusPill = nextHoverStatus;
                    hoverConfigBtn = nextHoverConfigBtn;
                    hoverProfileResetBtn = nextHoverProfileResetBtn;
                    hoverProfileDeleteBtn = nextHoverProfileDeleteBtn;
                    Invalidate();
                }
                return 0;
            }

            case WM_MOUSELEAVE:
                trackingMouseLeave = false;
                if (hoverNav != -1 || hoverProfilesCard || hoverSidebarAdd || hoverSidebarProf != -1 ||
                    hoverSidebarRename != -1 || hoverStatusPill || hoverConfigBtn ||
                    hoverProfileResetBtn || hoverProfileDeleteBtn)
                {
                    hoverNav = -1;
                    hoverProfilesCard = false;
                    hoverSidebarAdd = false;
                    hoverSidebarProf = -1;
                    hoverSidebarRename = -1;
                    hoverStatusPill = false;
                    hoverConfigBtn = false;
                    hoverProfileResetBtn = false;
                    hoverProfileDeleteBtn = false;
                    Invalidate();
                }
                return 0;

            case WM_LBUTTONUP:
                if (dragSlider >= 0)
                {
                    dragSlider = -1;
                    if (GetCapture() == hwnd)
                        ReleaseCapture();
                }
                return 0;

            case WM_MOUSEWHEEL:
            {
                const short wheelDelta = GET_WHEEL_DELTA_WPARAM(wp);
                if (wheelDelta == 0)
                    break;
                if (activeTab == 0)
                {
                    const float maxScroll = (std::max)(0.0f, ComputeTriggersContentHeight() - (kShortcutViewportBot - kShortcutViewportTop));
                    const float nextScroll = std::clamp(
                        triggerScrollDip - (static_cast<float>(wheelDelta) / 120.0f) * 32.0f,
                        0.0f, maxScroll);
                    if (maxScroll > 0.0f && std::abs(nextScroll - triggerScrollDip) > 0.25f)
                    {
                        triggerScrollDip = nextScroll;
                        Invalidate();
                    }
                    return 0;
                }
                if (activeTab == 2 && inlineForm == ProfileInlineForm::None)
                {
                    const float maxScroll = (std::max)(0.0f, ComputePinsContentHeight() - (kPinsViewportBot - kPinsViewportTop));
                    const float nextScroll = std::clamp(
                        pinsScrollDip - (static_cast<float>(wheelDelta) / 120.0f) * 32.0f,
                        0.0f, maxScroll);
                    if (maxScroll > 0.0f && std::abs(nextScroll - pinsScrollDip) > 0.25f)
                    {
                        pinsScrollDip = nextScroll;
                        Invalidate();
                    }
                    return 0;
                }
                break;
            }

            case WM_KEYDOWN:
                if (activeTab == 2 && inlineForm != ProfileInlineForm::None)
                {
                    std::wstring& activeStr = (inlineFocusField == 1 && inlineForm == ProfileInlineForm::AddLinkCmd)
                        ? inlineField2 : inlineField1;
                    const bool ctrlHeld = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                    if (wp == VK_ESCAPE)
                    {
                        CloseInlineForm();
                        return 0;
                    }
                    if (wp == VK_RETURN)
                    {
                        SubmitInlineForm();
                        return 0;
                    }
                    if (wp == VK_TAB && inlineForm == ProfileInlineForm::AddLinkCmd)
                    {
                        inlineFocusField = 1 - inlineFocusField;
                        inlineCaretOn = true;
                        Invalidate();
                        return 0;
                    }
                    if (ctrlHeld && wp == 'V')
                    {
                        const std::wstring clip = ReadClipboardTextSingleLine(hwnd);
                        if (!clip.empty())
                        {
                            activeStr += clip;
                            if (activeStr.size() > 512)
                                activeStr.resize(512);
                            inlineCaretOn = true;
                            Invalidate();
                        }
                        return 0;
                    }
                    if (ctrlHeld && wp == VK_BACK)
                    {
                        activeStr.clear();
                        inlineCaretOn = true;
                        Invalidate();
                        return 0;
                    }
                    return 0;
                }
                if (wp == VK_ESCAPE)
                {
                    if (recordingIndex >= 0)
                    {
                        CancelCapture();
                        Invalidate();
                        return 0;
                    }
                }
                break;

            case WM_CHAR:
                if (activeTab == 2 && inlineForm != ProfileInlineForm::None)
                {
                    std::wstring& activeStr = (inlineFocusField == 1 && inlineForm == ProfileInlineForm::AddLinkCmd)
                        ? inlineField2 : inlineField1;
                    const wchar_t ch = static_cast<wchar_t>(wp);
                    if (ch == L'\b')
                    {
                        if (!activeStr.empty())
                        {
                            activeStr.pop_back();
                            inlineCaretOn = true;
                            Invalidate();
                        }
                        return 0;
                    }
                    if (ch >= 0x20 && ch != 0x7F && activeStr.size() < 512)
                    {
                        activeStr.push_back(ch);
                        inlineCaretOn = true;
                        Invalidate();
                    }
                    return 0;
                }
                break;

            case WM_CLOSE:
                CloseWindow();
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    };

    SettingsHostUi::Impl* SettingsHostUi::Impl::s_instance = nullptr;

    SettingsHostUi::SettingsHostUi() : m_impl(std::make_unique<Impl>()) {}
    SettingsHostUi::~SettingsHostUi() { ShutdownTray(); }

    bool SettingsHostUi::InitializeTray(HINSTANCE instance, HWND hostWindow, UINT taskbarCreatedMsg, SettingsHostBridge bridge)
    {
        Impl::s_instance = m_impl.get();
        m_impl->instance = instance;
        m_impl->hostHwnd = hostWindow;
        m_impl->taskbarCreatedMsg = taskbarCreatedMsg;
        m_impl->bridge = std::move(bridge);

        EnableDarkWin32Menus();
        ChangeWindowMessageFilterEx(hostWindow, kTrayCallbackMsg, MSGFLT_ALLOW, nullptr);
        m_impl->AddTrayIcon();
        return true;
    }

    void SettingsHostUi::ShutdownTray() noexcept
    {
        if (!m_impl)
            return;
        m_impl->CloseWindow();
        m_impl->RemoveTrayIcon();
        if (Impl::s_instance == m_impl.get())
            Impl::s_instance = nullptr;
    }

    void SettingsHostUi::OpenWindow(bool activate)
    {
        m_impl->OpenWindow(activate);
    }

    void SettingsHostUi::CloseWindow()
    {
        m_impl->CloseWindow();
    }

    void SettingsHostUi::NotifyHostStatusChanged()
    {
        m_impl->NotifyStatus();
    }

    bool SettingsHostUi::HandleHostWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& outResult)
    {
        if (msg == kTrayCallbackMsg)
        {
            if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK)
            {
                OpenWindow(true);
            }
            else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU)
            {
                m_impl->ShowTrayContextMenu();
            }
            outResult = 0;
            return true;
        }
        if (msg == m_impl->taskbarCreatedMsg && m_impl->trayAdded)
        {
            m_impl->AddTrayIcon();
            // Do not consume TaskbarCreated so Host can still monitor Explorer lifecycle!
            return false;
        }
        if (msg == WM_TIMER && hwnd == m_impl->hostHwnd)
        {
            if (wp == kTrayRetryTimerId)
            {
                m_impl->AddTrayIcon();
                outResult = 0;
                return true;
            }
            if (wp == kTrayPromoteTimerId)
            {
                EnsureTrayIconPromoted();
                if (++m_impl->trayPromoteTicks >= 4)
                    KillTimer(hwnd, kTrayPromoteTimerId);
                outResult = 0;
                return true;
            }
        }
        if (msg == WM_ENDSESSION && wp)
        {
            CloseWindow();
            return false;
        }
        return false;
    }
} // namespace winrings::host
