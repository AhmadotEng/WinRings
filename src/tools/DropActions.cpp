#include "DropActions.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <sstream>
#include <vector>

#include "../shared/DropNaming.h"

namespace winrings::tools
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        std::wstring LowerExt(const std::wstring& path)
        {
            const size_t slash = path.find_last_of(L"\\/");
            const size_t dot = path.find_last_of(L'.');
            if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
                return {};
            std::wstring ext = path.substr(dot + 1);
            for (auto& ch : ext)
                ch = static_cast<wchar_t>(std::towlower(ch));
            return ext;
        }

        std::wstring QuoteArg(const std::wstring& arg)
        {
            std::wstring out = L"\"";
            for (wchar_t ch : arg)
            {
                if (ch == L'"')
                    out += L"\\\"";
                else
                    out.push_back(ch);
            }
            out.push_back(L'"');
            return out;
        }

        bool RunProcessCapture(const std::wstring& exePath, const std::wstring& args,
            DWORD& outExitCode, std::wstring& outText)
        {
            outExitCode = 1;
            outText.clear();

            SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
            HANDLE readPipe = nullptr;
            HANDLE writePipe = nullptr;
            if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
                return false;
            SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

            STARTUPINFOW si{ sizeof(si) };
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            si.hStdOutput = writePipe;
            si.hStdError = writePipe;

            std::wstring cmdLine = QuoteArg(exePath);
            if (!args.empty())
            {
                cmdLine.push_back(L' ');
                cmdLine += args;
            }
            std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
            cmdBuf.push_back(L'\0');

            PROCESS_INFORMATION pi{};
            const BOOL ok = CreateProcessW(
                exePath.c_str(),
                cmdBuf.data(),
                nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW,
                nullptr, nullptr,
                &si, &pi);

            CloseHandle(writePipe);

            if (!ok)
            {
                CloseHandle(readPipe);
                return false;
            }

            std::string raw;
            char buf[4096]{};
            DWORD readBytes = 0;
            while (ReadFile(readPipe, buf, sizeof(buf), &readBytes, nullptr) && readBytes > 0)
            {
                if (raw.size() < 262144)
                    raw.append(buf, readBytes);
            }
            CloseHandle(readPipe);

            WaitForSingleObject(pi.hProcess, 120000);
            GetExitCodeProcess(pi.hProcess, &outExitCode);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);

            outText = Utf8ToWide(raw);
            return true;
        }

        bool TryGetEncoderClsidForExt(const std::wstring& ext, CLSID& outFormat) noexcept
        {
            if (ext == L"png") { outFormat = GUID_ContainerFormatPng; return true; }
            if (ext == L"jpg" || ext == L"jpeg") { outFormat = GUID_ContainerFormatJpeg; return true; }
            if (ext == L"bmp") { outFormat = GUID_ContainerFormatBmp; return true; }
            if (ext == L"tif" || ext == L"tiff") { outFormat = GUID_ContainerFormatTiff; return true; }
            if (ext == L"gif") { outFormat = GUID_ContainerFormatGif; return true; }
            return false;
        }

        std::wstring ResolveSiblingToolFromFfmpeg(const std::wstring& ffmpegExe, const wchar_t* siblingName)
        {
            const size_t slash = ffmpegExe.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
            {
                const std::wstring cand = ffmpegExe.substr(0, slash + 1) + siblingName;
                if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES)
                    return cand;
            }
            wchar_t found[MAX_PATH]{};
            if (SearchPathW(nullptr, siblingName, nullptr, _countof(found), found, nullptr) > 0)
                return found;
            return {};
        }

        double ProbeDurationSeconds(const std::wstring& ffmpegExe, const std::wstring& inputPath)
        {
            const std::wstring ffprobe = ResolveSiblingToolFromFfmpeg(ffmpegExe, L"ffprobe.exe");
            if (ffprobe.empty())
                return 0.0;
            DWORD exitCode = 1;
            std::wstring output;
            const std::wstring args = L"-v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 " +
                QuoteArg(inputPath);
            if (!RunProcessCapture(ffprobe, args, exitCode, output) || exitCode != 0)
                return 0.0;
            try
            {
                return std::stod(output);
            }
            catch (...)
            {
                return 0.0;
            }
        }

        std::wstring MissingFfmpegMessage(const SwitcherConfig& cfg)
        {
            return L"ffmpeg executable was not found (configured: \"" + cfg.ffmpegPath +
                L"\").\n\nInstall ffmpeg on PATH or set a valid ffmpeg path in WinRings Settings -> Ring & Theme. "
                L"Note: PNG/JPG/BMP/TIFF/GIF image conversions and .zip archiving work without ffmpeg.";
        }

        struct ConvertDlgState
        {
            std::vector<std::wstring> files;
            SwitcherConfig cfg;
            HWND combo = nullptr;
            HWND statusLabel = nullptr;
            HBRUSH darkBg = nullptr;
            HFONT font = nullptr;
            bool accepted = false;
        };

        LRESULT CALLBACK ConvertWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            auto* st = reinterpret_cast<ConvertDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            switch (msg)
            {
            case WM_NCCREATE:
                st = static_cast<ConvertDlgState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
                break;

            case WM_CREATE:
            {
                const BOOL dark = TRUE;
                DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
                st->darkBg = CreateSolidBrush(RGB(24, 27, 34));
                st->font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

                const std::wstring header = L"Convert " + std::to_wstring(st->files.size()) +
                    (st->files.size() == 1 ? L" file to format:" : L" files to format:");
                HWND lbl = CreateWindowExW(0, L"STATIC", header.c_str(),
                    WS_CHILD | WS_VISIBLE, 18, 16, 360, 22, hwnd, nullptr, nullptr, nullptr);
                SendMessageW(lbl, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);

                st->combo = CreateWindowExW(0, L"COMBOBOX", nullptr,
                    WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                    18, 46, 220, 200, hwnd, reinterpret_cast<HMENU>(101), nullptr, nullptr);
                SendMessageW(st->combo, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);

                const MediaKind kind = !st->files.empty() ? ClassifyMediaFile(st->files[0]) : MediaKind::Unknown;
                const wchar_t* formats[] = { L"png", L"jpg", L"bmp", L"tif", L"gif", L"webp", L"mp4", L"mp3", L"wav" };
                int defaultIdx = 0;
                if (kind == MediaKind::Video) defaultIdx = 6;
                else if (kind == MediaKind::Audio) defaultIdx = 7;
                for (const wchar_t* fmt : formats)
                    SendMessageW(st->combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(fmt));
                SendMessageW(st->combo, CB_SETCURSEL, static_cast<WPARAM>(defaultIdx), 0);

                HWND okBtn = CreateWindowExW(0, L"BUTTON", L"Convert",
                    WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                    250, 46, 128, 28, hwnd, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
                SendMessageW(okBtn, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);

                st->statusLabel = CreateWindowExW(0, L"STATIC",
                    L"Outputs use non-destructive \"(converted)\" naming beside source files.",
                    WS_CHILD | WS_VISIBLE, 18, 86, 360, 36, hwnd, nullptr, nullptr, nullptr);
                SendMessageW(st->statusLabel, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);

                HWND cancelBtn = CreateWindowExW(0, L"BUTTON", L"Close",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    288, 128, 90, 28, hwnd, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
                SendMessageW(cancelBtn, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
                return 0;
            }

            case WM_CTLCOLORSTATIC:
            {
                HDC hdc = reinterpret_cast<HDC>(wp);
                SetTextColor(hdc, RGB(230, 234, 244));
                SetBkColor(hdc, RGB(24, 27, 34));
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
                if (LOWORD(wp) == IDOK)
                {
                    wchar_t ext[32]{};
                    const LRESULT idx = SendMessageW(st->combo, CB_GETCURSEL, 0, 0);
                    if (idx != CB_ERR)
                        SendMessageW(st->combo, CB_GETLBTEXT, static_cast<WPARAM>(idx), reinterpret_cast<LPARAM>(ext));
                    const ActionResult res = ConvertMediaFiles(st->files, ext, st->cfg);
                    st->accepted = res.ok;
                    SetWindowTextW(st->statusLabel, res.summary.c_str());
                    if (!res.ok)
                        MessageBoxW(hwnd, res.summary.c_str(), L"WinRings Convert", MB_ICONINFORMATION | MB_OK);
                    else
                    {
                        MessageBoxW(hwnd, res.summary.c_str(), L"WinRings Convert", MB_ICONINFORMATION | MB_OK);
                        DestroyWindow(hwnd);
                    }
                    return 0;
                }
                if (LOWORD(wp) == IDCANCEL)
                {
                    DestroyWindow(hwnd);
                    return 0;
                }
                break;

            case WM_DESTROY:
                if (st->darkBg) { DeleteObject(st->darkBg); st->darkBg = nullptr; }
                if (st->font) { DeleteObject(st->font); st->font = nullptr; }
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    } // namespace

    MediaKind ClassifyMediaFile(const std::wstring& path)
    {
        const std::wstring ext = LowerExt(path);
        if (ext == L"png" || ext == L"jpg" || ext == L"jpeg" || ext == L"bmp" ||
            ext == L"tif" || ext == L"tiff" || ext == L"gif" || ext == L"webp" || ext == L"ico")
            return MediaKind::Image;
        if (ext == L"mp4" || ext == L"mov" || ext == L"mkv" || ext == L"avi" || ext == L"webm")
            return MediaKind::Video;
        if (ext == L"mp3" || ext == L"wav" || ext == L"flac" || ext == L"m4a" || ext == L"ogg" || ext == L"aac")
            return MediaKind::Audio;
        return MediaKind::Unknown;
    }

    bool IsFfmpegAvailable(const SwitcherConfig& cfg, std::wstring* resolvedPathOut)
    {
        std::wstring spec = cfg.ffmpegPath.empty() ? L"ffmpeg" : cfg.ffmpegPath;
        if (spec.find(L'\\') != std::wstring::npos || spec.find(L'/') != std::wstring::npos)
        {
            const DWORD attrs = GetFileAttributesW(spec.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                if (resolvedPathOut) *resolvedPathOut = spec;
                return true;
            }
            return false;
        }
        wchar_t buf[MAX_PATH]{};
        if (SearchPathW(nullptr, spec.c_str(), L".exe", _countof(buf), buf, nullptr) > 0)
        {
            if (resolvedPathOut) *resolvedPathOut = buf;
            return true;
        }
        return false;
    }

    bool ConvertImageWic(const std::wstring& sourcePath, const std::wstring& targetExt,
        std::wstring& outPath, std::wstring& outError)
    {
        std::wstring normExt = targetExt;
        if (!normExt.empty() && normExt[0] == L'.')
            normExt.erase(normExt.begin());
        for (auto& ch : normExt)
            ch = static_cast<wchar_t>(std::towlower(ch));

        CLSID containerFormat{};
        if (!TryGetEncoderClsidForExt(normExt, containerFormat))
        {
            outError = L"WIC does not encode format: " + normExt;
            return false;
        }

        ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()));
        if (FAILED(hr) || !factory)
        {
            outError = L"Failed to initialize WIC.";
            return false;
        }

        ComPtr<IWICBitmapDecoder> decoder;
        hr = factory->CreateDecoderFromFilename(sourcePath.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, decoder.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !decoder)
        {
            outError = L"Failed to decode image: " + sourcePath;
            return false;
        }

        ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, frame.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !frame)
        {
            outError = L"Failed to read image frame.";
            return false;
        }

        outPath = BuildNonDestructiveOutputPath(sourcePath, L"converted", normExt);

        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> outFrame;
        if (FAILED(factory->CreateStream(stream.ReleaseAndGetAddressOf())) ||
            FAILED(stream->InitializeFromFilename(outPath.c_str(), GENERIC_WRITE)) ||
            FAILED(factory->CreateEncoder(containerFormat, nullptr, encoder.ReleaseAndGetAddressOf())) ||
            FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
            FAILED(encoder->CreateNewFrame(outFrame.ReleaseAndGetAddressOf(), nullptr)) ||
            FAILED(outFrame->Initialize(nullptr)))
        {
            outError = L"Failed to create output encoder for " + outPath;
            DeleteFileW(outPath.c_str());
            return false;
        }

        WICPixelFormatGUID desiredFmt = (normExt == L"jpg" || normExt == L"jpeg" || normExt == L"bmp")
            ? GUID_WICPixelFormat24bppBGR
            : GUID_WICPixelFormat32bppBGRA;

        ComPtr<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter(converter.ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr) && converter &&
            SUCCEEDED(converter->Initialize(frame.Get(), desiredFmt,
                WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeMedianCut)))
        {
            hr = outFrame->WriteSource(converter.Get(), nullptr);
        }
        else
        {
            hr = outFrame->WriteSource(frame.Get(), nullptr);
        }

        if (FAILED(hr) || FAILED(outFrame->Commit()) || FAILED(encoder->Commit()))
        {
            outError = L"Failed writing encoded pixels to " + outPath;
            DeleteFileW(outPath.c_str());
            return false;
        }

        return true;
    }

    ActionResult ConvertMediaFiles(const std::vector<std::wstring>& files,
        const std::wstring& targetExt, const SwitcherConfig& cfg)
    {
        ActionResult res{};
        if (files.empty())
        {
            res.summary = L"No files supplied.";
            return res;
        }

        std::wstring ext = targetExt;
        if (!ext.empty() && ext[0] == L'.')
            ext.erase(ext.begin());
        for (auto& ch : ext)
            ch = static_cast<wchar_t>(std::towlower(ch));

        CLSID dummy{};
        const bool wicSupported = TryGetEncoderClsidForExt(ext, dummy);

        std::wstring ffmpegExe;
        bool checkedFfmpeg = false;
        bool hasFfmpeg = false;
        std::wstring errors;

        for (const auto& f : files)
        {
            if (wicSupported && ClassifyMediaFile(f) == MediaKind::Image && LowerExt(f) != L"webp")
            {
                std::wstring outPath;
                std::wstring err;
                if (ConvertImageWic(f, ext, outPath, err))
                    res.outputs.push_back(outPath);
                else
                    errors += err + L"\n";
                continue;
            }

            if (!checkedFfmpeg)
            {
                checkedFfmpeg = true;
                hasFfmpeg = IsFfmpegAvailable(cfg, &ffmpegExe);
            }
            if (!hasFfmpeg)
            {
                errors += MissingFfmpegMessage(cfg) + L"\n";
                break;
            }

            const std::wstring outPath = BuildNonDestructiveOutputPath(f, L"converted", ext);
            const std::wstring args = L"-y -i " + QuoteArg(f) + L" " + QuoteArg(outPath);
            DWORD exitCode = 1;
            std::wstring procOut;
            if (RunProcessCapture(ffmpegExe, args, exitCode, procOut) && exitCode == 0)
            {
                res.outputs.push_back(outPath);
            }
            else
            {
                DeleteFileW(outPath.c_str());
                errors += L"ffmpeg failed for: " + f + L"\n";
            }
        }

        res.ok = !res.outputs.empty() && errors.empty();
        if (!res.outputs.empty())
        {
            res.summary = L"Converted " + std::to_wstring(res.outputs.size()) + L" file(s).\nFirst output: " + res.outputs.front();
            if (!errors.empty())
                res.summary += L"\n\nWarnings:\n" + errors;
        }
        else
        {
            res.summary = errors.empty() ? L"No files were converted." : errors;
        }
        return res;
    }

    ActionResult CompressMediaFile(const std::wstring& sourcePath, const SwitcherConfig& cfg,
        int mode, const std::wstring& paramValue)
    {
        ActionResult res{};
        std::wstring ffmpegExe;
        if (!IsFfmpegAvailable(cfg, &ffmpegExe))
        {
            res.summary = MissingFfmpegMessage(cfg);
            return res;
        }

        const std::wstring ext = LowerExt(sourcePath);
        const std::wstring outExt = ext.empty() ? L"mp4" : ext;
        const std::wstring outPath = BuildNonDestructiveOutputPath(sourcePath, L"compressed", outExt);

        std::wstring codecArgs;
        if (mode == 0)
        {
            int crf = 26;
            if (_wcsicmp(paramValue.c_str(), L"high") == 0) crf = 20;
            else if (_wcsicmp(paramValue.c_str(), L"small") == 0) crf = 32;
            codecArgs = L"-c:v libx264 -crf " + std::to_wstring(crf) + L" -preset medium -c:a aac -b:a 128k";
        }
        else
        {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            ULONGLONG origBytes = 0;
            if (GetFileAttributesExW(sourcePath.c_str(), GetFileExInfoStandard, &fad))
                origBytes = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;

            double targetBits = 0.0;
            if (mode == 1)
            {
                double mb = 10.0;
                try { mb = (std::max)(0.2, std::stod(paramValue)); } catch (...) {}
                targetBits = mb * 1024.0 * 1024.0 * 8.0;
            }
            else
            {
                double pct = 50.0;
                try { pct = (std::clamp)(std::stod(paramValue), 5.0, 95.0); } catch (...) {}
                targetBits = static_cast<double>(origBytes) * (pct / 100.0) * 8.0;
            }

            const double durSec = ProbeDurationSeconds(ffmpegExe, sourcePath);
            if (durSec > 0.5 && targetBits > 0.0)
            {
                const int audioKbps = 96;
                int totalKbps = static_cast<int>(targetBits / (durSec * 1000.0));
                int videoKbps = (std::max)(120, totalKbps - audioKbps);
                codecArgs = L"-c:v libx264 -b:v " + std::to_wstring(videoKbps) +
                    L"k -maxrate " + std::to_wstring(videoKbps + 128) +
                    L"k -bufsize " + std::to_wstring(videoKbps * 2) +
                    L"k -preset medium -c:a aac -b:a 96k";
            }
            else
            {
                codecArgs = L"-c:v libx264 -crf 28 -preset medium -c:a aac -b:a 96k";
            }
        }

        const std::wstring args = L"-y -i " + QuoteArg(sourcePath) + L" " + codecArgs + L" " + QuoteArg(outPath);
        DWORD exitCode = 1;
        std::wstring procOut;
        if (RunProcessCapture(ffmpegExe, args, exitCode, procOut) && exitCode == 0)
        {
            res.ok = true;
            res.outputs.push_back(outPath);
            res.summary = L"Saved compressed file:\n" + outPath;
        }
        else
        {
            DeleteFileW(outPath.c_str());
            res.summary = L"ffmpeg compression failed (exit " + std::to_wstring(exitCode) + L").";
        }
        return res;
    }

    ActionResult TrimMediaFile(const std::wstring& sourcePath, const SwitcherConfig& cfg,
        const std::wstring& startTime, const std::wstring& endTime, bool streamCopy)
    {
        ActionResult res{};
        std::wstring ffmpegExe;
        if (!IsFfmpegAvailable(cfg, &ffmpegExe))
        {
            res.summary = MissingFfmpegMessage(cfg);
            return res;
        }

        const std::wstring outPath = BuildNonDestructiveOutputPath(sourcePath, L"trimmed", LowerExt(sourcePath));
        std::wstring args = L"-y";
        if (!startTime.empty())
            args += L" -ss " + QuoteArg(startTime);
        if (!endTime.empty())
            args += L" -to " + QuoteArg(endTime);
        args += L" -i " + QuoteArg(sourcePath);
        if (streamCopy)
            args += L" -c copy";
        args += L" " + QuoteArg(outPath);

        DWORD exitCode = 1;
        std::wstring procOut;
        if (RunProcessCapture(ffmpegExe, args, exitCode, procOut) && exitCode == 0)
        {
            res.ok = true;
            res.outputs.push_back(outPath);
            res.summary = L"Saved trimmed file:\n" + outPath;
        }
        else
        {
            DeleteFileW(outPath.c_str());
            res.summary = L"ffmpeg trim failed (exit " + std::to_wstring(exitCode) + L").";
        }
        return res;
    }

    ActionResult CreateZipArchive(const std::vector<std::wstring>& files, HWND ownerHwnd)
    {
        (void)ownerHwnd;
        ActionResult res{};
        if (files.empty())
        {
            res.summary = L"No files supplied to zip.";
            return res;
        }

        const std::wstring outZip = BuildNonDestructiveOutputPath(files[0], L"archive", L".zip");
        wchar_t sysDir[MAX_PATH]{};
        GetSystemDirectoryW(sysDir, _countof(sysDir));
        const std::wstring tarExe = std::wstring(sysDir) + L"\\tar.exe";

        std::wstring args = L"-a -c -f " + QuoteArg(outZip);
        for (const auto& f : files)
        {
            const size_t slash = f.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
            {
                const std::wstring dir = f.substr(0, slash);
                const std::wstring leaf = f.substr(slash + 1);
                args += L" -C " + QuoteArg(dir) + L" " + QuoteArg(leaf);
            }
            else
            {
                args += L" " + QuoteArg(f);
            }
        }

        DWORD exitCode = 1;
        std::wstring procOut;
        if (RunProcessCapture(tarExe, args, exitCode, procOut) && exitCode == 0)
        {
            res.ok = true;
            res.outputs.push_back(outZip);
            res.summary = L"Created zip archive:\n" + outZip;
        }
        else
        {
            DeleteFileW(outZip.c_str());
            res.summary = L"Failed creating archive via tar.exe (exit " + std::to_wstring(exitCode) + L").";
        }
        return res;
    }

    int RunConvertUi(HINSTANCE instance, const std::vector<std::wstring>& files,
        const std::wstring& explicitFormat, const SwitcherConfig& cfg)
    {
        if (files.empty())
        {
            MessageBoxW(nullptr, L"No files passed to --convert.", L"WinRings Convert", MB_ICONINFORMATION | MB_OK);
            return 1;
        }

        if (!explicitFormat.empty())
        {
            const ActionResult res = ConvertMediaFiles(files, explicitFormat, cfg);
            MessageBoxW(nullptr, res.summary.c_str(), L"WinRings Convert",
                res.ok ? (MB_ICONINFORMATION | MB_OK) : (MB_ICONWARNING | MB_OK));
            return res.ok ? 0 : 1;
        }

        ConvertDlgState st{};
        st.files = files;
        st.cfg = cfg;

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = ConvertWndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"WinRings.ConvertDlg.v1";
        RegisterClassExW(&wc);

        RECT wa{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const int w = 412, h = 210;
        const int x = wa.left + (wa.right - wa.left - w) / 2;
        const int y = wa.top + (wa.bottom - wa.top - h) / 2;

        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"WinRings — Convert Files",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            x, y, w, h, nullptr, nullptr, instance, &st);
        if (!hwnd)
            return 1;
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
        return st.accepted ? 0 : 0;
    }
} // namespace winrings::tools
