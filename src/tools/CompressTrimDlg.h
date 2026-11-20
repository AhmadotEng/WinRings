#pragma once

#include <Windows.h>
#include <string>

#include "../shared/WinRingsConfig.h"

namespace winrings::tools
{
    int RunCompressDialog(HINSTANCE instance, const std::wstring& filePath, const SwitcherConfig& cfg);
    int RunTrimDialog(HINSTANCE instance, const std::wstring& filePath, const SwitcherConfig& cfg);
} // namespace winrings::tools
