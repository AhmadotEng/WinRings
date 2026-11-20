#include "SettingsDialog.h"

#include <CommCtrl.h>
#include <commdlg.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <sstream>
#include <string>
#include <vector>

#include "../shared/ClockFormat.h"
#include "../shared/WinRingsIdentity.h"

namespace winrings::tools
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr int kWinW = 860;
        constexpr int kWinH = 620;

        enum CtrlId
        {
            ID_TAB = 100,
            ID_BTN_APPLY = 101,
            ID_BTN_SAVE = 102,
            ID_BTN_RESET = 103,
            ID_BTN_CANCEL = 104,
            ID_BTN_PREVIEW = 105,

            // Tab 0: Triggers & Input
            ID_CB_MOUSE_TRIGGER = 200,
            ID_CB_SCROLL_MODE,
            ID_CHK_DOUBLE_TAP,
            ID_ED_TAP_MS,
            ID_ED_DTAP_MS,
            ID_CHK_HOTKEY,
            ID_CHK_MOD_ALT,
            ID_CHK_MOD_CTRL,
            ID_CHK_MOD_SHIFT,
            ID_CHK_MOD_WIN,
            ID_CB_HOTKEY_VK,
            ID_CHK_MENU_KEY,
            ID_CHK_WIN_TAB,
            ID_CHK_ALT_TAB,
            ID_CHK_TOPBAR_BTN,

            // Tab 1: Ring & Theme
            ID_CB_THEME = 300,
            ID_ED_ACCENT,
            ID_ED_RADIUS,
            ID_ED_DEADZONE,
            ID_ED_ICONSIZE,
            ID_ED_THUMBSCALE,
            ID_CHK_NO_ANIM,
            ID_CB_WINVIEW,
            ID_CHK_FANOUT_ONE,
            ID_CHK_PTR_FOLLOW,
            ID_CHK_SHIFT_CENTER,
            ID_CHK_INCLUDE_UNPINNED,
            ID_ED_MAXSLOTS,
            ID_ED_FFMPEG,
            ID_PREVIEW_RING_CANVAS,

            // Tab 2: Top Bar & Clock
            ID_CHK_TOPBAR_ON = 400,
            ID_CHK_DOCK_TOP,
            ID_SLD_BAR_H,
            ID_ED_BAR_H,
            ID_CHK_KEEP_FONT,
            ID_CHK_CLOCK_1L,
            ID_ED_CLOCK_FMT,
            ID_CB_CLOCK_CASING,
            ID_LBL_CLOCK_PREVIEW,
            ID_CHK_HIDE_APPS,
            ID_CHK_HIDE_START,
            ID_CHK_HIDE_SEARCH,
            ID_CHK_HIDE_TASKVIEW,
            ID_CHK_HIDE_WIDGETS,
            ID_CB_BACKDROP,
            ID_CHK_ALL_MON,

            // Tab 3: Pins & Collections
            ID_LB_PINS = 500,
            ID_CB_PIN_KIND,
            ID_ED_PIN_LABEL,
            ID_ED_PIN_PATH,
            ID_BTN_BROWSE_FILE,
            ID_BTN_BROWSE_DIR,
            ID_ED_PIN_FOLDER_MAX,
            ID_BTN_PIN_ADD,
            ID_BTN_PIN_UPDATE,
            ID_BTN_PIN_REMOVE,
            ID_BTN_PIN_UP,
            ID_BTN_PIN_DOWN,
        };

        struct KeyEntry
        {
            const wchar_t* label;
            UINT vk;
        };

        constexpr KeyEntry kHotkeyChoices[] = {
            { L"Space", VK_SPACE },
            { L"Tab", VK_TAB },
            { L"Q", 'Q' },
            { L"W", 'W' },
            { L"E", 'E' },
            { L"R", 'R' },
            { L"A", 'A' },
            { L"S", 'S' },
            { L"D", 'D' },
            { L"F", 'F' },
            { L"Z", 'Z' },
            { L"X", 'X' },
            { L"G", 'G' },
            { L"` (~)", VK_OEM_3 },
        };

        std::wstring GetText(HWND hwnd)
        {
            const int len = GetWindowTextLengthW(hwnd);
            if (len <= 0)
                return {};
            std::wstring s(static_cast<size_t>(len) + 1, L'\0');
            GetWindowTextW(hwnd, s.data(), len + 1);
            s.resize(static_cast<size_t>(len));
            return s;
        }

        double ParseDoubleOr(const std::wstring& s, double fallback)
        {
            try { return std::stod(s); }
            catch (...) { return fallback; }
        }

        int ParseIntOr(const std::wstring& s, int fallback)
        {
            try { return std::stoi(s); }
            catch (...) { return fallback; }
        }

        std::wstring ResolveSiblingHost()
        {
            std::wstring path(MAX_PATH, L'\0');
            const DWORD len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (!len)
                return {};
            path.resize(len);
            const size_t slash = path.find_last_of(L"\\/");
            return (slash != std::wstring::npos) ? (path.substr(0, slash + 1) + WINRINGS_HOST_FILENAME_W) : L"";
        }

        void TriggerHostCommand(const wchar_t* arg)
        {
            const std::wstring host = ResolveSiblingHost();
            if (host.empty() || GetFileAttributesW(host.c_str()) == INVALID_FILE_ATTRIBUTES)
                return;
            std::wstring cmd = L"\"" + host + L"\" " + arg;
            std::vector<wchar_t> buf(cmd.begin(), cmd.end());
            buf.push_back(L'\0');
            STARTUPINFOW si{ sizeof(si) };
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(host.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
            {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }

        class SettingsWindow
        {
        public:
            bool Run(HINSTANCE inst)
            {
                m_instance = inst;
                m_config = SwitcherConfig::LoadOrCreate();

                D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.ReleaseAndGetAddressOf());

                WNDCLASSEXW wc{ sizeof(wc) };
                wc.lpfnWndProc = WndProc;
                wc.hInstance = inst;
                wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                wc.lpszClassName = L"WinRings.Settings.v1";
                RegisterClassExW(&wc);

                WNDCLASSEXW pwc{ sizeof(pwc) };
                pwc.style = CS_HREDRAW | CS_VREDRAW;
                pwc.lpfnWndProc = PreviewCanvasProc;
                pwc.hInstance = inst;
                pwc.lpszClassName = L"WinRings.RingPreviewCanvas.v1";
                RegisterClassExW(&pwc);

                RECT wa{};
                SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
                m_hwnd = CreateWindowExW(
                    0, wc.lpszClassName, L"WinRings — Settings & Top Bar",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                    wa.left + (wa.right - wa.left - kWinW) / 2,
                    wa.top + (wa.bottom - wa.top - kWinH) / 2,
                    kWinW, kWinH,
                    nullptr, nullptr, inst, this);
                if (!m_hwnd)
                    return false;

                ShowWindow(m_hwnd, SW_SHOWNORMAL);

                MSG msg{};
                while (GetMessageW(&msg, nullptr, 0, 0) > 0)
                {
                    if (!IsDialogMessageW(m_hwnd, &msg))
                    {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                }
                return true;
            }

        private:
            static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
            {
                auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (msg == WM_NCCREATE)
                {
                    self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                    self->m_hwnd = hwnd;
                }
                if (self)
                    return self->HandleMsg(msg, wp, lp);
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            static LRESULT CALLBACK PreviewCanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
            {
                auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (msg == WM_NCCREATE)
                {
                    self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                }
                if (self && msg == WM_PAINT)
                {
                    PAINTSTRUCT ps{};
                    BeginPaint(hwnd, &ps);
                    self->PaintRingPreview(hwnd);
                    EndPaint(hwnd, &ps);
                    return 0;
                }
                if (msg == WM_ERASEBKGND)
                    return TRUE;
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            void ParseHexRgb(const std::wstring& hex, BYTE& r, BYTE& g, BYTE& b) const noexcept
            {
                r = 49; g = 134; b = 255;
                if (hex.empty())
                    return;
                const wchar_t* p = hex[0] == L'#' ? hex.c_str() + 1 : hex.c_str();
                unsigned int v = 0;
                if (wcslen(p) == 6 && swscanf_s(p, L"%06x", &v) == 1)
                {
                    r = static_cast<BYTE>((v >> 16) & 0xFF);
                    g = static_cast<BYTE>((v >> 8) & 0xFF);
                    b = static_cast<BYTE>(v & 0xFF);
                }
            }

            void PaintRingPreview(HWND canvas)
            {
                if (!m_d2dFactory)
                    return;
                if (!m_previewRt)
                {
                    RECT rc{};
                    GetClientRect(canvas, &rc);
                    D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties();
                    rtp.dpiX = 96.0f;
                    rtp.dpiY = 96.0f;
                    m_d2dFactory->CreateHwndRenderTarget(
                        rtp,
                        D2D1::HwndRenderTargetProperties(canvas,
                            D2D1::SizeU((std::max)(1u, static_cast<UINT>(rc.right - rc.left)),
                                (std::max)(1u, static_cast<UINT>(rc.bottom - rc.top)))),
                        m_previewRt.ReleaseAndGetAddressOf());
                }
                if (!m_previewRt)
                    return;

                const auto themeIdx = static_cast<int>(SendMessageW(m_cbTheme, CB_GETCURSEL, 0, 0));
                const RingTheme theme = (themeIdx >= 0 && themeIdx <= 5) ? static_cast<RingTheme>(themeIdx) : m_config.theme;
                BYTE ar = 49, ag = 134, ab = 255;
                ParseHexRgb(GetText(m_edAccent), ar, ag, ab);
                const D2D1_COLOR_F accent = D2D1::ColorF(ar / 255.0f, ag / 255.0f, ab / 255.0f, 1.0f);

                RECT rc{};
                GetClientRect(canvas, &rc);
                const float w = static_cast<float>(rc.right - rc.left);
                const float h = static_cast<float>(rc.bottom - rc.top);
                const D2D1_POINT_2F c{ w * 0.5f, h * 0.5f };

                m_previewRt->BeginDraw();
                m_previewRt->Clear(D2D1::ColorF(0.08f, 0.095f, 0.125f, 1.0f));

                D2D1_COLOR_F ringFill = D2D1::ColorF(0.08f, 0.10f, 0.14f, 0.78f);
                D2D1_COLOR_F stroke = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.25f);
                if (theme == RingTheme::Kiwi)
                {
                    ringFill = D2D1::ColorF(0.09f, 0.18f, 0.12f, 0.85f);
                    stroke = D2D1::ColorF(0.45f, 0.90f, 0.55f, 0.60f);
                }
                else if (theme == RingTheme::Aura)
                {
                    ringFill = D2D1::ColorF(ar / 255.0f * 0.4f, ag / 255.0f * 0.4f, ab / 255.0f * 0.4f, 0.65f);
                    stroke = D2D1::ColorF(ar / 255.0f, ag / 255.0f, ab / 255.0f, 0.80f);
                }
                else if (theme == RingTheme::Orbit)
                {
                    ringFill = D2D1::ColorF(0.05f, 0.06f, 0.08f, 0.92f);
                    stroke = accent;
                }
                else if (theme == RingTheme::Outline)
                {
                    ringFill = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.2f);
                    stroke = accent;
                }

                ComPtr<ID2D1SolidColorBrush> fillBr, strokeBr, accBr, slotBr, holeBr;
                m_previewRt->CreateSolidColorBrush(ringFill, fillBr.ReleaseAndGetAddressOf());
                m_previewRt->CreateSolidColorBrush(stroke, strokeBr.ReleaseAndGetAddressOf());
                m_previewRt->CreateSolidColorBrush(accent, accBr.ReleaseAndGetAddressOf());
                m_previewRt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f), slotBr.ReleaseAndGetAddressOf());
                m_previewRt->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.07f, 0.10f, 0.96f), holeBr.ReleaseAndGetAddressOf());

                const float outerR = (std::min)(w, h) * 0.42f;
                const float orbitR = outerR * 0.66f;
                if (fillBr && strokeBr && slotBr && holeBr && accBr)
                {
                    m_previewRt->FillEllipse(D2D1::Ellipse(c, outerR, outerR), fillBr.Get());
                    m_previewRt->DrawEllipse(D2D1::Ellipse(c, outerR, outerR), strokeBr.Get(), theme == RingTheme::Outline ? 2.5f : 1.5f);

                    constexpr int nSlots = 8;
                    for (int i = 0; i < nSlots; ++i)
                    {
                        const double a = -1.57079632679 + (6.28318530718 * i) / nSlots;
                        const D2D1_POINT_2F p{
                            c.x + static_cast<float>(std::cos(a) * orbitR),
                            c.y + static_cast<float>(std::sin(a) * orbitR)
                        };
                        const bool sel = (i == 1);
                        m_previewRt->FillEllipse(D2D1::Ellipse(p, sel ? 15.0f : 12.0f, sel ? 15.0f : 12.0f),
                            sel ? accBr.Get() : slotBr.Get());
                    }

                    m_previewRt->FillEllipse(D2D1::Ellipse(c, 16.0f, 16.0f), holeBr.Get());
                    m_previewRt->DrawEllipse(D2D1::Ellipse(c, 16.0f, 16.0f), accBr.Get(), 1.5f);
                }

                if (m_previewRt->EndDraw() == D2DERR_RECREATE_TARGET)
                    m_previewRt.Reset();
            }

            HWND MakeCtrl(int tabIdx, const wchar_t* cls, const wchar_t* text, DWORD style,
                int x, int y, int w, int h, int id)
            {
                HWND ctrl = CreateWindowExW(0, cls, text, WS_CHILD | style,
                    x, y, w, h, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance, nullptr);
                SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
                SetWindowTheme(ctrl, L"DarkMode_Explorer", nullptr);
                if (tabIdx >= 0 && tabIdx < 4)
                    m_tabCtrls[tabIdx].push_back(ctrl);
                return ctrl;
            }

            void BuildUi()
            {
                const BOOL dark = TRUE;
                DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

                m_bgBrush = CreateSolidBrush(RGB(22, 25, 32));
                m_editBrush = CreateSolidBrush(RGB(34, 38, 49));
                m_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                m_boldFont = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

                m_tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                    14, 12, kWinW - 44, kWinH - 94, m_hwnd, reinterpret_cast<HMENU>(ID_TAB), m_instance, nullptr);
                SendMessageW(m_tab, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);

                const wchar_t* tabs[] = {
                    L"Triggers & Input", L"Ring & Theme", L"Top Bar & Clock", L"Pins & Collections"
                };
                for (int i = 0; i < 4; ++i)
                {
                    TCITEMW ti{};
                    ti.mask = TCIF_TEXT;
                    ti.pszText = const_cast<LPWSTR>(tabs[i]);
                    TabCtrl_InsertItem(m_tab, i, &ti);
                }

                // Tab 0: Triggers & Input
                int y = 58;
                MakeCtrl(0, L"STATIC", L"Mouse Trigger Button:", 0, 32, y + 3, 180, 22, 0);
                m_cbMouseTrigger = MakeCtrl(0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 220, y, 220, 160, ID_CB_MOUSE_TRIGGER);
                for (const wchar_t* s : { L"Side Back (XBUTTON1)", L"Side Forward (XBUTTON2)", L"Middle Button", L"Right + Left Chord", L"Disabled" })
                    SendMessageW(m_cbMouseTrigger, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
                y += 38;

                MakeCtrl(0, L"STATIC", L"Scroll Wheel Behavior:", 0, 32, y + 3, 180, 22, 0);
                m_cbScrollMode = MakeCtrl(0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 220, y, 220, 140, ID_CB_SCROLL_MODE);
                for (const wchar_t* s : { L"Cycle Slots or Windows", L"Cycle Submenu Only", L"Disabled" })
                    SendMessageW(m_cbScrollMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
                y += 38;

                m_chkDoubleTap = MakeCtrl(0, L"BUTTON", L"Double-tap trigger switches immediately to last active window",
                    BS_AUTOCHECKBOX | WS_TABSTOP, 32, y, 480, 24, ID_CHK_DOUBLE_TAP);
                y += 36;

                MakeCtrl(0, L"STATIC", L"Quick Tap Threshold (ms):", 0, 32, y + 3, 180, 22, 0);
                m_edTapMs = MakeCtrl(0, L"EDIT", L"", WS_BORDER | WS_TABSTOP, 220, y, 80, 24, ID_ED_TAP_MS);
                MakeCtrl(0, L"STATIC", L"Double-Tap Window (ms):", 0, 330, y + 3, 180, 22, 0);
                m_edDTapMs = MakeCtrl(0, L"EDIT", L"", WS_BORDER | WS_TABSTOP, 520, y, 80, 24, ID_ED_DTAP_MS);
                y += 42;

                m_chkHotkey = MakeCtrl(0, L"BUTTON", L"Enable Keyboard Hotkey Trigger",
                    BS_AUTOCHECKBOX | WS_TABSTOP, 32, y, 280, 24, ID_CHK_HOTKEY);
                y += 30;
                m_chkModAlt = MakeCtrl(0, L"BUTTON", L"Alt", BS_AUTOCHECKBOX | WS_TABSTOP, 54, y, 60, 24, ID_CHK_MOD_ALT);
                m_chkModCtrl = MakeCtrl(0, L"BUTTON", L"Ctrl", BS_AUTOCHECKBOX | WS_TABSTOP, 120, y, 60, 24, ID_CHK_MOD_CTRL);
                m_chkModShift = MakeCtrl(0, L"BUTTON", L"Shift", BS_AUTOCHECKBOX | WS_TABSTOP, 186, y, 64, 24, ID_CHK_MOD_SHIFT);
                m_chkModWin = MakeCtrl(0, L"BUTTON", L"Win", BS_AUTOCHECKBOX | WS_TABSTOP, 256, y, 64, 24, ID_CHK_MOD_WIN);
                MakeCtrl(0, L"STATIC", L"Key:", 0, 336, y + 3, 40, 22, 0);
                m_cbHotkeyVk = MakeCtrl(0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 380, y, 130, 180, ID_CB_HOTKEY_VK);
                for (const auto& k : kHotkeyChoices)
                    SendMessageW(m_cbHotkeyVk, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(k.label));
                y += 42;

                m_chkMenuKey = MakeCtrl(0, L"BUTTON", L"Toggle Ring on Menu Key (VK_APPS)", BS_AUTOCHECKBOX | WS_TABSTOP, 32, y, 360, 24, ID_CHK_MENU_KEY);
                y += 30;
                m_chkWinTab = MakeCtrl(0, L"BUTTON", L"Optional Shell Interception: Win + Tab", BS_AUTOCHECKBOX | WS_TABSTOP, 32, y, 360, 24, ID_CHK_WIN_TAB);
                y += 30;
                m_chkAltTab = MakeCtrl(0, L"BUTTON", L"Optional Shell Interception: Alt + Tab", BS_AUTOCHECKBOX | WS_TABSTOP, 32, y, 360, 24, ID_CHK_ALT_TAB);
                y += 30;
                m_chkTopBarBtn = MakeCtrl(0, L"BUTTON", L"Show Top-Bar Ring Toggle Pill on Primary Monitor", BS_AUTOCHECKBOX | WS_TABSTOP, 32, y, 420, 24, ID_CHK_TOPBAR_BTN);

                // Tab 1: Ring & Theme
                y = 58;
                MakeCtrl(1, L"STATIC", L"Ring Theme:", 0, 32, y + 3, 150, 22, 0);
                m_cbTheme = MakeCtrl(1, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 190, y, 170, 160, ID_CB_THEME);
                for (const wchar_t* t : { L"LiquidGlass", L"Glass", L"Kiwi", L"Aura", L"Orbit", L"Outline" })
                    SendMessageW(m_cbTheme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));

                MakeCtrl(1, L"STATIC", L"Accent Hex:", 0, 380, y + 3, 90, 22, 0);
                m_edAccent = MakeCtrl(1, L"EDIT", L"", WS_BORDER | WS_TABSTOP, 476, y, 90, 24, ID_ED_ACCENT);
                y += 36;

                MakeCtrl(1, L"STATIC", L"Ring Radius (DIP):", 0, 32, y + 3, 150, 22, 0);
                m_edRadius = MakeCtrl(1, L"EDIT", L"", WS_BORDER | WS_TABSTOP, 190, y, 80, 24, ID_ED_RADIUS);
                MakeCtrl(1, L"STATIC", L"Deadzone (DIP):", 0, 300, y + 3, 120, 22, 0);
                m_edDeadzone = MakeCtrl(1, L"EDIT", L"", WS_BORDER | WS_TABSTOP, 430, y, 80, 24, ID_ED_DEADZONE);
                y += 34;

                MakeCtrl(1, L"STATIC", L"Icon Size (DIP):", 0, 32, y + 3, 150, 22, 0);
                m_edIconSize = MakeCtrl(1, L"EDIT", L"", WS_BORDER | WS_TABSTOP, 190, y, 80, 24, ID_ED_ICONSIZE);
                MakeCtrl(1, L"STATIC", L"Preview Scale:", 0, 300, y + 3, 120, 22, 0);
                m_edThumbScale = MakeCtrl(1, L"EDIT", L"", WS_BORDER | WS_TABSTOP, 430, y, 80, 24, ID_ED_THUMBSCALE);
                y += 36;

                MakeCtrl(1, L"STATIC", L"Window Submenu View:", 0, 32, y + 3, 150, 22, 0);
                m_cbWinView = MakeCtrl(1, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 190, y, 170, 120, ID_CB_WINVIEW);
                SendMessageW(m_cbWinView, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"OuterArc (Thumbnails)"));
                SendMessageW(m_cbWinView, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"TitleColumn"));
                y += 34;

                m_chkNoAnim = MakeCtrl(1, L"BUTTON", L"Disable Opening Animation (Frame-0 Instant)", BS_AUTOCHECKBOX | WS_TABSTOP, 32, y, 360, 24, ID_CHK_NO_ANIM);
                y += 28;
                m_chkFanOne = MakeCtrl(1, L"BUTTON", L"Fan out live DWM preview even for 1 window", BS_AUTOCHECKBOX | WS_TABSTOP, 32, y, 360, 24, ID_CHK_FANOUT_ONE);
                y += 28;
                m_chkPtrFollow = MakeCtrl(1, L"BUTTON", L"Pointer Follow (160ms glide on focus switch)", BS_AUTOCHECKBOX | WS_TABSTOP, 32, y, 360, 24, ID_CHK_PTR_FOLLOW);
                y += 28;
                m_chkShiftCenter = MakeCtrl(1, L"BUTTON", L"Shift-Center overlay & window on active monitor", BS_AUTOCHECKBOX | WS_TABSTOP, 32, y, 360, 24, ID_CHK_SHIFT_CENTER);
                y += 28;
                m_chkIncludeUnpinned = MakeCtrl(1, L"BUTTON", L"Include unpinned running apps in Ring slots", BS_AUTOCHECKBOX | WS_TABSTOP, 32, y, 360, 24, ID_CHK_INCLUDE_UNPINNED);
                y += 34;

                MakeCtrl(1, L"STATIC", L"Max Ring Slots:", 0, 32, y + 3, 150, 22, 0);
                m_edMaxSlots = MakeCtrl(1, L"EDIT", L"", WS_BORDER | WS_TABSTOP, 190, y, 70, 24, ID_ED_MAXSLOTS);
                y += 34;
                MakeCtrl(1, L"STATIC", L"ffmpeg Path:", 0, 32, y + 3, 150, 22, 0);
                m_edFfmpeg = MakeCtrl(1, L"EDIT", L"", WS_BORDER | WS_TABSTOP, 190, y, 320, 24, ID_ED_FFMPEG);

                // Live Ring Theme preview canvas on right side of Tab 1
                MakeCtrl(1, L"STATIC", L"Live Ring Preview:", 0, 595, 58, 200, 22, 0);
                m_ringCanvas = MakeCtrl(1, L"WinRings.RingPreviewCanvas.v1", L"", WS_BORDER, 595, 86, 218, 218, ID_PREVIEW_RING_CANVAS);

                // Tab 2: Top Bar & Clock
                y = 58;
                m_chkTopBarOn = MakeCtrl(2, L"BUTTON", L"Enable Unified Minimal Top Bar (Windows 11 Shell Integration)", BS_AUTOCHECKBOX | WS_TABSTOP, 32, y, 520, 24, ID_CHK_TOPBAR_ON);
                y += 28;
                m_chkDockTop = MakeCtrl(2, L"BUTTON", L"Dock Taskbar to Top Monitor Edge (updates Desktop Work Area)", BS_AUTOCHECKBOX | WS_TABSTOP, 32, y, 520, 24, ID_CHK_DOCK_TOP);
                y += 34;

                MakeCtrl(2, L"STATIC", L"Bar Height (28-40 DIP):", 0, 32, y + 3, 165, 22, 0);
                m_sldBarH = MakeCtrl(2, TRACKBAR_CLASSW, L"", TBS_AUTOTICKS | WS_TABSTOP, 204, y, 200, 28, ID_SLD_BAR_H);
                SendMessageW(m_sldBarH, TBM_SETRANGE, TRUE, MAKELPARAM(28, 40));
                m_edBarH = MakeCtrl(2, L"EDIT", L"", WS_BORDER | WS_TABSTOP, 416, y + 2, 56, 24, ID_ED_BAR_H);
                y += 34;

                m_chkKeepFont = MakeCtrl(2, L"BUTTON", L"Keep Text Font Size Unchanged (Native Taskbar Font Size)", BS_AUTOCHECKBOX | WS_TABSTOP, 32, y, 500, 24, ID_CHK_KEEP_FONT);
                y += 30;
                m_chkClock1L = MakeCtrl(2, L"BUTTON", L"Single-Line Compact Clock (replaces two-line date/time)", BS_AUTOCHECKBOX | WS_TABSTOP, 32, y, 480, 24, ID_CHK_CLOCK_1L);
                y += 32;

                MakeCtrl(2, L"STATIC", L"Clock Format:", 0, 32, y + 3, 120, 22, 0);
                m_edClockFmt = MakeCtrl(2, L"EDIT", L"", WS_BORDER | WS_TABSTOP, 160, y, 220, 24, ID_ED_CLOCK_FMT);
                MakeCtrl(2, L"STATIC", L"Casing:", 0, 392, y + 3, 60, 22, 0);
                m_cbClockCasing = MakeCtrl(2, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 456, y, 160, 140, ID_CB_CLOCK_CASING);
                for (const wchar_t* c : { L"CompactLower", L"Default", L"Lowercase", L"Uppercase" })
                    SendMessageW(m_cbClockCasing, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(c));
                y += 32;

                MakeCtrl(2, L"STATIC", L"Clock Preview:", 0, 32, y + 3, 120, 22, 0);
                m_lblClockPreview = MakeCtrl(2, L"STATIC", L"", SS_SUNKEN | SS_CENTERIMAGE, 160, y, 280, 26, ID_LBL_CLOCK_PREVIEW);
                SendMessageW(m_lblClockPreview, WM_SETFONT, reinterpret_cast<WPARAM>(m_boldFont), TRUE);
                y += 36;

                MakeCtrl(2, L"STATIC", L"Taskbar Chrome Elements:", 0, 32, y, 220, 22, 0);
                y += 24;
                m_chkHideApps = MakeCtrl(2, L"BUTTON", L"Hide Taskbar App Buttons (preserve System Tray)", BS_AUTOCHECKBOX | WS_TABSTOP, 50, y, 360, 22, ID_CHK_HIDE_APPS);
                m_chkHideStart = MakeCtrl(2, L"BUTTON", L"Hide Start Button", BS_AUTOCHECKBOX | WS_TABSTOP, 430, y, 200, 22, ID_CHK_HIDE_START);
                y += 26;
                m_chkHideSearch = MakeCtrl(2, L"BUTTON", L"Hide Search", BS_AUTOCHECKBOX | WS_TABSTOP, 50, y, 160, 22, ID_CHK_HIDE_SEARCH);
                m_chkHideTaskView = MakeCtrl(2, L"BUTTON", L"Hide Task View", BS_AUTOCHECKBOX | WS_TABSTOP, 220, y, 170, 22, ID_CHK_HIDE_TASKVIEW);
                m_chkHideWidgets = MakeCtrl(2, L"BUTTON", L"Hide Widgets", BS_AUTOCHECKBOX | WS_TABSTOP, 430, y, 180, 22, ID_CHK_HIDE_WIDGETS);
                y += 34;

                MakeCtrl(2, L"STATIC", L"Backdrop Style:", 0, 32, y + 3, 120, 22, 0);
                m_cbBackdrop = MakeCtrl(2, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 160, y, 160, 120, ID_CB_BACKDROP);
                for (const wchar_t* b : { L"Clear", L"Frosted", L"Native" })
                    SendMessageW(m_cbBackdrop, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(b));
                m_chkAllMon = MakeCtrl(2, L"BUTTON", L"Apply to All Monitors", BS_AUTOCHECKBOX | WS_TABSTOP, 350, y + 2, 220, 24, ID_CHK_ALL_MON);

                // Tab 3: Pins & Collections
                m_lbPins = MakeCtrl(3, L"LISTBOX", L"", LBS_NOTIFY | WS_BORDER | WS_VSCROLL | WS_TABSTOP, 28, 56, 780, 210, ID_LB_PINS);
                int py = 278;
                MakeCtrl(3, L"STATIC", L"Kind:", 0, 28, py + 3, 60, 22, 0);
                m_cbPinKind = MakeCtrl(3, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 94, py, 140, 160, ID_CB_PIN_KIND);
                for (const wchar_t* k : { L"App", L"Website", L"File", L"Folder", L"Script", L"Collection", L"RecycleBin" })
                    SendMessageW(m_cbPinKind, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(k));
                MakeCtrl(3, L"STATIC", L"Label:", 0, 250, py + 3, 60, 22, 0);
                m_edPinLabel = MakeCtrl(3, L"EDIT", L"", WS_BORDER | WS_TABSTOP, 314, py, 240, 24, ID_ED_PIN_LABEL);
                MakeCtrl(3, L"STATIC", L"Folder Max:", 0, 570, py + 3, 90, 22, 0);
                m_edPinFolderMax = MakeCtrl(3, L"EDIT", L"8", WS_BORDER | WS_TABSTOP, 666, py, 60, 24, ID_ED_PIN_FOLDER_MAX);
                py += 34;

                MakeCtrl(3, L"STATIC", L"Path / URL:", 0, 28, py + 3, 80, 22, 0);
                m_edPinPath = MakeCtrl(3, L"EDIT", L"", WS_BORDER | WS_TABSTOP, 114, py, 480, 24, ID_ED_PIN_PATH);
                MakeCtrl(3, L"BUTTON", L"File...", WS_TABSTOP, 604, py, 94, 26, ID_BTN_BROWSE_FILE);
                MakeCtrl(3, L"BUTTON", L"Folder...", WS_TABSTOP, 706, py, 102, 26, ID_BTN_BROWSE_DIR);
                py += 38;

                MakeCtrl(3, L"BUTTON", L"Add Slot", WS_TABSTOP, 28, py, 110, 28, ID_BTN_PIN_ADD);
                MakeCtrl(3, L"BUTTON", L"Update Selected", WS_TABSTOP, 146, py, 130, 28, ID_BTN_PIN_UPDATE);
                MakeCtrl(3, L"BUTTON", L"Remove", WS_TABSTOP, 284, py, 100, 28, ID_BTN_PIN_REMOVE);
                MakeCtrl(3, L"BUTTON", L"Move Up", WS_TABSTOP, 392, py, 100, 28, ID_BTN_PIN_UP);
                MakeCtrl(3, L"BUTTON", L"Move Down", WS_TABSTOP, 500, py, 100, 28, ID_BTN_PIN_DOWN);

                // Persistent Footer
                const int fy = kWinH - 72;
                auto footerBtn = [&](const wchar_t* t, DWORD s, int x, int w, int id) {
                    HWND b = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | s,
                        x, fy, w, 30, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance, nullptr);
                    SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
                    SetWindowTheme(b, L"DarkMode_Explorer", nullptr);
                };
                footerBtn(L"Launch Ring Preview", 0, 16, 165, ID_BTN_PREVIEW);
                footerBtn(L"Reset Defaults", 0, 192, 130, ID_BTN_RESET);
                footerBtn(L"Apply", 0, kWinW - 332, 92, ID_BTN_APPLY);
                footerBtn(L"Save & Close", BS_DEFPUSHBUTTON, kWinW - 230, 110, ID_BTN_SAVE);
                footerBtn(L"Cancel", 0, kWinW - 110, 82, ID_BTN_CANCEL);

                PopulateControlsFromConfig();
                ShowTab(0);
            }

            void ShowTab(int index)
            {
                m_activeTab = index;
                for (int t = 0; t < 4; ++t)
                {
                    const int cmd = (t == index) ? SW_SHOW : SW_HIDE;
                    for (HWND c : m_tabCtrls[t])
                        ShowWindow(c, cmd);
                }
                if (index == 1 && m_ringCanvas)
                    InvalidateRect(m_ringCanvas, nullptr, FALSE);
            }

            void PopulateControlsFromConfig()
            {
                m_populating = true;

                SendMessageW(m_cbMouseTrigger, CB_SETCURSEL, static_cast<WPARAM>(static_cast<int>(m_config.mouseTrigger)), 0);
                SendMessageW(m_cbScrollMode, CB_SETCURSEL, static_cast<WPARAM>(static_cast<int>(m_config.scrollWheelMode)), 0);
                SendMessageW(m_chkDoubleTap, BM_SETCHECK, m_config.doubleTapLastApp ? BST_CHECKED : BST_UNCHECKED, 0);
                SetWindowTextW(m_edTapMs, std::to_wstring(m_config.tapThresholdMs).c_str());
                SetWindowTextW(m_edDTapMs, std::to_wstring(m_config.doubleTapWindowMs).c_str());
                SendMessageW(m_chkHotkey, BM_SETCHECK, m_config.hotkeyEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkModAlt, BM_SETCHECK, (m_config.hotkeyModifiers & MOD_ALT) ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkModCtrl, BM_SETCHECK, (m_config.hotkeyModifiers & MOD_CONTROL) ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkModShift, BM_SETCHECK, (m_config.hotkeyModifiers & MOD_SHIFT) ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkModWin, BM_SETCHECK, (m_config.hotkeyModifiers & MOD_WIN) ? BST_CHECKED : BST_UNCHECKED, 0);

                int vkIdx = 0;
                for (int i = 0; i < static_cast<int>(_countof(kHotkeyChoices)); ++i)
                {
                    if (kHotkeyChoices[i].vk == m_config.hotkeyVk)
                    {
                        vkIdx = i;
                        break;
                    }
                }
                SendMessageW(m_cbHotkeyVk, CB_SETCURSEL, static_cast<WPARAM>(vkIdx), 0);
                SendMessageW(m_chkMenuKey, BM_SETCHECK, m_config.menuKeyEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkWinTab, BM_SETCHECK, m_config.interceptWinTab ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkAltTab, BM_SETCHECK, m_config.interceptAltTab ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkTopBarBtn, BM_SETCHECK, m_config.topBarRingButton ? BST_CHECKED : BST_UNCHECKED, 0);

                SendMessageW(m_cbTheme, CB_SETCURSEL, static_cast<WPARAM>(static_cast<int>(m_config.theme)), 0);
                SetWindowTextW(m_edAccent, m_config.accentColor.c_str());

                wchar_t num[32]{};
                swprintf_s(num, L"%.0f", m_config.ringRadiusDip);
                SetWindowTextW(m_edRadius, num);
                swprintf_s(num, L"%.0f", m_config.deadzoneDip);
                SetWindowTextW(m_edDeadzone, num);
                swprintf_s(num, L"%.0f", m_config.iconSizeDip);
                SetWindowTextW(m_edIconSize, num);
                swprintf_s(num, L"%.2f", m_config.thumbnailScale);
                SetWindowTextW(m_edThumbScale, num);

                SendMessageW(m_cbWinView, CB_SETCURSEL, static_cast<WPARAM>(static_cast<int>(m_config.windowViewMode)), 0);
                SendMessageW(m_chkNoAnim, BM_SETCHECK, m_config.disableOpeningAnimation ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkFanOne, BM_SETCHECK, m_config.fanOutSingleWindow ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkPtrFollow, BM_SETCHECK, m_config.pointerFollow ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkShiftCenter, BM_SETCHECK, m_config.shiftCenterOnActiveMonitor ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkIncludeUnpinned, BM_SETCHECK, m_config.includeUnpinnedRunningApps ? BST_CHECKED : BST_UNCHECKED, 0);
                SetWindowTextW(m_edMaxSlots, std::to_wstring(m_config.maxSlots).c_str());
                SetWindowTextW(m_edFfmpeg, m_config.ffmpegPath.c_str());

                SendMessageW(m_chkTopBarOn, BM_SETCHECK, m_config.topBarEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkDockTop, BM_SETCHECK, m_config.topBarDockTop ? BST_CHECKED : BST_UNCHECKED, 0);
                const int barH = (std::clamp)(m_config.topBarHeightDip, 28, 40);
                SendMessageW(m_sldBarH, TBM_SETPOS, TRUE, static_cast<LPARAM>(barH));
                SetWindowTextW(m_edBarH, std::to_wstring(barH).c_str());
                SendMessageW(m_chkKeepFont, BM_SETCHECK, m_config.topBarPreserveFontSize ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkClock1L, BM_SETCHECK, m_config.topBarClockSingleLine ? BST_CHECKED : BST_UNCHECKED, 0);
                SetWindowTextW(m_edClockFmt, m_config.topBarClockFormat.c_str());
                SendMessageW(m_cbClockCasing, CB_SETCURSEL, static_cast<WPARAM>(static_cast<int>(m_config.topBarClockCasing)), 0);

                SendMessageW(m_chkHideApps, BM_SETCHECK, m_config.topBarHideAppButtons ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkHideStart, BM_SETCHECK, m_config.topBarHideStart ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkHideSearch, BM_SETCHECK, m_config.topBarHideSearch ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkHideTaskView, BM_SETCHECK, m_config.topBarHideTaskView ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkHideWidgets, BM_SETCHECK, m_config.topBarHideWidgets ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_cbBackdrop, CB_SETCURSEL, static_cast<WPARAM>(static_cast<int>(m_config.topBarBackdrop)), 0);
                SendMessageW(m_chkAllMon, BM_SETCHECK, m_config.topBarAllMonitors ? BST_CHECKED : BST_UNCHECKED, 0);

                RefreshPinsList();
                UpdateClockPreview();

                m_populating = false;
            }

            void RefreshPinsList()
            {
                SendMessageW(m_lbPins, LB_RESETCONTENT, 0, 0);
                for (const auto& p : m_config.pinnedSlots)
                {
                    const std::wstring row = std::wstring(L"[") + ToString(p.kind) + L"] " +
                        p.label + (p.pathOrUrl.empty() ? L"" : (L" — " + p.pathOrUrl));
                    SendMessageW(m_lbPins, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
                }
                SendMessageW(m_cbPinKind, CB_SETCURSEL, 0, 0);
            }

            void UpdateClockPreview()
            {
                const std::wstring fmt = GetText(m_edClockFmt);
                const int cIdx = static_cast<int>(SendMessageW(m_cbClockCasing, CB_GETCURSEL, 0, 0));
                const ClockCasing casing = (cIdx >= 0 && cIdx <= 3) ? static_cast<ClockCasing>(cIdx) : ClockCasing::CompactLower;
                SYSTEMTIME st{};
                GetLocalTime(&st);
                const std::wstring text = FormatClock(st, fmt, casing);
                SetWindowTextW(m_lblClockPreview, text.c_str());
            }

            void CollectControlsIntoConfig()
            {
                auto cbIdx = [](HWND c, int maxVal) {
                    const int v = static_cast<int>(SendMessageW(c, CB_GETCURSEL, 0, 0));
                    return (std::clamp)(v, 0, maxVal);
                };
                auto isChk = [](HWND c) { return SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED; };

                m_config.mouseTrigger = static_cast<MouseTriggerButton>(cbIdx(m_cbMouseTrigger, 4));
                m_config.scrollWheelMode = static_cast<ScrollWheelBehavior>(cbIdx(m_cbScrollMode, 2));
                m_config.doubleTapLastApp = isChk(m_chkDoubleTap);
                m_config.tapThresholdMs = (std::clamp)(ParseIntOr(GetText(m_edTapMs), 180), 60, 600);
                m_config.doubleTapWindowMs = (std::clamp)(ParseIntOr(GetText(m_edDTapMs), 280), 100, 900);
                m_config.hotkeyEnabled = isChk(m_chkHotkey);

                UINT mods = 0;
                if (isChk(m_chkModAlt)) mods |= MOD_ALT;
                if (isChk(m_chkModCtrl)) mods |= MOD_CONTROL;
                if (isChk(m_chkModShift)) mods |= MOD_SHIFT;
                if (isChk(m_chkModWin)) mods |= MOD_WIN;
                m_config.hotkeyModifiers = mods;
                m_config.hotkeyVk = kHotkeyChoices[cbIdx(m_cbHotkeyVk, static_cast<int>(_countof(kHotkeyChoices)) - 1)].vk;
                m_config.menuKeyEnabled = isChk(m_chkMenuKey);
                m_config.interceptWinTab = isChk(m_chkWinTab);
                m_config.interceptAltTab = isChk(m_chkAltTab);
                m_config.topBarRingButton = isChk(m_chkTopBarBtn);

                m_config.theme = static_cast<RingTheme>(cbIdx(m_cbTheme, 5));
                const std::wstring acc = GetText(m_edAccent);
                if (!acc.empty()) m_config.accentColor = acc;
                m_config.ringRadiusDip = (std::clamp)(ParseDoubleOr(GetText(m_edRadius), 112.0), 68.0, 220.0);
                m_config.deadzoneDip = (std::clamp)(ParseDoubleOr(GetText(m_edDeadzone), 18.0), 8.0, 48.0);
                m_config.iconSizeDip = (std::clamp)(ParseDoubleOr(GetText(m_edIconSize), 42.0), 24.0, 72.0);
                m_config.thumbnailScale = (std::clamp)(ParseDoubleOr(GetText(m_edThumbScale), 1.0), 0.5, 2.0);
                m_config.windowViewMode = static_cast<WindowViewMode>(cbIdx(m_cbWinView, 1));
                m_config.disableOpeningAnimation = isChk(m_chkNoAnim);
                m_config.fanOutSingleWindow = isChk(m_chkFanOne);
                m_config.pointerFollow = isChk(m_chkPtrFollow);
                m_config.shiftCenterOnActiveMonitor = isChk(m_chkShiftCenter);
                m_config.includeUnpinnedRunningApps = isChk(m_chkIncludeUnpinned);
                m_config.maxSlots = (std::clamp)(ParseIntOr(GetText(m_edMaxSlots), 14), 4, 24);
                m_config.ffmpegPath = GetText(m_edFfmpeg);
                if (m_config.ffmpegPath.empty()) m_config.ffmpegPath = L"ffmpeg";

                m_config.topBarEnabled = isChk(m_chkTopBarOn);
                m_config.topBarDockTop = isChk(m_chkDockTop);
                m_config.topBarHeightDip = (std::clamp)(ParseIntOr(GetText(m_edBarH), 30), 28, 40);
                m_config.topBarPreserveFontSize = isChk(m_chkKeepFont);
                m_config.topBarClockSingleLine = isChk(m_chkClock1L);
                m_config.topBarClockFormat = GetText(m_edClockFmt);
                if (m_config.topBarClockFormat.empty()) m_config.topBarClockFormat = L"ddd d MMM  h:mm tt";
                m_config.topBarClockCasing = static_cast<ClockCasing>(cbIdx(m_cbClockCasing, 3));
                m_config.topBarHideAppButtons = isChk(m_chkHideApps);
                m_config.topBarHideStart = isChk(m_chkHideStart);
                m_config.topBarHideSearch = isChk(m_chkHideSearch);
                m_config.topBarHideTaskView = isChk(m_chkHideTaskView);
                m_config.topBarHideWidgets = isChk(m_chkHideWidgets);
                m_config.topBarBackdrop = static_cast<TopBarBackdrop>(cbIdx(m_cbBackdrop, 2));
                m_config.topBarAllMonitors = isChk(m_chkAllMon);
            }

            void SaveAndReload()
            {
                CollectControlsIntoConfig();
                if (!m_config.Save())
                {
                    MessageBoxW(m_hwnd, L"Failed writing configuration file.", L"WinRings Settings", MB_ICONERROR | MB_OK);
                    return;
                }
                TriggerHostCommand(L"--reload");
            }

            LRESULT HandleMsg(UINT msg, WPARAM wp, LPARAM lp)
            {
                switch (msg)
                {
                case WM_CREATE:
                    BuildUi();
                    return 0;

                case WM_NOTIFY:
                {
                    const auto* hdr = reinterpret_cast<const NMHDR*>(lp);
                    if (hdr && hdr->idFrom == ID_TAB && hdr->code == TCN_SELCHANGE)
                    {
                        ShowTab(TabCtrl_GetCurSel(m_tab));
                        return 0;
                    }
                    break;
                }

                case WM_HSCROLL:
                    if (reinterpret_cast<HWND>(lp) == m_sldBarH)
                    {
                        const int v = static_cast<int>(SendMessageW(m_sldBarH, TBM_GETPOS, 0, 0));
                        m_populating = true;
                        SetWindowTextW(m_edBarH, std::to_wstring(v).c_str());
                        m_populating = false;
                        return 0;
                    }
                    break;

                case WM_CTLCOLORSTATIC:
                case WM_CTLCOLORBTN:
                {
                    HDC hdc = reinterpret_cast<HDC>(wp);
                    SetTextColor(hdc, RGB(230, 235, 245));
                    SetBkColor(hdc, RGB(22, 25, 32));
                    return reinterpret_cast<LRESULT>(m_bgBrush);
                }

                case WM_CTLCOLOREDIT:
                case WM_CTLCOLORLISTBOX:
                {
                    HDC hdc = reinterpret_cast<HDC>(wp);
                    SetTextColor(hdc, RGB(238, 242, 250));
                    SetBkColor(hdc, RGB(34, 38, 49));
                    return reinterpret_cast<LRESULT>(m_editBrush);
                }

                case WM_ERASEBKGND:
                {
                    RECT rc{};
                    GetClientRect(m_hwnd, &rc);
                    FillRect(reinterpret_cast<HDC>(wp), &rc, m_bgBrush);
                    return TRUE;
                }

                case WM_COMMAND:
                {
                    const int id = LOWORD(wp);
                    const int code = HIWORD(wp);

                    if (!m_populating)
                    {
                        if ((id == ID_ED_CLOCK_FMT && code == EN_CHANGE) ||
                            (id == ID_CB_CLOCK_CASING && code == CBN_SELCHANGE))
                        {
                            UpdateClockPreview();
                        }
                        else if (id == ID_ED_BAR_H && code == EN_CHANGE)
                        {
                            const int v = (std::clamp)(ParseIntOr(GetText(m_edBarH), 30), 28, 40);
                            SendMessageW(m_sldBarH, TBM_SETPOS, TRUE, static_cast<LPARAM>(v));
                        }
                        else if ((id == ID_CB_THEME && code == CBN_SELCHANGE) ||
                                 (id == ID_ED_ACCENT && code == EN_CHANGE))
                        {
                            if (m_ringCanvas)
                                InvalidateRect(m_ringCanvas, nullptr, FALSE);
                        }
                    }

                    if (id == ID_LB_PINS && code == LBN_SELCHANGE)
                    {
                        const int sel = static_cast<int>(SendMessageW(m_lbPins, LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(m_config.pinnedSlots.size()))
                        {
                            const auto& s = m_config.pinnedSlots[static_cast<size_t>(sel)];
                            SendMessageW(m_cbPinKind, CB_SETCURSEL, static_cast<WPARAM>(static_cast<int>(s.kind)), 0);
                            SetWindowTextW(m_edPinLabel, s.label.c_str());
                            SetWindowTextW(m_edPinPath, s.pathOrUrl.c_str());
                            SetWindowTextW(m_edPinFolderMax, std::to_wstring(s.maxFolderItems).c_str());
                        }
                        return 0;
                    }

                    switch (id)
                    {
                    case ID_BTN_APPLY:
                        SaveAndReload();
                        return 0;
                    case ID_BTN_SAVE:
                        SaveAndReload();
                        DestroyWindow(m_hwnd);
                        return 0;
                    case ID_BTN_RESET:
                        if (MessageBoxW(m_hwnd, L"Reset all WinRings settings and default pins?", L"WinRings Settings",
                            MB_YESNO | MB_ICONQUESTION) == IDYES)
                        {
                            m_config = SwitcherConfig::CreateDefault();
                            PopulateControlsFromConfig();
                            if (m_ringCanvas) InvalidateRect(m_ringCanvas, nullptr, FALSE);
                        }
                        return 0;
                    case ID_BTN_CANCEL:
                        DestroyWindow(m_hwnd);
                        return 0;
                    case ID_BTN_PREVIEW:
                        SaveAndReload();
                        TriggerHostCommand(L"--preview");
                        return 0;

                    case ID_BTN_BROWSE_FILE:
                    {
                        wchar_t fileBuf[MAX_PATH]{};
                        OPENFILENAMEW ofn{ sizeof(ofn) };
                        ofn.hwndOwner = m_hwnd;
                        ofn.lpstrFilter = L"All Files\0*.*\0Executables\0*.exe;*.lnk;*.bat;*.cmd;*.ps1\0";
                        ofn.lpstrFile = fileBuf;
                        ofn.nMaxFile = _countof(fileBuf);
                        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                        if (GetOpenFileNameW(&ofn))
                            SetWindowTextW(m_edPinPath, fileBuf);
                        return 0;
                    }
                    case ID_BTN_BROWSE_DIR:
                    {
                        BROWSEINFOW bi{};
                        bi.hwndOwner = m_hwnd;
                        bi.lpszTitle = L"Select Folder to Pin";
                        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
                        if (pidl)
                        {
                            wchar_t dirBuf[MAX_PATH]{};
                            if (SHGetPathFromIDListW(pidl, dirBuf))
                                SetWindowTextW(m_edPinPath, dirBuf);
                            CoTaskMemFree(pidl);
                        }
                        return 0;
                    }

                    case ID_BTN_PIN_ADD:
                    case ID_BTN_PIN_UPDATE:
                    {
                        PinnedSlotConfig slot;
                        const int kIdx = (std::clamp)(static_cast<int>(SendMessageW(m_cbPinKind, CB_GETCURSEL, 0, 0)), 0, 6);
                        slot.kind = static_cast<SlotKind>(kIdx);
                        slot.label = GetText(m_edPinLabel);
                        slot.pathOrUrl = GetText(m_edPinPath);
                        slot.maxFolderItems = (std::clamp)(ParseIntOr(GetText(m_edPinFolderMax), 8), 1, 9);
                        if (slot.label.empty())
                            slot.label = ToString(slot.kind);
                        slot.id = L"pin-" + std::to_wstring(GetTickCount64());

                        if (id == ID_BTN_PIN_ADD)
                        {
                            m_config.pinnedSlots.push_back(std::move(slot));
                            RefreshPinsList();
                            SendMessageW(m_lbPins, LB_SETCURSEL, static_cast<WPARAM>(m_config.pinnedSlots.size() - 1), 0);
                        }
                        else
                        {
                            const int sel = static_cast<int>(SendMessageW(m_lbPins, LB_GETCURSEL, 0, 0));
                            if (sel >= 0 && sel < static_cast<int>(m_config.pinnedSlots.size()))
                            {
                                slot.id = m_config.pinnedSlots[static_cast<size_t>(sel)].id;
                                slot.items = m_config.pinnedSlots[static_cast<size_t>(sel)].items;
                                m_config.pinnedSlots[static_cast<size_t>(sel)] = std::move(slot);
                                RefreshPinsList();
                                SendMessageW(m_lbPins, LB_SETCURSEL, static_cast<WPARAM>(sel), 0);
                            }
                        }
                        return 0;
                    }
                    case ID_BTN_PIN_REMOVE:
                    {
                        const int sel = static_cast<int>(SendMessageW(m_lbPins, LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(m_config.pinnedSlots.size()))
                        {
                            m_config.pinnedSlots.erase(m_config.pinnedSlots.begin() + sel);
                            RefreshPinsList();
                        }
                        return 0;
                    }
                    case ID_BTN_PIN_UP:
                    {
                        const int sel = static_cast<int>(SendMessageW(m_lbPins, LB_GETCURSEL, 0, 0));
                        if (sel > 0 && sel < static_cast<int>(m_config.pinnedSlots.size()))
                        {
                            std::swap(m_config.pinnedSlots[static_cast<size_t>(sel)],
                                m_config.pinnedSlots[static_cast<size_t>(sel - 1)]);
                            RefreshPinsList();
                            SendMessageW(m_lbPins, LB_SETCURSEL, static_cast<WPARAM>(sel - 1), 0);
                        }
                        return 0;
                    }
                    case ID_BTN_PIN_DOWN:
                    {
                        const int sel = static_cast<int>(SendMessageW(m_lbPins, LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel + 1 < static_cast<int>(m_config.pinnedSlots.size()))
                        {
                            std::swap(m_config.pinnedSlots[static_cast<size_t>(sel)],
                                m_config.pinnedSlots[static_cast<size_t>(sel + 1)]);
                            RefreshPinsList();
                            SendMessageW(m_lbPins, LB_SETCURSEL, static_cast<WPARAM>(sel + 1), 0);
                        }
                        return 0;
                    }
                    }
                    break;
                }

                case WM_DESTROY:
                    m_previewRt.Reset();
                    if (m_bgBrush) { DeleteObject(m_bgBrush); m_bgBrush = nullptr; }
                    if (m_editBrush) { DeleteObject(m_editBrush); m_editBrush = nullptr; }
                    if (m_font) { DeleteObject(m_font); m_font = nullptr; }
                    if (m_boldFont) { DeleteObject(m_boldFont); m_boldFont = nullptr; }
                    PostQuitMessage(0);
                    return 0;
                }
                return DefWindowProcW(m_hwnd, msg, wp, lp);
            }

            HINSTANCE m_instance = nullptr;
            HWND m_hwnd = nullptr;
            HWND m_tab = nullptr;
            HBRUSH m_bgBrush = nullptr;
            HBRUSH m_editBrush = nullptr;
            HFONT m_font = nullptr;
            HFONT m_boldFont = nullptr;
            bool m_populating = false;
            int m_activeTab = 0;

            std::vector<HWND> m_tabCtrls[4];
            SwitcherConfig m_config;

            ComPtr<ID2D1Factory> m_d2dFactory;
            ComPtr<ID2D1HwndRenderTarget> m_previewRt;

            // Controls
            HWND m_cbMouseTrigger{}, m_cbScrollMode{}, m_chkDoubleTap{}, m_edTapMs{}, m_edDTapMs{};
            HWND m_chkHotkey{}, m_chkModAlt{}, m_chkModCtrl{}, m_chkModShift{}, m_chkModWin{}, m_cbHotkeyVk{};
            HWND m_chkMenuKey{}, m_chkWinTab{}, m_chkAltTab{}, m_chkTopBarBtn{};

            HWND m_cbTheme{}, m_edAccent{}, m_edRadius{}, m_edDeadzone{}, m_edIconSize{}, m_edThumbScale{};
            HWND m_cbWinView{}, m_chkNoAnim{}, m_chkFanOne{}, m_chkPtrFollow{}, m_chkShiftCenter{}, m_chkIncludeUnpinned{};
            HWND m_edMaxSlots{}, m_edFfmpeg{}, m_ringCanvas{};

            HWND m_chkTopBarOn{}, m_chkDockTop{}, m_sldBarH{}, m_edBarH{}, m_chkKeepFont{}, m_chkClock1L{};
            HWND m_edClockFmt{}, m_cbClockCasing{}, m_lblClockPreview{};
            HWND m_chkHideApps{}, m_chkHideStart{}, m_chkHideSearch{}, m_chkHideTaskView{}, m_chkHideWidgets{};
            HWND m_cbBackdrop{}, m_chkAllMon{};

            HWND m_lbPins{}, m_cbPinKind{}, m_edPinLabel{}, m_edPinPath{}, m_edPinFolderMax{};
        };
    } // namespace

    int RunSettingsDialog(HINSTANCE instance)
    {
        SettingsWindow win;
        return win.Run(instance) ? 0 : 1;
    }
} // namespace winrings::tools
