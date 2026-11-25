#pragma once

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

#include "RadialMath.h"
#include "SearchMatch.h"
#include "WinRingsConfig.h"

namespace winrings
{
    inline bool IsExplorerExecutablePath(const std::wstring& path) noexcept
    {
        if (path.empty())
            return false;
        const size_t slash = path.find_last_of(L"\\/");
        const wchar_t* name = (slash != std::wstring::npos) ? (path.c_str() + slash + 1) : path.c_str();
        return _wcsicmp(name, L"explorer.exe") == 0;
    }

    inline bool IsSystemSettingsExePath(const std::wstring& exePath) noexcept
    {
        if (exePath.empty())
            return false;
        const size_t slash = exePath.find_last_of(L"\\/");
        const wchar_t* name = (slash != std::wstring::npos) ? (exePath.c_str() + slash + 1) : exePath.c_str();
        return _wcsicmp(name, L"SystemSettings.exe") == 0;
    }

    inline bool ShouldShowSlotWindowPreview(SlotKind kind, size_t windowCount, bool fanOutSingleWindow) noexcept
    {
        if (kind != SlotKind::App)
            return false;
        const size_t minWin = fanOutSingleWindow ? 1 : 2;
        return windowCount >= minWin;
    }

    inline std::wstring StripExplorerTitleSuffix(const std::wstring& title)
    {
        std::wstring s = TrimCopyW(title);
        constexpr wchar_t kSuffix[] = L" - File Explorer";
        constexpr size_t kSuffixLen = _countof(kSuffix) - 1;
        if (s.size() > kSuffixLen &&
            _wcsicmp(s.c_str() + (s.size() - kSuffixLen), kSuffix) == 0)
        {
            s.resize(s.size() - kSuffixLen);
            s = TrimCopyW(s);
        }
        return s;
    }

    inline std::wstring NormalizeWindowsSlashes(std::wstring path)
    {
        for (auto& ch : path)
        {
            if (ch == L'/')
                ch = L'\\';
        }
        path = TrimCopyW(path);
        while (path.size() > 3 && path.back() == L'\\')
            path.pop_back();
        if (path.size() > 1 && path.back() == L'\\' && !(path.size() == 3 && path[1] == L':'))
            path.pop_back();
        return path;
    }

    inline bool ExplorerWindowMatchesFolder(const std::wstring& windowTitle, const std::wstring& expandedFolderPath)
    {
        const std::wstring stripped = StripExplorerTitleSuffix(windowTitle);
        if (stripped.empty() || expandedFolderPath.empty())
            return false;

        const std::wstring normPath = NormalizeWindowsSlashes(expandedFolderPath);
        if (normPath.empty())
            return false;

        const std::wstring normTitle = NormalizeWindowsSlashes(stripped);
        if (_wcsicmp(normTitle.c_str(), normPath.c_str()) == 0)
            return true;

        std::wstring trimmed = normPath;
        while (!trimmed.empty() && trimmed.back() == L'\\')
            trimmed.pop_back();
        if (trimmed.empty() || (trimmed.size() == 2 && trimmed[1] == L':'))
            return false;

        const size_t slash = trimmed.find_last_of(L'\\');
        const std::wstring leaf = (slash != std::wstring::npos) ? trimmed.substr(slash + 1) : trimmed;
        return !leaf.empty() && _wcsicmp(stripped.c_str(), leaf.c_str()) == 0;
    }

    inline bool ExplorerWindowMatchesRecycleBin(
        const std::wstring& windowTitle,
        const std::wstring& localizedRecycleBinName = L"Recycle Bin")
    {
        const std::wstring stripped = StripExplorerTitleSuffix(windowTitle);
        if (stripped.empty())
            return false;
        if (_wcsicmp(stripped.c_str(), L"Recycle Bin") == 0)
            return true;
        return !localizedRecycleBinName.empty() &&
            _wcsicmp(stripped.c_str(), localizedRecycleBinName.c_str()) == 0;
    }

    inline bool ExplorerWindowMatchesControlPanel(const std::wstring& windowTitle)
    {
        const std::wstring stripped = StripExplorerTitleSuffix(windowTitle);
        if (stripped.empty())
            return false;
        return _wcsicmp(stripped.c_str(), L"Control Panel") == 0 ||
            _wcsicmp(stripped.c_str(), L"All Control Panel Items") == 0;
    }

    struct SpringHoverEval
    {
        double scaleMultiplier = 1.0;
        bool pulsing = false;
        bool shouldActivate = false;
    };

