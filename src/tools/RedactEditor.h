#pragma once

#include <Windows.h>
#include <string>

namespace winrings::tools
{
    int RunRedactEditor(HINSTANCE instance, const std::wstring& imagePath);
} // namespace winrings::tools
