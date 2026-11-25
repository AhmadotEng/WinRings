#include <Windows.h>
#include <CommCtrl.h>
#include <bcrypt.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <uxtheme.h>
#include <wrl/client.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../../build/gen/payload_hashes.h"
#include "../shared/WinRingsIdentity.h"

namespace
{
    using Microsoft::WRL::ComPtr;

    constexpr int ID_CHK_STARTUP = 101;
    constexpr int ID_CHK_LAUNCH = 102;
    constexpr int ID_LBL_STATUS = 104;
    constexpr int ID_BTN_INSTALL = 105;
    constexpr int ID_BTN_UNINSTALL = 106;
    constexpr int ID_BTN_CLOSE = 107;

    std::wstring GetEnvFolderPath(REFKNOWNFOLDERID rfid)
    {
        PWSTR p = nullptr;
        std::wstring out;
        if (SUCCEEDED(SHGetKnownFolderPath(rfid, 0, nullptr, &p)) && p)
        {
            out = p;
            CoTaskMemFree(p);
        }
        return out;
    }

    std::wstring GetInstallRootDir()
    {
        return GetEnvFolderPath(FOLDERID_LocalAppData) + L"\\" + WINRINGS_PRODUCT_NAME_W;
    }

    std::wstring GetInstallVersionDir()
    {
        return GetInstallRootDir() + L"\\v" + WINRINGS_PRODUCT_VERSION_STR_W;
    }

    std::wstring GetStartMenuShortcutPath()
    {
        return GetEnvFolderPath(FOLDERID_Programs) + L"\\WinRings.lnk";
    }

    bool FileExists(const std::wstring& path) noexcept
    {
        const DWORD attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    std::wstring ComputeSha256Hex(const BYTE* data, DWORD size)
    {
        BCRYPT_ALG_HANDLE hAlg = nullptr;
        BCRYPT_HASH_HANDLE hHash = nullptr;
        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
            return {};

        DWORD objLen = 0, cbData = 0;
        BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cbData, 0);
        std::vector<BYTE> hashObj(objLen);
        BYTE digest[32]{};
        std::wstring hex;

        if (BCryptCreateHash(hAlg, &hHash, hashObj.data(), objLen, nullptr, 0, 0) == 0 &&
            BCryptHashData(hHash, const_cast<PUCHAR>(data), size, 0) == 0 &&
            BCryptFinishHash(hHash, digest, sizeof(digest), 0) == 0)
        {
            static constexpr wchar_t kHex[] = L"0123456789abcdef";
            hex.reserve(64);
            for (BYTE b : digest)
            {
                hex.push_back(kHex[(b >> 4) & 0x0F]);
                hex.push_back(kHex[b & 0x0F]);
            }
        }

        if (hHash) BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return hex;
    }

