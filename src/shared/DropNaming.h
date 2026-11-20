#pragma once

#include <Windows.h>
#include <string>

namespace winrings
{
    inline std::wstring BuildNonDestructiveOutputPath(
        const std::wstring& sourcePath,
        const std::wstring& suffix,
        const std::wstring& newExtension = L"")
    {
        const size_t slashPos = sourcePath.find_last_of(L"\\/");
        std::wstring dir = (slashPos != std::wstring::npos) ? sourcePath.substr(0, slashPos) : L".";
        std::wstring fileName = (slashPos != std::wstring::npos) ? sourcePath.substr(slashPos + 1) : sourcePath;

        const size_t dotPos = fileName.find_last_of(L'.');
        std::wstring stem = (dotPos != std::wstring::npos) ? fileName.substr(0, dotPos) : fileName;
        std::wstring origExt = (dotPos != std::wstring::npos) ? fileName.substr(dotPos) : L"";

        std::wstring ext = newExtension.empty() ? origExt : newExtension;
        if (!ext.empty() && ext[0] != L'.')
            ext.insert(ext.begin(), L'.');

        auto fileExists = [](const std::wstring& p) noexcept -> bool
        {
            const DWORD attrs = GetFileAttributesW(p.c_str());
            return attrs != INVALID_FILE_ATTRIBUTES;
        };

        std::wstring candidate = dir + L"\\" + stem + L" (" + suffix + L")" + ext;
        int counter = 2;
        while (fileExists(candidate))
        {
            candidate = dir + L"\\" + stem + L" (" + suffix + L" " + std::to_wstring(counter++) + L")" + ext;
        }
        return candidate;
    }
} // namespace winrings
