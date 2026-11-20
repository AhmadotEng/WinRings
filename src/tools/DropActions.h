#pragma once

#include <Windows.h>
#include <string>
#include <vector>

#include "../shared/WinRingsConfig.h"

namespace winrings::tools
{
    enum class MediaKind
    {
        Unknown,
        Image,
        Video,
        Audio
    };

    struct ActionResult
    {
        bool ok = false;
        std::wstring summary;
        std::vector<std::wstring> outputs;
    };

    MediaKind ClassifyMediaFile(const std::wstring& path);
    bool IsFfmpegAvailable(const SwitcherConfig& cfg, std::wstring* resolvedPathOut = nullptr);

    // Pure native WIC image conversion (.png, .jpg, .bmp, .tif, .gif)
    bool ConvertImageWic(const std::wstring& sourcePath, const std::wstring& targetExt,
        std::wstring& outPath, std::wstring& outError);

    // ffmpeg/ffprobe invocation helpers (fails gracefully when ffmpeg is not present)
    ActionResult ConvertMediaFiles(const std::vector<std::wstring>& files,
        const std::wstring& targetExt, const SwitcherConfig& cfg);
    ActionResult CompressMediaFile(const std::wstring& sourcePath, const SwitcherConfig& cfg,
        int mode, const std::wstring& paramValue); // mode: 0=preset(high/med/small), 1=targetMB, 2=percent
    ActionResult TrimMediaFile(const std::wstring& sourcePath, const SwitcherConfig& cfg,
        const std::wstring& startTime, const std::wstring& endTime, bool streamCopy);

    // Native tar.exe -a -c -f zip creation (zero .NET / PowerShell)
    ActionResult CreateZipArchive(const std::vector<std::wstring>& files, HWND ownerHwnd);

    // Interactive dark-mode format picker when invoked with --convert <files...> and no --format
    int RunConvertUi(HINSTANCE instance, const std::vector<std::wstring>& files,
        const std::wstring& explicitFormat, const SwitcherConfig& cfg);
} // namespace winrings::tools