    bool ExtractVerifiedPayload(HINSTANCE inst, UINT resId, const wchar_t* expectedSha256,
        const std::wstring& destPath, std::wstring& outError)
    {
        HRSRC hr = FindResourceW(inst, MAKEINTRESOURCEW(resId), RT_RCDATA);
        if (!hr)
        {
            outError = L"Embedded payload resource missing (ID " + std::to_wstring(resId) + L").";
            return false;
        }
        HGLOBAL hg = LoadResource(inst, hr);
        const DWORD size = SizeofResource(inst, hr);
        const void* ptr = hg ? LockResource(hg) : nullptr;
        if (!ptr || size == 0)
        {
            outError = L"Failed reading embedded payload (ID " + std::to_wstring(resId) + L").";
            return false;
        }

        const std::wstring actualHash = ComputeSha256Hex(static_cast<const BYTE*>(ptr), size);
        if (_wcsicmp(actualHash.c_str(), expectedSha256) != 0)
        {
            outError = L"SHA-256 verification failed for payload -> " + destPath;
            return false;
        }

        const size_t slash = destPath.find_last_of(L"\\/");
        const std::wstring destDir = (slash != std::wstring::npos) ? destPath.substr(0, slash + 1) : L"";
        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileW((destPath + L".old*").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do
            {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                    DeleteFileW((destDir + fd.cFileName).c_str());
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }

        const std::wstring tmpPath = destPath + L".tmp";
        HANDLE f = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
        {
            outError = L"Failed opening output file: " + destPath;
            return false;
        }

        DWORD written = 0;
        const BOOL writeOk = WriteFile(f, ptr, size, &written, nullptr) && written == size;
        FlushFileBuffers(f);
        CloseHandle(f);

        bool replaced = writeOk && MoveFileExW(tmpPath.c_str(), destPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!replaced && writeOk && FileExists(destPath))
        {
            // Support hot upgrade even when the existing file is mapped/locked (e.g. WinRingsShell.dll in Explorer)
            const std::wstring oldPath = destPath + L".old." + std::to_wstring(GetTickCount64());
            if (MoveFileExW(destPath.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                if (!DeleteFileW(oldPath.c_str()))
                    MoveFileExW(oldPath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
                replaced = MoveFileExW(tmpPath.c_str(), destPath.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            }
        }
        if (!replaced)
        {
            DeleteFileW(tmpPath.c_str());
            outError = L"Failed replacing file (close running WinRings or Explorer if locked): " + destPath;
            return false;
        }
        return true;
    }

    void StopRunningWinRingsHost(const std::wstring& installDir)
    {
        const UINT wakeMsg = RegisterWindowMessageW(WINRINGS_TRAY_WAKE_MESSAGE_W);
        if (wakeMsg)
        {
            HWND trayHwnd = FindWindowW(WINRINGS_TRAY_WINDOW_CLASS_W, nullptr);
            if (trayHwnd)
                SendMessageTimeoutW(trayHwnd, wakeMsg, 2 /* quit */, 0, SMTO_ABORTIFHUNG, 1500, nullptr);
        }

        if (HWND legacyV2 = FindWindowW(L"WinRings.Settings.v2", nullptr))
            SendMessageTimeoutW(legacyV2, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 500, nullptr);
        if (HWND legacyDlg = FindWindowW(L"#32770", L"WinRings Settings"))
            SendMessageTimeoutW(legacyDlg, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 500, nullptr);

        const std::wstring hostExe = installDir + L"\\" + WINRINGS_HOST_FILENAME_W;
        if (FileExists(hostExe))
        {
            STARTUPINFOW si{ sizeof(si) };
            PROCESS_INFORMATION pi{};
            std::wstring cmd = L"\"" + hostExe + L"\" --exit";
            std::vector<wchar_t> buf(cmd.begin(), cmd.end());
            buf.push_back(L'\0');
            if (CreateProcessW(hostExe.c_str(), buf.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            {
                WaitForSingleObject(pi.hProcess, 4000);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }

        const UINT ctrlMsg = RegisterWindowMessageW(WINRINGS_HOST_CONTROL_MESSAGE_W);
        if (ctrlMsg)
        {
            HWND hostHwnd = FindWindowW(WINRINGS_HOST_WINDOW_CLASS_W, WINRINGS_HOST_WINDOW_TITLE_W);
            if (hostHwnd)
                SendMessageTimeoutW(hostHwnd, ctrlMsg, 2 /* WINRINGS_HOST_CTRL_EXIT */, 0, SMTO_ABORTIFHUNG, 2500, nullptr);
        }

        // Wait briefly for running WinRingsHost.exe and WinRingsTools.exe processes to exit gracefully
        bool anyRemaining = false;
        for (int attempt = 0; attempt < 12; ++attempt)
        {
            anyRemaining = false;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap != INVALID_HANDLE_VALUE)
            {
                PROCESSENTRY32W pe{ sizeof(pe) };
                for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe))
                {
                    if (_wcsicmp(pe.szExeFile, WINRINGS_HOST_FILENAME_W) == 0 ||
                        _wcsicmp(pe.szExeFile, WINRINGS_TOOLS_FILENAME_W) == 0)
                    {
                        anyRemaining = true;
                        break;
                    }
                }
                CloseHandle(snap);
            }
            if (!anyRemaining)
                break;
            Sleep(200);
        }

        if (anyRemaining)
        {
            const DWORD selfPid = GetCurrentProcessId();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap != INVALID_HANDLE_VALUE)
            {
                PROCESSENTRY32W pe{ sizeof(pe) };
                for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe))
                {
                    if (pe.th32ProcessID == selfPid)
                        continue;
                    if (_wcsicmp(pe.szExeFile, WINRINGS_HOST_FILENAME_W) == 0 ||
                        _wcsicmp(pe.szExeFile, WINRINGS_TOOLS_FILENAME_W) == 0)
                    {
                        if (HANDLE proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID))
                        {
                            TerminateProcess(proc, 0);
                            WaitForSingleObject(proc, 1500);
                            CloseHandle(proc);
                        }
                    }
                }
                CloseHandle(snap);
            }
        }
    }

