#include "CompressTrimDlg.h"

#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <string>

#include "DropActions.h"

namespace winrings::tools
{
    namespace
    {
        struct CompressDlgState
        {
            std::wstring filePath;
            SwitcherConfig cfg;
            HWND radioPreset = nullptr;
            HWND radioMb = nullptr;
            HWND radioPct = nullptr;
            HWND comboPreset = nullptr;
            HWND editMb = nullptr;
            HWND editPct = nullptr;
            HBRUSH darkBg = nullptr;
            HBRUSH editBg = nullptr;
            HFONT font = nullptr;
        };

        struct TrimDlgState
        {
            std::wstring filePath;
            SwitcherConfig cfg;
            HWND editStart = nullptr;
            HWND editEnd = nullptr;
            HWND checkCopy = nullptr;
            HBRUSH darkBg = nullptr;
            HBRUSH editBg = nullptr;
            HFONT font = nullptr;
        };

        void ApplyDarkDialogFrame(HWND hwnd) noexcept
        {
            if (HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            {
                using SetPreferredAppModeFn = int (WINAPI*)(int);
                if (auto setMode = reinterpret_cast<SetPreferredAppModeFn>(
                    GetProcAddress(ux, MAKEINTRESOURCEA(135))))
                    setMode(2);
                FreeLibrary(ux);
            }
            const BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
            const int corner = 2; // DWMWCP_ROUND
            DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
            const COLORREF border = RGB(62, 62, 62);
            DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
        }

        std::wstring GetEditText(HWND edit)
        {
            const int len = GetWindowTextLengthW(edit);
            if (len <= 0)
                return {};
            std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
            GetWindowTextW(edit, buf.data(), len + 1);
            buf.resize(static_cast<size_t>(len));
            return buf;
        }

        LRESULT CALLBACK CompressWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            auto* st = reinterpret_cast<CompressDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            switch (msg)
            {
            case WM_NCCREATE:
                st = static_cast<CompressDlgState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
                break;

            case WM_CREATE:
            {
                ApplyDarkDialogFrame(hwnd);
                st->darkBg = CreateSolidBrush(RGB(28, 28, 30));
                st->editBg = CreateSolidBrush(RGB(46, 46, 52));
                st->font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");

                auto make = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, int id) {
                    HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style,
                        x, y, w, h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
                    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
                    SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
                    return c;
                };

                const std::wstring hdr = L"Compress media: " + st->filePath;
                make(L"STATIC", hdr.c_str(), SS_PATHELLIPSIS, 18, 14, 410, 22, 0);

                st->radioPreset = make(L"BUTTON", L"Quality preset:", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 18, 48, 150, 24, 101);
                st->comboPreset = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 180, 48, 180, 120, 102);
                SendMessageW(st->comboPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"high"));
                SendMessageW(st->comboPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"medium"));
                SendMessageW(st->comboPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"small"));
                SendMessageW(st->comboPreset, CB_SETCURSEL, 1, 0);

                st->radioMb = make(L"BUTTON", L"Target size (MB):", BS_AUTORADIOBUTTON | WS_TABSTOP, 18, 86, 150, 24, 103);
                st->editMb = make(L"EDIT", L"10", WS_BORDER | WS_TABSTOP, 180, 86, 100, 24, 104);

                st->radioPct = make(L"BUTTON", L"Target % of source:", BS_AUTORADIOBUTTON | WS_TABSTOP, 18, 124, 150, 24, 105);
                st->editPct = make(L"EDIT", L"50", WS_BORDER | WS_TABSTOP, 180, 124, 100, 24, 106);

                SendMessageW(st->radioPreset, BM_SETCHECK, BST_CHECKED, 0);

                make(L"BUTTON", L"Compress", BS_DEFPUSHBUTTON | WS_TABSTOP, 228, 168, 100, 28, IDOK);
                make(L"BUTTON", L"Cancel", WS_TABSTOP, 336, 168, 92, 28, IDCANCEL);
                return 0;
            }

            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORBTN:
            {
                HDC hdc = reinterpret_cast<HDC>(wp);
                SetTextColor(hdc, RGB(244, 244, 246));
                SetBkColor(hdc, RGB(28, 28, 30));
                return reinterpret_cast<LRESULT>(st->darkBg);
            }

            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX:
            {
                HDC hdc = reinterpret_cast<HDC>(wp);
                SetTextColor(hdc, RGB(244, 244, 246));
                SetBkColor(hdc, RGB(46, 46, 52));
                return reinterpret_cast<LRESULT>(st->editBg);
            }

            case WM_ERASEBKGND:
            {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                FillRect(reinterpret_cast<HDC>(wp), &rc, st->darkBg);
                return TRUE;
            }

            case WM_COMMAND:
                if (LOWORD(wp) == IDOK)
                {
                    int mode = 0;
                    std::wstring val = L"medium";
                    if (SendMessageW(st->radioMb, BM_GETCHECK, 0, 0) == BST_CHECKED)
                    {
                        mode = 1;
                        val = GetEditText(st->editMb);
                    }
                    else if (SendMessageW(st->radioPct, BM_GETCHECK, 0, 0) == BST_CHECKED)
                    {
                        mode = 2;
                        val = GetEditText(st->editPct);
                    }
                    else
                    {
                        wchar_t buf[32]{};
                        const LRESULT sel = SendMessageW(st->comboPreset, CB_GETCURSEL, 0, 0);
                        if (sel != CB_ERR)
                        {
                            SendMessageW(st->comboPreset, CB_GETLBTEXT, static_cast<WPARAM>(sel), reinterpret_cast<LPARAM>(buf));
                            val = buf;
                        }
                    }
                    const ActionResult res = CompressMediaFile(st->filePath, st->cfg, mode, val);
                    MessageBoxW(hwnd, res.summary.c_str(), L"WinRings Compress",
                        res.ok ? (MB_ICONINFORMATION | MB_OK) : (MB_ICONWARNING | MB_OK));
                    if (res.ok)
                        DestroyWindow(hwnd);
                    return 0;
                }
                if (LOWORD(wp) == IDCANCEL)
                {
                    DestroyWindow(hwnd);
                    return 0;
                }
                break;

            case WM_DESTROY:
                if (st->darkBg) { DeleteObject(st->darkBg); st->darkBg = nullptr; }
                if (st->editBg) { DeleteObject(st->editBg); st->editBg = nullptr; }
                if (st->font) { DeleteObject(st->font); st->font = nullptr; }
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        LRESULT CALLBACK TrimWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            auto* st = reinterpret_cast<TrimDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            switch (msg)
            {
            case WM_NCCREATE:
                st = static_cast<TrimDlgState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
                break;

            case WM_CREATE:
            {
                ApplyDarkDialogFrame(hwnd);
                st->darkBg = CreateSolidBrush(RGB(28, 28, 30));
                st->editBg = CreateSolidBrush(RGB(46, 46, 52));
                st->font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");

                auto make = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, int id) {
                    HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style,
                        x, y, w, h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
                    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
                    SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
                    return c;
                };

                const std::wstring hdr = L"Trim media: " + st->filePath;
                make(L"STATIC", hdr.c_str(), SS_PATHELLIPSIS, 18, 14, 390, 22, 0);

                make(L"STATIC", L"Start time (HH:MM:SS):", 0, 18, 50, 160, 22, 0);
                st->editStart = make(L"EDIT", L"00:00:00", WS_BORDER | WS_TABSTOP, 190, 48, 150, 24, 101);

                make(L"STATIC", L"End time (HH:MM:SS):", 0, 18, 86, 160, 22, 0);
                st->editEnd = make(L"EDIT", L"00:00:15", WS_BORDER | WS_TABSTOP, 190, 84, 150, 24, 102);

                st->checkCopy = make(L"BUTTON", L"Fast stream copy (-c copy, no re-encode)",
                    BS_AUTOCHECKBOX | WS_TABSTOP, 18, 120, 320, 24, 103);
                SendMessageW(st->checkCopy, BM_SETCHECK, BST_CHECKED, 0);

                make(L"BUTTON", L"Trim", BS_DEFPUSHBUTTON | WS_TABSTOP, 210, 160, 96, 28, IDOK);
                make(L"BUTTON", L"Cancel", WS_TABSTOP, 314, 160, 94, 28, IDCANCEL);
                return 0;
            }

            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORBTN:
            {
                HDC hdc = reinterpret_cast<HDC>(wp);
                SetTextColor(hdc, RGB(244, 244, 246));
                SetBkColor(hdc, RGB(28, 28, 30));
                return reinterpret_cast<LRESULT>(st->darkBg);
            }

            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX:
            {
                HDC hdc = reinterpret_cast<HDC>(wp);
                SetTextColor(hdc, RGB(244, 244, 246));
                SetBkColor(hdc, RGB(46, 46, 52));
                return reinterpret_cast<LRESULT>(st->editBg);
            }

            case WM_ERASEBKGND:
            {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                FillRect(reinterpret_cast<HDC>(wp), &rc, st->darkBg);
                return TRUE;
            }

            case WM_COMMAND:
                if (LOWORD(wp) == IDOK)
                {
                    const std::wstring start = GetEditText(st->editStart);
                    const std::wstring end = GetEditText(st->editEnd);
                    const bool copy = SendMessageW(st->checkCopy, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    const ActionResult res = TrimMediaFile(st->filePath, st->cfg, start, end, copy);
                    MessageBoxW(hwnd, res.summary.c_str(), L"WinRings Trim",
                        res.ok ? (MB_ICONINFORMATION | MB_OK) : (MB_ICONWARNING | MB_OK));
                    if (res.ok)
                        DestroyWindow(hwnd);
                    return 0;
                }
                if (LOWORD(wp) == IDCANCEL)
                {
                    DestroyWindow(hwnd);
                    return 0;
                }
                break;

            case WM_DESTROY:
                if (st->darkBg) { DeleteObject(st->darkBg); st->darkBg = nullptr; }
                if (st->editBg) { DeleteObject(st->editBg); st->editBg = nullptr; }
                if (st->font) { DeleteObject(st->font); st->font = nullptr; }
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    } // namespace

    int RunCompressDialog(HINSTANCE instance, const std::wstring& filePath, const SwitcherConfig& cfg)
    {
        if (filePath.empty())
            return 1;
        CompressDlgState st{};
        st.filePath = filePath;
        st.cfg = cfg;

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = CompressWndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"WinRings.CompressDlg.v1";
        RegisterClassExW(&wc);

        RECT wa{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const int w = 460, h = 250;
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"WinRings — Compress Media",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            wa.left + (wa.right - wa.left - w) / 2,
            wa.top + (wa.bottom - wa.top - h) / 2,
            w, h, nullptr, nullptr, instance, &st);
        if (!hwnd)
            return 1;
        ShowWindow(hwnd, SW_SHOWNORMAL);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            if (!IsDialogMessageW(hwnd, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        return 0;
    }

    int RunTrimDialog(HINSTANCE instance, const std::wstring& filePath, const SwitcherConfig& cfg)
    {
        if (filePath.empty())
            return 1;
        TrimDlgState st{};
        st.filePath = filePath;
        st.cfg = cfg;

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = TrimWndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"WinRings.TrimDlg.v1";
        RegisterClassExW(&wc);

        RECT wa{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const int w = 440, h = 240;
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"WinRings — Trim Media",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            wa.left + (wa.right - wa.left - w) / 2,
            wa.top + (wa.bottom - wa.top - h) / 2,
            w, h, nullptr, nullptr, instance, &st);
        if (!hwnd)
            return 1;
        ShowWindow(hwnd, SW_SHOWNORMAL);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            if (!IsDialogMessageW(hwnd, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        return 0;
    }
} // namespace winrings::tools
