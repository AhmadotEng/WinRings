#pragma once

#include <Windows.h>
#include <cstdint>

namespace winrings
{
    void ShellDiagLog(const wchar_t* tag, const wchar_t* fmt, ...) noexcept;

    class ShellController
    {
    public:
        static void BootstrapInExplorer(HMODULE shellModule);
        static int RunStandalonePreview(HMODULE shellModule);
        static bool RequestShutdownFromApi(HWND controlWindow) noexcept;
    };
} // namespace winrings