    bool CreateStartMenuShortcut(const std::wstring& targetExe)
    {
        ComPtr<IShellLinkW> link;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(link.ReleaseAndGetAddressOf()))))
            return false;

        link->SetPath(targetExe.c_str());
        link->SetDescription(L"WinRings Radial Switcher");

        ComPtr<IPersistFile> pf;
        if (FAILED(link.As(&pf)))
            return false;

        const std::wstring lnkPath = GetStartMenuShortcutPath();
        return SUCCEEDED(pf->Save(lnkPath.c_str(), TRUE));
    }

    bool WriteRegistryStrings(const std::wstring& installDir, bool addRunEntry)
    {
        const std::wstring hostExe = installDir + L"\\" + WINRINGS_HOST_FILENAME_W;
        const std::wstring toolsExe = installDir + L"\\" + WINRINGS_TOOLS_FILENAME_W;
        const std::wstring setupExe = installDir + L"\\" + WINRINGS_SETUP_FILENAME_W;

        if (addRunEntry)
        {
            HKEY runKey = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                0, nullptr, 0, KEY_SET_VALUE, nullptr, &runKey, nullptr) == ERROR_SUCCESS)
            {
                const std::wstring hostCmd = L"\"" + hostExe + L"\"";
                RegSetValueExW(runKey, WINRINGS_PRODUCT_NAME_W, 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(hostCmd.c_str()),
                    static_cast<DWORD>((hostCmd.size() + 1) * sizeof(wchar_t)));

                const std::wstring trayCmd = L"\"" + toolsExe + L"\" --tray";
                RegSetValueExW(runKey, L"WinRingsTray", 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(trayCmd.c_str()),
                    static_cast<DWORD>((trayCmd.size() + 1) * sizeof(wchar_t)));
                RegCloseKey(runKey);
            }
        }
        else
        {
            HKEY runKey = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                0, KEY_SET_VALUE, &runKey) == ERROR_SUCCESS)
            {
                RegDeleteValueW(runKey, WINRINGS_PRODUCT_NAME_W);
                RegDeleteValueW(runKey, L"WinRingsTray");
                RegCloseKey(runKey);
            }
        }

        HKEY uninstKey = nullptr;
        const LSTATUS st = RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\WinRings",
            0, nullptr, 0, KEY_SET_VALUE, nullptr, &uninstKey, nullptr);
        if (st != ERROR_SUCCESS)
            return false;

        auto setStr = [&](const wchar_t* name, const std::wstring& val) {
            RegSetValueExW(uninstKey, name, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(val.c_str()),
                static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));
        };
        auto setDword = [&](const wchar_t* name, DWORD val) {
            RegSetValueExW(uninstKey, name, 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&val), sizeof(val));
        };

        setStr(L"DisplayName", L"WinRings");
        setStr(L"DisplayVersion", WINRINGS_PRODUCT_VERSION_STR_W);
        setStr(L"Publisher", L"WinRings");
        setStr(L"InstallLocation", installDir);
        setStr(L"UninstallString", L"\"" + setupExe + L"\" --uninstall");
        setStr(L"DisplayIcon", hostExe + L",0");
        setDword(L"NoModify", 1);
        setDword(L"NoRepair", 0);
        RegCloseKey(uninstKey);
        return true;
    }

    bool PerformInstall(HINSTANCE inst, bool addStartup, bool launchAfter, std::wstring& outMessage)
    {
        const std::wstring installDir = GetInstallVersionDir();
        SHCreateDirectoryExW(nullptr, installDir.c_str(), nullptr);

        StopRunningWinRingsHost(installDir);

        std::wstring err;
        if (!ExtractVerifiedPayload(inst, IDR_PAYLOAD_HOST, kHashWinRingsHost,
                installDir + L"\\" + WINRINGS_HOST_FILENAME_W, err) ||
            !ExtractVerifiedPayload(inst, IDR_PAYLOAD_SHELL, kHashWinRingsShell,
                installDir + L"\\" + WINRINGS_SHELL_FILENAME_W, err) ||
            !ExtractVerifiedPayload(inst, IDR_PAYLOAD_TOOLS, kHashWinRingsTools,
                installDir + L"\\" + WINRINGS_TOOLS_FILENAME_W, err))
        {
            outMessage = err;
            return false;
        }

        // Copy Setup executable for Control Panel / Settings uninstall
        wchar_t selfPath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, selfPath, _countof(selfPath)) > 0)
        {
            const std::wstring installedSetup = installDir + L"\\" + WINRINGS_SETUP_FILENAME_W;
            if (_wcsicmp(selfPath, installedSetup.c_str()) != 0)
                CopyFileW(selfPath, installedSetup.c_str(), FALSE);
        }

        WriteRegistryStrings(installDir, addStartup);
        RegDeleteTreeW(HKEY_CURRENT_USER, WINRINGS_HOST_SAFETY_REGISTRY_W);
        CreateStartMenuShortcut(installDir + L"\\" + WINRINGS_TOOLS_FILENAME_W);

        if (launchAfter)
        {
            auto spawnExe = [&](const std::wstring& exePath, const std::wstring& args) {
                STARTUPINFOW si{ sizeof(si) };
                PROCESS_INFORMATION pi{};
                std::wstring cmd = L"\"" + exePath + L"\"" + (args.empty() ? L"" : (L" " + args));
                std::vector<wchar_t> buf(cmd.begin(), cmd.end());
                buf.push_back(L'\0');
                if (CreateProcessW(exePath.c_str(), buf.data(), nullptr, nullptr, FALSE,
                    0, nullptr, installDir.c_str(), &si, &pi))
                {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                }
            };
            spawnExe(installDir + L"\\" + WINRINGS_HOST_FILENAME_W, L"");
            spawnExe(installDir + L"\\" + WINRINGS_TOOLS_FILENAME_W, L"--tray");
        }

        outMessage = L"Installed WinRings v" WINRINGS_PRODUCT_VERSION_STR_W L" to:\n" + installDir;
        return true;
    }

    bool PerformUninstall(std::wstring& outMessage)
    {
        const std::wstring installDir = GetInstallVersionDir();
        StopRunningWinRingsHost(installDir);

        HKEY runKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_SET_VALUE, &runKey) == ERROR_SUCCESS)
        {
            RegDeleteValueW(runKey, WINRINGS_PRODUCT_NAME_W);
            RegDeleteValueW(runKey, L"WinRingsTray");
            RegCloseKey(runKey);
        }
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\WinRings");
        DeleteFileW(GetStartMenuShortcutPath().c_str());

        bool scheduledRebootCleanup = false;
        const wchar_t* files[] = {
            WINRINGS_HOST_FILENAME_W,
            WINRINGS_TOOLS_FILENAME_W,
            WINRINGS_SHELL_FILENAME_W,
            WINRINGS_SETUP_FILENAME_W
        };
        for (const wchar_t* fn : files)
        {
            const std::wstring full = installDir + L"\\" + fn;
            if (FileExists(full))
            {
                if (!DeleteFileW(full.c_str()))
                {
                    MoveFileExW(full.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
                    scheduledRebootCleanup = true;
                }
            }
        }
        if (!RemoveDirectoryW(installDir.c_str()))
            MoveFileExW(installDir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);

        outMessage = L"WinRings has been uninstalled.";
        if (scheduledRebootCleanup)
            outMessage += L"\nNote: WinRingsShell.dll remains mapped in Explorer until next Windows sign-out/restart and is scheduled for automatic cleanup.";
        return true;
    }

    struct SetupUiState
    {
        HINSTANCE instance = nullptr;
        HBRUSH darkBg = nullptr;
        HFONT font = nullptr;
        HFONT boldFont = nullptr;
        HWND chkStartup = nullptr;
        HWND chkLaunch = nullptr;
        HWND lblStatus = nullptr;
    };

    LRESULT CALLBACK SetupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* st = reinterpret_cast<SetupUiState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg)
        {
        case WM_NCCREATE:
            st = static_cast<SetupUiState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
            break;

        case WM_CREATE:
        {
            const BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
            st->darkBg = CreateSolidBrush(RGB(22, 25, 32));
            st->font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            st->boldFont = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

            auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, int id) {
                HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style,
                    x, y, w, h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), st->instance, nullptr);
                SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
                SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
                return c;
            };

            HWND title = mk(L"STATIC", L"WinRings Setup (v" WINRINGS_PRODUCT_VERSION_STR_W L")", 0, 22, 18, 460, 28, 0);
            SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(st->boldFont), TRUE);

            const std::wstring pathLine = L"Install location: " + GetInstallVersionDir();
            mk(L"STATIC", pathLine.c_str(), SS_PATHELLIPSIS, 22, 54, 490, 22, 0);

            st->chkStartup = mk(L"BUTTON", L"Start WinRings automatically at sign-in", BS_AUTOCHECKBOX | WS_TABSTOP, 22, 92, 460, 24, ID_CHK_STARTUP);
            SendMessageW(st->chkStartup, BM_SETCHECK, BST_CHECKED, 0);

            st->chkLaunch = mk(L"BUTTON", L"Launch WinRings immediately after installation", BS_AUTOCHECKBOX | WS_TABSTOP, 22, 122, 460, 24, ID_CHK_LAUNCH);
            SendMessageW(st->chkLaunch, BM_SETCHECK, BST_CHECKED, 0);

            st->lblStatus = mk(L"STATIC", L"Ready. 100% pure native C++17 (x64).", 0, 22, 166, 490, 64, ID_LBL_STATUS);

            mk(L"BUTTON", L"Install / Repair", BS_DEFPUSHBUTTON | WS_TABSTOP, 22, 274, 145, 32, ID_BTN_INSTALL);
            mk(L"BUTTON", L"Uninstall", WS_TABSTOP, 178, 274, 110, 32, ID_BTN_UNINSTALL);
            mk(L"BUTTON", L"Close", WS_TABSTOP, 412, 274, 100, 32, ID_BTN_CLOSE);
            return 0;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetTextColor(hdc, RGB(232, 236, 246));
            SetBkColor(hdc, RGB(22, 25, 32));
            return reinterpret_cast<LRESULT>(st->darkBg);
        }

        case WM_ERASEBKGND:
        {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, st->darkBg);
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wp))
            {
            case ID_BTN_INSTALL:
            {
                const bool addStartup = SendMessageW(st->chkStartup, BM_GETCHECK, 0, 0) == BST_CHECKED;
                const bool launch = SendMessageW(st->chkLaunch, BM_GETCHECK, 0, 0) == BST_CHECKED;
                std::wstring statusMsg;
                const bool ok = PerformInstall(st->instance, addStartup, launch, statusMsg);
                SetWindowTextW(st->lblStatus, statusMsg.c_str());
                MessageBoxW(hwnd, statusMsg.c_str(), L"WinRings Setup",
                    ok ? (MB_ICONINFORMATION | MB_OK) : (MB_ICONERROR | MB_OK));
                return 0;
            }
            case ID_BTN_UNINSTALL:
            {
                std::wstring statusMsg;
                const bool ok = PerformUninstall(statusMsg);
                SetWindowTextW(st->lblStatus, statusMsg.c_str());
                MessageBoxW(hwnd, statusMsg.c_str(), L"WinRings Setup",
                    ok ? (MB_ICONINFORMATION | MB_OK) : (MB_ICONWARNING | MB_OK));
                return 0;
            }
            case ID_BTN_CLOSE:
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_DESTROY:
            if (st->darkBg) { DeleteObject(st->darkBg); st->darkBg = nullptr; }
            if (st->font) { DeleteObject(st->font); st->font = nullptr; }
            if (st->boldFont) { DeleteObject(st->boldFont); st->boldFont = nullptr; }
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool flagVersion = false;
    bool flagInstall = false;
    bool flagRepair = false;
    bool flagUninstall = false;
    bool flagSilent = false;
    bool flagNoLaunch = false;

    if (argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"--version") == 0) flagVersion = true;
            else if (_wcsicmp(argv[i], L"--install") == 0) flagInstall = true;
            else if (_wcsicmp(argv[i], L"--repair") == 0) flagRepair = true;
            else if (_wcsicmp(argv[i], L"--uninstall") == 0) flagUninstall = true;
            else if (_wcsicmp(argv[i], L"--silent") == 0) flagSilent = true;
            else if (_wcsicmp(argv[i], L"--no-launch") == 0) flagNoLaunch = true;
        }
        LocalFree(argv);
    }

    if (flagVersion)
        return 0;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, WINRINGS_SETUP_MUTEX_W);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(mutex);
        if (!flagSilent)
            MessageBoxW(nullptr, L"Another WinRings Setup instance is already running.", L"WinRings Setup", MB_ICONINFORMATION | MB_OK);
        return 1;
    }

    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int exitCode = 0;

    if (flagInstall || flagRepair)
    {
        std::wstring msg;
        const bool ok = PerformInstall(instance, true, !flagNoLaunch, msg);
        if (!flagSilent)
            MessageBoxW(nullptr, msg.c_str(), L"WinRings Setup", ok ? (MB_ICONINFORMATION | MB_OK) : (MB_ICONERROR | MB_OK));
        exitCode = ok ? 0 : 1;
    }
    else if (flagUninstall)
    {
        std::wstring msg;
        const bool ok = PerformUninstall(msg);
        if (!flagSilent)
            MessageBoxW(nullptr, msg.c_str(), L"WinRings Setup", ok ? (MB_ICONINFORMATION | MB_OK) : (MB_ICONWARNING | MB_OK));
        exitCode = ok ? 0 : 1;
    }
    else
    {
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icc);

        SetupUiState st{};
        st.instance = instance;

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = SetupWndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"WinRings.Setup.v1";
        RegisterClassExW(&wc);

        RECT wa{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const int w = 550, h = 368;
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"WinRings Setup",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            wa.left + (wa.right - wa.left - w) / 2,
            wa.top + (wa.bottom - wa.top - h) / 2,
            w, h, nullptr, nullptr, instance, &st);
        if (hwnd)
        {
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
        }
        else
        {
            exitCode = 1;
        }
    }

    if (SUCCEEDED(hrCo))
        CoUninitialize();
    if (mutex)
        CloseHandle(mutex);
    return exitCode;
}
