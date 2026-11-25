#include "SlotModel.h"

#include <ShlObj.h>
#include <ShObjIdl.h>
#include <Shellapi.h>
#include <d2d1_1.h>
#include <winhttp.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "../shared/SearchMatch.h"
#include "AppIndex.h"

namespace winrings
{
    namespace
    {
        std::wstring FileStemOfPath(const std::wstring& path)
        {
            const size_t slash = path.find_last_of(L"\\/");
            std::wstring name = (slash != std::wstring::npos) ? path.substr(slash + 1) : path;
            const size_t dot = name.find_last_of(L'.');
            if (dot != std::wstring::npos && dot > 0)
                name.resize(dot);
            return name;
        }

        std::wstring ParentFolderNameOf(const std::wstring& path)
        {
            const size_t slash = path.find_last_of(L"\\/");
            if (slash == std::wstring::npos || slash == 0)
                return {};
            const size_t prevSlash = path.find_last_of(L"\\/", slash - 1);
            if (prevSlash == std::wstring::npos)
                return {};
            std::wstring folder = path.substr(prevSlash + 1, slash - prevSlash - 1);
            if (_wcsicmp(folder.c_str(), L"Application") == 0 ||
                _wcsicmp(folder.c_str(), L"bin") == 0 ||
                _wcsicmp(folder.c_str(), L"x64") == 0 ||
                _wcsicmp(folder.c_str(), L"win64") == 0 ||
                _wcsicmp(folder.c_str(), L"Program Files") == 0 ||
                _wcsicmp(folder.c_str(), L"Program Files (x86)") == 0 ||
                _wcsicmp(folder.c_str(), L"System32") == 0 ||
                _wcsicmp(folder.c_str(), L"Windows") == 0)
            {
                return ParentFolderNameOf(path.substr(0, slash));
            }
            return folder;
        }

        void AddAliasUnique(std::vector<std::wstring>& aliases, const std::wstring& value)
        {
            const std::wstring t = TrimCopyW(value);
            if (t.empty())
                return;
            const std::wstring low = ToLowerFold(t);
            for (const auto& existing : aliases)
            {
                if (ToLowerFold(existing) == low)
                    return;
            }
            aliases.push_back(t);
        }

        void PopulateSearchAliases(RingSlot& slot, const std::wstring& configuredLabel,
            const std::wstring& exePath, const AppIndex* appIndex)
        {
            AddAliasUnique(slot.searchAliases, slot.label);
            AddAliasUnique(slot.searchAliases, configuredLabel);
            if (!exePath.empty())
            {
                const std::wstring stem = FileStemOfPath(exePath);
                AddAliasUnique(slot.searchAliases, stem);
                AddAliasUnique(slot.searchAliases, AppIndex::ResolveExecutableFriendlyName(exePath));
                if (appIndex)
                    AddAliasUnique(slot.searchAliases, appIndex->FindFriendlyNameForExe(exePath));
                const std::wstring parent = ParentFolderNameOf(exePath);
                if (!parent.empty() && parent.size() <= 40)
                    AddAliasUnique(slot.secondarySearchAliases, parent);
            }
            for (const auto& w : slot.windows)
                AddAliasUnique(slot.secondarySearchAliases, w.title);
            for (const auto& sub : slot.subItems)
                AddAliasUnique(slot.secondarySearchAliases, sub.label);
        }

        std::wstring InferLabel(SlotKind kind, const std::wstring& pathOrUrl)
        {
            if (kind == SlotKind::RecycleBin)
                return L"Recycle Bin";
            if (kind == SlotKind::Website)
            {
                URL_COMPONENTS uc{ sizeof(uc) };
                wchar_t host[256]{};
                uc.lpszHostName = host;
                uc.dwHostNameLength = _countof(host);
                if (WinHttpCrackUrl(pathOrUrl.c_str(), static_cast<DWORD>(pathOrUrl.size()), 0, &uc) && host[0])
                    return host;
                return pathOrUrl;
            }
            const size_t slash = pathOrUrl.find_last_of(L"\\/");
            std::wstring name = (slash != std::wstring::npos) ? pathOrUrl.substr(slash + 1) : pathOrUrl;
            if (name.empty())
                return pathOrUrl;
            const size_t dot = name.find_last_of(L'.');
            if (dot != std::wstring::npos && dot > 0 && kind != SlotKind::Folder)
                name.resize(dot);
            return name;
        }

