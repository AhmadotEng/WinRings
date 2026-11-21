#include <Windows.h>
#include <CommCtrl.h>
#include <ole2.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "../shared/WinRingsConfig.h"
#include "../shared/WinRingsIdentity.h"
#include "SettingsDialog.h"

namespace
{
    void EnsureCommonControls()
    {
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icc);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    const HRESULT hrOle = OleInitialize(nullptr);
    EnsureCommonControls();

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int exitCode = 0;

    std::wstring mode = L"--settings";
    if (argv && argc > 1)
        mode = argv[1];
    if (argv)
        LocalFree(argv);

    if (_wcsicmp(mode.c_str(), L"--version") == 0)
        exitCode = 0;
    else
        exitCode = winrings::tools::RunSettingsDialog(instance);

    if (SUCCEEDED(hrOle))
        OleUninitialize();
    return exitCode;
}
