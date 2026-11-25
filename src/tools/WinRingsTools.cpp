#include <Windows.h>
#include <CommCtrl.h>
#include <ole2.h>
#include <shellapi.h>

#include <string>

#include "../shared/WinRingsIdentity.h"
#include "TrayApp.h"

namespace
{
    void EnsureCommonControls()
    {
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icc);
    }

    int SendWakeToExistingTray(WPARAM cmd)
    {
        const UINT wakeMsg = RegisterWindowMessageW(WINRINGS_TRAY_WAKE_MESSAGE_W);
        HWND trayHwnd = FindWindowW(WINRINGS_TRAY_WINDOW_CLASS_W, nullptr);
        if (!trayHwnd)
        {
            for (int i = 0; i < 20 && !trayHwnd; ++i)
            {
                Sleep(25);
                trayHwnd = FindWindowW(WINRINGS_TRAY_WINDOW_CLASS_W, nullptr);
            }
        }
        if (!trayHwnd || !wakeMsg)
            return ERROR_SERVICE_NOT_ACTIVE;

        DWORD pid = 0;
        GetWindowThreadProcessId(trayHwnd, &pid);
        if (pid)
            AllowSetForegroundWindow(pid);

        DWORD_PTR res = 0;
        if (!SendMessageTimeoutW(trayHwnd, wakeMsg, cmd, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &res))
            return static_cast<int>(GetLastError());
        return 0;
    }
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    const HRESULT hrOle = OleInitialize(nullptr);
    EnsureCommonControls();

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring mode = L"--open";
    if (argv && argc > 1)
        mode = argv[1];
    if (argv)
        LocalFree(argv);

    int exitCode = 0;
    if (_wcsicmp(mode.c_str(), L"--version") == 0)
    {
        exitCode = 0;
    }
    else if (_wcsicmp(mode.c_str(), L"--quit") == 0)
    {
        const int rc = SendWakeToExistingTray(2);
        exitCode = (rc == ERROR_SERVICE_NOT_ACTIVE) ? 0 : rc;
    }
    else
    {
        const bool startHidden = (_wcsicmp(mode.c_str(), L"--tray") == 0);
        SetLastError(ERROR_SUCCESS);
        HANDLE mutex = CreateMutexW(nullptr, TRUE, WINRINGS_TRAY_MUTEX_W);
        const DWORD err = GetLastError();
        if (mutex && (err == ERROR_ALREADY_EXISTS || err == ERROR_ACCESS_DENIED))
        {
            CloseHandle(mutex);
            exitCode = startHidden ? 0 : SendWakeToExistingTray(1);
        }
        else if (!mutex && err == ERROR_ACCESS_DENIED)
        {
            exitCode = startHidden ? 0 : SendWakeToExistingTray(1);
        }
        else
        {
            exitCode = winrings::tools::RunTrayApp(instance, !startHidden);
            if (mutex)
            {
                ReleaseMutex(mutex);
                CloseHandle(mutex);
            }
        }
    }

    if (SUCCEEDED(hrOle))
        OleUninitialize();
    return exitCode;
}