        bool IsRecycleBinFull()
        {
            SHQUERYRBINFO info{ sizeof(info) };
            if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &info)))
                return info.i64NumItems > 0;
            return false;
        }

        Microsoft::WRL::ComPtr<IWICBitmapSource> LoadImageFromFileWic(
            IWICImagingFactory* wic, const std::wstring& path)
        {
            if (!wic || path.empty())
                return nullptr;
            Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
            if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf())) || !decoder)
                return nullptr;
            Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
            if (FAILED(decoder->GetFrame(0, frame.GetAddressOf())) || !frame)
                return nullptr;
            Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
            if (FAILED(wic->CreateFormatConverter(converter.GetAddressOf())) || !converter)
                return nullptr;
            if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
                return nullptr;
            return converter;
        }

        Microsoft::WRL::ComPtr<ID2D1Factory> GetSharedWicD2DFactory()
        {
            static std::mutex s_facMutex;
            static Microsoft::WRL::ComPtr<ID2D1Factory> s_factory;
            std::lock_guard<std::mutex> lock(s_facMutex);
            if (!s_factory)
                D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, s_factory.GetAddressOf());
            return s_factory;
        }

        Microsoft::WRL::ComPtr<IWICBitmapSource> RenderBuiltinIconWic(
            IWICImagingFactory* wic, const std::wstring& spec)
        {
            if (!wic || spec.rfind(L"builtin:", 0) != 0)
                return nullptr;

            static std::mutex s_builtinMutex;
            static std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<IWICBitmapSource>> s_builtinCache;
            {
                std::lock_guard<std::mutex> lock(s_builtinMutex);
                if (auto it = s_builtinCache.find(spec); it != s_builtinCache.end())
                    return it->second;
            }

            Microsoft::WRL::ComPtr<ID2D1Factory> factory = GetSharedWicD2DFactory();
            if (!factory)
                return nullptr;

            Microsoft::WRL::ComPtr<IWICBitmap> bmp;
            if (FAILED(wic->CreateBitmap(64, 64, GUID_WICPixelFormat32bppPBGRA,
                WICBitmapCacheOnDemand, bmp.GetAddressOf())) || !bmp)
                return nullptr;

            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            Microsoft::WRL::ComPtr<ID2D1RenderTarget> rt;
            if (FAILED(factory->CreateWicBitmapRenderTarget(bmp.Get(), props, rt.GetAddressOf())) || !rt)
                return nullptr;

            auto mk = [&](D2D1_COLOR_F c) {
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> br;
                rt->CreateSolidColorBrush(c, br.GetAddressOf());
                return br;
            };
            auto hex = [](uint32_t rgb, float a = 1.0f) {
                return D2D1::ColorF(((rgb >> 16) & 0xFFu) / 255.0f,
                    ((rgb >> 8) & 0xFFu) / 255.0f, (rgb & 0xFFu) / 255.0f, a);
            };

            rt->BeginDraw();
            rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));

            const std::wstring name = spec.substr(8);
            if (name == L"windows")
            {
                auto b1 = mk(hex(0x38BDF8));
                auto b2 = mk(hex(0x0284C7));
                if (b1 && b2)
                {
                    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(10, 10, 30, 30), 2.5f, 2.5f), b1.Get());
                    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(34, 10, 54, 30), 2.5f, 2.5f), b1.Get());
                    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(10, 34, 30, 54), 2.5f, 2.5f), b2.Get());
                    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(34, 34, 54, 54), 2.5f, 2.5f), b2.Get());
                }
            }
            else if (name == L"power" || name == L"shutdown")
            {
                const D2D1_COLOR_F col = (name == L"shutdown") ? hex(0xF87171) : hex(0xE2E8F0);
                if (auto br = mk(col))
                {
                    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> ss;
                    D2D1_STROKE_STYLE_PROPERTIES sp{};
                    sp.startCap = D2D1_CAP_STYLE_ROUND;
                    sp.endCap = D2D1_CAP_STYLE_ROUND;
                    factory->CreateStrokeStyle(sp, nullptr, 0, ss.GetAddressOf());

                    Microsoft::WRL::ComPtr<ID2D1PathGeometry> pg;
                    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(factory->CreatePathGeometry(pg.GetAddressOf())) &&
                        SUCCEEDED(pg->Open(sink.GetAddressOf())))
                    {
                        const float cx = 32.0f, cy = 34.0f, r = 17.5f;
                        const float startRad = -0.95f;
                        const float endRad = 3.14159265f * 2.0f - 2.19f;
                        const D2D1_POINT_2F p0{ cx + r * std::cos(startRad), cy + r * std::sin(startRad) };
                        const D2D1_POINT_2F p1{ cx + r * std::cos(endRad), cy + r * std::sin(endRad) };
                        sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddArc(D2D1::ArcSegment(p1, D2D1::SizeF(r, r), 0.0f,
                            D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
                        sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        sink->Close();
                        rt->DrawGeometry(pg.Get(), br.Get(), 5.0f, ss.Get());
                    }
                    rt->DrawLine(D2D1::Point2F(32, 10), D2D1::Point2F(32, 29), br.Get(), 5.0f, ss.Get());
                }
            }
            else if (name == L"restart")
            {
                if (auto br = mk(hex(0x34D399)))
                {
                    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> ss;
                    D2D1_STROKE_STYLE_PROPERTIES sp{};
                    sp.startCap = D2D1_CAP_STYLE_ROUND;
                    sp.endCap = D2D1_CAP_STYLE_ROUND;
                    factory->CreateStrokeStyle(sp, nullptr, 0, ss.GetAddressOf());

                    Microsoft::WRL::ComPtr<ID2D1PathGeometry> pg;
                    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(factory->CreatePathGeometry(pg.GetAddressOf())) &&
                        SUCCEEDED(pg->Open(sink.GetAddressOf())))
                    {
                        sink->BeginFigure(D2D1::Point2F(46, 22), D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(18, 22), D2D1::SizeF(18, 18), 0.0f,
                            D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(47, 42), D2D1::SizeF(18, 18), 0.0f,
                            D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
                        sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        sink->Close();
                        rt->DrawGeometry(pg.Get(), br.Get(), 4.8f, ss.Get());
                    }
                    rt->DrawLine(D2D1::Point2F(48, 13), D2D1::Point2F(48, 24), br.Get(), 4.6f, ss.Get());
                    rt->DrawLine(D2D1::Point2F(37, 24), D2D1::Point2F(48, 24), br.Get(), 4.6f, ss.Get());
                }
            }
            else if (name == L"switch-user")
            {
                if (auto br = mk(hex(0x38BDF8)))
                {
                    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(26, 22), 8.5f, 8.5f), br.Get());
                    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(12, 35, 40, 52), 8.5f, 8.5f), br.Get());
                    rt->DrawLine(D2D1::Point2F(41, 24), D2D1::Point2F(54, 24), br.Get(), 3.6f);
                    rt->DrawLine(D2D1::Point2F(49, 18), D2D1::Point2F(55, 24), br.Get(), 3.6f);
                    rt->DrawLine(D2D1::Point2F(49, 30), D2D1::Point2F(55, 24), br.Get(), 3.6f);
                    rt->DrawLine(D2D1::Point2F(41, 40), D2D1::Point2F(54, 40), br.Get(), 3.6f);
                    rt->DrawLine(D2D1::Point2F(46, 34), D2D1::Point2F(40, 40), br.Get(), 3.6f);
                    rt->DrawLine(D2D1::Point2F(46, 46), D2D1::Point2F(40, 40), br.Get(), 3.6f);
                }
            }
            else if (name == L"settings")
            {
                auto metal = mk(hex(0x94A3B8));
                auto core = mk(hex(0x38BDF8));
                auto hole = mk(hex(0x1E293B));
                if (metal && core && hole)
                {
                    for (int t = 0; t < 8; ++t)
                    {
                        const float ang = t * 45.0f;
                        rt->SetTransform(D2D1::Matrix3x2F::Rotation(ang, D2D1::Point2F(32, 32)));
                        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(28, 8, 36, 56), 2.5f, 2.5f), metal.Get());
                    }
                    rt->SetTransform(D2D1::Matrix3x2F::Identity());
                    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(32, 32), 18.0f, 18.0f), metal.Get());
                    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(32, 32), 10.0f, 10.0f), hole.Get());
                    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(32, 32), 10.0f, 10.0f), core.Get(), 2.2f);
                }
            }
            else if (name == L"apps")
            {
                auto a1 = mk(hex(0xCBD5E1));
                auto a2 = mk(hex(0x38BDF8));
                if (a1 && a2)
                {
                    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(12, 12, 29, 29), 4.0f, 4.0f), a1.Get());
                    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(35, 12, 52, 29), 4.0f, 4.0f), a2.Get());
                    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(12, 35, 29, 52), 4.0f, 4.0f), a1.Get());
                    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(35, 35, 52, 52), 4.0f, 4.0f), a1.Get());
                }
            }
            else if (name == L"power-opts")
            {
                if (auto br = mk(hex(0xFBBF24)))
                {
                    Microsoft::WRL::ComPtr<ID2D1PathGeometry> pg;
                    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(factory->CreatePathGeometry(pg.GetAddressOf())) &&
                        SUCCEEDED(pg->Open(sink.GetAddressOf())))
                    {
                        sink->BeginFigure(D2D1::Point2F(36, 9), D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddLine(D2D1::Point2F(17, 35));
                        sink->AddLine(D2D1::Point2F(30, 35));
                        sink->AddLine(D2D1::Point2F(26, 55));
                        sink->AddLine(D2D1::Point2F(47, 29));
                        sink->AddLine(D2D1::Point2F(34, 29));
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sink->Close();
                        rt->FillGeometry(pg.Get(), br.Get());
                    }
                }
            }
            else if (name == L"system")
            {
                auto frame = mk(hex(0x60A5FA));
                auto screen = mk(hex(0x1E3A5F));
                if (frame && screen)
                {
                    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(9, 13, 55, 43), 4.0f, 4.0f), frame.Get());
                    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(13, 17, 51, 39), 2.0f, 2.0f), screen.Get());
                    rt->FillRectangle(D2D1::RectF(27, 43, 37, 50), frame.Get());
                    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(20, 49, 44, 53), 2.0f, 2.0f), frame.Get());
                }
            }
            else if (name == L"run")
            {
                auto box = mk(hex(0x1E293B));
                auto rim = mk(hex(0x38BDF8));
                if (box && rim)
                {
                    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(10, 14, 54, 50), 6.0f, 6.0f), box.Get());
                    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(10, 14, 54, 50), 6.0f, 6.0f), rim.Get(), 3.0f);
                    rt->DrawLine(D2D1::Point2F(18, 25), D2D1::Point2F(28, 32), rim.Get(), 3.6f);
                    rt->DrawLine(D2D1::Point2F(18, 39), D2D1::Point2F(28, 32), rim.Get(), 3.6f);
                    rt->DrawLine(D2D1::Point2F(32, 39), D2D1::Point2F(45, 39), rim.Get(), 3.6f);
                }
            }

            if (FAILED(rt->EndDraw()))
                return nullptr;

            {
                std::lock_guard<std::mutex> lock(s_builtinMutex);
                if (s_builtinCache.size() > 64)
                    s_builtinCache.clear();
                s_builtinCache.emplace(spec, bmp);
            }
            return bmp;
        }

        Microsoft::WRL::ComPtr<IWICBitmapSource> OverlayAdminShieldBadge(
            IWICImagingFactory* wic, IWICBitmapSource* baseIcon)
        {
            if (!wic || !baseIcon)
                return baseIcon;

            static std::mutex s_badgeMutex;
            static std::unordered_map<IWICBitmapSource*, Microsoft::WRL::ComPtr<IWICBitmapSource>> s_badgeCache;
            {
                std::lock_guard<std::mutex> lock(s_badgeMutex);
                if (auto it = s_badgeCache.find(baseIcon); it != s_badgeCache.end())
                    return it->second;
            }

            Microsoft::WRL::ComPtr<ID2D1Factory> factory = GetSharedWicD2DFactory();
            if (!factory)
                return baseIcon;

            Microsoft::WRL::ComPtr<IWICBitmap> bmp;
            if (FAILED(wic->CreateBitmap(64, 64, GUID_WICPixelFormat32bppPBGRA,
                WICBitmapCacheOnDemand, bmp.GetAddressOf())) || !bmp)
                return baseIcon;

            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            Microsoft::WRL::ComPtr<ID2D1RenderTarget> rt;
            if (FAILED(factory->CreateWicBitmapRenderTarget(bmp.Get(), props, rt.GetAddressOf())) || !rt)
                return baseIcon;

            Microsoft::WRL::ComPtr<ID2D1Bitmap> d2dBase;
            rt->CreateBitmapFromWicBitmap(baseIcon, d2dBase.GetAddressOf());
            if (!d2dBase)
                return baseIcon;

            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBr, blueBr, goldBr;
            rt->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.97f, 1.0f, 0.95f), borderBr.GetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.14f, 0.45f, 0.92f, 1.0f), blueBr.GetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.98f, 0.75f, 0.14f, 1.0f), goldBr.GetAddressOf());

            rt->BeginDraw();
            rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));
            rt->DrawBitmap(d2dBase.Get(), D2D1::RectF(0, 0, 64, 64));

            if (borderBr && blueBr && goldBr)
            {
                // Clean 22x22 UAC-style shield badge at bottom-right
                const D2D1_RECT_F badge = D2D1::RectF(40.0f, 40.0f, 62.0f, 62.0f);
                rt->FillRoundedRectangle(D2D1::RoundedRect(badge, 5.0f, 5.0f), borderBr.Get());
                rt->FillRectangle(D2D1::RectF(42.5f, 42.5f, 51.0f, 51.0f), blueBr.Get());
                rt->FillRectangle(D2D1::RectF(51.0f, 42.5f, 59.5f, 51.0f), goldBr.Get());
                rt->FillRectangle(D2D1::RectF(42.5f, 51.0f, 51.0f, 59.5f), goldBr.Get());
                rt->FillRectangle(D2D1::RectF(51.0f, 51.0f, 59.5f, 59.5f), blueBr.Get());
            }

            if (FAILED(rt->EndDraw()))
                return baseIcon;

            {
                std::lock_guard<std::mutex> lock(s_badgeMutex);
                if (s_badgeCache.size() > 64)
                    s_badgeCache.clear();
                s_badgeCache.emplace(baseIcon, bmp);
            }
            return bmp;
        }

        void PopulateFolderSubItems(RingSlot& folderSlot, const std::wstring& folderPath, int maxItems, WindowTracker& tracker)
        {
            struct DirEntry
            {
                std::wstring name;
                std::wstring fullPath;
                bool isDir = false;
                ULONGLONG writeTime = 0;
            };

            std::vector<DirEntry> entries;
            const std::wstring pattern = folderPath + L"\\*";
            WIN32_FIND_DATAW fd{};
            HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
            if (find == INVALID_HANDLE_VALUE)
                return;

            do
            {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                    continue;
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0 &&
                    (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0)
                    continue;
                ULARGE_INTEGER ft{};
                ft.LowPart = fd.ftLastWriteTime.dwLowDateTime;
                ft.HighPart = fd.ftLastWriteTime.dwHighDateTime;
                DirEntry e;
                e.name = fd.cFileName;
                e.fullPath = folderPath + L"\\" + fd.cFileName;
                e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                e.writeTime = ft.QuadPart;
                entries.push_back(std::move(e));
            } while (FindNextFileW(find, &fd));
            FindClose(find);

            std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
                return a.writeTime > b.writeTime;
            });

            const int limit = std::clamp(maxItems, 1, 9);
            const int fileLimit = (limit >= 2) ? (std::min)(limit, 7) : limit;
            for (int i = 0; i < static_cast<int>(entries.size()) && i < fileLimit; ++i)
            {
                RingSlot sub;
                sub.id = L"file-" + std::to_wstring(i);
                sub.kind = entries[i].isDir ? SlotKind::Folder : SlotKind::File;
                sub.label = entries[i].name;
                sub.pathOrUrl = entries[i].fullPath;
                sub.iconBitmap = tracker.GetShellIconBitmap(entries[i].fullPath);
                sub.isPinned = false;
                folderSlot.subItems.push_back(std::move(sub));
            }
            if (limit >= 2)
            {
                RingSlot openDir;
                openDir.id = L"folder-open";
                openDir.kind = SlotKind::Folder;
                openDir.label = L"Open in Explorer";
                openDir.pathOrUrl = folderPath;
                openDir.iconBitmap = tracker.GetStockIconBitmap(SIID_FOLDER);
                openDir.isPinned = false;
                folderSlot.subItems.push_back(std::move(openDir));
            }
        }

        template <typename Pred>
        void CollectMatchingExplorerWindows(
            const std::vector<AppWindowGroup>& running,
            std::vector<WindowEntry>& outWindows,
            Pred&& matchTitle)
        {
            for (const auto& grp : running)
            {
                const bool isExplorerGroup = IsExplorerExecutablePath(grp.exePath) ||
                    _wcsicmp(grp.processName.c_str(), L"explorer") == 0;
                if (!isExplorerGroup)
                    continue;
                for (const auto& w : grp.windows)
                {
                    if (matchTitle(w.title))
                        outWindows.push_back(w);
                }
            }
        }

        std::wstring GetLocalizedRecycleBinName()
        {
            std::wstring name = L"Recycle Bin";
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, 0, nullptr, &pidl)) && pidl)
            {
                PWSTR display = nullptr;
                if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY, &display)) && display)
                {
                    if (display[0] != L'\0')
                        name = display;
                    CoTaskMemFree(display);
                }
                CoTaskMemFree(pidl);
            }
            return name;
        }

        RingSlot CreateSlotFromConfig(
            const PinnedSlotConfig& cfg,
            const std::vector<AppWindowGroup>& running,
            std::unordered_set<std::wstring>& consumedKeys,
            WindowTracker& tracker,
            IWICImagingFactory* wicFactory,
            const AppIndex* appIndex)
        {
            const std::wstring expanded = SlotModel::ExpandEnv(cfg.pathOrUrl);
            RingSlot slot;
            slot.id = cfg.id;
            slot.kind = cfg.kind;
            slot.label = cfg.label.empty() ? InferLabel(cfg.kind, expanded) : cfg.label;
            slot.pathOrUrl = expanded;
            slot.isPinned = true;
            slot.runAsAdmin = cfg.runAsAdmin;
            slot.arguments = SlotModel::ExpandEnv(cfg.arguments);
            slot.maxFolderItems = std::clamp(cfg.maxFolderItems, 1, 12);

            std::wstring resolvedExePath = expanded;
            if (cfg.kind == SlotKind::App)
            {
                const std::wstring key = WindowTracker::NormalizeAppKey(expanded, cfg.label);
                bool matched = false;
                auto bindGroup = [&](const AppWindowGroup& grp) {
                    slot.windows = grp.windows;
                    if (!grp.exePath.empty())
                        resolvedExePath = grp.exePath;
                    slot.iconBitmap = grp.iconBitmap ? grp.iconBitmap : tracker.GetShellIconBitmap(expanded);
                    consumedKeys.insert(grp.normalizedKey);
                    matched = true;
                };

                // Exact path match first (including runAsAdmin apps so CMD/PowerShell Admin link to open windows)
                for (const auto& grp : running)
                {
                    if (WindowTracker::AppKeysMatch(grp.normalizedKey, key))
                    {
                        bindGroup(grp);
                        break;
                    }
                }
                // Fallback: match by executable filename (handles e.g. System32 vs SysWOW64 powershell.exe / cmd.exe)
                if (!matched && !expanded.empty())
                {
                    const size_t slash = expanded.find_last_of(L"\\/");
                    const std::wstring cfgExeName = (slash != std::wstring::npos) ? expanded.substr(slash + 1) : expanded;
                    if (!cfgExeName.empty() && _wcsicmp(cfgExeName.c_str(), L"explorer.exe") != 0)
                    {
                        for (const auto& grp : running)
                        {
                            const size_t gSlash = grp.exePath.find_last_of(L"\\/");
                            const std::wstring gName = (gSlash != std::wstring::npos) ? grp.exePath.substr(gSlash + 1) : grp.exePath;
                            if (!gName.empty() && _wcsicmp(gName.c_str(), cfgExeName.c_str()) == 0)
                            {
                                bindGroup(grp);
                                break;
                            }
                        }
                    }
                }
                if (!matched)
                    slot.iconBitmap = tracker.GetShellIconBitmap(expanded);

                const std::wstring stem = FileStemOfPath(resolvedExePath);
                const bool isExplorer = IsExplorerExecutablePath(resolvedExePath) ||
                    _wcsicmp(stem.c_str(), L"explorer") == 0;
                if (isExplorer)
                {
                    auto explorerIcon = tracker.GetShellIconBitmap(!expanded.empty() ? expanded : resolvedExePath);
                    if (explorerIcon)
                        slot.iconBitmap = std::move(explorerIcon);
                }
                else if (_wcsicmp(stem.c_str(), L"control") == 0)
                {
                    CollectMatchingExplorerWindows(running, slot.windows, [](const std::wstring& title) {
                        return ExplorerWindowMatchesControlPanel(title);
                    });
                }

                std::wstring friendly = isExplorer
                    ? std::wstring(L"File Explorer")
                    : (appIndex ? appIndex->FindFriendlyNameForExe(resolvedExePath) : L"");
                if (friendly.empty())
                    friendly = AppIndex::ResolveExecutableFriendlyName(resolvedExePath);
                if (!friendly.empty() &&
                    (cfg.label.empty() || _wcsicmp(cfg.label.c_str(), stem.c_str()) == 0))
                {
                    slot.label = friendly;
                }
            }
            else if (cfg.kind == SlotKind::File || cfg.kind == SlotKind::Folder || cfg.kind == SlotKind::Script)
            {
                slot.iconBitmap = tracker.GetShellIconBitmap(expanded);
                if (cfg.kind == SlotKind::Folder)
                {
                    PopulateFolderSubItems(slot, expanded, slot.maxFolderItems, tracker);
                    CollectMatchingExplorerWindows(running, slot.windows, [&](const std::wstring& title) {
                        return SlotModel::ExplorerWindowMatchesFolder(title, expanded);
                    });
                }
                else if (cfg.kind == SlotKind::File && expanded.rfind(L"ms-settings:", 0) == 0)
                {
                    for (const auto& grp : running)
                    {
                        if (IsSystemSettingsExePath(grp.exePath))
                        {
                            slot.windows = grp.windows;
                            consumedKeys.insert(grp.normalizedKey);
                            break;
                        }
                    }
                }
            }
            else if (cfg.kind == SlotKind::Website)
            {
                if (!cfg.iconCacheFile.empty() && cfg.iconCacheFile.rfind(L"builtin:", 0) != 0)
                {
                    const std::wstring fullIcon = (cfg.iconCacheFile.find(L':') != std::wstring::npos)
                        ? cfg.iconCacheFile
                        : (SwitcherConfig::AppDataDirectory() + L"\\" + cfg.iconCacheFile);
                    slot.iconBitmap = LoadImageFromFileWic(wicFactory, fullIcon);
                }
            }
            else if (cfg.kind == SlotKind::RecycleBin)
            {
                slot.iconBitmap = tracker.GetStockIconBitmap(IsRecycleBinFull() ? SIID_RECYCLERFULL : SIID_RECYCLER);
                const std::wstring recycleBinName = GetLocalizedRecycleBinName();
                CollectMatchingExplorerWindows(running, slot.windows, [&](const std::wstring& title) {
                    return SlotModel::ExplorerWindowMatchesRecycleBin(title, recycleBinName);
                });
            }
            else if (cfg.kind == SlotKind::Collection)
            {
                slot.iconBitmap = tracker.GetStockIconBitmap(SIID_FOLDER);
                for (const auto& child : cfg.items)
                    slot.subItems.push_back(CreateSlotFromConfig(child, running, consumedKeys, tracker, wicFactory, appIndex));
            }

            if (cfg.iconCacheFile.rfind(L"builtin:", 0) == 0)
            {
                auto builtinBmp = RenderBuiltinIconWic(wicFactory, cfg.iconCacheFile);
                if (builtinBmp)
                    slot.iconBitmap = std::move(builtinBmp);
            }
            else if (!cfg.iconCacheFile.empty() && cfg.kind != SlotKind::Website)
            {
                const std::wstring expandedIcon = SlotModel::ExpandEnv(cfg.iconCacheFile);
                const std::wstring fullIcon = (expandedIcon.find(L':') != std::wstring::npos ||
                    expandedIcon.rfind(L"\\\\", 0) == 0)
                    ? expandedIcon
                    : (SwitcherConfig::AppDataDirectory() + L"\\" + expandedIcon);
                auto customBmp = LoadImageFromFileWic(wicFactory, fullIcon);
                if (!customBmp)
                    customBmp = tracker.GetShellIconBitmap(fullIcon);
                if (customBmp)
                    slot.iconBitmap = std::move(customBmp);
            }

            if (cfg.runAsAdmin && slot.iconBitmap)
                slot.iconBitmap = OverlayAdminShieldBadge(wicFactory, slot.iconBitmap.Get());

            const HWND frontmost = tracker.LastActiveWindow();
            for (const auto& w : slot.windows)
            {
                if (frontmost && w.hwnd == frontmost)
                {
                    slot.isFrontmost = true;
                    break;
                }
            }

            PopulateSearchAliases(slot, cfg.label,
                cfg.kind == SlotKind::App ? resolvedExePath : std::wstring(), appIndex);
            return slot;
        }
    } // namespace

    bool SlotModel::ExplorerWindowMatchesFolder(const std::wstring& windowTitle, const std::wstring& expandedFolderPath)
    {
        return ::winrings::ExplorerWindowMatchesFolder(windowTitle, expandedFolderPath);
    }

    bool SlotModel::ExplorerWindowMatchesRecycleBin(const std::wstring& windowTitle, const std::wstring& localizedRecycleBinName)
    {
        return ::winrings::ExplorerWindowMatchesRecycleBin(windowTitle, localizedRecycleBinName);
    }

    std::wstring SlotModel::ExpandEnv(const std::wstring& input)
    {
        if (input.empty())
            return {};
        const DWORD needed = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
        if (!needed)
            return input;
        std::wstring out(static_cast<size_t>(needed), L'\0');
        const DWORD actual = ExpandEnvironmentStringsW(input.c_str(), out.data(), needed);
        if (!actual)
            return input;
        if (!out.empty() && out.back() == L'\0')
            out.pop_back();
        return out;
    }

    std::vector<RingSlot> SlotModel::BuildRingSlots(
        HWND ignoreHwnd,
        const SwitcherConfig& config,
        WindowTracker& tracker,
        IWICImagingFactory* wicFactory,
        const AppIndex* appIndex,
        const std::wstring& profileId)
    {
        const RingProfileConfig& activeProf = config.ResolveProfile(profileId);
        const auto running = tracker.EnumerateGroupedWindows(ignoreHwnd);
        const HWND frontmost = tracker.LastActiveWindow();
        std::vector<RingSlot> slots;
        std::unordered_set<std::wstring> consumedKeys;

        for (const auto& pinned : activeProf.pinnedSlots)
        {
            if (pinned.hiddenFromRing)
                continue;
            if (static_cast<int>(slots.size()) >= config.maxSlots)
                break;
            slots.push_back(CreateSlotFromConfig(pinned, running, consumedKeys, tracker, wicFactory, appIndex));
        }

        if (activeProf.includeRunningApps)
        {
            for (const auto& grp : running)
            {
                if (static_cast<int>(slots.size()) >= config.maxSlots)
                    break;
                if (consumedKeys.count(grp.normalizedKey))
                    continue;
                RingSlot s;
                s.id = L"run-" + grp.normalizedKey;
                s.kind = SlotKind::App;
                const bool isExplorer = IsExplorerExecutablePath(grp.exePath) ||
                    _wcsicmp(grp.processName.c_str(), L"explorer") == 0;
                std::wstring friendly = isExplorer
                    ? std::wstring(L"File Explorer")
                    : (appIndex ? appIndex->FindFriendlyNameForExe(grp.exePath) : L"");
                if (friendly.empty() && !grp.exePath.empty())
                    friendly = AppIndex::ResolveExecutableFriendlyName(grp.exePath);
                s.label = !friendly.empty() ? friendly : grp.processName;
                s.pathOrUrl = grp.exePath;
                if (isExplorer && !grp.exePath.empty())
                    s.iconBitmap = tracker.GetShellIconBitmap(grp.exePath);
                if (!s.iconBitmap)
                    s.iconBitmap = grp.iconBitmap;
                s.isPinned = false;
                s.windows = grp.windows;
                for (const auto& w : s.windows)
                {
                    if (frontmost && w.hwnd == frontmost)
                    {
                        s.isFrontmost = true;
                        break;
                    }
                }
                PopulateSearchAliases(s, grp.processName, grp.exePath, appIndex);
                slots.push_back(std::move(s));
            }
        }

        return slots;
    }

    RingSlot SlotModel::BuildSearchSlot(
        const PinnedSlotConfig& cfg,
        const std::vector<AppWindowGroup>& running,
        WindowTracker& tracker,
        IWICImagingFactory* wicFactory,
        const AppIndex* appIndex,
        const std::wstring& sourceProfileId)
    {
        std::unordered_set<std::wstring> scratchConsumed;
        RingSlot slot = CreateSlotFromConfig(cfg, running, scratchConsumed, tracker, wicFactory, appIndex);
        slot.isPinned = false;
        slot.isHiddenPin = true;
        slot.sourceProfileId = sourceProfileId.empty() ? L"default" : sourceProfileId;

        const std::wstring expanded = ExpandEnv(cfg.pathOrUrl);
        for (const auto& alias : BuildBasicSearchAliases(slot.label, expanded))
        {
            const std::wstring lowAlias = ToLowerFold(alias);
            bool exists = false;
            for (const auto& cur : slot.searchAliases)
            {
                if (ToLowerFold(cur) == lowAlias)
                {
                    exists = true;
                    break;
                }
            }
            if (!exists)
                slot.searchAliases.push_back(alias);
        }
        return slot;
    }

    void SlotModel::LaunchPathOrUrl(const std::wstring& pathOrUrl, const std::wstring& arguments, bool runAsAdmin)
    {
        if (pathOrUrl.empty())
            return;

        if (_wcsicmp(pathOrUrl.c_str(), L"winrings://power/switch-user") == 0)
        {
            bool ok = false;
            if (HMODULE wts = LoadLibraryExW(L"wtsapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            {
                using WtsDisconnectFn = BOOL (WINAPI*)(HANDLE, DWORD, BOOL);
                if (auto fn = reinterpret_cast<WtsDisconnectFn>(GetProcAddress(wts, "WTSDisconnectSession")))
                    ok = fn(nullptr, 0xFFFFFFFFu /* WTS_CURRENT_SESSION */, FALSE) != FALSE;
                FreeLibrary(wts);
            }
            if (!ok)
                LockWorkStation();
            return;
        }
        if (_wcsicmp(pathOrUrl.c_str(), L"winrings://power/shutdown") == 0 ||
            _wcsicmp(pathOrUrl.c_str(), L"winrings://power/restart") == 0)
        {
            const bool isRestart = (_wcsicmp(pathOrUrl.c_str(), L"winrings://power/restart") == 0);
            const std::wstring exe = ExpandEnv(L"%SystemRoot%\\System32\\shutdown.exe");
            const wchar_t* args = isRestart ? L"/r /t 0" : L"/s /t 0";
            std::wstring cmd = L"\"" + exe + L"\" " + args;
            std::vector<wchar_t> buf(cmd.begin(), cmd.end());
            buf.push_back(L'\0');
            STARTUPINFOW si{ sizeof(si) };
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            else
            {
                ShellExecuteW(nullptr, L"open", exe.c_str(), args, nullptr, SW_HIDE);
            }
            return;
        }
        if (_wcsicmp(pathOrUrl.c_str(), L"winrings://system/run-dialog") == 0)
        {
            ShellExecuteW(nullptr, L"open", L"explorer.exe",
                L"shell:::{2559a1f3-21d7-11d4-bdaf-00c04f60b9f0}", nullptr, SW_SHOWNORMAL);
            return;
        }
        if (_wcsicmp(pathOrUrl.c_str(), L"winrings://system/start-menu") == 0)
        {
            INPUT in[2]{};
            in[0].type = INPUT_KEYBOARD;
            in[0].ki.wVk = VK_LWIN;
            in[0].ki.dwExtraInfo = 0x5752494Eu; // kWinRingsInjectedExtra
            in[1].type = INPUT_KEYBOARD;
            in[1].ki.wVk = VK_LWIN;
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            in[1].ki.dwExtraInfo = 0x5752494Eu;
            SendInput(2, in, sizeof(INPUT));
            return;
        }

        constexpr const wchar_t* kAppsPrefix = L"shell:AppsFolder\\";
        if (!runAsAdmin && arguments.empty() && pathOrUrl.rfind(kAppsPrefix, 0) == 0)
        {
            const std::wstring aumid = pathOrUrl.substr(wcslen(kAppsPrefix));
            Microsoft::WRL::ComPtr<IApplicationActivationManager> aam;
            if (SUCCEEDED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
                CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(aam.GetAddressOf()))) && aam)
            {
                DWORD pid = 0;
                if (SUCCEEDED(aam->ActivateApplication(aumid.c_str(), nullptr, AO_NONE, &pid)))
                    return;
            }
            SHELLEXECUTEINFOW seiApp{ sizeof(seiApp) };
            seiApp.fMask = SEE_MASK_FLAG_NO_UI;
            seiApp.lpVerb = L"open";
            seiApp.lpFile = L"explorer.exe";
            seiApp.lpParameters = pathOrUrl.c_str();
            seiApp.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&seiApp);
            return;
        }

        SHELLEXECUTEINFOW sei{ sizeof(sei) };
        sei.fMask = runAsAdmin ? 0 : SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = runAsAdmin ? L"runas" : L"open";
        sei.lpFile = pathOrUrl.c_str();
        sei.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
        sei.nShow = SW_SHOWNORMAL;
        ShellExecuteExW(&sei);
    }

    void SlotModel::RevealInExplorer(const std::wstring& path)
    {
        if (path.empty())
            return;
        const std::wstring params = L"/select,\"" + path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    }

    namespace
    {
        std::wstring BuildDoubleNullList(const std::vector<std::wstring>& paths)
        {
            std::wstring doubleNull;
            for (const auto& p : paths)
            {
                if (!p.empty())
                {
                    doubleNull += p;
                    doubleNull.push_back(L'\0');
                }
            }
            if (!doubleNull.empty())
                doubleNull.push_back(L'\0');
            return doubleNull;
        }
    }

    bool SlotModel::SendToRecycleBin(const std::vector<std::wstring>& paths)
    {
        const std::wstring doubleNull = BuildDoubleNullList(paths);
        if (doubleNull.empty())
            return false;

        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = doubleNull.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
    }

    bool SlotModel::CopyFilesToFolder(const std::vector<std::wstring>& paths, const std::wstring& destFolder)
    {
        if (destFolder.empty())
            return false;
        const std::wstring srcList = BuildDoubleNullList(paths);
        if (srcList.empty())
            return false;

        std::wstring dstList = destFolder;
        dstList.push_back(L'\0');
        dstList.push_back(L'\0');

        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_COPY;
        op.pFrom = srcList.c_str();
        op.pTo = dstList.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_RENAMEONCOLLISION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI;
        return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
    }

    bool SlotModel::EmptyRecycleBin(HWND ownerHwnd)
    {
        return SUCCEEDED(SHEmptyRecycleBinW(ownerHwnd, nullptr,
            SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND));
    }

    void SlotModel::FetchFaviconAsync(
        const std::wstring& url,
        std::function<void(std::wstring relativeIconPath)> onCompleted)
    {
        std::thread([url, cb = std::move(onCompleted)]() {
            URL_COMPONENTS uc{ sizeof(uc) };
            wchar_t scheme[32]{};
            wchar_t host[256]{};
            uc.lpszScheme = scheme;
            uc.dwSchemeLength = _countof(scheme);
            uc.lpszHostName = host;
            uc.dwHostNameLength = _countof(host);
            if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc) || !host[0])
            {
                if (cb) cb(L"");
                return;
            }

            std::wstring safeHost;
            for (wchar_t c : std::wstring(host))
            {
                if (std::iswalnum(c) || c == L'-' || c == L'.')
                    safeHost.push_back(c);
            }
            if (safeHost.empty())
            {
                if (cb) cb(L"");
                return;
            }

            const std::wstring appDir = SwitcherConfig::AppDataDirectory();
            const std::wstring iconsDir = appDir + L"\\icons";
            SHCreateDirectoryExW(nullptr, iconsDir.c_str(), nullptr);
            const std::wstring relPath = L"icons\\" + safeHost + L".ico";
            const std::wstring fullPath = appDir + L"\\" + relPath;
            if (GetFileAttributesW(fullPath.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                if (cb) cb(relPath);
                return;
            }

            std::wstring resultPath;
            HINTERNET session = WinHttpOpen(L"WinRings/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (session)
            {
                WinHttpSetTimeouts(session, 4000, 4000, 4000, 4000);
                const INTERNET_PORT port = uc.nPort ? uc.nPort : (uc.nScheme == INTERNET_SCHEME_HTTPS ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);
                HINTERNET conn = WinHttpConnect(session, host, port, 0);
                if (conn)
                {
                    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
                    HINTERNET req = WinHttpOpenRequest(conn, L"GET", L"/favicon.ico", nullptr,
                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
                    if (req)
                    {
                        DWORD disableCookies = WINHTTP_DISABLE_COOKIES;
                        WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &disableCookies, sizeof(disableCookies));
                        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
                            WinHttpReceiveResponse(req, nullptr))
                        {
                            DWORD statusCode = 0;
                            DWORD statusSize = sizeof(statusCode);
                            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
                            if (statusCode == 200)
                            {
                                std::string data;
                                DWORD avail = 0;
                                while (WinHttpQueryDataAvailable(req, &avail) && avail > 0 && data.size() < 512 * 1024)
                                {
                                    const size_t offset = data.size();
                                    data.resize(offset + avail);
                                    DWORD read = 0;
                                    if (!WinHttpReadData(req, data.data() + offset, avail, &read) || read == 0)
                                    {
                                        data.resize(offset);
                                        break;
                                    }
                                    data.resize(offset + read);
                                }
                                if (!data.empty())
                                {
                                    HANDLE file = CreateFileW(fullPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                                    if (file != INVALID_HANDLE_VALUE)
                                    {
                                        DWORD written = 0;
                                        if (WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                                            written == data.size())
                                        {
                                            resultPath = relPath;
                                        }
                                        CloseHandle(file);
                                    }
                                }
                            }
                        }
                        WinHttpCloseHandle(req);
                    }
                    WinHttpCloseHandle(conn);
                }
                WinHttpCloseHandle(session);
            }

            if (cb)
                cb(std::move(resultPath));
        }).detach();
    }
} // namespace winrings