    inline SpringHoverEval EvaluateSpringHover(
        double elapsedMs,
        double dwellMs = 500.0,
        double pulseDurationMs = 420.0) noexcept
    {
        if (elapsedMs < dwellMs)
            return {};

        const double dur = (pulseDurationMs > 1.0) ? pulseDurationMs : 1.0;
        const double t = (elapsedMs - dwellMs) / dur;
        if (t >= 1.0)
            return { 1.0, false, true };

        const double beatPos = t * 2.0;
        const double u = beatPos - std::floor(beatPos);
        double scaleMultiplier = 1.0;
        if (u < 0.5)
            scaleMultiplier = 1.0 + 0.24 * std::sin(u * 2.0 * kPi);
        else
            scaleMultiplier = 1.0 - 0.14 * std::sin((u - 0.5) * 2.0 * kPi);
        return { scaleMultiplier, true, false };
    }

    inline int ComputeReorderTargetSlotIndex(double dx, double dy, int pinnedCount, int totalSlotCount) noexcept
    {
        if (pinnedCount <= 1 || totalSlotCount <= 0)
            return 0;
        const int idx = CalculateSectorIndex(dx, dy, totalSlotCount, 0.0);
        if (idx < 0)
            return 0;
        return (std::clamp)(idx, 0, pinnedCount - 1);
    }

    /// Computes insertion/target index when dragging an unpinned open-app slot around the Ring
    /// so it promotes into the pinned list at the dropped sector (up to the end of pinned slots).
    inline int ComputeUnpinnedDragTargetIndex(double dx, double dy, int pinnedCount, int totalSlotCount) noexcept
    {
        if (totalSlotCount <= 0)
            return 0;
        const int idx = CalculateSectorIndex(dx, dy, totalSlotCount, 0.0);
        if (idx < 0)
            return 0;
        const int maxTarget = (std::min)(pinnedCount, totalSlotCount - 1);
        return (std::clamp)(idx, 0, (std::max)(0, maxTarget));
    }

    template <typename T>
    inline void ReorderVectorElement(std::vector<T>& items, size_t fromIdx, size_t toIdx)
    {
        if (fromIdx >= items.size() || toIdx >= items.size() || fromIdx == toIdx)
            return;
        if (fromIdx < toIdx)
        {
            std::rotate(
                items.begin() + static_cast<std::ptrdiff_t>(fromIdx),
                items.begin() + static_cast<std::ptrdiff_t>(fromIdx + 1),
                items.begin() + static_cast<std::ptrdiff_t>(toIdx + 1));
        }
        else
        {
            std::rotate(
                items.begin() + static_cast<std::ptrdiff_t>(toIdx),
                items.begin() + static_cast<std::ptrdiff_t>(fromIdx),
                items.begin() + static_cast<std::ptrdiff_t>(fromIdx + 1));
        }
    }

    inline bool CanSlotToggleRunAsAdmin(SlotKind kind, std::wstring_view pathOrUrl) noexcept
    {
        if (kind != SlotKind::App && kind != SlotKind::Script && kind != SlotKind::File)
            return false;
        const std::wstring low = ToLowerFold(TrimCopyW(pathOrUrl));
        if (low.rfind(L"winrings://", 0) == 0 || low.rfind(L"ms-settings:", 0) == 0)
            return false;
        return true;
    }

    inline std::wstring FormatCustomIconDisplayName(std::wstring_view iconCacheFile)
    {
        const std::wstring trimmed = TrimCopyW(iconCacheFile);
        if (trimmed.empty())
            return L"Default icon";
        if (ToLowerFold(trimmed).rfind(L"builtin:", 0) == 0)
            return L"Built-in icon";
        const size_t slash = trimmed.find_last_of(L"\\/");
        return (slash != std::wstring::npos && slash + 1 < trimmed.size())
            ? trimmed.substr(slash + 1)
            : trimmed;
    }

    inline size_t MapVisiblePinnedIndexToConfigIndex(
        const std::vector<bool>& hiddenFlags,
        size_t visibleIndex) noexcept
    {
        size_t vis = 0;
        for (size_t i = 0; i < hiddenFlags.size(); ++i)
        {
            if (hiddenFlags[i])
                continue;
            if (vis == visibleIndex)
                return i;
            ++vis;
        }
        return static_cast<size_t>(-1);
    }

    inline size_t MapVisiblePinnedInsertPosToConfigIndex(
        const std::vector<bool>& hiddenFlags,
        size_t visibleInsertPos) noexcept
    {
        size_t vis = 0;
        for (size_t i = 0; i < hiddenFlags.size(); ++i)
        {
            if (hiddenFlags[i])
                continue;
            if (vis == visibleInsertPos)
                return i;
            ++vis;
        }
        return hiddenFlags.size();
    }

    inline constexpr bool ShouldKeepSearchSelectionOnPointer(int nextRingIdx, int nextSubIdx) noexcept
    {
        return nextRingIdx < 0 && nextSubIdx < 0;
    }
} // namespace winrings
