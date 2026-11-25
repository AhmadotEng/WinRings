#include "TrayApp.h"

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
#include "../shared/SearchMatch.h"
#include "../shared/SlotMatching.h"
#include "../shared/TriggerModel.h"
#include "../shared/WinRingsApi.h"
#include "../shared/WinRingsConfig.h"
#include "../shared/WinRingsDeltaIpc.h"
#include "../shared/WinRingsIdentity.h"

namespace winrings::tools
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr UINT kTrayCallbackMsg = WM_APP + 100;
        constexpr UINT kTrayIconId = 1;
        constexpr UINT_PTR kTrayRetryTimerId = 1;
        constexpr UINT_PTR kHostPollTimerId = 2;
        constexpr UINT_PTR kTrayPromoteTimerId = 3;
        constexpr UINT_PTR kCaretBlinkTimerId = 4;

        enum class ProfileInlineForm
        {
            None,
            AddLinkCmd,
            NewProfile,
            EditSlot
        };

        constexpr WPARAM kWakeOpenFlyout = 1;
        constexpr WPARAM kWakeQuitTray = 2;

        constexpr float kFlyoutW = 376.0f;
        constexpr float kFlyoutH = 544.0f;
        constexpr float kTriggerViewportTop = 118.0f;
        constexpr float kTriggerViewportBot = 330.0f;
        constexpr float kProfilePinsViewportTop = 302.0f;
        constexpr float kProfilePinsViewportBot = 478.0f;
        constexpr float kProfilePinRowH = 35.0f;

        constexpr ULONG_PTR kTrayMaskExtra = 0x5752544Du; // 'WRTM'
        constexpr WORD kMaskVirtualKey = 0xE8;

        enum TrayMenuCmd : UINT
        {
            CmdHeaderStatus = 1000,
            CmdTogglePauseOrStart = 1001,
            CmdOpenRing = 1002,
            CmdOpenFlyout = 1003,
            CmdQuitTrayOnly = 1004,
            CmdStopAll = 1005
        };

        enum class EngineMode
        {
            Stopped,
            Starting,
            Active,
            Paused,
            SafetyLocked,
            PreviewOnly
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
            for (int y = 0; y < kSize; ++y)
            {
                for (int x = 0; x < kSize; ++x)
                {
                    const float dx = static_cast<float>(x) - cx;
                    const float dy = static_cast<float>(y) - cy;
                    const float dist = std::sqrt(dx * dx + dy * dy);
                    const float ringDist = std::fabs(dist - 10.8f);
                    float alpha = 0.0f;
                    if (ringDist < 2.4f)
                        alpha = (std::clamp)(2.4f - ringDist, 0.0f, 1.0f);
                    if (dist < 2.8f)
                        alpha = (std::max)(alpha, (std::clamp)(2.8f - dist, 0.0f, 1.0f));

                    if (alpha > 0.01f)
                    {
                        const uint32_t a = static_cast<uint32_t>(alpha * 255.0f + 0.5f);
                        const float t = static_cast<float>(y) / static_cast<float>(kSize);
                        const uint32_t r = static_cast<uint32_t>((49.0f * (1.0f - t) + 31.0f * t) * alpha);
                        const uint32_t g = static_cast<uint32_t>((200.0f * (1.0f - t) + 134.0f * t) * alpha);
                        const uint32_t b = static_cast<uint32_t>(255.0f * alpha);
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

                if ((hasExe && (wcsstr(exePath, L"WinRingsTools.exe") != nullptr || wcsstr(exePath, L"WinRingsHost.exe") != nullptr)) ||
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

        std::wstring ResolveSiblingHostExe()
        {
            std::wstring path(MAX_PATH, L'\0');
            const DWORD len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (!len)
                return {};
            path.resize(len);
            const size_t slash = path.find_last_of(L"\\/");
            return (slash != std::wstring::npos) ? (path.substr(0, slash + 1) + WINRINGS_HOST_FILENAME_W) : L"";
        }

        bool LaunchSiblingHost(const wchar_t* args = L"")
        {
            const std::wstring host = ResolveSiblingHostExe();
            if (host.empty() || GetFileAttributesW(host.c_str()) == INVALID_FILE_ATTRIBUTES)
                return false;
            std::wstring cmd = L"\"" + host + L"\"";
            if (args && args[0] != L'\0')
            {
                cmd.push_back(L' ');
                cmd += args;
            }
            std::vector<wchar_t> buf(cmd.begin(), cmd.end());
            buf.push_back(L'\0');
            STARTUPINFOW si{ sizeof(si) };
            PROCESS_INFORMATION pi{};
            if (!CreateProcessW(host.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
                return false;
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return true;
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

        RECT ComputeTaskbarSafeWorkRect(HMONITOR mon, float dpiScale) noexcept
        {
            MONITORINFO mi{ sizeof(mi) };
            if (!GetMonitorInfoW(mon, &mi))
                return RECT{ 0, 0, 1920, 1020 };

            RECT safe = mi.rcWork;
            const int bottomClearPx = static_cast<int>(std::lround(62.0f * dpiScale));
            const int padPx = static_cast<int>(std::lround(10.0f * dpiScale));

            // Keep left/right-click Tray menus cleanly above auto-hidden or translucent taskbars
            bool topDocked = false;
            if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr))
            {
                RECT tRc{};
                if (GetWindowRect(tray, &tRc) &&
                    tRc.left < mi.rcMonitor.right && tRc.right > mi.rcMonitor.left)
                {
                    if (tRc.top <= mi.rcMonitor.top + 8 && tRc.bottom < (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2)
                    {
                        topDocked = true;
                        if (IsWindowVisible(tray))
                            safe.top = (std::max)(safe.top, static_cast<LONG>(tRc.bottom + padPx));
                    }
                    else if (IsWindowVisible(tray) && tRc.top > (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2)
                    {
                        safe.bottom = (std::min)(safe.bottom, static_cast<LONG>(tRc.top - padPx));
                    }
                }
            }
            if (!topDocked)
                safe.bottom = (std::min)(safe.bottom, static_cast<LONG>(mi.rcMonitor.bottom - bottomClearPx));
            if (safe.bottom <= safe.top + 120)
                safe = mi.rcWork;
            return safe;
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
            if (folderOnly)
                opts |= FOS_PICKFOLDERS;
            else
                opts |= FOS_FILEMUSTEXIST;
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

        class TrayControlApp
        {
        public:
            int Run(HINSTANCE instance, bool openFlyoutOnStart)
            {
                s_instance = this;
                m_instance = instance;
                m_wakeMessage = RegisterWindowMessageW(WINRINGS_TRAY_WAKE_MESSAGE_W);
                m_taskbarCreatedMessage = RegisterWindowMessageW(WINRINGS_TASKBAR_CREATED_MESSAGE_W);
                m_hostControlMessage = RegisterWindowMessageW(WINRINGS_HOST_CONTROL_MESSAGE_W);
                m_shellCommandMessage = RegisterWindowMessageW(WINRINGS_SHELL_COMMAND_MESSAGE_W);

                EnableDarkWin32Menus();

                WNDCLASSEXW trayWc{ sizeof(trayWc) };
                trayWc.lpfnWndProc = TrayWndProc;
                trayWc.hInstance = m_instance;
                trayWc.lpszClassName = WINRINGS_TRAY_WINDOW_CLASS_W;
                if (!RegisterClassExW(&trayWc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                    return 1;

                WNDCLASSEXW flyWc{ sizeof(flyWc) };
                flyWc.lpfnWndProc = FlyoutWndProc;
                flyWc.hInstance = m_instance;
                flyWc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                flyWc.lpszClassName = L"WinRings.TrayFlyout.v1";
                if (!RegisterClassExW(&flyWc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                    return 1;

                m_trayHwnd = CreateWindowExW(
                    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                    WINRINGS_TRAY_WINDOW_CLASS_W,
                    L"WinRingsTray",
                    WS_POPUP,
                    0, 0, 0, 0,
                    nullptr, nullptr, m_instance, this);
                if (!m_trayHwnd)
                    return 1;

                ChangeWindowMessageFilterEx(m_trayHwnd, m_wakeMessage, MSGFLT_ALLOW, nullptr);
                ChangeWindowMessageFilterEx(m_trayHwnd, m_taskbarCreatedMessage, MSGFLT_ALLOW, nullptr);
                ChangeWindowMessageFilterEx(m_trayHwnd, kTrayCallbackMsg, MSGFLT_ALLOW, nullptr);

                AddTrayIcon();
                if (openFlyoutOnStart)
                    OpenFlyout();

                MSG msg{};
                while (GetMessageW(&msg, nullptr, 0, 0) > 0)
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }

                CloseFlyout();
                RemoveTrayIcon();
                if (m_trayHwnd && IsWindow(m_trayHwnd))
                {
                    DestroyWindow(m_trayHwnd);
                    m_trayHwnd = nullptr;
                }
                s_instance = nullptr;
                return 0;
            }

        private:
            static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
            {
                auto* self = reinterpret_cast<TrayControlApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (msg == WM_NCCREATE)
                {
                    self = static_cast<TrayControlApp*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                    self->m_trayHwnd = hwnd;
                }
                if (self)
                    return self->HandleTrayMessage(hwnd, msg, wp, lp);
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            static LRESULT CALLBACK FlyoutWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
            {
                auto* self = reinterpret_cast<TrayControlApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (msg == WM_NCCREATE)
                {
                    self = static_cast<TrayControlApp*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                    self->m_flyoutHwnd = hwnd;
                }
                if (self)
                    return self->HandleFlyoutMessage(hwnd, msg, wp, lp);
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            static LRESULT CALLBACK CaptureKeyboardProc(int code, WPARAM wp, LPARAM lp)
            {
                if (code >= 0 && s_instance && s_instance->m_recordingIndex >= 0)
                    return s_instance->HandleCaptureKeyboard(code, wp, lp);
                return CallNextHookEx(nullptr, code, wp, lp);
            }

            static LRESULT CALLBACK CaptureMouseProc(int code, WPARAM wp, LPARAM lp)
            {
                if (code >= 0 && s_instance && s_instance->m_recordingIndex >= 0)
                    return s_instance->HandleCaptureMouse(code, wp, lp);
                return CallNextHookEx(nullptr, code, wp, lp);
            }

            void AddTrayIcon()
            {
                if (!m_trayIcon)
                    m_trayIcon = CreateWinRingsTrayIcon();

                NOTIFYICONDATAW nid{ sizeof(nid) };
                nid.hWnd = m_trayHwnd;
                nid.uID = kTrayIconId;
                nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
                nid.uCallbackMessage = kTrayCallbackMsg;
                nid.hIcon = m_trayIcon;
                wcscpy_s(nid.szTip, L"WinRings \u2014 Radial Switcher Control");
                if (Shell_NotifyIconW(NIM_ADD, &nid) || Shell_NotifyIconW(NIM_MODIFY, &nid))
                {
                    m_trayAdded = true;
                    KillTimer(m_trayHwnd, kTrayRetryTimerId);
                    EnsureTrayIconPromoted();
                    m_trayPromoteTicks = 0;
                    SetTimer(m_trayHwnd, kTrayPromoteTimerId, 700, nullptr);
                }
                else if (m_trayRetryCount < 30)
                {
                    ++m_trayRetryCount;
                    SetTimer(m_trayHwnd, kTrayRetryTimerId, 1000, nullptr);
                }
            }

            void RemoveTrayIcon()
            {
                if (m_trayHwnd)
                {
                    KillTimer(m_trayHwnd, kTrayRetryTimerId);
                    KillTimer(m_trayHwnd, kTrayPromoteTimerId);
                }
                if (m_trayAdded && m_trayHwnd)
                {
                    NOTIFYICONDATAW nid{ sizeof(nid) };
                    nid.hWnd = m_trayHwnd;
                    nid.uID = kTrayIconId;
                    Shell_NotifyIconW(NIM_DELETE, &nid);
                    m_trayAdded = false;
                }
                if (m_trayIcon)
                {
                    DestroyIcon(m_trayIcon);
                    m_trayIcon = nullptr;
                }
            }

            HWND FindHostWindow() const noexcept
            {
                return FindWindowW(WINRINGS_HOST_WINDOW_CLASS_W, WINRINGS_HOST_WINDOW_TITLE_W);
            }

            HWND FindPreviewControlWindow() const noexcept
            {
                return FindWindowExW(HWND_MESSAGE, nullptr, L"WinRings.Preview.Control.v1", nullptr);
            }

            EngineMode QueryEngineState() const noexcept
            {
                if (HWND host = FindHostWindow())
                {
                    DWORD_PTR result = 0;
                    if (SendMessageTimeoutW(host, m_hostControlMessage, WINRINGS_HOST_CTRL_QUERY_DETAILS, 0,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 350, &result))
                    {
                        const UINT32 packed = static_cast<UINT32>(result);
                        const auto hostStatus = static_cast<WinRingsHostStatus>(packed & 0xFFFFu);
                        const UINT32 shellBits = (packed >> 16) & 0xFFFFu;
                        if (hostStatus == WINRINGS_HOST_STATUS_SAFETY_DISABLED)
                            return EngineMode::SafetyLocked;
                        if (hostStatus == WINRINGS_HOST_STATUS_READY)
                            return (shellBits & WINRINGS_STATE_RING_PAUSED) ? EngineMode::Paused : EngineMode::Active;
                        if (hostStatus == WINRINGS_HOST_STATUS_STARTING || hostStatus == WINRINGS_HOST_STATUS_INJECTING)
                            return EngineMode::Starting;
                    }
                    return EngineMode::Starting;
                }
                if (FindPreviewControlWindow())
                    return EngineMode::PreviewOnly;
                return EngineMode::Stopped;
            }

            bool SendHostControl(WinRingsHostControl ctrl, LPARAM lp = 0) const noexcept
            {
                HWND host = FindHostWindow();
                if (!host)
                    return false;
                DWORD_PTR res = 0;
                return SendMessageTimeoutW(host, m_hostControlMessage, static_cast<WPARAM>(ctrl), lp,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &res) != FALSE && res != 0;
            }

            bool SendDeltaPacket(const WinRingsDeltaPacket& pkt) const noexcept
            {
                HWND target = FindHostWindow();
                if (!target)
                    target = FindPreviewControlWindow();
                if (!target)
                    return false;

                COPYDATASTRUCT cds{};
                cds.dwData = WINRINGS_COPYDATA_DELTA_V1;
                cds.cbData = sizeof(WinRingsDeltaPacket);
                cds.lpData = const_cast<WinRingsDeltaPacket*>(&pkt);
                DWORD_PTR res = 0;
                return SendMessageTimeoutW(target, WM_COPYDATA, reinterpret_cast<WPARAM>(m_trayHwnd),
                    reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &res) != FALSE && res != 0;
            }

            POINT ComputeSideBySideRingAnchor() const noexcept
            {
                if (!m_flyoutHwnd || !IsWindow(m_flyoutHwnd))
                    return POINT{ 0, 0 };

                RECT fRc{};
                GetWindowRect(m_flyoutHwnd, &fRc);
                HMONITOR mon = MonitorFromWindow(m_flyoutHwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{ sizeof(mi) };
                if (!GetMonitorInfoW(mon, &mi))
                    return POINT{ 0, 0 };

                const RECT& w = mi.rcWork;
                const int cxWork = (w.left + w.right) / 2;
                const int cyWork = (w.top + w.bottom) / 2;
                const int sepPx = static_cast<int>(std::lround(190.0 * m_dpiScale));

                if (cxWork < fRc.left - sepPx || cxWork > fRc.right + sepPx)
                    return POINT{ cxWork, cyWork };

                const int leftAvail = fRc.left - w.left;
                const int rightAvail = w.right - fRc.right;
                const int targetX = (leftAvail >= rightAvail)
                    ? (w.left + leftAvail / 2)
                    : (fRc.right + rightAvail / 2);
                return POINT{ targetX, cyWork };
            }

            void SyncLiveRingPreview(bool wantOpen)
            {
                if (wantOpen)
                {
                    if (m_engineMode == EngineMode::Active || m_engineMode == EngineMode::PreviewOnly)
                    {
                        const POINT anchor = ComputeSideBySideRingAnchor();
                        SendDeltaPacket(MakeLookDelta(m_config));
                        if (SendDeltaPacket(MakePreviewDelta(true, anchor.x, anchor.y, m_selectedProfileId)))
                            m_livePreviewSent = true;
                    }
                }
                else
                {
                    if (m_livePreviewSent)
                    {
                        SendDeltaPacket(MakePreviewDelta(false, 0, 0, m_selectedProfileId));
                        m_livePreviewSent = false;
                    }
                }
            }

            void CommitTriggersDelta()
            {
                m_config.SyncLegacyTriggerMirrors();
                m_dirty = true;
                SendDeltaPacket(MakeTriggersDelta(m_config));
                InvalidateFlyout();
            }

            void CommitGesturesDelta()
            {
                m_dirty = true;
                SendDeltaPacket(MakeGesturesDelta(m_config));
                InvalidateFlyout();
            }

            void CommitLookDelta()
            {
                m_dirty = true;
                SendDeltaPacket(MakeLookDelta(m_config));
                if (!m_livePreviewSent && (m_activeTab == 1 || m_activeTab == 2))
                    SyncLiveRingPreview(true);
                InvalidateFlyout();
            }

            void CommitProfileEdit()
            {
                m_config.PullFromDefaultProfileMirror();
                m_config.Save();
                m_dirty = false;
                ClampProfilePinsScroll();
                SendDeltaPacket(MakeReloadFromDiskDelta(m_selectedProfileId));
                if (m_activeTab == 1 || m_activeTab == 2)
                    SyncLiveRingPreview(true);
                InvalidateFlyout();
            }

            void ShowTrayMenu()
            {
                POINT pt{};
                GetCursorPos(&pt);
                const EngineMode mode = QueryEngineState();
                m_engineMode = mode;

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
                    AppendMenuW(menu, MF_STRING, CmdTogglePauseOrStart, L"Reset Safety & Start Ring");
                else if (mode == EngineMode::Stopped)
                    AppendMenuW(menu, MF_STRING, CmdTogglePauseOrStart, L"Start Ring Engine");

                AppendMenuW(menu, MF_STRING | (mode == EngineMode::Active ? 0u : MF_GRAYED), CmdOpenRing, L"Open Ring Now");
                AppendMenuW(menu, MF_STRING, CmdOpenFlyout, L"Open Control Flyout");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, CmdQuitTrayOnly, L"Quit Tray (Keep Ring Running)");
                if (mode != EngineMode::Stopped && mode != EngineMode::PreviewOnly)
                    AppendMenuW(menu, MF_STRING, CmdStopAll, L"Stop Everything (Ring + Tray)");

                SetMenuDefaultItem(menu, CmdOpenFlyout, FALSE);

                UINT menuFlags = TPM_RETURNCMD | TPM_RIGHTBUTTON;
                HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
                const float monDpi = QueryMonitorDpiScale(mon);
                const RECT safe = ComputeTaskbarSafeWorkRect(mon, monDpi);
                if (pt.y > (safe.top + safe.bottom) / 2)
                {
                    pt.y = (std::min)(pt.y, static_cast<LONG>(safe.bottom));
                    menuFlags |= TPM_BOTTOMALIGN;
                }
                else
                {
                    pt.y = (std::max)(pt.y, static_cast<LONG>(safe.top));
                    menuFlags |= TPM_TOPALIGN;
                }

                SetForegroundWindow(m_trayHwnd);
                const UINT cmd = TrackPopupMenuEx(menu, menuFlags, pt.x, pt.y, m_trayHwnd, nullptr);
                DestroyMenu(menu);
                PostMessageW(m_trayHwnd, WM_NULL, 0, 0);

                switch (cmd)
                {
                case CmdTogglePauseOrStart:
                    if (mode == EngineMode::Active)
                        SendHostControl(WINRINGS_HOST_CTRL_SET_PAUSED, 1);
                    else if (mode == EngineMode::Paused)
                        SendHostControl(WINRINGS_HOST_CTRL_SET_PAUSED, 0);
                    else if (mode == EngineMode::SafetyLocked)
                        SendHostControl(WINRINGS_HOST_CTRL_RESET_SAFETY, 0);
                    else if (mode == EngineMode::Stopped)
                        LaunchSiblingHost();
                    if (m_flyoutHwnd)
                    {
                        SetTimer(m_flyoutHwnd, kHostPollTimerId, 250, nullptr);
                        m_engineMode = QueryEngineState();
                        InvalidateFlyout();
                    }
                    break;
                case CmdOpenRing:
                    SendHostControl(WINRINGS_HOST_CTRL_TOGGLE, 0);
                    break;
                case CmdOpenFlyout:
                    OpenFlyout();
                    break;
                case CmdQuitTrayOnly:
                    CloseFlyout();
                    PostQuitMessage(0);
                    break;
                case CmdStopAll:
                    SendHostControl(WINRINGS_HOST_CTRL_EXIT, 0);
                    CloseFlyout();
                    PostQuitMessage(0);
                    break;
                default:
                    break;
                }
            }

            void OpenFlyout()
            {
                if (m_flyoutHwnd && IsWindow(m_flyoutHwnd))
                {
                    SetForegroundWindow(m_flyoutHwnd);
                    return;
                }

                m_config = SwitcherConfig::LoadOrCreate();
                if (!m_config.FindProfile(m_selectedProfileId))
                    m_selectedProfileId = L"default";
                m_dirty = false;
                m_livePreviewSent = false;
                m_recordingIndex = -1;
                m_dragSlider = -1;
                m_hoverButton = -1;
                m_triggerScrollDip = 0.0f;
                m_profilePinsScrollDip = 0.0f;
                m_inlineForm = ProfileInlineForm::None;
                m_inlineFocusField = 0;
                m_inlineField1.clear();
                m_inlineField2.clear();
                m_inlineAdmin = false;
                m_inlineCaretOn = true;
                m_engineMode = QueryEngineState();
                if (m_engineMode == EngineMode::Stopped && LaunchSiblingHost())
                    m_engineMode = EngineMode::Starting;

                POINT anchor{};
                GetCursorPos(&anchor);
                HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{ sizeof(mi) };
                GetMonitorInfoW(mon, &mi);

                m_dpiScale = QueryMonitorDpiScale(mon);
                const RECT safe = ComputeTaskbarSafeWorkRect(mon, m_dpiScale);

                const int wPx = static_cast<int>(std::lround(kFlyoutW * m_dpiScale));
                const int hPx = static_cast<int>(std::lround(kFlyoutH * m_dpiScale));
                const int padPx = static_cast<int>(std::lround(10.0f * m_dpiScale));

                int x = safe.right - wPx - padPx;
                int y = safe.bottom - hPx;
                if (anchor.x >= mi.rcMonitor.left && anchor.x <= mi.rcMonitor.right &&
                    anchor.y >= mi.rcMonitor.top - 140 && anchor.y <= mi.rcMonitor.bottom + 140)
                {
                    x = std::clamp(static_cast<int>(anchor.x - wPx / 2),
                        static_cast<int>(safe.left + padPx),
                        (std::max)(static_cast<int>(safe.left + padPx), static_cast<int>(safe.right - wPx - padPx)));
                    if (anchor.y < (safe.top + safe.bottom) / 2)
                        y = safe.top + padPx;
                    else
                        y = safe.bottom - hPx;
                }

                m_flyoutHwnd = CreateWindowExW(
                    WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                    L"WinRings.TrayFlyout.v1",
                    L"WinRings Control",
                    WS_POPUP,
                    x, y, wPx, hPx,
                    nullptr, nullptr, m_instance, this);
                if (!m_flyoutHwnd)
                    return;

                const BOOL dark = TRUE;
                DwmSetWindowAttribute(m_flyoutHwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
                const int corner = 2; // DWMWCP_ROUND
                DwmSetWindowAttribute(m_flyoutHwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
                const COLORREF border = RGB(62, 62, 68);
                DwmSetWindowAttribute(m_flyoutHwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));

                if (!InitFlyoutGraphics(wPx, hPx))
                {
                    CloseFlyout();
                    return;
                }

                if (m_engineMode == EngineMode::Starting)
                    SetTimer(m_flyoutHwnd, kHostPollTimerId, 250, nullptr);

                ShowWindow(m_flyoutHwnd, SW_SHOWNORMAL);
                SetForegroundWindow(m_flyoutHwnd);
                RenderFlyout();

                if (m_activeTab == 1 || m_activeTab == 2)
                    SyncLiveRingPreview(true);
            }

            void CloseFlyout()
            {
                CancelCapture();
                m_inlineForm = ProfileInlineForm::None;
                if (m_flyoutHwnd)
                {
                    KillTimer(m_flyoutHwnd, kHostPollTimerId);
                    KillTimer(m_flyoutHwnd, kCaretBlinkTimerId);
                }

                SyncLiveRingPreview(false);

                if (m_dirty)
                {
                    if (!SendDeltaPacket(MakeSimpleDelta(DeltaOp::FlushSaveToDisk)))
                        m_config.Save();
                    m_dirty = false;
                }

                ReleaseFlyoutGraphics();
                if (m_flyoutHwnd && IsWindow(m_flyoutHwnd))
                {
                    DestroyWindow(m_flyoutHwnd);
                    m_flyoutHwnd = nullptr;
                }
            }

            bool InitFlyoutGraphics(int wPx, int hPx)
            {
                ReleaseFlyoutGraphics();

                const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
                D3D_DRIVER_TYPE drivers[] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };
                HRESULT hr = E_FAIL;
                for (auto drv : drivers)
                {
                    hr = D3D11CreateDevice(nullptr, drv, nullptr, flags, nullptr, 0,
                        D3D11_SDK_VERSION, m_d3dDevice.ReleaseAndGetAddressOf(), nullptr, nullptr);
                    if (SUCCEEDED(hr)) break;
                }
                if (FAILED(hr) || !m_d3dDevice || FAILED(m_d3dDevice.As(&m_dxgiDevice)) || !m_dxgiDevice)
                    return false;

                D2D1_FACTORY_OPTIONS opts{};
                if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                    &opts, reinterpret_cast<void**>(m_d2dFactory.ReleaseAndGetAddressOf()))))
                    return false;
                if (FAILED(m_d2dFactory->CreateDevice(m_dxgiDevice.Get(), m_d2dDevice.ReleaseAndGetAddressOf())))
                    return false;
                if (FAILED(m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                    m_d2dCtx.ReleaseAndGetAddressOf())))
                    return false;

                if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                    reinterpret_cast<IUnknown**>(m_dwrite.ReleaseAndGetAddressOf()))))
                    return false;

                auto mkFmt = [&](const wchar_t* face, float sz, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT align,
                    ComPtr<IDWriteTextFormat>& out) {
                    m_dwrite->CreateTextFormat(face, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                        sz, L"en-us", out.ReleaseAndGetAddressOf());
                    if (out)
                    {
                        out->SetTextAlignment(align);
                        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                        DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                        ComPtr<IDWriteInlineObject> ell;
                        m_dwrite->CreateEllipsisTrimmingSign(out.Get(), ell.GetAddressOf());
                        out->SetTrimming(&trim, ell.Get());
                    }
                };
                mkFmt(L"Segoe UI Variable Display", 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, m_fmtTitle);
                mkFmt(L"Segoe UI Variable Text", 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, m_fmtBold);
                mkFmt(L"Segoe UI Variable Text", 13.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, m_fmtBody);
                mkFmt(L"Segoe UI Variable Text", 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, m_fmtCenter);
                mkFmt(L"Segoe UI Variable Text", 10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, m_fmtCaption);
                mkFmt(L"Segoe UI Variable Text", 10.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, m_fmtSmallCenter);

                m_dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"en-us", m_fmtGuideBody.ReleaseAndGetAddressOf());
                if (m_fmtGuideBody)
                {
                    m_fmtGuideBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    m_fmtGuideBody->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                    m_fmtGuideBody->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                }

                return ResizeFlyoutSwapChain(wPx, hPx);
            }

            bool ResizeFlyoutSwapChain(int wPx, int hPx)
            {
                if (!m_dxgiDevice || !m_d2dCtx || !m_flyoutHwnd)
                    return false;
                wPx = (std::max)(1, wPx);
                hPx = (std::max)(1, hPx);
                if (m_swapChain && m_swapW == wPx && m_swapH == hPx && m_targetBmp)
                    return true;

                m_d2dCtx->SetTarget(nullptr);
                m_targetBmp.Reset();

                if (!m_swapChain)
                {
                    ComPtr<IDXGIAdapter> adapter;
                    ComPtr<IDXGIFactory2> factory;
                    if (FAILED(m_dxgiDevice->GetAdapter(adapter.GetAddressOf())) ||
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

                    if (FAILED(factory->CreateSwapChainForHwnd(m_d3dDevice.Get(), m_flyoutHwnd, &desc, nullptr, nullptr,
                        m_swapChain.ReleaseAndGetAddressOf())))
                        return false;
                }
                else
                {
                    if (FAILED(m_swapChain->ResizeBuffers(2, static_cast<UINT>(wPx), static_cast<UINT>(hPx),
                        DXGI_FORMAT_B8G8R8A8_UNORM, 0)))
                        return false;
                }

                ComPtr<IDXGISurface> surf;
                if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(surf.GetAddressOf()))))
                    return false;
                D2D1_BITMAP_PROPERTIES1 bp{};
                bp.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
                bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
                bp.dpiX = 96.0f;
                bp.dpiY = 96.0f;
                if (FAILED(m_d2dCtx->CreateBitmapFromDxgiSurface(surf.Get(), &bp, m_targetBmp.ReleaseAndGetAddressOf())))
                    return false;

                m_swapW = wPx;
                m_swapH = hPx;
                return true;
            }

            void ReleaseFlyoutGraphics()
            {
                if (m_d2dCtx) m_d2dCtx->SetTarget(nullptr);
                m_targetBmp.Reset();
                m_swapChain.Reset();
                m_fmtGuideBody.Reset();
                m_fmtSmallCenter.Reset();
                m_fmtCaption.Reset();
                m_fmtCenter.Reset();
                m_fmtBody.Reset();
                m_fmtBold.Reset();
                m_fmtTitle.Reset();
                m_dwrite.Reset();
                m_d2dCtx.Reset();
                m_d2dDevice.Reset();
                m_d2dFactory.Reset();
                m_dxgiDevice.Reset();
                m_d3dDevice.Reset();
                m_swapW = 0;
                m_swapH = 0;
            }

            void InvalidateFlyout() noexcept
            {
                if (m_flyoutHwnd && IsWindow(m_flyoutHwnd))
                    InvalidateRect(m_flyoutHwnd, nullptr, FALSE);
            }

            void StartCapture(int index)
            {
                StopCapture();
                if (index < 0 || index >= static_cast<int>(m_config.triggers.size()))
                    return;

                m_recordingIndex = index;
                m_capModDown = 0;
                m_capSoloDisqualified = false;
                m_capRightDown = false;
                m_swallowNextKeyUpVk = 0;

                HMODULE selfMod = nullptr;
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&CaptureKeyboardProc), &selfMod);
                m_capKeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, CaptureKeyboardProc, selfMod, 0);
                m_capMouseHook = SetWindowsHookExW(WH_MOUSE_LL, CaptureMouseProc, selfMod, 0);
                InvalidateFlyout();
            }

            void StopCapture() noexcept
            {
                m_recordingIndex = -1;
                m_capModDown = 0;
                m_capSoloDisqualified = false;
                m_capRightDown = false;
                m_swallowNextKeyUpVk = 0;
                if (m_capKeyHook) { UnhookWindowsHookEx(m_capKeyHook); m_capKeyHook = nullptr; }
                if (m_capMouseHook) { UnhookWindowsHookEx(m_capMouseHook); m_capMouseHook = nullptr; }
            }

            void PruneDisabledPlaceholders()
            {
                const auto it = std::remove_if(m_config.triggers.begin(), m_config.triggers.end(),
                    [](const TriggerBinding& t) { return !t.IsActive(); });
                m_config.triggers.erase(it, m_config.triggers.end());
            }

            float ComputeTriggersContentHeight() const noexcept
            {
                const size_t tCount = (std::min)(m_config.triggers.size(), kMaxTriggerBindings);
                if (tCount == 0)
                    return 0.0f;
                float y = 0.0f;
                for (size_t i = 0; i < tCount; ++i)
                {
                    const TriggerConflictInfo ci = AnalyzeTriggerConflict(m_config.triggers[i]);
                    const bool isLast = (i + 1 == tCount);
                    if (isLast)
                        y += ci.summary.empty() ? 34.0f : 51.0f;
                    else
                        y += ci.summary.empty() ? 42.0f : 58.0f;
                }
                return y;
            }

            void ClampTriggerScroll() noexcept
            {
                const float viewH = kTriggerViewportBot - kTriggerViewportTop;
                const float maxScroll = (std::max)(0.0f, ComputeTriggersContentHeight() - viewH);
                m_triggerScrollDip = std::clamp(m_triggerScrollDip, 0.0f, maxScroll);
            }

            float ComputeProfilePinsContentHeight() const noexcept
            {
                const RingProfileConfig& prof = m_config.ResolveProfile(m_selectedProfileId);
                return static_cast<float>(prof.pinnedSlots.size()) * kProfilePinRowH;
            }

            void ClampProfilePinsScroll() noexcept
            {
                const float viewH = kProfilePinsViewportBot - kProfilePinsViewportTop;
                const float maxScroll = (std::max)(0.0f, ComputeProfilePinsContentHeight() - viewH);
                m_profilePinsScrollDip = std::clamp(m_profilePinsScrollDip, 0.0f, maxScroll);
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

            void FinishCapturedBinding(const TriggerBinding& captured)
            {
                const int idx = m_recordingIndex;
                StopCapture();
                if ((captured.modifiers & (MOD_WIN | MOD_ALT)) != 0)
                    InjectMaskTap();
                if (idx >= 0 && idx < static_cast<int>(m_config.triggers.size()))
                {
                    TriggerBinding next = captured;
                    if (!m_config.triggers[static_cast<size_t>(idx)].profileId.empty())
                        next.profileId = m_config.triggers[static_cast<size_t>(idx)].profileId;
                    m_config.triggers[static_cast<size_t>(idx)] = std::move(next);
                    PruneDisabledPlaceholders();
                    ClampTriggerScroll();
                    CommitTriggersDelta();
                }
                else
                {
                    PruneDisabledPlaceholders();
                    ClampTriggerScroll();
                    InvalidateFlyout();
                }
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

            LRESULT HandleCaptureKeyboard(int code, WPARAM wp, LPARAM lp)
            {
                const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
                if (!kb || kb->dwExtraInfo == kTrayMaskExtra || kb->vkCode == kMaskVirtualKey)
                    return CallNextHookEx(m_capKeyHook, code, wp, lp);

                const bool isDown = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
                const bool isUp = (wp == WM_KEYUP || wp == WM_SYSKEYUP);
                const UINT normVk = NormalizeTriggerVk(kb->vkCode);

                if (isUp && m_swallowNextKeyUpVk != 0 && normVk == m_swallowNextKeyUpVk)
                {
                    m_swallowNextKeyUpVk = 0;
                    return 1;
                }

                if (IsModifierVk(normVk))
                {
                    const UINT flag = ModifierFlagFromVk(normVk);
                    if (isDown)
                    {
                        if ((m_capModDown & ~flag) != 0)
                            m_capSoloDisqualified = true;
                        m_capModDown |= flag;
                        return CallNextHookEx(m_capKeyHook, code, wp, lp);
                    }
                    if (isUp)
                    {
                        const bool cleanSolo = ((m_capModDown & flag) != 0) &&
                            ((m_capModDown & ~flag) == 0) && !m_capSoloDisqualified;
                        m_capModDown &= ~flag;
                        if (cleanSolo)
                        {
                            if (normVk == VK_LWIN || normVk == VK_MENU)
                                InjectMaskTap();
                            TriggerBinding b;
                            b.kind = TriggerKind::SoloModifier;
                            b.vk = normVk;
                            FinishCapturedBinding(b);
                        }
                        return CallNextHookEx(m_capKeyHook, code, wp, lp);
                    }
                }

                if (isDown)
                {
                    m_capSoloDisqualified = true;
                    const UINT mods = CurrentHeldModifiers();
                    if (normVk == VK_ESCAPE && mods == 0)
                    {
                        CancelCapture();
                        InvalidateFlyout();
                        return 1;
                    }
                    TriggerBinding b;
                    b.kind = TriggerKind::KeyChord;
                    b.modifiers = mods;
                    b.vk = normVk;
                    m_swallowNextKeyUpVk = normVk;
                    FinishCapturedBinding(b);
                    return 1;
                }

                return CallNextHookEx(m_capKeyHook, code, wp, lp);
            }

            LRESULT HandleCaptureMouse(int code, WPARAM wp, LPARAM lp)
            {
                const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
                if (!ms)
                    return CallNextHookEx(m_capMouseHook, code, wp, lp);

                const UINT msg = static_cast<UINT>(wp);
                if (msg == WM_MOUSEMOVE)
                    return CallNextHookEx(m_capMouseHook, code, wp, lp);

                if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN)
                    m_capSoloDisqualified = true;

                bool insideFlyout = false;
                if (m_flyoutHwnd && IsWindow(m_flyoutHwnd))
                {
                    RECT rc{};
                    GetWindowRect(m_flyoutHwnd, &rc);
                    insideFlyout = PtInRect(&rc, ms->pt) != FALSE;
                }

                if (msg == WM_RBUTTONDOWN)
                {
                    m_capRightDown = true;
                    return CallNextHookEx(m_capMouseHook, code, wp, lp);
                }
                if (msg == WM_RBUTTONUP)
                {
                    m_capRightDown = false;
                    return CallNextHookEx(m_capMouseHook, code, wp, lp);
                }
                if (msg == WM_LBUTTONDOWN)
                {
                    if (m_capRightDown)
                    {
                        m_capRightDown = false;
                        TriggerBinding b;
                        b.kind = TriggerKind::RightLeftChord;
                        b.mouseButton = MouseTriggerButton::RightLeftChord;
                        b.modifiers = CurrentHeldModifiers();
                        FinishCapturedBinding(b);
                        return 1;
                    }
                    if (!insideFlyout)
                    {
                        CancelCapture();
                        InvalidateFlyout();
                    }
                    return CallNextHookEx(m_capMouseHook, code, wp, lp);
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

                return CallNextHookEx(m_capMouseHook, code, wp, lp);
            }

            // Layout metrics for the 376 x 544 DIP Flyout
            struct FlyoutLayout
            {
                HitRect statusPill{ 104, 13, 250, 37 };
                HitRect pauseSwitch{ 264, 15, 312, 35 };
                HitRect closeBtn{ 334, 12, 360, 38 };
                HitRect tabTriggers{ 18, 56, 131, 86 };
                HitRect tabProfiles{ 131, 56, 245, 86 };
                HitRect tabLook{ 245, 56, 358, 86 };

                // Tab 0: Triggers
                HitRect addTriggerBtn{ 298, 95, 360, 115 };
                HitRect triggerPill[kMaxTriggerBindings]{};
                HitRect triggerProfileBadge[kMaxTriggerBindings]{};
                HitRect triggerRemove[kMaxTriggerBindings]{};
                float triggerContentH = 0.0f;
                HitRect behaviorRow[4]{};

                // Tab 1: Profiles
                std::vector<HitRect> profileChip;
                std::vector<size_t> profileChipIndex;
                HitRect profileNewBtn{};
                HitRect profileDelOrResetBtn{};
                HitRect openOnRingBtn{ 16, 132, 246, 166 };
                HitRect openConfigBtn{ 254, 132, 360, 166 };
                HitRect profileRunningRow{ 16, 174, 360, 208 };
                HitRect addAppBtn{ 16, 238, 126, 268 };
                HitRect addFolderBtn{ 133, 238, 243, 268 };
                HitRect addLinkBtn{ 250, 238, 360, 268 };
                HitRect inlineField1Rect{ 28, 322, 348, 350 };
                HitRect inlineField2Rect{ 28, 372, 348, 400 };
                HitRect inlineAdminRow{ 28, 406, 348, 434 };
                HitRect editNameRect{ 28, 314, 348, 340 };
                HitRect editIconStatusRect{ 28, 358, 214, 384 };
                HitRect editIconBrowseBtn{ 220, 358, 288, 384 };
                HitRect editIconResetBtn{ 294, 358, 348, 384 };
                HitRect editHideRow{ 28, 389, 348, 413 };
                HitRect editAdminRow{ 28, 416, 348, 440 };
                HitRect inlineCancelBtn{ 176, 446, 256, 474 };
                HitRect inlineSubmitBtn{ 264, 446, 348, 474 };
                std::vector<HitRect> profilePinRowRect;
                std::vector<HitRect> profilePinEditBtn;
                std::vector<HitRect> profilePinRemoveBtn;
                float profilePinsContentH = 0.0f;
                HitRect openProfilesFolderBtn{ 16, 492, 360, 526 };

                // Tab 2: Look
                HitRect texCard[3]{};
                HitRect modeChip[3]{};
                HitRect surfaceDot[8]{};
                HitRect accentAutoChip{ 16, 298, 82, 324 };
                HitRect accentDot[7]{};
                HitRect sliderTrack[3]{};
            };

            FlyoutLayout ComputeLayout() const
            {
                FlyoutLayout lo;
                lo.triggerContentH = ComputeTriggersContentHeight();
                const float viewH = kTriggerViewportBot - kTriggerViewportTop;
                const float maxScroll = (std::max)(0.0f, lo.triggerContentH - viewH);
                const float scroll = std::clamp(m_triggerScrollDip, 0.0f, maxScroll);

                float y = kTriggerViewportTop - scroll;
                const size_t tCount = (std::min)(m_config.triggers.size(), kMaxTriggerBindings);
                const float pillRight = (lo.triggerContentH > viewH) ? 320.0f : 326.0f;
                const float remLeft = pillRight + 6.0f;
                const float remRight = pillRight + 32.0f;
                for (size_t i = 0; i < tCount; ++i)
                {
                    lo.triggerPill[i] = HitRect{ 16.0f, y, pillRight, y + 34.0f };
                    lo.triggerProfileBadge[i] = HitRect{ pillRight - 82.0f, y + 6.0f, pillRight - 6.0f, y + 28.0f };
                    lo.triggerRemove[i] = HitRect{ remLeft, y + 2.0f, remRight, y + 32.0f };
                    const TriggerConflictInfo ci = AnalyzeTriggerConflict(m_config.triggers[i]);
                    y += ci.summary.empty() ? 42.0f : 58.0f;
                }

                for (int i = 0; i < 4; ++i)
                {
                    const float ry = 364.0f + static_cast<float>(i) * 40.0f;
                    lo.behaviorRow[i] = HitRect{ 16.0f, ry, 360.0f, ry + 40.0f };
                }

                // Tab 1: Profiles layout
                {
                    float cx = 16.0f;
                    const float chipY = 96.0f;
                    const float chipH = 26.0f;
                    const std::wstring activeId = m_config.ResolveProfile(m_selectedProfileId).id;
                    size_t activeIdx = 0;
                    for (size_t i = 0; i < m_config.profiles.size(); ++i)
                    {
                        if (_wcsicmp(m_config.profiles[i].id.c_str(), activeId.c_str()) == 0)
                        {
                            activeIdx = i;
                            break;
                        }
                    }

                    std::vector<size_t> order;
                    order.reserve(m_config.profiles.size());
                    for (size_t i = 0; i < m_config.profiles.size(); ++i)
                        order.push_back(i);
                    if (activeIdx >= 2)
                    {
                        order.erase(order.begin() + static_cast<ptrdiff_t>(activeIdx));
                        order.insert(order.begin() + (std::min)(size_t{ 1 }, order.size()), activeIdx);
                    }

                    for (size_t idx : order)
                    {
                        const auto& p = m_config.profiles[idx];
                        const float chipW = std::clamp(24.0f + static_cast<float>(p.name.size()) * 6.8f, 52.0f, 92.0f);
                        if (cx + chipW > 256.0f)
                            break;
                        lo.profileChip.push_back(HitRect{ cx, chipY, cx + chipW, chipY + chipH });
                        lo.profileChipIndex.push_back(idx);
                        cx += chipW + 6.0f;
                    }
                    lo.profileNewBtn = HitRect{ 260.0f, chipY, 306.0f, chipY + chipH };
                    lo.profileDelOrResetBtn = HitRect{ 312.0f, chipY, 360.0f, chipY + chipH };
                }

                // Inline form coordinates inside Profiles card (y = 296..484)
                {
                    constexpr float formTop = 296.0f;
                    lo.inlineField1Rect = HitRect{ 28.0f, formTop + 26.0f, 348.0f, formTop + 54.0f };
                    lo.inlineField2Rect = HitRect{ 28.0f, formTop + 76.0f, 348.0f, formTop + 104.0f };
                    lo.inlineAdminRow = HitRect{ 28.0f, formTop + 110.0f, 348.0f, formTop + 138.0f };
                    lo.editNameRect = HitRect{ 28.0f, formTop + 18.0f, 348.0f, formTop + 44.0f };
                    lo.editIconStatusRect = HitRect{ 28.0f, formTop + 62.0f, 214.0f, formTop + 88.0f };
                    lo.editIconBrowseBtn = HitRect{ 220.0f, formTop + 62.0f, 288.0f, formTop + 88.0f };
                    lo.editIconResetBtn = HitRect{ 294.0f, formTop + 62.0f, 348.0f, formTop + 88.0f };
                    lo.editHideRow = HitRect{ 28.0f, formTop + 93.0f, 348.0f, formTop + 117.0f };
                    lo.editAdminRow = HitRect{ 28.0f, formTop + 120.0f, 348.0f, formTop + 144.0f };
                    lo.inlineCancelBtn = HitRect{ 176.0f, formTop + 150.0f, 256.0f, formTop + 178.0f };
                    lo.inlineSubmitBtn = HitRect{ 264.0f, formTop + 150.0f, 348.0f, formTop + 178.0f };
                }

                // Tab 1: Profile pins customize list
                {
                    const RingProfileConfig& activeProf = m_config.ResolveProfile(m_selectedProfileId);
                    lo.profilePinsContentH = static_cast<float>(activeProf.pinnedSlots.size()) * kProfilePinRowH;
                    const float pViewH = kProfilePinsViewportBot - kProfilePinsViewportTop;
                    const float pMaxScroll = (std::max)(0.0f, lo.profilePinsContentH - pViewH);
                    const float pScroll = std::clamp(m_profilePinsScrollDip, 0.0f, pMaxScroll);
                    const float pRight = (lo.profilePinsContentH > pViewH) ? 344.0f : 352.0f;

                    float py = kProfilePinsViewportTop - pScroll;
                    for (size_t i = 0; i < activeProf.pinnedSlots.size(); ++i)
                    {
                        lo.profilePinRowRect.push_back(HitRect{ 24.0f, py + 2.0f, pRight, py + kProfilePinRowH - 2.0f });
                        lo.profilePinRemoveBtn.push_back(HitRect{ pRight - 26.0f, py + 6.0f, pRight - 4.0f, py + kProfilePinRowH - 6.0f });
                        lo.profilePinEditBtn.push_back(HitRect{ pRight - 66.0f, py + 6.0f, pRight - 30.0f, py + kProfilePinRowH - 6.0f });
                        py += kProfilePinRowH;
                    }
                }

                for (int i = 0; i < 3; ++i)
                {
                    const float x0 = 16.0f + static_cast<float>(i) * 117.0f;
                    lo.texCard[i] = HitRect{ x0, 118.0f, x0 + 110.0f, 174.0f };
                }
                lo.modeChip[0] = HitRect{ 16.0f, 204.0f, 126.0f, 230.0f };
                lo.modeChip[1] = HitRect{ 133.0f, 204.0f, 243.0f, 230.0f };
                lo.modeChip[2] = HitRect{ 250.0f, 204.0f, 360.0f, 230.0f };

                for (int i = 0; i < 8; ++i)
                {
                    const float cx = 32.0f + static_cast<float>(i) * 44.5f;
                    lo.surfaceDot[i] = HitRect{ cx - 14.0f, 239.0f, cx + 14.0f, 267.0f };
                }

                for (int i = 0; i < 7; ++i)
                {
                    const float cx = 108.0f + static_cast<float>(i) * 39.5f;
                    lo.accentDot[i] = HitRect{ cx - 13.0f, 297.0f, cx + 13.0f, 325.0f };
                }

                for (int i = 0; i < 3; ++i)
                {
                    const float sy = 368.0f + static_cast<float>(i) * 50.0f;
                    lo.sliderTrack[i] = HitRect{ 26.0f, sy + 18.0f, 350.0f, sy + 44.0f };
                }
                return lo;
            }

            void DrawTogglePill(float x, float y, bool on, D2D1_COLOR_F accent, bool enabled = true)
            {
                auto mk = [&](D2D1_COLOR_F c) {
                    ComPtr<ID2D1SolidColorBrush> br;
                    m_d2dCtx->CreateSolidColorBrush(c, br.GetAddressOf());
                    return br;
                };
                const float w = 38.0f;
                const float h = 20.0f;
                const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), 10.0f, 10.0f);
                D2D1_COLOR_F fill = on ? accent : RgbF(54, 54, 62);
                if (!enabled) fill.a *= 0.45f;
                if (auto br = mk(fill)) m_d2dCtx->FillRoundedRectangle(rr, br.Get());
                if (!on)
                {
                    if (auto rb = mk(RgbF(86, 86, 96))) m_d2dCtx->DrawRoundedRectangle(rr, rb.Get(), 1.0f);
                }
                const float kx = on ? (x + w - 10.0f) : (x + 10.0f);
                const float ky = y + 10.0f;
                if (auto kb = mk(RgbF(250, 250, 252, enabled ? 1.0f : 0.5f)))
                    m_d2dCtx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, ky), 6.5f, 6.5f), kb.Get());
            }

            void RenderFlyout()
            {
                if (!m_d2dCtx || !m_targetBmp || !m_swapChain || !m_flyoutHwnd)
                    return;

                auto mk = [&](D2D1_COLOR_F c) {
                    ComPtr<ID2D1SolidColorBrush> br;
                    m_d2dCtx->CreateSolidColorBrush(c, br.GetAddressOf());
                    return br;
                };

                const SwitcherConfig eff = ResolveEffectiveConfigForSystem(m_config);
                const D2D1_COLOR_F accentCol = HexToD2D(eff.accentColor);
                const FlyoutLayout lo = ComputeLayout();

                m_d2dCtx->SetTarget(m_targetBmp.Get());
                m_d2dCtx->BeginDraw();
                m_d2dCtx->SetTransform(D2D1::Matrix3x2F::Scale(m_dpiScale, m_dpiScale));
                m_d2dCtx->Clear(RgbF(22, 22, 26));

                // Header bar
                if (auto b = mk(RgbF(28, 28, 33)))
                    m_d2dCtx->FillRectangle(D2D1::RectF(0, 0, kFlyoutW, 50.0f), b.Get());
                if (auto b = mk(RgbF(48, 48, 56)))
                    m_d2dCtx->DrawLine(D2D1::Point2F(0, 50.0f), D2D1::Point2F(kFlyoutW, 50.0f), b.Get(), 1.0f);

                if (m_fmtTitle)
                {
                    if (auto b = mk(RgbF(246, 246, 250)))
                        m_d2dCtx->DrawTextW(L"WinRings", 8, m_fmtTitle.Get(), D2D1::RectF(18.0f, 10.0f, 102.0f, 40.0f), b.Get());
                }

                // Engine status pill
                {
                    D2D1_COLOR_F pillBg = RgbF(42, 42, 48);
                    D2D1_COLOR_F dotCol = RgbF(160, 160, 170);
                    const wchar_t* stText = L"Stopped \u00B7 Start";
                    if (m_engineMode == EngineMode::Active) { pillBg = RgbF(18, 48, 34); dotCol = RgbF(52, 211, 153); stText = L"Active"; }
                    else if (m_engineMode == EngineMode::Paused) { pillBg = RgbF(54, 40, 18); dotCol = RgbF(251, 191, 36); stText = L"Paused"; }
                    else if (m_engineMode == EngineMode::Starting) { pillBg = RgbF(22, 42, 66); dotCol = RgbF(56, 189, 248); stText = L"Starting..."; }
                    else if (m_engineMode == EngineMode::SafetyLocked) { pillBg = RgbF(60, 26, 28); dotCol = RgbF(248, 113, 113); stText = L"Safety \u00B7 Reset"; }
                    else if (m_engineMode == EngineMode::PreviewOnly) { pillBg = RgbF(22, 46, 68); dotCol = RgbF(56, 189, 248); stText = L"Preview"; }
                    else { pillBg = RgbF(58, 28, 32); dotCol = RgbF(248, 113, 113); }

                    const D2D1_ROUNDED_RECT sr = D2D1::RoundedRect(lo.statusPill.ToD2D(), 12.0f, 12.0f);
                    if (auto b = mk(pillBg)) m_d2dCtx->FillRoundedRectangle(sr, b.Get());
                    if (auto b = mk(dotCol))
                        m_d2dCtx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(lo.statusPill.l + 12.0f, 25.0f), 3.6f, 3.6f), b.Get());
                    if (m_fmtCaption)
                    {
                        if (auto b = mk(RgbF(236, 236, 242)))
                            m_d2dCtx->DrawTextW(stText, static_cast<UINT32>(wcslen(stText)), m_fmtCaption.Get(),
                                D2D1::RectF(lo.statusPill.l + 22.0f, lo.statusPill.t, lo.statusPill.r - 8.0f, lo.statusPill.b), b.Get());
                    }
                }

                if (m_engineMode == EngineMode::Active || m_engineMode == EngineMode::Paused)
                    DrawTogglePill(lo.pauseSwitch.l + 5.0f, lo.pauseSwitch.t, m_engineMode == EngineMode::Active, accentCol);

                // Close button [X]
                {
                    const D2D1_POINT_2F cc{ (lo.closeBtn.l + lo.closeBtn.r) * 0.5f, (lo.closeBtn.t + lo.closeBtn.b) * 0.5f };
                    if (m_hoverButton == 99)
                    {
                        if (auto b = mk(RgbF(56, 56, 64)))
                            m_d2dCtx->FillEllipse(D2D1::Ellipse(cc, 12.0f, 12.0f), b.Get());
                    }
                    if (auto b = mk(RgbF(195, 195, 204)))
                    {
                        m_d2dCtx->DrawLine(D2D1::Point2F(cc.x - 4.5f, cc.y - 4.5f), D2D1::Point2F(cc.x + 4.5f, cc.y + 4.5f), b.Get(), 1.4f);
                        m_d2dCtx->DrawLine(D2D1::Point2F(cc.x + 4.5f, cc.y - 4.5f), D2D1::Point2F(cc.x - 4.5f, cc.y + 4.5f), b.Get(), 1.4f);
                    }
                }

                // Segmented tabs
                {
                    const D2D1_ROUNDED_RECT track = D2D1::RoundedRect(D2D1::RectF(16.0f, 54.0f, 360.0f, 88.0f), 10.0f, 10.0f);
                    if (auto b = mk(RgbF(30, 30, 36))) m_d2dCtx->FillRoundedRectangle(track, b.Get());
                    if (auto b = mk(RgbF(48, 48, 56))) m_d2dCtx->DrawRoundedRectangle(track, b.Get(), 1.0f);

                    const HitRect& activeR = (m_activeTab == 0) ? lo.tabTriggers
                        : (m_activeTab == 1) ? lo.tabProfiles : lo.tabLook;
                    const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(activeR.ToD2D(), 8.0f, 8.0f);
                    if (auto b = mk(RgbF(46, 46, 54))) m_d2dCtx->FillRoundedRectangle(pill, b.Get());
                    const float indX = (activeR.l + activeR.r) * 0.5f;
                    if (auto b = mk(accentCol))
                        m_d2dCtx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(indX - 14.0f, activeR.b - 2.5f, indX + 14.0f, activeR.b), 1.2f, 1.2f), b.Get());

                    if (m_fmtCenter)
                    {
                        if (auto b = mk(m_activeTab == 0 ? RgbF(248, 248, 252) : RgbF(158, 158, 168)))
                            m_d2dCtx->DrawTextW(L"Triggers", 8, m_fmtCenter.Get(), lo.tabTriggers.ToD2D(), b.Get());
                        if (auto b = mk(m_activeTab == 1 ? RgbF(248, 248, 252) : RgbF(158, 158, 168)))
                            m_d2dCtx->DrawTextW(L"Profiles", 8, m_fmtCenter.Get(), lo.tabProfiles.ToD2D(), b.Get());
                        if (auto b = mk(m_activeTab == 2 ? RgbF(248, 248, 252) : RgbF(158, 158, 168)))
                            m_d2dCtx->DrawTextW(L"Look", 4, m_fmtCenter.Get(), lo.tabLook.ToD2D(), b.Get());
                    }
                }

                if (m_activeTab == 0)
                    RenderTriggersTab(lo, accentCol);
                else if (m_activeTab == 1)
                    RenderProfilesTab(lo, accentCol);
                else
                    RenderLookTab(lo, eff, accentCol);

                const HRESULT endHr = m_d2dCtx->EndDraw();
                m_d2dCtx->SetTarget(nullptr);
                if (endHr == D2DERR_RECREATE_TARGET)
                {
                    InitFlyoutGraphics(m_swapW, m_swapH);
                    return;
                }
                if (m_swapChain)
                    m_swapChain->Present(0, 0);
            }

            void RenderTriggersTab(const FlyoutLayout& lo, D2D1_COLOR_F accentCol)
            {
                auto mk = [&](D2D1_COLOR_F c) {
                    ComPtr<ID2D1SolidColorBrush> br;
                    m_d2dCtx->CreateSolidColorBrush(c, br.GetAddressOf());
                    return br;
                };

                if (m_fmtCaption)
                {
                    if (auto b = mk(RgbF(144, 144, 156)))
                        m_d2dCtx->DrawTextW(L"SHORTCUTS", 9, m_fmtCaption.Get(), D2D1::RectF(18.0f, 96.0f, 200.0f, 114.0f), b.Get());
                }

                if (m_config.triggers.size() < kMaxTriggerBindings)
                {
                    const D2D1_ROUNDED_RECT addR = D2D1::RoundedRect(lo.addTriggerBtn.ToD2D(), 7.0f, 7.0f);
                    if (auto b = mk(RgbF(38, 38, 46))) m_d2dCtx->FillRoundedRectangle(addR, b.Get());
                    if (auto b = mk(RgbF(66, 66, 76))) m_d2dCtx->DrawRoundedRectangle(addR, b.Get(), 1.0f);
                    if (m_fmtSmallCenter)
                    {
                        if (auto b = mk(RgbF(228, 228, 236)))
                            m_d2dCtx->DrawTextW(L"+ Add", 5, m_fmtSmallCenter.Get(), lo.addTriggerBtn.ToD2D(), b.Get());
                    }
                }

                const size_t tCount = (std::min)(m_config.triggers.size(), kMaxTriggerBindings);
                if (tCount == 0 && m_fmtBody)
                {
                    if (auto b = mk(RgbF(136, 136, 148)))
                        m_d2dCtx->DrawTextW(L"No triggers bound. Click + Add to record a key or mouse button.", 61,
                            m_fmtBody.Get(), D2D1::RectF(20.0f, 140.0f, 356.0f, 180.0f), b.Get());
                }

                m_d2dCtx->PushAxisAlignedClip(D2D1::RectF(12.0f, kTriggerViewportTop, 368.0f, kTriggerViewportBot),
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

                for (size_t i = 0; i < tCount; ++i)
                {
                    const TriggerBinding& tb = m_config.triggers[i];
                    const bool rec = (m_recordingIndex == static_cast<int>(i));
                    const HitRect& pr = lo.triggerPill[i];
                    const D2D1_ROUNDED_RECT pillR = D2D1::RoundedRect(pr.ToD2D(), 9.0f, 9.0f);

                    if (auto b = mk(rec ? RgbF(28, 38, 52) : RgbF(34, 34, 40)))
                        m_d2dCtx->FillRoundedRectangle(pillR, b.Get());
                    if (auto b = mk(rec ? accentCol : RgbF(60, 60, 68)))
                        m_d2dCtx->DrawRoundedRectangle(pillR, b.Get(), rec ? 1.5f : 1.0f);

                    const std::wstring label = rec ? L"Press Win, Alt+Tab, key, or mouse..." : FormatTriggerBinding(tb);
                    if (m_fmtBold)
                    {
                        if (auto b = mk(rec ? RgbF(185, 222, 255) : RgbF(244, 244, 248)))
                            m_d2dCtx->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), m_fmtBold.Get(),
                                D2D1::RectF(pr.l + 12.0f, pr.t, pr.r - 86.0f, pr.b), b.Get());
                    }

                    if (!rec)
                    {
                        const std::wstring prof = (tb.profileId.empty() ? std::wstring(L"default") : tb.profileId) + L" \u25BE";
                        const D2D1_ROUNDED_RECT profR = D2D1::RoundedRect(lo.triggerProfileBadge[i].ToD2D(), 5.0f, 5.0f);
                        const bool isCustomProf = (_wcsicmp(tb.profileId.c_str(), L"default") != 0 && !tb.profileId.empty());
                        if (auto b = mk(isCustomProf ? RgbF(38, 52, 70) : RgbF(48, 48, 56)))
                            m_d2dCtx->FillRoundedRectangle(profR, b.Get());
                        if (auto b = mk(isCustomProf ? accentCol : RgbF(72, 72, 82)))
                            m_d2dCtx->DrawRoundedRectangle(profR, b.Get(), 1.0f);
                        if (m_fmtSmallCenter)
                        {
                            if (auto b = mk(isCustomProf ? RgbF(216, 238, 255) : RgbF(186, 186, 198)))
                                m_d2dCtx->DrawTextW(prof.c_str(), static_cast<UINT32>(prof.size()), m_fmtSmallCenter.Get(), profR.rect, b.Get());
                        }
                    }

                    // Remove [X]
                    const HitRect& rm = lo.triggerRemove[i];
                    const D2D1_ROUNDED_RECT rmR = D2D1::RoundedRect(rm.ToD2D(), 7.0f, 7.0f);
                    if (auto b = mk(RgbF(38, 38, 44))) m_d2dCtx->FillRoundedRectangle(rmR, b.Get());
                    if (auto b = mk(RgbF(180, 180, 190)))
                    {
                        const float cx = (rm.l + rm.r) * 0.5f;
                        const float cy = (rm.t + rm.b) * 0.5f;
                        m_d2dCtx->DrawLine(D2D1::Point2F(cx - 3.5f, cy - 3.5f), D2D1::Point2F(cx + 3.5f, cy + 3.5f), b.Get(), 1.3f);
                        m_d2dCtx->DrawLine(D2D1::Point2F(cx + 3.5f, cy - 3.5f), D2D1::Point2F(cx - 3.5f, cy + 3.5f), b.Get(), 1.3f);
                    }

                    const TriggerConflictInfo ci = AnalyzeTriggerConflict(tb);
                    if (!ci.summary.empty() && m_fmtCaption)
                    {
                        D2D1_COLOR_F noteCol = ci.hasWarning ? RgbF(251, 191, 36) : RgbF(148, 163, 184);
                        if (auto b = mk(noteCol))
                            m_d2dCtx->DrawTextW(ci.summary.c_str(), static_cast<UINT32>(ci.summary.size()), m_fmtCaption.Get(),
                                D2D1::RectF(20.0f, pr.b + 1.0f, 358.0f, pr.b + 17.0f), b.Get());
                    }
                }

                const float viewH = kTriggerViewportBot - kTriggerViewportTop;
                if (lo.triggerContentH > viewH)
                {
                    if (auto b = mk(RgbF(38, 38, 46)))
                        m_d2dCtx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(364.0f, kTriggerViewportTop, 367.0f, kTriggerViewportBot), 1.5f, 1.5f), b.Get());
                    const float thumbH = (std::max)(26.0f, viewH * (viewH / lo.triggerContentH));
                    const float maxScroll = lo.triggerContentH - viewH;
                    const float frac = (maxScroll > 0.0f) ? std::clamp(m_triggerScrollDip / maxScroll, 0.0f, 1.0f) : 0.0f;
                    const float thumbY = kTriggerViewportTop + frac * (viewH - thumbH);
                    if (auto b = mk(RgbF(124, 124, 138)))
                        m_d2dCtx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(364.0f, thumbY, 367.0f, thumbY + thumbH), 1.5f, 1.5f), b.Get());
                }

                m_d2dCtx->PopAxisAlignedClip();

                // Behavior section
                if (m_fmtCaption)
                {
                    if (auto b = mk(RgbF(144, 144, 156)))
                        m_d2dCtx->DrawTextW(L"BEHAVIOR", 8, m_fmtCaption.Get(), D2D1::RectF(18.0f, 344.0f, 200.0f, 362.0f), b.Get());
                }

                const D2D1_ROUNDED_RECT behCard = D2D1::RoundedRect(D2D1::RectF(16.0f, 364.0f, 360.0f, 524.0f), 10.0f, 10.0f);
                if (auto b = mk(RgbF(32, 32, 38))) m_d2dCtx->FillRoundedRectangle(behCard, b.Get());
                if (auto b = mk(RgbF(52, 52, 60))) m_d2dCtx->DrawRoundedRectangle(behCard, b.Get(), 1.0f);

                struct BehItem { const wchar_t* label; bool val; };
                const BehItem items[4] = {
                    { L"Tap to Open / Close", m_config.gestureTapOpenClose },
                    { L"Hold, Point & Release to Switch", m_config.gestureHoldReleaseSwitch },
                    { L"Double-Tap for Last 2 Apps", m_config.doubleTapLastApp },
                    { L"Glide Pointer to Window", m_config.pointerFollow },
                };
                for (int i = 0; i < 4; ++i)
                {
                    const HitRect& rr = lo.behaviorRow[i];
                    if (i > 0)
                    {
                        if (auto b = mk(RgbF(44, 44, 52)))
                            m_d2dCtx->DrawLine(D2D1::Point2F(rr.l + 12.0f, rr.t), D2D1::Point2F(rr.r - 12.0f, rr.t), b.Get(), 1.0f);
                    }
                    if (m_fmtBody)
                    {
                        if (auto b = mk(RgbF(238, 238, 244)))
                            m_d2dCtx->DrawTextW(items[i].label, static_cast<UINT32>(wcslen(items[i].label)), m_fmtBody.Get(),
                                D2D1::RectF(rr.l + 14.0f, rr.t, rr.r - 60.0f, rr.b), b.Get());
                    }
                    DrawTogglePill(rr.r - 50.0f, (rr.t + rr.b) * 0.5f - 10.0f, items[i].val, accentCol);
                }
            }

            void RenderProfilesTab(const FlyoutLayout& lo, D2D1_COLOR_F accentCol)
            {
                auto mk = [&](D2D1_COLOR_F c) {
                    ComPtr<ID2D1SolidColorBrush> br;
                    m_d2dCtx->CreateSolidColorBrush(c, br.GetAddressOf());
                    return br;
                };

                const RingProfileConfig& prof = m_config.ResolveProfile(m_selectedProfileId);

                // Profile selector chips + [+ New] + [Delete / Reset]
                const size_t chipCount = (std::min)(lo.profileChip.size(), lo.profileChipIndex.size());
                for (size_t i = 0; i < chipCount; ++i)
                {
                    const auto& p = m_config.profiles[lo.profileChipIndex[i]];
                    const bool active = (_wcsicmp(p.id.c_str(), prof.id.c_str()) == 0);
                    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(lo.profileChip[i].ToD2D(), 7.0f, 7.0f);
                    if (auto b = mk(active ? RgbF(38, 48, 64) : RgbF(32, 32, 38)))
                        m_d2dCtx->FillRoundedRectangle(rr, b.Get());
                    if (auto b = mk(active ? accentCol : RgbF(54, 54, 62)))
                        m_d2dCtx->DrawRoundedRectangle(rr, b.Get(), active ? 1.5f : 1.0f);
                    if (m_fmtSmallCenter)
                    {
                        if (auto b = mk(active ? RgbF(248, 248, 252) : RgbF(186, 186, 198)))
                            m_d2dCtx->DrawTextW(p.name.c_str(), static_cast<UINT32>(p.name.size()), m_fmtSmallCenter.Get(),
                                lo.profileChip[i].ToD2D(), b.Get());
                    }
                }

                // [+ New] button
                {
                    const bool newOpen = (m_inlineForm == ProfileInlineForm::NewProfile);
                    const D2D1_ROUNDED_RECT nr = D2D1::RoundedRect(lo.profileNewBtn.ToD2D(), 7.0f, 7.0f);
                    if (auto b = mk(newOpen ? RgbF(38, 48, 64) : RgbF(38, 38, 46))) m_d2dCtx->FillRoundedRectangle(nr, b.Get());
                    if (auto b = mk(newOpen ? accentCol : RgbF(66, 66, 76))) m_d2dCtx->DrawRoundedRectangle(nr, b.Get(), newOpen ? 1.4f : 1.0f);
                    if (m_fmtSmallCenter)
                    {
                        if (auto b = mk(newOpen ? RgbF(246, 250, 255) : RgbF(228, 228, 236)))
                            m_d2dCtx->DrawTextW(L"+ New", 5, m_fmtSmallCenter.Get(), lo.profileNewBtn.ToD2D(), b.Get());
                    }
                }

                // [Reset] for SMenu or [Delete] for custom profiles
                const bool isDefaultProf = (_wcsicmp(prof.id.c_str(), L"default") == 0);
                const bool isSMenuProf = (_wcsicmp(prof.id.c_str(), L"SMenu") == 0);
                if (!isDefaultProf)
                {
                    const D2D1_ROUNDED_RECT dr = D2D1::RoundedRect(lo.profileDelOrResetBtn.ToD2D(), 7.0f, 7.0f);
                    if (auto b = mk(isSMenuProf ? RgbF(38, 38, 46) : RgbF(58, 28, 32))) m_d2dCtx->FillRoundedRectangle(dr, b.Get());
                    if (auto b = mk(isSMenuProf ? RgbF(66, 66, 76) : RgbF(120, 56, 60))) m_d2dCtx->DrawRoundedRectangle(dr, b.Get(), 1.0f);
                    const wchar_t* actTxt = isSMenuProf ? L"Reset" : L"Delete";
                    if (m_fmtSmallCenter)
                    {
                        if (auto b = mk(isSMenuProf ? RgbF(220, 220, 230) : RgbF(250, 175, 175)))
                            m_d2dCtx->DrawTextW(actTxt, static_cast<UINT32>(wcslen(actTxt)), m_fmtSmallCenter.Get(), lo.profileDelOrResetBtn.ToD2D(), b.Get());
                    }
                }

                // Primary action: Open on Ring to Edit + Config File button
                {
                    D2D1_COLOR_F openFill = accentCol;
                    openFill.a = 0.20f;
                    const D2D1_ROUNDED_RECT orr = D2D1::RoundedRect(lo.openOnRingBtn.ToD2D(), 8.0f, 8.0f);
                    if (auto b = mk(openFill)) m_d2dCtx->FillRoundedRectangle(orr, b.Get());
                    if (auto b = mk(accentCol)) m_d2dCtx->DrawRoundedRectangle(orr, b.Get(), 1.25f);
                    const std::wstring openTxt = L"Open \"" + prof.name + L"\" on Ring";
                    if (m_fmtCenter)
                    {
                        if (auto b = mk(RgbF(246, 250, 255)))
                            m_d2dCtx->DrawTextW(openTxt.c_str(), static_cast<UINT32>(openTxt.size()), m_fmtCenter.Get(),
                                lo.openOnRingBtn.ToD2D(), b.Get());
                    }

                    const D2D1_ROUNDED_RECT cfr = D2D1::RoundedRect(lo.openConfigBtn.ToD2D(), 8.0f, 8.0f);
                    if (auto b = mk(RgbF(34, 34, 41))) m_d2dCtx->FillRoundedRectangle(cfr, b.Get());
                    if (auto b = mk(RgbF(60, 60, 70))) m_d2dCtx->DrawRoundedRectangle(cfr, b.Get(), 1.0f);
                    if (m_fmtSmallCenter)
                    {
                        if (auto b = mk(RgbF(220, 220, 232)))
                            m_d2dCtx->DrawTextW(L"Config File", 11, m_fmtSmallCenter.Get(), lo.openConfigBtn.ToD2D(), b.Get());
                    }
                }

                // Show Open Windows toggle card
                {
                    const D2D1_ROUNDED_RECT rc = D2D1::RoundedRect(lo.profileRunningRow.ToD2D(), 9.0f, 9.0f);
                    if (auto b = mk(RgbF(32, 32, 38))) m_d2dCtx->FillRoundedRectangle(rc, b.Get());
                    if (auto b = mk(RgbF(52, 52, 60))) m_d2dCtx->DrawRoundedRectangle(rc, b.Get(), 1.0f);
                    if (m_fmtBody)
                    {
                        const wchar_t* t = L"Show Open Windows on Ring";
                        if (auto b = mk(RgbF(238, 238, 244)))
                            m_d2dCtx->DrawTextW(t, static_cast<UINT32>(wcslen(t)), m_fmtBody.Get(),
                                D2D1::RectF(lo.profileRunningRow.l + 12.0f, lo.profileRunningRow.t, lo.profileRunningRow.r - 58.0f, lo.profileRunningRow.b), b.Get());
                    }
                    DrawTogglePill(lo.profileRunningRow.r - 50.0f,
                        (lo.profileRunningRow.t + lo.profileRunningRow.b) * 0.5f - 10.0f,
                        prof.includeRunningApps, accentCol);
                }

                // Quick-add buttons
                if (m_fmtCaption)
                {
                    const std::wstring addTitle = L"QUICK ADD TO \"" + prof.name + L"\"";
                    if (auto b = mk(RgbF(144, 144, 156)))
                        m_d2dCtx->DrawTextW(addTitle.c_str(), static_cast<UINT32>(addTitle.size()), m_fmtCaption.Get(),
                            D2D1::RectF(18.0f, 218.0f, 358.0f, 234.0f), b.Get());
                }

                struct AddBtn { HitRect r; const wchar_t* txt; bool active; };
                const AddBtn addBtns[3] = {
                    { lo.addAppBtn, L"+ App / File", false },
                    { lo.addFolderBtn, L"+ Folder", false },
                    { lo.addLinkBtn, L"+ Link / Cmd", m_inlineForm == ProfileInlineForm::AddLinkCmd }
                };
                for (const auto& ab : addBtns)
                {
                    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(ab.r.ToD2D(), 7.0f, 7.0f);
                    if (auto b = mk(ab.active ? RgbF(38, 48, 64) : RgbF(36, 36, 44))) m_d2dCtx->FillRoundedRectangle(rr, b.Get());
                    if (auto b = mk(ab.active ? accentCol : RgbF(62, 62, 72))) m_d2dCtx->DrawRoundedRectangle(rr, b.Get(), ab.active ? 1.4f : 1.0f);
                    if (m_fmtSmallCenter)
                    {
                        if (auto b = mk(ab.active ? RgbF(246, 250, 255) : RgbF(230, 230, 240)))
                            m_d2dCtx->DrawTextW(ab.txt, static_cast<UINT32>(wcslen(ab.txt)), m_fmtSmallCenter.Get(), ab.r.ToD2D(), b.Get());
                    }
                }

                // Customize Pins Card OR Inline Add/Edit/New Form inside the exact same card
                std::wstring profCardHdr;
                if (m_inlineForm == ProfileInlineForm::AddLinkCmd)
                    profCardHdr = L"ADD LINK, APP, OR COMMAND";
                else if (m_inlineForm == ProfileInlineForm::NewProfile)
                    profCardHdr = L"CREATE NEW PROFILE";
                else if (m_inlineForm == ProfileInlineForm::EditSlot)
                    profCardHdr = L"EDIT PINNED ITEM";
                else
                    profCardHdr = L"CUSTOMIZE PINS (" + std::to_wstring(prof.pinnedSlots.size()) + L")";

                if (m_fmtCaption)
                {
                    if (auto b = mk(RgbF(144, 144, 156)))
                        m_d2dCtx->DrawTextW(profCardHdr.c_str(), static_cast<UINT32>(profCardHdr.size()), m_fmtCaption.Get(),
                            D2D1::RectF(18.0f, 276.0f, 358.0f, 292.0f), b.Get());
                }

                const D2D1_ROUNDED_RECT guideCard = D2D1::RoundedRect(D2D1::RectF(16.0f, 296.0f, 360.0f, 484.0f), 10.0f, 10.0f);
                if (auto b = mk(RgbF(30, 30, 36))) m_d2dCtx->FillRoundedRectangle(guideCard, b.Get());
                if (auto b = mk(m_inlineForm != ProfileInlineForm::None ? accentCol : RgbF(52, 52, 60)))
                    m_d2dCtx->DrawRoundedRectangle(guideCard, b.Get(), 1.0f);

                if (m_inlineForm != ProfileInlineForm::None)
                {
                    RenderInlineFormFields(lo, 296.0f, accentCol);
                }
                else if (prof.pinnedSlots.empty())
                {
                    if (m_fmtGuideBody)
                    {
                        const wchar_t* emptyMsg = L"No pinned items in this profile yet.\nUse + App / File, + Folder, or + Link / Cmd above, or drop items onto the Ring center.";
                        if (auto b = mk(RgbF(150, 150, 166)))
                            m_d2dCtx->DrawTextW(emptyMsg, static_cast<UINT32>(wcslen(emptyMsg)), m_fmtGuideBody.Get(),
                                D2D1::RectF(28.0f, 326.0f, 348.0f, 440.0f), b.Get());
                    }
                }
                else
                {
                    m_d2dCtx->PushAxisAlignedClip(D2D1::RectF(18.0f, kProfilePinsViewportTop, 358.0f, kProfilePinsViewportBot),
                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

                    const size_t count = (std::min)(prof.pinnedSlots.size(), lo.profilePinRowRect.size());
                    for (size_t i = 0; i < count; ++i)
                    {
                        const auto& slot = prof.pinnedSlots[i];
                        const HitRect& rr = lo.profilePinRowRect[i];
                        if (rr.b < kProfilePinsViewportTop || rr.t > kProfilePinsViewportBot)
                            continue;

                        const D2D1_ROUNDED_RECT rowR = D2D1::RoundedRect(rr.ToD2D(), 6.0f, 6.0f);
                        if (auto b = mk(slot.hiddenFromRing ? RgbF(30, 30, 37) : RgbF(36, 36, 44)))
                            m_d2dCtx->FillRoundedRectangle(rowR, b.Get());
                        if (auto b = mk(slot.hiddenFromRing ? RgbF(48, 48, 58) : RgbF(54, 54, 64)))
                            m_d2dCtx->DrawRoundedRectangle(rowR, b.Get(), 1.0f);

                        std::wstring title = slot.label.empty() ? InferProfileItemLabel(slot.kind, slot.pathOrUrl) : slot.label;
                        if (slot.hiddenFromRing)
                            title += L" \u00B7 Hidden";
                        if (slot.runAsAdmin)
                            title += L" \u00B7 Admin";
                        if (!slot.iconCacheFile.empty() && slot.iconCacheFile.rfind(L"builtin:", 0) != 0)
                            title += L" \u00B7 Custom Icon";

                        if (m_fmtBody)
                        {
                            if (auto b = mk(slot.hiddenFromRing ? RgbF(190, 190, 204) : RgbF(240, 240, 248)))
                                m_d2dCtx->DrawTextW(title.c_str(), static_cast<UINT32>(title.size()), m_fmtBody.Get(),
                                    D2D1::RectF(rr.l + 10.0f, rr.t, lo.profilePinEditBtn[i].l - 6.0f, rr.b), b.Get());
                        }

                        // [Edit] button
                        const D2D1_ROUNDED_RECT er = D2D1::RoundedRect(lo.profilePinEditBtn[i].ToD2D(), 5.0f, 5.0f);
                        if (auto b = mk(RgbF(44, 52, 66))) m_d2dCtx->FillRoundedRectangle(er, b.Get());
                        if (auto b = mk(RgbF(70, 84, 108))) m_d2dCtx->DrawRoundedRectangle(er, b.Get(), 1.0f);
                        if (m_fmtSmallCenter)
                        {
                            if (auto b = mk(RgbF(214, 232, 255)))
                                m_d2dCtx->DrawTextW(L"Edit", 4, m_fmtSmallCenter.Get(), lo.profilePinEditBtn[i].ToD2D(), b.Get());
                        }

                        // [X] remove button
                        const HitRect& rm = lo.profilePinRemoveBtn[i];
                        const D2D1_ROUNDED_RECT rmR = D2D1::RoundedRect(rm.ToD2D(), 5.0f, 5.0f);
                        if (auto b = mk(RgbF(46, 36, 40))) m_d2dCtx->FillRoundedRectangle(rmR, b.Get());
                        if (auto b = mk(RgbF(204, 152, 156)))
                        {
                            const float cx = (rm.l + rm.r) * 0.5f;
                            const float cy = (rm.t + rm.b) * 0.5f;
                            m_d2dCtx->DrawLine(D2D1::Point2F(cx - 3.0f, cy - 3.0f), D2D1::Point2F(cx + 3.0f, cy + 3.0f), b.Get(), 1.3f);
                            m_d2dCtx->DrawLine(D2D1::Point2F(cx + 3.0f, cy - 3.0f), D2D1::Point2F(cx - 3.0f, cy + 3.0f), b.Get(), 1.3f);
                        }
                    }

                    const float viewH = kProfilePinsViewportBot - kProfilePinsViewportTop;
                    if (lo.profilePinsContentH > viewH)
                    {
                        if (auto b = mk(RgbF(38, 38, 46)))
                            m_d2dCtx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(351.0f, kProfilePinsViewportTop, 354.0f, kProfilePinsViewportBot), 1.5f, 1.5f), b.Get());
                        const float thumbH = (std::max)(26.0f, viewH * (viewH / lo.profilePinsContentH));
                        const float maxScroll = lo.profilePinsContentH - viewH;
                        const float frac = (maxScroll > 0.0f) ? std::clamp(m_profilePinsScrollDip / maxScroll, 0.0f, 1.0f) : 0.0f;
                        const float thumbY = kProfilePinsViewportTop + frac * (viewH - thumbH);
                        if (auto b = mk(RgbF(124, 124, 138)))
                            m_d2dCtx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(351.0f, thumbY, 354.0f, thumbY + thumbH), 1.5f, 1.5f), b.Get());
                    }

                    m_d2dCtx->PopAxisAlignedClip();
                }

                // Standalone Profile Config File Footer
                {
                    const D2D1_ROUNDED_RECT fr = D2D1::RoundedRect(lo.openProfilesFolderBtn.ToD2D(), 8.0f, 8.0f);
                    if (auto b = mk(RgbF(30, 30, 36))) m_d2dCtx->FillRoundedRectangle(fr, b.Get());
                    if (auto b = mk(RgbF(50, 50, 58))) m_d2dCtx->DrawRoundedRectangle(fr, b.Get(), 1.0f);

                    const std::wstring footerLeft = L"profiles\\" + SwitcherConfig::SanitizeProfileId(prof.id) + L".json  \u00B7  " +
                        std::to_wstring(prof.pinnedSlots.size()) + L" items";
                    if (m_fmtCaption)
                    {
                        if (auto b = mk(RgbF(168, 168, 182)))
                            m_d2dCtx->DrawTextW(footerLeft.c_str(), static_cast<UINT32>(footerLeft.size()), m_fmtCaption.Get(),
                                D2D1::RectF(lo.openProfilesFolderBtn.l + 10.0f, lo.openProfilesFolderBtn.t,
                                    lo.openProfilesFolderBtn.r - 84.0f, lo.openProfilesFolderBtn.b), b.Get());
                    }
                    if (m_fmtSmallCenter)
                    {
                        if (auto b = mk(RgbF(220, 224, 236)))
                            m_d2dCtx->DrawTextW(L"Open Folder", 11, m_fmtSmallCenter.Get(),
                                D2D1::RectF(lo.openProfilesFolderBtn.r - 80.0f, lo.openProfilesFolderBtn.t,
                                    lo.openProfilesFolderBtn.r - 8.0f, lo.openProfilesFolderBtn.b), b.Get());
                    }
                }
            }

            void RenderInlineFormFields(const FlyoutLayout& lo, float formTop, D2D1_COLOR_F accentCol)
            {
                auto mk = [&](D2D1_COLOR_F c) {
                    ComPtr<ID2D1SolidColorBrush> br;
                    m_d2dCtx->CreateSolidColorBrush(c, br.GetAddressOf());
                    return br;
                };

                auto drawInlineInput = [&](const HitRect& box, const std::wstring& value, const wchar_t* placeholder, bool focused) {
                    const D2D1_ROUNDED_RECT br = D2D1::RoundedRect(box.ToD2D(), 6.0f, 6.0f);
                    if (auto b = mk(RgbF(20, 20, 26))) m_d2dCtx->FillRoundedRectangle(br, b.Get());
                    if (auto b = mk(focused ? accentCol : RgbF(60, 60, 70))) m_d2dCtx->DrawRoundedRectangle(br, b.Get(), focused ? 1.4f : 1.0f);

                    const float innerL = box.l + 9.0f;
                    const float innerR = box.r - 9.0f;
                    const float innerW = (std::max)(20.0f, innerR - innerL);
                    float textW = 0.0f;
                    if (!value.empty() && m_dwrite && m_fmtBody)
                    {
                        ComPtr<IDWriteTextLayout> layout;
                        if (SUCCEEDED(m_dwrite->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()),
                            m_fmtBody.Get(), 4000.0f, box.b - box.t, layout.GetAddressOf())) && layout)
                        {
                            DWRITE_TEXT_METRICS tm{};
                            if (SUCCEEDED(layout->GetMetrics(&tm)))
                                textW = tm.widthIncludingTrailingWhitespace;
                        }
                    }

                    const float scrollOffset = (textW > innerW - 3.0f) ? (textW - (innerW - 3.0f)) : 0.0f;
                    m_d2dCtx->PushAxisAlignedClip(D2D1::RectF(innerL, box.t + 2.0f, innerR, box.b - 2.0f),
                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    if (m_fmtBody)
                    {
                        if (value.empty())
                        {
                            if (auto b = mk(RgbF(112, 112, 126)))
                                m_d2dCtx->DrawTextW(placeholder, static_cast<UINT32>(wcslen(placeholder)), m_fmtBody.Get(),
                                    D2D1::RectF(innerL, box.t, innerR, box.b), b.Get());
                        }
                        else if (auto b = mk(RgbF(242, 242, 248)))
                        {
                            m_d2dCtx->DrawTextW(value.c_str(), static_cast<UINT32>(value.size()), m_fmtBody.Get(),
                                D2D1::RectF(innerL - scrollOffset, box.t, innerL - scrollOffset + 4000.0f, box.b), b.Get());
                        }
                    }
                    if (focused && m_inlineCaretOn)
                    {
                        const float caretX = std::clamp(innerL + textW - scrollOffset, innerL, innerR - 1.5f);
                        if (auto b = mk(RgbF(236, 244, 255)))
                            m_d2dCtx->DrawLine(D2D1::Point2F(caretX, box.t + 6.0f), D2D1::Point2F(caretX, box.b - 6.0f), b.Get(), 1.4f);
                    }
                    m_d2dCtx->PopAxisAlignedClip();
                };

                if (m_inlineForm == ProfileInlineForm::AddLinkCmd)
                {
                    if (m_fmtCaption)
                    {
                        if (auto b = mk(RgbF(168, 168, 182)))
                        {
                            m_d2dCtx->DrawTextW(L"URL, PATH, OR COMMAND", 21, m_fmtCaption.Get(), D2D1::RectF(28.0f, formTop + 8.0f, 348.0f, formTop + 24.0f), b.Get());
                            m_d2dCtx->DrawTextW(L"LABEL (OPTIONAL)", 16, m_fmtCaption.Get(), D2D1::RectF(28.0f, formTop + 58.0f, 348.0f, formTop + 74.0f), b.Get());
                        }
                    }
                    drawInlineInput(lo.inlineField1Rect, m_inlineField1, L"https://... or path\\app.exe / cmd.exe", m_inlineFocusField == 0);
                    drawInlineInput(lo.inlineField2Rect, m_inlineField2, L"Auto-detected from URL or path", m_inlineFocusField == 1);

                    if (m_fmtBody)
                    {
                        if (auto b = mk(RgbF(230, 230, 240)))
                            m_d2dCtx->DrawTextW(L"Run as Administrator", 20, m_fmtBody.Get(),
                                D2D1::RectF(lo.inlineAdminRow.l, lo.inlineAdminRow.t, lo.inlineAdminRow.r - 50.0f, lo.inlineAdminRow.b), b.Get());
                    }
                    DrawTogglePill(lo.inlineAdminRow.r - 40.0f, (lo.inlineAdminRow.t + lo.inlineAdminRow.b) * 0.5f - 10.0f, m_inlineAdmin, accentCol);

                    const D2D1_ROUNDED_RECT cancelR = D2D1::RoundedRect(lo.inlineCancelBtn.ToD2D(), 7.0f, 7.0f);
                    if (auto b = mk(RgbF(40, 40, 48))) m_d2dCtx->FillRoundedRectangle(cancelR, b.Get());
                    if (auto b = mk(RgbF(68, 68, 78))) m_d2dCtx->DrawRoundedRectangle(cancelR, b.Get(), 1.0f);
                    if (m_fmtSmallCenter)
                    {
                        if (auto b = mk(RgbF(214, 214, 226)))
                            m_d2dCtx->DrawTextW(L"Cancel", 6, m_fmtSmallCenter.Get(), lo.inlineCancelBtn.ToD2D(), b.Get());
                    }

                    const bool canSubmit = !TrimWide(m_inlineField1).empty();
                    D2D1_COLOR_F addFill = accentCol;
                    addFill.a = canSubmit ? 0.28f : 0.12f;
                    const D2D1_ROUNDED_RECT subR = D2D1::RoundedRect(lo.inlineSubmitBtn.ToD2D(), 7.0f, 7.0f);
                    if (auto b = mk(addFill)) m_d2dCtx->FillRoundedRectangle(subR, b.Get());
                    if (auto b = mk(canSubmit ? accentCol : RgbF(68, 68, 78))) m_d2dCtx->DrawRoundedRectangle(subR, b.Get(), 1.2f);
                    if (m_fmtSmallCenter)
                    {
                        if (auto b = mk(canSubmit ? RgbF(248, 250, 255) : RgbF(130, 130, 144)))
                            m_d2dCtx->DrawTextW(L"Add", 3, m_fmtSmallCenter.Get(), lo.inlineSubmitBtn.ToD2D(), b.Get());
                    }
                }
                else if (m_inlineForm == ProfileInlineForm::NewProfile)
                {
                    if (m_fmtCaption)
                    {
                        if (auto b = mk(RgbF(168, 168, 182)))
                            m_d2dCtx->DrawTextW(L"PROFILE NAME", 12, m_fmtCaption.Get(), D2D1::RectF(28.0f, formTop + 8.0f, 348.0f, formTop + 24.0f), b.Get());
                    }
                    drawInlineInput(lo.inlineField1Rect, m_inlineField1, L"e.g. Work, Gaming, Dev...", true);

                    if (m_fmtGuideBody)
                    {
                        const std::wstring previewId = TrimWide(m_inlineField1).empty()
                            ? std::wstring(L"<id>")
                            : SwitcherConfig::SanitizeProfileId(TrimWide(m_inlineField1));
                        const std::wstring note = L"Creates standalone config profiles\\" + previewId + L".json. Drag items directly onto the Ring after creation.";
                        if (auto b = mk(RgbF(158, 158, 174)))
                            m_d2dCtx->DrawTextW(note.c_str(), static_cast<UINT32>(note.size()), m_fmtGuideBody.Get(),
                                D2D1::RectF(28.0f, formTop + 66.0f, 348.0f, formTop + 134.0f), b.Get());
                    }

                    const D2D1_ROUNDED_RECT cancelR = D2D1::RoundedRect(lo.inlineCancelBtn.ToD2D(), 7.0f, 7.0f);
                    if (auto b = mk(RgbF(40, 40, 48))) m_d2dCtx->FillRoundedRectangle(cancelR, b.Get());
                    if (auto b = mk(RgbF(68, 68, 78))) m_d2dCtx->DrawRoundedRectangle(cancelR, b.Get(), 1.0f);
                    if (m_fmtSmallCenter)
                    {
                        if (auto b = mk(RgbF(214, 214, 226)))
                            m_d2dCtx->DrawTextW(L"Cancel", 6, m_fmtSmallCenter.Get(), lo.inlineCancelBtn.ToD2D(), b.Get());
                    }

                    const bool canCreate = !TrimWide(m_inlineField1).empty();
                    D2D1_COLOR_F crFill = accentCol;
                    crFill.a = canCreate ? 0.28f : 0.12f;
                    const D2D1_ROUNDED_RECT subR = D2D1::RoundedRect(lo.inlineSubmitBtn.ToD2D(), 7.0f, 7.0f);
                    if (auto b = mk(crFill)) m_d2dCtx->FillRoundedRectangle(subR, b.Get());
                    if (auto b = mk(canCreate ? accentCol : RgbF(68, 68, 78))) m_d2dCtx->DrawRoundedRectangle(subR, b.Get(), 1.2f);
                    if (m_fmtSmallCenter)
                    {
                        if (auto b = mk(canCreate ? RgbF(248, 250, 255) : RgbF(130, 130, 144)))
                            m_d2dCtx->DrawTextW(L"Create", 6, m_fmtSmallCenter.Get(), lo.inlineSubmitBtn.ToD2D(), b.Get());
                    }
                }
                else if (m_inlineForm == ProfileInlineForm::EditSlot)
                {
                    if (m_fmtCaption)
                    {
                        if (auto b = mk(RgbF(168, 168, 182)))
                        {
                            m_d2dCtx->DrawTextW(L"DISPLAY NAME", 12, m_fmtCaption.Get(), D2D1::RectF(28.0f, formTop + 4.0f, 348.0f, formTop + 16.0f), b.Get());
                            m_d2dCtx->DrawTextW(L"CUSTOM ICON", 11, m_fmtCaption.Get(), D2D1::RectF(28.0f, formTop + 48.0f, 348.0f, formTop + 60.0f), b.Get());
                        }
                    }
                    drawInlineInput(lo.editNameRect, m_inlineField1, L"Pin display label...", true);

                    // Icon status box + Browse + Reset
                    {
                        const D2D1_ROUNDED_RECT sr = D2D1::RoundedRect(lo.editIconStatusRect.ToD2D(), 6.0f, 6.0f);
                        if (auto b = mk(RgbF(24, 24, 30))) m_d2dCtx->FillRoundedRectangle(sr, b.Get());
                        if (auto b = mk(RgbF(56, 56, 66))) m_d2dCtx->DrawRoundedRectangle(sr, b.Get(), 1.0f);
                        const std::wstring iconName = FormatCustomIconDisplayName(m_editIconPath);
                        if (m_fmtBody)
                        {
                            if (auto b = mk(m_editIconPath.empty() ? RgbF(150, 150, 164) : RgbF(232, 232, 242)))
                                m_d2dCtx->DrawTextW(iconName.c_str(), static_cast<UINT32>(iconName.size()), m_fmtBody.Get(),
                                    D2D1::RectF(lo.editIconStatusRect.l + 9.0f, lo.editIconStatusRect.t,
                                        lo.editIconStatusRect.r - 8.0f, lo.editIconStatusRect.b), b.Get());
                        }

                        const D2D1_ROUNDED_RECT br = D2D1::RoundedRect(lo.editIconBrowseBtn.ToD2D(), 6.0f, 6.0f);
                        if (auto b = mk(RgbF(40, 46, 58))) m_d2dCtx->FillRoundedRectangle(br, b.Get());
                        if (auto b = mk(RgbF(70, 82, 102))) m_d2dCtx->DrawRoundedRectangle(br, b.Get(), 1.0f);
                        if (m_fmtSmallCenter)
                        {
                            if (auto b = mk(RgbF(224, 236, 255)))
                                m_d2dCtx->DrawTextW(L"Browse...", 9, m_fmtSmallCenter.Get(), lo.editIconBrowseBtn.ToD2D(), b.Get());
                        }

                        const bool hasCustomIcon = !m_editIconPath.empty();
                        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(lo.editIconResetBtn.ToD2D(), 6.0f, 6.0f);
                        if (auto b = mk(RgbF(38, 38, 46))) m_d2dCtx->FillRoundedRectangle(rr, b.Get());
                        if (auto b = mk(hasCustomIcon ? RgbF(76, 76, 88) : RgbF(52, 52, 60))) m_d2dCtx->DrawRoundedRectangle(rr, b.Get(), 1.0f);
                        if (m_fmtSmallCenter)
                        {
                            if (auto b = mk(hasCustomIcon ? RgbF(226, 226, 236) : RgbF(110, 110, 124)))
                                m_d2dCtx->DrawTextW(L"Reset", 5, m_fmtSmallCenter.Get(), lo.editIconResetBtn.ToD2D(), b.Get());
                        }
                    }

                    const bool canHide = (m_editSlotKind != SlotKind::Collection && m_editSlotKind != SlotKind::RecycleBin);
                    if (canHide)
                    {
                        if (m_fmtBody)
                        {
                            if (auto b = mk(RgbF(230, 230, 240)))
                                m_d2dCtx->DrawTextW(L"Hide from Ring (Search Only)", 28, m_fmtBody.Get(),
                                    D2D1::RectF(lo.editHideRow.l, lo.editHideRow.t, lo.editHideRow.r - 50.0f, lo.editHideRow.b), b.Get());
                        }
                        DrawTogglePill(lo.editHideRow.r - 40.0f, (lo.editHideRow.t + lo.editHideRow.b) * 0.5f - 10.0f, m_editHiddenFromRing, accentCol);
                    }

                    if (CanSlotToggleRunAsAdmin(m_editSlotKind, m_editSlotTarget))
                    {
                        if (m_fmtBody)
                        {
                            if (auto b = mk(RgbF(230, 230, 240)))
                                m_d2dCtx->DrawTextW(L"Run as Administrator", 20, m_fmtBody.Get(),
                                    D2D1::RectF(lo.editAdminRow.l, lo.editAdminRow.t, lo.editAdminRow.r - 50.0f, lo.editAdminRow.b), b.Get());
                        }
                        DrawTogglePill(lo.editAdminRow.r - 40.0f, (lo.editAdminRow.t + lo.editAdminRow.b) * 0.5f - 10.0f, m_inlineAdmin, accentCol);
                    }
                    else if (m_fmtGuideBody)
                    {
                        const wchar_t* note = canHide
                            ? L"Hidden pins stay searchable without taking a Ring slot."
                            : L"Changes apply immediately to this Ring item after Save.";
                        if (auto b = mk(RgbF(146, 146, 162)))
                            m_d2dCtx->DrawTextW(note, static_cast<UINT32>(wcslen(note)), m_fmtGuideBody.Get(),
                                lo.editAdminRow.ToD2D(), b.Get());
                    }

                    const D2D1_ROUNDED_RECT cancelR = D2D1::RoundedRect(lo.inlineCancelBtn.ToD2D(), 7.0f, 7.0f);
                    if (auto b = mk(RgbF(40, 40, 48))) m_d2dCtx->FillRoundedRectangle(cancelR, b.Get());
                    if (auto b = mk(RgbF(68, 68, 78))) m_d2dCtx->DrawRoundedRectangle(cancelR, b.Get(), 1.0f);
                    if (m_fmtSmallCenter)
                    {
                        if (auto b = mk(RgbF(214, 214, 226)))
                            m_d2dCtx->DrawTextW(L"Cancel", 6, m_fmtSmallCenter.Get(), lo.inlineCancelBtn.ToD2D(), b.Get());
                    }

                    const bool canSave = !TrimWide(m_inlineField1).empty();
                    D2D1_COLOR_F svFill = accentCol;
                    svFill.a = canSave ? 0.28f : 0.12f;
                    const D2D1_ROUNDED_RECT subR = D2D1::RoundedRect(lo.inlineSubmitBtn.ToD2D(), 7.0f, 7.0f);
                    if (auto b = mk(svFill)) m_d2dCtx->FillRoundedRectangle(subR, b.Get());
                    if (auto b = mk(canSave ? accentCol : RgbF(68, 68, 78))) m_d2dCtx->DrawRoundedRectangle(subR, b.Get(), 1.2f);
                    if (m_fmtSmallCenter)
                    {
                        if (auto b = mk(canSave ? RgbF(248, 250, 255) : RgbF(130, 130, 144)))
                            m_d2dCtx->DrawTextW(L"Save", 4, m_fmtSmallCenter.Get(), lo.inlineSubmitBtn.ToD2D(), b.Get());
                    }
                }
            }

            void RenderLookTab(const FlyoutLayout& lo, const SwitcherConfig& eff, D2D1_COLOR_F accentCol)
            {
                auto mk = [&](D2D1_COLOR_F c) {
                    ComPtr<ID2D1SolidColorBrush> br;
                    m_d2dCtx->CreateSolidColorBrush(c, br.GetAddressOf());
                    return br;
                };

                // 1. Surface Texture
                if (m_fmtCaption)
                {
                    if (auto b = mk(RgbF(144, 144, 156)))
                        m_d2dCtx->DrawTextW(L"TEXTURE", 7, m_fmtCaption.Get(), D2D1::RectF(18.0f, 96.0f, 200.0f, 114.0f), b.Get());
                }

                struct TexOpt { const wchar_t* title; const wchar_t* sub; SurfaceTexture tex; };
                const TexOpt texOpts[3] = {
                    { L"A4S Acrylic", L"Win11 WCA", SurfaceTexture::A4SAcrylic },
                    { L"Taskbar Glass", L"Host Blur", SurfaceTexture::TaskbarGlass },
                    { L"Matte Tint", L"Zero Blur", SurfaceTexture::PureMatte },
                };
                for (int i = 0; i < 3; ++i)
                {
                    const bool active = (m_config.surfaceTexture == texOpts[i].tex);
                    const HitRect& cr = lo.texCard[i];
                    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(cr.ToD2D(), 9.0f, 9.0f);
                    if (auto b = mk(active ? RgbF(38, 44, 54) : RgbF(32, 32, 38)))
                        m_d2dCtx->FillRoundedRectangle(rr, b.Get());
                    if (auto b = mk(active ? accentCol : RgbF(54, 54, 62)))
                        m_d2dCtx->DrawRoundedRectangle(rr, b.Get(), active ? 1.6f : 1.0f);

                    if (m_fmtCenter)
                    {
                        if (auto b = mk(active ? RgbF(248, 248, 252) : RgbF(216, 216, 224)))
                            m_d2dCtx->DrawTextW(texOpts[i].title, static_cast<UINT32>(wcslen(texOpts[i].title)), m_fmtCenter.Get(),
                                D2D1::RectF(cr.l + 4.0f, cr.t + 8.0f, cr.r - 4.0f, cr.t + 28.0f), b.Get());
                    }
                    if (m_fmtSmallCenter)
                    {
                        if (auto b = mk(RgbF(146, 146, 158)))
                            m_d2dCtx->DrawTextW(texOpts[i].sub, static_cast<UINT32>(wcslen(texOpts[i].sub)), m_fmtSmallCenter.Get(),
                                D2D1::RectF(cr.l + 4.0f, cr.t + 30.0f, cr.r - 4.0f, cr.b - 6.0f), b.Get());
                    }
                }

                // 2. Surface Color + Mode
                if (m_fmtCaption)
                {
                    if (auto b = mk(RgbF(144, 144, 156)))
                        m_d2dCtx->DrawTextW(L"SURFACE COLOR", 13, m_fmtCaption.Get(), D2D1::RectF(18.0f, 184.0f, 220.0f, 202.0f), b.Get());
                }

                const bool isDarkMode = !m_config.useCustomSurfaceColor && !m_config.autoLightDark && m_config.lightDarkMode < 50;
                const bool isLightMode = !m_config.useCustomSurfaceColor && !m_config.autoLightDark && m_config.lightDarkMode >= 50;
                const bool isAutoMode = !m_config.useCustomSurfaceColor && m_config.autoLightDark;
                const wchar_t* modeNames[3] = { L"Dark", L"Light", L"Auto System" };
                const bool modeActive[3] = { isDarkMode, isLightMode, isAutoMode };
                for (int i = 0; i < 3; ++i)
                {
                    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(lo.modeChip[i].ToD2D(), 7.0f, 7.0f);
                    if (auto b = mk(modeActive[i] ? RgbF(42, 48, 58) : RgbF(32, 32, 38)))
                        m_d2dCtx->FillRoundedRectangle(rr, b.Get());
                    if (auto b = mk(modeActive[i] ? accentCol : RgbF(54, 54, 62)))
                        m_d2dCtx->DrawRoundedRectangle(rr, b.Get(), modeActive[i] ? 1.4f : 1.0f);
                    if (m_fmtSmallCenter)
                    {
                        if (auto b = mk(modeActive[i] ? RgbF(248, 248, 252) : RgbF(186, 186, 198)))
                            m_d2dCtx->DrawTextW(modeNames[i], static_cast<UINT32>(wcslen(modeNames[i])), m_fmtSmallCenter.Get(),
                                lo.modeChip[i].ToD2D(), b.Get());
                    }
                }

                // Surface swatches + custom color picker [+]
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
                        else selected = m_config.useCustomSurfaceColor && _wcsicmp(m_config.surfaceColor.c_str(), sw.hex) == 0;

                        if (auto b = mk(RgbF(GetRValue(sw.rgb), GetGValue(sw.rgb), GetBValue(sw.rgb))))
                            m_d2dCtx->FillEllipse(D2D1::Ellipse(cc, 10.5f, 10.5f), b.Get());
                    }
                    else
                    {
                        bool matchesBuiltIn = false;
                        for (const auto& sw : kSurfaceSwatches)
                        {
                            if (_wcsicmp(m_config.surfaceColor.c_str(), sw.hex) == 0)
                                matchesBuiltIn = true;
                        }
                        selected = m_config.useCustomSurfaceColor && !matchesBuiltIn;
                        if (auto b = mk(selected ? HexToD2D(m_config.surfaceColor) : RgbF(46, 46, 54)))
                            m_d2dCtx->FillEllipse(D2D1::Ellipse(cc, 10.5f, 10.5f), b.Get());
                        if (auto b = mk(RgbF(228, 228, 236)))
                        {
                            m_d2dCtx->DrawLine(D2D1::Point2F(cc.x - 4.0f, cc.y), D2D1::Point2F(cc.x + 4.0f, cc.y), b.Get(), 1.4f);
                            m_d2dCtx->DrawLine(D2D1::Point2F(cc.x, cc.y - 4.0f), D2D1::Point2F(cc.x, cc.y + 4.0f), b.Get(), 1.4f);
                        }
                    }

                    if (selected)
                    {
                        if (auto b = mk(RgbF(248, 248, 252)))
                            m_d2dCtx->DrawEllipse(D2D1::Ellipse(cc, 12.8f, 12.8f), b.Get(), 1.6f);
                    }
                    else if (auto b = mk(RgbF(72, 72, 82)))
                    {
                        m_d2dCtx->DrawEllipse(D2D1::Ellipse(cc, 10.5f, 10.5f), b.Get(), 1.0f);
                    }
                }

                // 3. Glow & Highlight Accent
                if (m_fmtCaption)
                {
                    if (auto b = mk(RgbF(144, 144, 156)))
                        m_d2dCtx->DrawTextW(L"GLOW & HIGHLIGHT", 16, m_fmtCaption.Get(), D2D1::RectF(18.0f, 276.0f, 220.0f, 294.0f), b.Get());
                }

                {
                    const D2D1_ROUNDED_RECT autoR = D2D1::RoundedRect(lo.accentAutoChip.ToD2D(), 7.0f, 7.0f);
                    if (auto b = mk(m_config.autoWindowsAccent ? RgbF(42, 48, 58) : RgbF(32, 32, 38)))
                        m_d2dCtx->FillRoundedRectangle(autoR, b.Get());
                    if (auto b = mk(m_config.autoWindowsAccent ? accentCol : RgbF(54, 54, 62)))
                        m_d2dCtx->DrawRoundedRectangle(autoR, b.Get(), m_config.autoWindowsAccent ? 1.4f : 1.0f);
                    if (m_fmtSmallCenter)
                    {
                        if (auto b = mk(m_config.autoWindowsAccent ? RgbF(248, 248, 252) : RgbF(186, 186, 198)))
                            m_d2dCtx->DrawTextW(L"Auto", 4, m_fmtSmallCenter.Get(), lo.accentAutoChip.ToD2D(), b.Get());
                    }

                    for (int i = 0; i < 7; ++i)
                    {
                        const HitRect& ar = lo.accentDot[i];
                        const D2D1_POINT_2F cc{ (ar.l + ar.r) * 0.5f, (ar.t + ar.b) * 0.5f };
                        bool selected = false;
                        if (i < 6)
                        {
                            const auto& aw = kAccentSwatches[i];
                            selected = !m_config.autoWindowsAccent && _wcsicmp(m_config.accentColor.c_str(), aw.hex) == 0;
                            if (auto b = mk(RgbF(GetRValue(aw.rgb), GetGValue(aw.rgb), GetBValue(aw.rgb))))
                                m_d2dCtx->FillEllipse(D2D1::Ellipse(cc, 9.5f, 9.5f), b.Get());
                        }
                        else
                        {
                            bool matchesBuiltIn = false;
                            for (const auto& aw : kAccentSwatches)
                            {
                                if (_wcsicmp(m_config.accentColor.c_str(), aw.hex) == 0)
                                    matchesBuiltIn = true;
                            }
                            selected = !m_config.autoWindowsAccent && !matchesBuiltIn;
                            if (auto b = mk(selected ? HexToD2D(m_config.accentColor) : RgbF(46, 46, 54)))
                                m_d2dCtx->FillEllipse(D2D1::Ellipse(cc, 9.5f, 9.5f), b.Get());
                            if (auto b = mk(RgbF(228, 228, 236)))
                            {
                                m_d2dCtx->DrawLine(D2D1::Point2F(cc.x - 3.8f, cc.y), D2D1::Point2F(cc.x + 3.8f, cc.y), b.Get(), 1.3f);
                                m_d2dCtx->DrawLine(D2D1::Point2F(cc.x, cc.y - 3.8f), D2D1::Point2F(cc.x, cc.y + 3.8f), b.Get(), 1.3f);
                            }
                        }

                        if (selected)
                        {
                            if (auto b = mk(RgbF(248, 248, 252)))
                                m_d2dCtx->DrawEllipse(D2D1::Ellipse(cc, 11.8f, 11.8f), b.Get(), 1.5f);
                        }
                    }
                }

                // 4. Tune Sliders
                if (m_fmtCaption)
                {
                    if (auto b = mk(RgbF(144, 144, 156)))
                        m_d2dCtx->DrawTextW(L"TUNE", 4, m_fmtCaption.Get(), D2D1::RectF(18.0f, 344.0f, 200.0f, 362.0f), b.Get());
                }

                const D2D1_ROUNDED_RECT tuneCard = D2D1::RoundedRect(D2D1::RectF(16.0f, 364.0f, 360.0f, 524.0f), 10.0f, 10.0f);
                if (auto b = mk(RgbF(32, 32, 38))) m_d2dCtx->FillRoundedRectangle(tuneCard, b.Get());
                if (auto b = mk(RgbF(52, 52, 60))) m_d2dCtx->DrawRoundedRectangle(tuneCard, b.Get(), 1.0f);

                const int sizePct = std::clamp(static_cast<int>(std::lround((m_config.ringRadiusDip - 90.0) / 76.0 * 100.0)), 0, 100);
                const bool brightDisabled = (m_config.autoLightDark && !m_config.useCustomSurfaceColor);
                struct SliderSpec { const wchar_t* label; int val; bool disabled; };
                const SliderSpec sliders[3] = {
                    { L"Glass / Opacity", m_config.glassLevel, false },
                    { L"Brightness", eff.lightDarkMode, brightDisabled },
                    { L"Ring Size", sizePct, false }
                };

                for (int i = 0; i < 3; ++i)
                {
                    const HitRect& tr = lo.sliderTrack[i];
                    const float topY = tr.t - 16.0f;
                    const float midY = (tr.t + tr.b) * 0.5f;
                    const float frac = static_cast<float>(std::clamp(sliders[i].val, 0, 100)) / 100.0f;
                    const float kx = tr.l + (tr.r - tr.l) * frac;

                    if (m_fmtBody)
                    {
                        if (auto b = mk(RgbF(236, 236, 242, sliders[i].disabled ? 0.45f : 1.0f)))
                            m_d2dCtx->DrawTextW(sliders[i].label, static_cast<UINT32>(wcslen(sliders[i].label)), m_fmtBody.Get(),
                                D2D1::RectF(tr.l, topY, tr.r - 48.0f, topY + 18.0f), b.Get());
                    }
                    wchar_t pctTxt[16]{};
                    swprintf_s(pctTxt, L"%d%%", sliders[i].val);
                    if (m_fmtCaption)
                    {
                        if (auto b = mk(RgbF(168, 168, 180, sliders[i].disabled ? 0.45f : 1.0f)))
                            m_d2dCtx->DrawTextW(pctTxt, static_cast<UINT32>(wcslen(pctTxt)), m_fmtCaption.Get(),
                                D2D1::RectF(tr.r - 44.0f, topY, tr.r, topY + 18.0f), b.Get());
                    }

                    if (auto b = mk(RgbF(60, 60, 68, sliders[i].disabled ? 0.45f : 1.0f)))
                        m_d2dCtx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tr.l, midY - 2.0f, tr.r, midY + 2.0f), 2.0f, 2.0f), b.Get());
                    D2D1_COLOR_F fillC = accentCol;
                    if (sliders[i].disabled) fillC.a *= 0.45f;
                    if (auto b = mk(fillC))
                        m_d2dCtx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tr.l, midY - 2.0f, kx, midY + 2.0f), 2.0f, 2.0f), b.Get());
                    if (auto b = mk(RgbF(248, 248, 252, sliders[i].disabled ? 0.45f : 1.0f)))
                        m_d2dCtx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, midY), 6.8f, 6.8f), b.Get());
                }
            }

            void ApplySliderAtX(int sliderIdx, float xDip)
            {
                const FlyoutLayout lo = ComputeLayout();
                const HitRect& tr = lo.sliderTrack[sliderIdx];
                const float frac = std::clamp((xDip - tr.l) / (tr.r - tr.l), 0.0f, 1.0f);
                const int pct = static_cast<int>(std::lround(frac * 100.0f));

                if (sliderIdx == 0)
                {
                    if (m_config.glassLevel != pct)
                    {
                        m_config.glassLevel = pct;
                        CommitLookDelta();
                    }
                }
                else if (sliderIdx == 1)
                {
                    if (m_config.autoLightDark && !m_config.useCustomSurfaceColor)
                        m_config.autoLightDark = false;
                    if (m_config.lightDarkMode != pct)
                    {
                        m_config.lightDarkMode = pct;
                        CommitLookDelta();
                    }
                }
                else if (sliderIdx == 2)
                {
                    const double nextRadius = 90.0 + static_cast<double>(pct) / 100.0 * 76.0;
                    const double nextIcon = 34.0 + static_cast<double>(pct) / 100.0 * 22.0;
                    if (std::abs(m_config.ringRadiusDip - nextRadius) > 0.25)
                    {
                        m_config.ringRadiusDip = nextRadius;
                        m_config.iconSizeDip = nextIcon;
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
                cc.hwndOwner = m_flyoutHwnd;
                cc.rgbResult = initialColor;
                cc.lpCustColors = s_customColors.data();
                cc.Flags = CC_FULLOPEN | CC_RGBINIT;

                m_modalOpen = true;
                const BOOL ok = ChooseColorW(&cc);
                m_modalOpen = false;
                if (m_flyoutHwnd && IsWindow(m_flyoutHwnd))
                    SetForegroundWindow(m_flyoutHwnd);
                if (!ok)
                    return false;
                outHex = ColorRefToHex(cc.rgbResult);
                return true;
            }

            void HandleFlyoutClick(float xDip, float yDip)
            {
                const FlyoutLayout lo = ComputeLayout();

                if (lo.closeBtn.Contains(xDip, yDip))
                {
                    CloseFlyout();
                    return;
                }
                if (lo.statusPill.Contains(xDip, yDip))
                {
                    if (m_engineMode == EngineMode::Stopped)
                    {
                        LaunchSiblingHost();
                        SetTimer(m_flyoutHwnd, kHostPollTimerId, 250, nullptr);
                        m_engineMode = EngineMode::Starting;
                        InvalidateFlyout();
                    }
                    else if (m_engineMode == EngineMode::SafetyLocked)
                    {
                        SendHostControl(WINRINGS_HOST_CTRL_RESET_SAFETY, 0);
                        SetTimer(m_flyoutHwnd, kHostPollTimerId, 250, nullptr);
                        m_engineMode = QueryEngineState();
                        InvalidateFlyout();
                    }
                    return;
                }
                if ((m_engineMode == EngineMode::Active || m_engineMode == EngineMode::Paused) &&
                    lo.pauseSwitch.Contains(xDip, yDip))
                {
                    const bool wantPause = (m_engineMode == EngineMode::Active);
                    SendHostControl(WINRINGS_HOST_CTRL_SET_PAUSED, wantPause ? 1 : 0);
                    m_engineMode = wantPause ? EngineMode::Paused : EngineMode::Active;
                    if (wantPause)
                        m_livePreviewSent = false;
                    else if (m_activeTab == 1 || m_activeTab == 2)
                        SyncLiveRingPreview(true);
                    InvalidateFlyout();
                    return;
                }

                // Tabs
                if (lo.tabTriggers.Contains(xDip, yDip) && m_activeTab != 0)
                {
                    CancelCapture();
                    CloseInlineForm();
                    m_activeTab = 0;
                    SyncLiveRingPreview(false);
                    InvalidateFlyout();
                    return;
                }
                if (lo.tabProfiles.Contains(xDip, yDip) && m_activeTab != 1)
                {
                    CancelCapture();
                    CloseInlineForm();
                    m_activeTab = 1;
                    SyncLiveRingPreview(true);
                    InvalidateFlyout();
                    return;
                }
                if (lo.tabLook.Contains(xDip, yDip) && m_activeTab != 2)
                {
                    CancelCapture();
                    CloseInlineForm();
                    m_activeTab = 2;
                    SyncLiveRingPreview(true);
                    InvalidateFlyout();
                    return;
                }

                // Header drag
                if (yDip < 50.0f)
                {
                    m_draggingWindow = true;
                    GetCursorPos(&m_dragStartCursor);
                    RECT rc{};
                    GetWindowRect(m_flyoutHwnd, &rc);
                    m_dragStartWin = POINT{ rc.left, rc.top };
                    SetCapture(m_flyoutHwnd);
                    return;
                }

                if (m_activeTab == 0)
                {
                    if (lo.addTriggerBtn.Contains(xDip, yDip))
                    {
                        CancelCapture();
                        if (m_config.triggers.size() < kMaxTriggerBindings)
                        {
                            m_config.triggers.push_back(TriggerBinding{ TriggerKind::Disabled, MouseTriggerButton::Disabled, 0, 0, L"default" });
                            const float viewH = kTriggerViewportBot - kTriggerViewportTop;
                            m_triggerScrollDip = (std::max)(0.0f, ComputeTriggersContentHeight() - viewH);
                            StartCapture(static_cast<int>(m_config.triggers.size()) - 1);
                        }
                        return;
                    }
                    if (yDip >= kTriggerViewportTop && yDip <= kTriggerViewportBot)
                    {
                        const size_t tCount = (std::min)(m_config.triggers.size(), kMaxTriggerBindings);
                        for (size_t i = 0; i < tCount; ++i)
                        {
                            if (lo.triggerRemove[i].Contains(xDip, yDip))
                            {
                                StopCapture();
                                m_config.triggers.erase(m_config.triggers.begin() + static_cast<ptrdiff_t>(i));
                                PruneDisabledPlaceholders();
                                ClampTriggerScroll();
                                CommitTriggersDelta();
                                return;
                            }
                            if (lo.triggerProfileBadge[i].Contains(xDip, yDip) &&
                                m_recordingIndex != static_cast<int>(i))
                            {
                                CancelCapture();
                                HMENU pMenu = CreatePopupMenu();
                                if (pMenu)
                                {
                                    for (size_t pi = 0; pi < m_config.profiles.size(); ++pi)
                                    {
                                        const auto& p = m_config.profiles[pi];
                                        const std::wstring curId = m_config.triggers[i].profileId.empty() ? L"default" : m_config.triggers[i].profileId;
                                        const UINT flags = MF_STRING | ((_wcsicmp(p.id.c_str(), curId.c_str()) == 0) ? MF_CHECKED : 0u);
                                        AppendMenuW(pMenu, flags, 5000u + static_cast<UINT>(pi), p.name.c_str());
                                    }
                                    POINT menuPt{
                                        static_cast<LONG>(std::lround(lo.triggerProfileBadge[i].l * m_dpiScale)),
                                        static_cast<LONG>(std::lround(lo.triggerProfileBadge[i].b * m_dpiScale))
                                    };
                                    ClientToScreen(m_flyoutHwnd, &menuPt);
                                    m_modalOpen = true;
                                    const UINT picked = TrackPopupMenuEx(pMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                        menuPt.x, menuPt.y, m_flyoutHwnd, nullptr);
                                    DestroyMenu(pMenu);
                                    m_modalOpen = false;
                                    if (picked >= 5000u)
                                    {
                                        const size_t pIdx = static_cast<size_t>(picked - 5000u);
                                        if (pIdx < m_config.profiles.size())
                                        {
                                            m_config.triggers[i].profileId = m_config.profiles[pIdx].id;
                                            CommitTriggersDelta();
                                        }
                                    }
                                }
                                return;
                            }
                            if (lo.triggerPill[i].Contains(xDip, yDip))
                            {
                                if (m_recordingIndex == static_cast<int>(i))
                                {
                                    CancelCapture();
                                    InvalidateFlyout();
                                }
                                else
                                {
                                    if (m_recordingIndex >= 0 &&
                                        m_recordingIndex < static_cast<int>(m_config.triggers.size()) &&
                                        !m_config.triggers[static_cast<size_t>(m_recordingIndex)].IsActive())
                                    {
                                        const size_t remIdx = static_cast<size_t>(m_recordingIndex);
                                        StopCapture();
                                        m_config.triggers.erase(m_config.triggers.begin() + static_cast<ptrdiff_t>(remIdx));
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
                            if (i == 0) m_config.gestureTapOpenClose = !m_config.gestureTapOpenClose;
                            else if (i == 1) m_config.gestureHoldReleaseSwitch = !m_config.gestureHoldReleaseSwitch;
                            else if (i == 2) m_config.doubleTapLastApp = !m_config.doubleTapLastApp;
                            else if (i == 3) m_config.pointerFollow = !m_config.pointerFollow;
                            CommitGesturesDelta();
                            return;
                        }
                    }
                }
                else if (m_activeTab == 1)
                {
                    const size_t chipCount = (std::min)(lo.profileChip.size(), lo.profileChipIndex.size());
                    for (size_t i = 0; i < chipCount; ++i)
                    {
                        if (lo.profileChip[i].Contains(xDip, yDip))
                        {
                            CloseInlineForm();
                            m_selectedProfileId = m_config.profiles[lo.profileChipIndex[i]].id;
                            m_profilePinsScrollDip = 0.0f;
                            SyncLiveRingPreview(true);
                            InvalidateFlyout();
                            return;
                        }
                    }

                    if (lo.profileNewBtn.Contains(xDip, yDip))
                    {
                        if (m_inlineForm == ProfileInlineForm::NewProfile)
                            CloseInlineForm();
                        else
                            OpenInlineForm(ProfileInlineForm::NewProfile);
                        return;
                    }

                    RingProfileConfig* curProf = m_config.FindProfile(m_selectedProfileId);
                    if (!curProf && !m_config.profiles.empty())
                        curProf = &m_config.profiles.front();

                    if (curProf && lo.profileDelOrResetBtn.Contains(xDip, yDip))
                    {
                        CloseInlineForm();
                        if (_wcsicmp(curProf->id.c_str(), L"SMenu") == 0)
                        {
                            *curProf = SwitcherConfig::CreateBuiltInSMenuProfile();
                            CommitProfileEdit();
                        }
                        else if (_wcsicmp(curProf->id.c_str(), L"default") != 0)
                        {
                            const std::wstring removedId = curProf->id;
                            m_config.profiles.erase(
                                std::remove_if(m_config.profiles.begin(), m_config.profiles.end(),
                                    [&](const RingProfileConfig& p) { return _wcsicmp(p.id.c_str(), removedId.c_str()) == 0; }),
                                m_config.profiles.end());
                            SwitcherConfig::DeleteProfileFile(removedId);
                            bool trigChanged = false;
                            for (auto& t : m_config.triggers)
                            {
                                if (_wcsicmp(t.profileId.c_str(), removedId.c_str()) == 0)
                                {
                                    t.profileId = L"default";
                                    trigChanged = true;
                                }
                            }
                            if (trigChanged)
                            {
                                m_config.SyncLegacyTriggerMirrors();
                                SendDeltaPacket(MakeTriggersDelta(m_config));
                            }
                            m_selectedProfileId = L"default";
                            CommitProfileEdit();
                        }
                        return;
                    }

                    if (!curProf)
                        return;

                    if (lo.openOnRingBtn.Contains(xDip, yDip))
                    {
                        CloseInlineForm();
                        if (m_dirty)
                        {
                            m_config.PullFromDefaultProfileMirror();
                            m_config.Save();
                            m_dirty = false;
                            SendDeltaPacket(MakeReloadFromDiskDelta(curProf->id));
                        }
                        const POINT anchor = ComputeSideBySideRingAnchor();
                        SendDeltaPacket(MakeLookDelta(m_config));
                        if (SendDeltaPacket(MakeOpenProfileForEditDelta(anchor.x, anchor.y, curProf->id)))
                        {
                            m_livePreviewSent = false;
                            CloseFlyout();
                        }
                        return;
                    }

                    if (lo.openConfigBtn.Contains(xDip, yDip))
                    {
                        m_config.SaveProfile(curProf->id);
                        const std::wstring cfgFile = SwitcherConfig::ProfileFilePath(curProf->id);
                        const std::wstring params = L"/select,\"" + cfgFile + L"\"";
                        ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
                        return;
                    }

                    if (lo.openProfilesFolderBtn.Contains(xDip, yDip))
                    {
                        m_config.Save();
                        const std::wstring dir = SwitcherConfig::ProfilesDirectory();
                        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        return;
                    }

                    if (lo.profileRunningRow.Contains(xDip, yDip))
                    {
                        curProf->includeRunningApps = !curProf->includeRunningApps;
                        CommitProfileEdit();
                        return;
                    }

                    if (lo.addAppBtn.Contains(xDip, yDip) || lo.addFolderBtn.Contains(xDip, yDip))
                    {
                        CloseInlineForm();
                        const bool folderOnly = lo.addFolderBtn.Contains(xDip, yDip);
                        std::wstring pickedPath;
                        m_modalOpen = true;
                        const bool ok = PickFileOrFolderDialog(m_flyoutHwnd, folderOnly, pickedPath);
                        m_modalOpen = false;
                        if (ok && !pickedPath.empty())
                        {
                            curProf->pinnedSlots.push_back(BuildPickedSlotConfig(folderOnly, pickedPath));
                            CommitProfileEdit();
                        }
                        return;
                    }

                    if (lo.addLinkBtn.Contains(xDip, yDip))
                    {
                        if (m_inlineForm == ProfileInlineForm::AddLinkCmd)
                            CloseInlineForm();
                        else
                            OpenInlineForm(ProfileInlineForm::AddLinkCmd);
                        return;
                    }

                    if (m_inlineForm != ProfileInlineForm::None)
                    {
                        HandleInlineFormClick(lo, xDip, yDip);
                        return;
                    }
                    if (yDip >= kProfilePinsViewportTop && yDip <= kProfilePinsViewportBot)
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
                else
                {
                    const SurfaceTexture texValues[3] = {
                        SurfaceTexture::A4SAcrylic, SurfaceTexture::TaskbarGlass, SurfaceTexture::PureMatte
                    };
                    for (int i = 0; i < 3; ++i)
                    {
                        if (lo.texCard[i].Contains(xDip, yDip))
                        {
                            m_config.surfaceTexture = texValues[i];
                            CommitLookDelta();
                            return;
                        }
                    }
                    for (int i = 0; i < 3; ++i)
                    {
                        if (lo.modeChip[i].Contains(xDip, yDip))
                        {
                            m_config.useCustomSurfaceColor = false;
                            if (i == 0) { m_config.autoLightDark = false; m_config.lightDarkMode = 0; }
                            else if (i == 1) { m_config.autoLightDark = false; m_config.lightDarkMode = 100; }
                            else { m_config.autoLightDark = true; }
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
                                    m_config.useCustomSurfaceColor = false;
                                    m_config.autoLightDark = false;
                                    m_config.lightDarkMode = 0;
                                }
                                else if (sw.isNeutralLight)
                                {
                                    m_config.useCustomSurfaceColor = false;
                                    m_config.autoLightDark = false;
                                    m_config.lightDarkMode = 100;
                                }
                                else
                                {
                                    m_config.useCustomSurfaceColor = true;
                                    m_config.autoLightDark = false;
                                    m_config.surfaceColor = sw.hex;
                                    m_config.lightDarkMode = 0;
                                }
                                CommitLookDelta();
                            }
                            else
                            {
                                std::wstring picked;
                                if (PickNativeColor(HexToColorRef(m_config.surfaceColor), picked))
                                {
                                    m_config.useCustomSurfaceColor = true;
                                    m_config.autoLightDark = false;
                                    m_config.surfaceColor = picked;
                                    m_config.lightDarkMode = 0;
                                    CommitLookDelta();
                                }
                            }
                            return;
                        }
                    }
                    if (lo.accentAutoChip.Contains(xDip, yDip))
                    {
                        m_config.autoWindowsAccent = !m_config.autoWindowsAccent;
                        CommitLookDelta();
                        return;
                    }
                    for (int i = 0; i < 7; ++i)
                    {
                        if (lo.accentDot[i].Contains(xDip, yDip))
                        {
                            if (i < 6)
                            {
                                m_config.autoWindowsAccent = false;
                                m_config.accentColor = kAccentSwatches[i].hex;
                                CommitLookDelta();
                            }
                            else
                            {
                                std::wstring picked;
                                if (PickNativeColor(HexToColorRef(m_config.accentColor), picked))
                                {
                                    m_config.autoWindowsAccent = false;
                                    m_config.accentColor = picked;
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
                            m_dragSlider = i;
                            SetCapture(m_flyoutHwnd);
                            ApplySliderAtX(i, xDip);
                            return;
                        }
                    }
                }
            }

            LRESULT HandleTrayMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
            {
                if (msg == m_wakeMessage)
                {
                    if (wp == kWakeQuitTray)
                    {
                        CloseFlyout();
                        PostQuitMessage(0);
                    }
                    else
                    {
                        OpenFlyout();
                    }
                    return 0;
                }
                if (msg == m_taskbarCreatedMessage)
                {
                    m_trayAdded = false;
                    AddTrayIcon();
                    return 0;
                }
                if (msg == kTrayCallbackMsg)
                {
                    if (lp == WM_LBUTTONUP)
                    {
                        if (m_flyoutHwnd && IsWindow(m_flyoutHwnd))
                            CloseFlyout();
                        else
                            OpenFlyout();
                    }
                    else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU)
                    {
                        ShowTrayMenu();
                    }
                    return 0;
                }
                if (msg == WM_TIMER && wp == kTrayRetryTimerId)
                {
                    AddTrayIcon();
                    return 0;
                }
                if (msg == WM_TIMER && wp == kTrayPromoteTimerId)
                {
                    EnsureTrayIconPromoted();
                    if (++m_trayPromoteTicks >= 4)
                        KillTimer(hwnd, kTrayPromoteTimerId);
                    return 0;
                }
                if (msg == WM_QUERYENDSESSION)
                    return TRUE;
                if (msg == WM_ENDSESSION && wp)
                {
                    CloseFlyout();
                    PostQuitMessage(0);
                    return 0;
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            LRESULT HandleFlyoutMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
            {
                switch (msg)
                {
                case WM_ERASEBKGND:
                    return 1;

                case WM_PAINT:
                {
                    PAINTSTRUCT ps{};
                    BeginPaint(hwnd, &ps);
                    RenderFlyout();
                    EndPaint(hwnd, &ps);
                    return 0;
                }

                case WM_DPICHANGED:
                {
                    const UINT newDpi = HIWORD(wp);
                    if (newDpi > 0)
                        m_dpiScale = static_cast<float>(newDpi) / 96.0f;
                    const RECT* suggested = reinterpret_cast<const RECT*>(lp);
                    const int wPx = static_cast<int>(std::lround(kFlyoutW * m_dpiScale));
                    const int hPx = static_cast<int>(std::lround(kFlyoutH * m_dpiScale));
                    if (suggested)
                        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, wPx, hPx, SWP_NOZORDER | SWP_NOACTIVATE);
                    ResizeFlyoutSwapChain(wPx, hPx);
                    InvalidateFlyout();
                    return 0;
                }

                case WM_TIMER:
                    if (wp == kHostPollTimerId)
                    {
                        const EngineMode next = QueryEngineState();
                        if (next != m_engineMode)
                        {
                            m_engineMode = next;
                            if (next == EngineMode::Active)
                            {
                                KillTimer(hwnd, kHostPollTimerId);
                                if (m_dirty)
                                {
                                    SendDeltaPacket(MakeTriggersDelta(m_config));
                                    SendDeltaPacket(MakeGesturesDelta(m_config));
                                    SendDeltaPacket(MakeLookDelta(m_config));
                                }
                                if (m_activeTab == 1 || m_activeTab == 2)
                                    SyncLiveRingPreview(true);
                            }
                            InvalidateFlyout();
                        }
                        return 0;
                    }
                    if (wp == kCaretBlinkTimerId)
                    {
                        if (m_activeTab == 1 && m_inlineForm != ProfileInlineForm::None)
                        {
                            m_inlineCaretOn = !m_inlineCaretOn;
                            InvalidateFlyout();
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
                    const float xDip = static_cast<float>(GET_X_LPARAM(lp)) / m_dpiScale;
                    const float yDip = static_cast<float>(GET_Y_LPARAM(lp)) / m_dpiScale;
                    HandleFlyoutClick(xDip, yDip);
                    return 0;
                }

                case WM_MOUSEMOVE:
                {
                    const float xDip = static_cast<float>(GET_X_LPARAM(lp)) / m_dpiScale;
                    const float yDip = static_cast<float>(GET_Y_LPARAM(lp)) / m_dpiScale;
                    if (m_draggingWindow)
                    {
                        POINT cur{};
                        GetCursorPos(&cur);
                        SetWindowPos(hwnd, nullptr,
                            m_dragStartWin.x + (cur.x - m_dragStartCursor.x),
                            m_dragStartWin.y + (cur.y - m_dragStartCursor.y),
                            0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                        return 0;
                    }
                    if (m_dragSlider >= 0)
                    {
                        ApplySliderAtX(m_dragSlider, xDip);
                        return 0;
                    }
                    const FlyoutLayout lo = ComputeLayout();
                    const int nextHover = lo.closeBtn.Contains(xDip, yDip) ? 99 : -1;
                    if (nextHover != m_hoverButton)
                    {
                        m_hoverButton = nextHover;
                        InvalidateFlyout();
                    }
                    return 0;
                }

                case WM_LBUTTONUP:
                    if (m_draggingWindow || m_dragSlider >= 0)
                    {
                        m_draggingWindow = false;
                        m_dragSlider = -1;
                        if (GetCapture() == hwnd)
                            ReleaseCapture();
                    }
                    return 0;

                case WM_MOUSEWHEEL:
                    if (m_activeTab == 0)
                    {
                        const short wheelDelta = GET_WHEEL_DELTA_WPARAM(wp);
                        if (wheelDelta != 0)
                        {
                            const float maxScroll = (std::max)(0.0f, ComputeTriggersContentHeight() - (kTriggerViewportBot - kTriggerViewportTop));
                            const float nextScroll = std::clamp(
                                m_triggerScrollDip - (static_cast<float>(wheelDelta) / 120.0f) * 32.0f,
                                0.0f, maxScroll);
                            if (maxScroll > 0.0f && std::abs(nextScroll - m_triggerScrollDip) > 0.25f)
                            {
                                m_triggerScrollDip = nextScroll;
                                InvalidateFlyout();
                            }
                        }
                        return 0;
                    }
                    if (m_activeTab == 1 && m_inlineForm == ProfileInlineForm::None)
                    {
                        const short wheelDelta = GET_WHEEL_DELTA_WPARAM(wp);
                        if (wheelDelta != 0)
                        {
                            const float maxScroll = (std::max)(0.0f, ComputeProfilePinsContentHeight() - (kProfilePinsViewportBot - kProfilePinsViewportTop));
                            const float nextScroll = std::clamp(
                                m_profilePinsScrollDip - (static_cast<float>(wheelDelta) / 120.0f) * 32.0f,
                                0.0f, maxScroll);
                            if (maxScroll > 0.0f && std::abs(nextScroll - m_profilePinsScrollDip) > 0.25f)
                            {
                                m_profilePinsScrollDip = nextScroll;
                                InvalidateFlyout();
                            }
                        }
                        return 0;
                    }
                    break;

                case WM_KEYDOWN:
                    if (m_activeTab == 1 && m_inlineForm != ProfileInlineForm::None)
                    {
                        std::wstring& activeStr = (m_inlineFocusField == 1 && m_inlineForm == ProfileInlineForm::AddLinkCmd)
                            ? m_inlineField2 : m_inlineField1;
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
                        if (wp == VK_TAB && m_inlineForm == ProfileInlineForm::AddLinkCmd)
                        {
                            m_inlineFocusField = 1 - m_inlineFocusField;
                            m_inlineCaretOn = true;
                            InvalidateFlyout();
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
                                m_inlineCaretOn = true;
                                InvalidateFlyout();
                            }
                            return 0;
                        }
                        if (ctrlHeld && wp == VK_BACK)
                        {
                            activeStr.clear();
                            m_inlineCaretOn = true;
                            InvalidateFlyout();
                            return 0;
                        }
                        return 0;
                    }
                    if (wp == VK_ESCAPE)
                    {
                        if (m_recordingIndex >= 0)
                        {
                            CancelCapture();
                            InvalidateFlyout();
                        }
                        else
                        {
                            CloseFlyout();
                        }
                        return 0;
                    }
                    break;

                case WM_CHAR:
                    if (m_activeTab == 1 && m_inlineForm != ProfileInlineForm::None)
                    {
                        std::wstring& activeStr = (m_inlineFocusField == 1 && m_inlineForm == ProfileInlineForm::AddLinkCmd)
                            ? m_inlineField2 : m_inlineField1;
                        const wchar_t ch = static_cast<wchar_t>(wp);
                        if (ch == L'\b')
                        {
                            if (!activeStr.empty())
                            {
                                activeStr.pop_back();
                                m_inlineCaretOn = true;
                                InvalidateFlyout();
                            }
                            return 0;
                        }
                        if (ch >= 0x20 && ch != 0x7F && activeStr.size() < 512)
                        {
                            activeStr.push_back(ch);
                            m_inlineCaretOn = true;
                            InvalidateFlyout();
                        }
                        return 0;
                    }
                    break;

                case WM_ACTIVATE:
                    if (LOWORD(wp) == WA_INACTIVE && !m_modalOpen && m_recordingIndex < 0)
                    {
                        HWND fg = GetForegroundWindow();
                        wchar_t cls[64]{};
                        if (fg && GetClassNameW(fg, cls, _countof(cls)) > 0 &&
                            (wcscmp(cls, WINRINGS_OVERLAY_CLASS_W) == 0 ||
                             wcscmp(cls, L"WinRings.ContextFlyout.v1") == 0))
                        {
                            return 0;
                        }
                        CloseFlyout();
                        return 0;
                    }
                    break;

                case WM_CLOSE:
                    CloseFlyout();
                    return 0;
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            static TrayControlApp* s_instance;

            HINSTANCE m_instance = nullptr;
            HWND m_trayHwnd = nullptr;
            HWND m_flyoutHwnd = nullptr;
            HICON m_trayIcon = nullptr;
            bool m_trayAdded = false;
            unsigned m_trayRetryCount = 0;
            unsigned m_trayPromoteTicks = 0;
            float m_triggerScrollDip = 0.0f;
            float m_profilePinsScrollDip = 0.0f;
            std::wstring m_selectedProfileId = L"default";

            ProfileInlineForm m_inlineForm = ProfileInlineForm::None;
            int m_inlineFocusField = 0;
            std::wstring m_inlineField1;
            std::wstring m_inlineField2;
            bool m_inlineAdmin = false;
            bool m_inlineCaretOn = true;
            std::wstring m_editSlotId;
            bool m_editHiddenFromRing = false;
            std::wstring m_editIconPath;
            SlotKind m_editSlotKind = SlotKind::App;
            std::wstring m_editSlotTarget;

            void OpenInlineForm(ProfileInlineForm form)
            {
                m_inlineForm = form;
                m_inlineFocusField = 0;
                m_inlineField1.clear();
                m_inlineField2.clear();
                m_inlineAdmin = false;
                m_editSlotId.clear();
                m_editHiddenFromRing = false;
                m_editIconPath.clear();
                m_inlineCaretOn = true;
                if (m_flyoutHwnd)
                    SetTimer(m_flyoutHwnd, kCaretBlinkTimerId, 530, nullptr);
                InvalidateFlyout();
            }

            void OpenEditSlotForm(const PinnedSlotConfig& slot)
            {
                m_inlineForm = ProfileInlineForm::EditSlot;
                m_inlineFocusField = 0;
                m_inlineField1 = slot.label.empty() ? InferProfileItemLabel(slot.kind, slot.pathOrUrl) : slot.label;
                m_inlineField2.clear();
                m_inlineAdmin = slot.runAsAdmin;
                m_editSlotId = slot.id;
                m_editHiddenFromRing = slot.hiddenFromRing;
                m_editIconPath = slot.iconCacheFile;
                m_editSlotKind = slot.kind;
                m_editSlotTarget = slot.pathOrUrl;
                m_inlineCaretOn = true;
                if (m_flyoutHwnd)
                    SetTimer(m_flyoutHwnd, kCaretBlinkTimerId, 530, nullptr);
                InvalidateFlyout();
            }

            void CloseInlineForm()
            {
                if (m_inlineForm == ProfileInlineForm::None)
                    return;
                m_inlineForm = ProfileInlineForm::None;
                m_inlineField1.clear();
                m_inlineField2.clear();
                m_inlineAdmin = false;
                m_editSlotId.clear();
                m_editHiddenFromRing = false;
                m_editIconPath.clear();
                if (m_flyoutHwnd)
                    KillTimer(m_flyoutHwnd, kCaretBlinkTimerId);
                InvalidateFlyout();
            }

            void HandleInlineFormClick(const FlyoutLayout& lo, float xDip, float yDip)
            {
                if (m_inlineForm == ProfileInlineForm::EditSlot)
                {
                    if (lo.editNameRect.Contains(xDip, yDip))
                    {
                        m_inlineFocusField = 0;
                        m_inlineCaretOn = true;
                        InvalidateFlyout();
                        return;
                    }
                    if (lo.editIconBrowseBtn.Contains(xDip, yDip))
                    {
                        std::wstring picked;
                        m_modalOpen = true;
                        const bool ok = PickCustomIconDialog(m_flyoutHwnd, picked);
                        m_modalOpen = false;
                        if (m_flyoutHwnd && IsWindow(m_flyoutHwnd))
                            SetForegroundWindow(m_flyoutHwnd);
                        if (ok && !picked.empty())
                        {
                            m_editIconPath = std::move(picked);
                            InvalidateFlyout();
                        }
                        return;
                    }
                    if (lo.editIconResetBtn.Contains(xDip, yDip))
                    {
                        if (!m_editIconPath.empty())
                        {
                            m_editIconPath.clear();
                            InvalidateFlyout();
                        }
                        return;
                    }
                    if (m_editSlotKind != SlotKind::Collection && m_editSlotKind != SlotKind::RecycleBin &&
                        lo.editHideRow.Contains(xDip, yDip))
                    {
                        m_editHiddenFromRing = !m_editHiddenFromRing;
                        InvalidateFlyout();
                        return;
                    }
                    if (CanSlotToggleRunAsAdmin(m_editSlotKind, m_editSlotTarget) && lo.editAdminRow.Contains(xDip, yDip))
                    {
                        m_inlineAdmin = !m_inlineAdmin;
                        InvalidateFlyout();
                        return;
                    }
                }
                else
                {
                    if (lo.inlineField1Rect.Contains(xDip, yDip))
                    {
                        m_inlineFocusField = 0;
                        m_inlineCaretOn = true;
                        InvalidateFlyout();
                        return;
                    }
                    if (m_inlineForm == ProfileInlineForm::AddLinkCmd && lo.inlineField2Rect.Contains(xDip, yDip))
                    {
                        m_inlineFocusField = 1;
                        m_inlineCaretOn = true;
                        InvalidateFlyout();
                        return;
                    }
                    if (m_inlineForm == ProfileInlineForm::AddLinkCmd && lo.inlineAdminRow.Contains(xDip, yDip))
                    {
                        m_inlineAdmin = !m_inlineAdmin;
                        InvalidateFlyout();
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
                if (m_inlineForm == ProfileInlineForm::EditSlot)
                {
                    const std::wstring newLabel = TrimWide(m_inlineField1);
                    if (newLabel.empty())
                        return;

                    RingProfileConfig* curProf = m_config.FindProfile(m_selectedProfileId);
                    if (!curProf && !m_config.profiles.empty())
                        curProf = &m_config.profiles.front();
                    if (curProf)
                    {
                        for (auto& slot : curProf->pinnedSlots)
                        {
                            if (slot.id == m_editSlotId)
                            {
                                slot.label = newLabel;
                                slot.iconCacheFile = m_editIconPath;
                                slot.runAsAdmin = CanSlotToggleRunAsAdmin(slot.kind, slot.pathOrUrl) && m_inlineAdmin;
                                slot.hiddenFromRing = (slot.kind != SlotKind::Collection && slot.kind != SlotKind::RecycleBin) && m_editHiddenFromRing;
                                CloseInlineForm();
                                CommitProfileEdit();
                                return;
                            }
                        }
                    }
                    CloseInlineForm();
                    return;
                }
                if (m_inlineForm == ProfileInlineForm::NewProfile)
                {
                    const std::wstring name = TrimWide(m_inlineField1);
                    if (name.empty())
                        return;
                    std::wstring newId = SwitcherConfig::SanitizeProfileId(name);
                    if (m_config.FindProfile(newId))
                    {
                        std::wstring suffix = L"-" + std::to_wstring(GetTickCount64() % 10000);
                        if (newId.size() + suffix.size() > 28)
                            newId.resize(28 - suffix.size());
                        newId += suffix;
                    }
                    RingProfileConfig np;
                    np.id = newId;
                    np.name = name;
                    np.includeRunningApps = false;
                    m_config.profiles.push_back(std::move(np));
                    m_selectedProfileId = newId;
                    CloseInlineForm();
                    CommitProfileEdit();
                    return;
                }

                if (m_inlineForm == ProfileInlineForm::AddLinkCmd)
                {
                    const std::wstring tgt = TrimWide(m_inlineField1);
                    const std::wstring lbl = TrimWide(m_inlineField2);
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
                    item.runAsAdmin = !isWeb && !isUri && (kind != SlotKind::Folder) && m_inlineAdmin;

                    RingProfileConfig* curProf = m_config.FindProfile(m_selectedProfileId);
                    if (!curProf && !m_config.profiles.empty())
                        curProf = &m_config.profiles.front();
                    if (!curProf)
                        return;
                    curProf->pinnedSlots.push_back(std::move(item));
                    CloseInlineForm();
                    CommitProfileEdit();
                }
            }

            UINT m_wakeMessage = 0;
            UINT m_taskbarCreatedMessage = 0;
            UINT m_hostControlMessage = 0;
            UINT m_shellCommandMessage = 0;

            SwitcherConfig m_config;
            EngineMode m_engineMode = EngineMode::Stopped;
            float m_dpiScale = 1.0f;
            int m_activeTab = 0;
            int m_hoverButton = -1;
            int m_dragSlider = -1;
            bool m_draggingWindow = false;
            POINT m_dragStartCursor{};
            POINT m_dragStartWin{};
            bool m_dirty = false;
            bool m_livePreviewSent = false;
            bool m_modalOpen = false;

            int m_recordingIndex = -1;
            HHOOK m_capKeyHook = nullptr;
            HHOOK m_capMouseHook = nullptr;
            UINT m_capModDown = 0;
            bool m_capSoloDisqualified = false;
            bool m_capRightDown = false;
            UINT m_swallowNextKeyUpVk = 0;

            ComPtr<ID3D11Device> m_d3dDevice;
            ComPtr<IDXGIDevice> m_dxgiDevice;
            ComPtr<ID2D1Factory1> m_d2dFactory;
            ComPtr<ID2D1Device> m_d2dDevice;
            ComPtr<ID2D1DeviceContext> m_d2dCtx;
            ComPtr<IDXGISwapChain1> m_swapChain;
            ComPtr<ID2D1Bitmap1> m_targetBmp;
            ComPtr<IDWriteFactory> m_dwrite;
            ComPtr<IDWriteTextFormat> m_fmtTitle;
            ComPtr<IDWriteTextFormat> m_fmtBold;
            ComPtr<IDWriteTextFormat> m_fmtBody;
            ComPtr<IDWriteTextFormat> m_fmtCenter;
            ComPtr<IDWriteTextFormat> m_fmtCaption;
            ComPtr<IDWriteTextFormat> m_fmtSmallCenter;
            ComPtr<IDWriteTextFormat> m_fmtGuideBody;
            int m_swapW = 0;
            int m_swapH = 0;
        };

        TrayControlApp* TrayControlApp::s_instance = nullptr;
    } // namespace

    int RunTrayApp(HINSTANCE instance, bool openFlyoutOnStart)
    {
        TrayControlApp app;
        return app.Run(instance, openFlyoutOnStart);
    }
} // namespace winrings::tools
