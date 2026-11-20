#include <Windows.h>
#include <CommCtrl.h>
#include <ole2.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "../shared/WinRingsConfig.h"
#include "../shared/WinRingsIdentity.h"
#include "CompressTrimDlg.h"
#include "DropActions.h"
#include "RedactEditor.h"
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
    std::wstring explicitFmt;
    std::vector<std::wstring> files;

    if (argv && argc > 1)
    {
        mode = argv[1];
        for (int i = 2; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"--format") == 0 && i + 1 < argc)
                explicitFmt = argv[++i];
            else
                files.emplace_back(argv[i]);
        }
    }
    if (argv)
        LocalFree(argv);

    const winrings::SwitcherConfig cfg = winrings::SwitcherConfig::LoadOrCreate();

    if (_wcsicmp(mode.c_str(), L"--version") == 0)
    {
        exitCode = 0;
    }
    else if (_wcsicmp(mode.c_str(), L"--settings") == 0)
    {
        exitCode = winrings::tools::RunSettingsDialog(instance);
    }
    else if (_wcsicmp(mode.c_str(), L"--convert") == 0)
    {
        exitCode = winrings::tools::RunConvertUi(instance, files, explicitFmt, cfg);
    }
    else if (_wcsicmp(mode.c_str(), L"--compress") == 0)
    {
        exitCode = files.empty() ? 1 : winrings::tools::RunCompressDialog(instance, files[0], cfg);
    }
    else if (_wcsicmp(mode.c_str(), L"--trim") == 0)
    {
        exitCode = files.empty() ? 1 : winrings::tools::RunTrimDialog(instance, files[0], cfg);
    }
    else if (_wcsicmp(mode.c_str(), L"--redact") == 0)
    {
        exitCode = files.empty() ? 1 : winrings::tools::RunRedactEditor(instance, files[0]);
    }
    else if (_wcsicmp(mode.c_str(), L"--zip") == 0)
    {
        const auto res = winrings::tools::CreateZipArchive(files, nullptr);
        MessageBoxW(nullptr, res.summary.c_str(), L"WinRings Zip",
            res.ok ? (MB_ICONINFORMATION | MB_OK) : (MB_ICONWARNING | MB_OK));
        exitCode = res.ok ? 0 : 1;
    }
    else
    {
        exitCode = winrings::tools::RunSettingsDialog(instance);
    }

    if (SUCCEEDED(hrOle))
        OleUninitialize();
    return exitCode;
}
