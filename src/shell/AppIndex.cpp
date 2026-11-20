#include "AppIndex.h"

#include <ShlObj.h>
#include <ShObjIdl.h>
#include <algorithm>
#include <cstring>
#include <cwctype>

#include "../shared/SearchMatch.h"

namespace winrings
{
    namespace
    {
        constexpr ULONGLONG kRefreshIntervalMs = 5ULL * 60ULL * 1000ULL;

        // PKEY_AppUserModel_ID: {9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3}, 5
        constexpr PROPERTYKEY kPropKeyAumid = {
            { 0x9F4C2855u, 0x9F79u, 0x4B39u, { 0xA8u, 0xD0u, 0xE1u, 0xD4u, 0x2Du, 0xE1u, 0xD5u, 0xF3u } },
            5u
        };

        std::wstring FileNamePart(const std::wstring& path)
        {
            const size_t slash = path.find_last_of(L"\\/");
            return (slash != std::wstring::npos) ? path.substr(slash + 1) : path;
        }

        std::wstring FileStemPart(const std::wstring& path)
        {
            std::wstring fn = FileNamePart(path);
            const size_t dot = fn.find_last_of(L'.');
            return (dot != std::wstring::npos && dot > 0) ? fn.substr(0, dot) : fn;
        }

        bool EndsWithInsensitive(const std::wstring& s, const wchar_t* suffix)
        {
            const size_t slen = wcslen(suffix);
            if (s.size() < slen)
                return false;
            return _wcsicmp(s.c_str() + (s.size() - slen), suffix) == 0;
        }

        bool ContainsWordInsensitive(const std::wstring& s, const wchar_t* sub)
        {
            const std::wstring low = ToLowerFold(s);
            return low.find(ToLowerFold(sub)) != std::wstring::npos;
        }

        bool IsUnwantedShortcutName(const std::wstring& nameOrStem)
        {
            return ContainsWordInsensitive(nameOrStem, L"uninstall") ||
                   ContainsWordInsensitive(nameOrStem, L"unins0") ||
                   ContainsWordInsensitive(nameOrStem, L"readme") ||
                   ContainsWordInsensitive(nameOrStem, L"release notes") ||
                   ContainsWordInsensitive(nameOrStem, L"license") ||
                   ContainsWordInsensitive(nameOrStem, L"manual") ||
                   ContainsWordInsensitive(nameOrStem, L"support center") ||
                   ContainsWordInsensitive(nameOrStem, L"documentation");
        }

        bool IsGenericWindowsProductName(const std::wstring& name)
        {
            return ContainsWordInsensitive(name, L"operating system");
        }

        void AddAliasUnique(std::vector<std::wstring>& aliases, const std::wstring& candidate)
        {
            const std::wstring trimmed = TrimCopyW(candidate);
            if (trimmed.empty())
                return;
            const std::wstring low = ToLowerFold(trimmed);
            for (const auto& existing : aliases)
            {
                if (ToLowerFold(existing) == low)
                    return;
            }
            aliases.push_back(trimmed);
        }

        std::wstring ReadVerString(const void* data, DWORD langCp, const wchar_t* key)
        {
            wchar_t subBlock[96]{};
            wsprintfW(subBlock, L"\\StringFileInfo\\%04X%04X\\%s",
                LOWORD(langCp), HIWORD(langCp), key);
            void* valPtr = nullptr;
            UINT valLen = 0;
            if (VerQueryValueW(data, subBlock, &valPtr, &valLen) && valPtr && valLen > 1)
            {
                const wchar_t* ws = static_cast<const wchar_t*>(valPtr);
                std::wstring result(ws, wcsnlen(ws, valLen));
                return TrimCopyW(result);
            }
            return {};
        }

        Microsoft::WRL::ComPtr<IWICBitmapSource> HBitmapToWicPbgra(
            IWICImagingFactory* wic, HBITMAP hbm)
        {
            if (!wic || !hbm)
                return nullptr;

            BITMAP bm{};
            if (GetObjectW(hbm, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0)
                return nullptr;

            WICBitmapAlphaChannelOption alphaOpt = WICBitmapUsePremultipliedAlpha;
            if (bm.bmBitsPixel == 32)
            {
                const size_t pixelCount = static_cast<size_t>(bm.bmWidth) * static_cast<size_t>(bm.bmHeight);
                std::vector<DWORD> pixels(pixelCount, 0);
                BITMAPINFO bmi{};
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = bm.bmWidth;
                bmi.bmiHeader.biHeight = -bm.bmHeight;
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 32;
                bmi.bmiHeader.biCompression = BI_RGB;

                HDC hdc = GetDC(nullptr);
                const int rows = hdc ? GetDIBits(hdc, hbm, 0, static_cast<UINT>(bm.bmHeight),
                    pixels.data(), &bmi, DIB_RGB_COLORS) : 0;
                if (hdc)
                    ReleaseDC(nullptr, hdc);

                if (rows > 0)
                {
                    bool hasAnyAlpha = false;
                    for (DWORD px : pixels)
                    {
                        if ((px & 0xFF000000u) != 0)
                        {
                            hasAnyAlpha = true;
                            break;
                        }
                    }
                    alphaOpt = hasAnyAlpha ? WICBitmapUseAlpha : WICBitmapIgnoreAlpha;
                }
            }
            else
            {
                alphaOpt = WICBitmapIgnoreAlpha;
            }

            Microsoft::WRL::ComPtr<IWICBitmap> wicBmp;
            if (FAILED(wic->CreateBitmapFromHBITMAP(hbm, nullptr, alphaOpt, wicBmp.GetAddressOf())) || !wicBmp)
                return nullptr;

            Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
            if (FAILED(wic->CreateFormatConverter(converter.GetAddressOf())) || !converter)
                return nullptr;

            if (FAILED(converter->Initialize(wicBmp.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
                return nullptr;

            return converter;
        }

        void ScanStartMenuFolder(
            const std::wstring& dir,
            std::vector<IndexedApp>& out,
            std::unordered_set<std::wstring>& seenTargets,
            std::unordered_set<std::wstring>& seenNames,
            std::unordered_map<std::wstring, std::wstring>& exeToName,
            const std::atomic<bool>& stopFlag)
        {
            if (stopFlag.load(std::memory_order_acquire))
                return;

            WIN32_FIND_DATAW fd{};
            HANDLE hFind = FindFirstFileW((dir + L"\\*").c_str(), &fd);
            if (hFind == INVALID_HANDLE_VALUE)
                return;

            do
            {
                if (stopFlag.load(std::memory_order_acquire))
                    break;
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                    continue;

                const std::wstring fullPath = dir + L"\\" + fd.cFileName;
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                {
                    if (_wcsicmp(fd.cFileName, L"Startup") == 0 ||
                        _wcsicmp(fd.cFileName, L"Administrative Tools") == 0)
                        continue;
                    ScanStartMenuFolder(fullPath, out, seenTargets, seenNames, exeToName, stopFlag);
                    continue;
                }

                if (!EndsWithInsensitive(fullPath, L".lnk"))
                    continue;

                const std::wstring dispName = TrimCopyW(FileStemPart(fd.cFileName));
                if (dispName.empty() || IsUnwantedShortcutName(dispName))
                    continue;

                std::wstring resolvedTarget;
                Microsoft::WRL::ComPtr<IShellLinkW> shellLink;
                if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(shellLink.GetAddressOf()))) && shellLink)
                {
                    Microsoft::WRL::ComPtr<IPersistFile> pf;
                    if (SUCCEEDED(shellLink.As(&pf)) && pf &&
                        SUCCEEDED(pf->Load(fullPath.c_str(), STGM_READ)))
                    {
                        wchar_t targetBuf[1024]{};
                        if (SUCCEEDED(shellLink->GetPath(targetBuf, _countof(targetBuf), nullptr, SLGP_RAWPATH)))
                        {
                            wchar_t expanded[1024]{};
                            ExpandEnvironmentStringsW(targetBuf, expanded, _countof(expanded));
                            resolvedTarget = expanded;
                        }
                    }
                }

                if (!resolvedTarget.empty())
                {
                    if (EndsWithInsensitive(resolvedTarget, L".chm") ||
                        EndsWithInsensitive(resolvedTarget, L".hlp") ||
                        EndsWithInsensitive(resolvedTarget, L".html") ||
                        EndsWithInsensitive(resolvedTarget, L".htm") ||
                        EndsWithInsensitive(resolvedTarget, L".url") ||
                        IsUnwantedShortcutName(FileNamePart(resolvedTarget)))
                    {
                        continue;
                    }
                    // Skip broken shortcut pointing to nonexistent executable on local disk
                    if (resolvedTarget.size() >= 2 && resolvedTarget[1] == L':' &&
                        GetFileAttributesW(resolvedTarget.c_str()) == INVALID_FILE_ATTRIBUTES)
                    {
                        continue;
                    }
                }

                const std::wstring normTarget = !resolvedTarget.empty()
                    ? AppIndex::CanonicalPathKey(resolvedTarget)
                    : AppIndex::CanonicalPathKey(fullPath);
                const std::wstring lowName = ToLowerFold(dispName);

                if (!normTarget.empty() && EndsWithInsensitive(resolvedTarget, L".exe"))
                {
                    if (exeToName.find(normTarget) == exeToName.end())
                        exeToName.emplace(normTarget, dispName);
                }

                if (seenNames.count(lowName))
                    continue;
                if (!normTarget.empty() && EndsWithInsensitive(resolvedTarget, L".exe") && seenTargets.count(normTarget))
                    continue;

                seenNames.insert(lowName);
                if (!normTarget.empty())
                    seenTargets.insert(normTarget);

                IndexedApp app;
                app.name = dispName;
                app.launchTarget = fullPath;
                app.normalizedTarget = normTarget;
                AddAliasUnique(app.aliases, dispName);
                if (!resolvedTarget.empty())
                {
                    AddAliasUnique(app.aliases, FileStemPart(resolvedTarget));
                    if (EndsWithInsensitive(resolvedTarget, L".exe"))
                        AddAliasUnique(app.aliases, AppIndex::ResolveExecutableFriendlyName(resolvedTarget));
                }
                out.push_back(std::move(app));
            } while (FindNextFileW(hFind, &fd));

            FindClose(hFind);
        }

        void ScanAppsFolder(
            std::vector<IndexedApp>& out,
            std::unordered_set<std::wstring>& seenTargets,
            std::unordered_set<std::wstring>& seenNames,
            const std::atomic<bool>& stopFlag)
        {
            Microsoft::WRL::ComPtr<IShellItem> appsFolder;
            if (FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr,
                IID_PPV_ARGS(appsFolder.GetAddressOf()))) || !appsFolder)
                return;

            Microsoft::WRL::ComPtr<IEnumShellItems> enumItems;
            if (FAILED(appsFolder->BindToHandler(nullptr, BHID_EnumItems,
                IID_PPV_ARGS(enumItems.GetAddressOf()))) || !enumItems)
                return;

            for (;;)
            {
                if (stopFlag.load(std::memory_order_acquire))
                    break;
                Microsoft::WRL::ComPtr<IShellItem> item;
                ULONG fetched = 0;
                if (enumItems->Next(1, item.GetAddressOf(), &fetched) != S_OK || !fetched || !item)
                    break;

                PWSTR rawName = nullptr;
                if (FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &rawName)) || !rawName)
                    continue;
                std::wstring dispName = TrimCopyW(rawName);
                CoTaskMemFree(rawName);
                if (dispName.empty() || IsUnwantedShortcutName(dispName))
                    continue;

                Microsoft::WRL::ComPtr<IShellItem2> item2;
                if (FAILED(item.As(&item2)) || !item2)
                    continue;

                PWSTR rawAumid = nullptr;
                if (FAILED(item2->GetString(kPropKeyAumid, &rawAumid)) || !rawAumid || !rawAumid[0])
                {
                    if (rawAumid) CoTaskMemFree(rawAumid);
                    continue;
                }
                std::wstring aumid = TrimCopyW(rawAumid);
                CoTaskMemFree(rawAumid);

                const std::wstring normAumid = ToLowerFold(aumid);
                const std::wstring lowName = ToLowerFold(dispName);
                if (seenNames.count(lowName) || seenTargets.count(normAumid))
                    continue;

                // If AUMID happens to be a direct .exe path, normalize cleanly for dedupe against Start Menu
                const std::wstring normPath = AppIndex::CanonicalPathKey(aumid);
                if (EndsWithInsensitive(normPath, L".exe") && seenTargets.count(normPath))
                    continue;

                seenNames.insert(lowName);
                seenTargets.insert(normAumid);

                IndexedApp app;
                app.name = dispName;
                app.aumid = aumid;
                app.launchTarget = L"shell:AppsFolder\\" + aumid;
                app.normalizedTarget = EndsWithInsensitive(normPath, L".exe") ? normPath : normAumid;
                AddAliasUnique(app.aliases, dispName);
                out.push_back(std::move(app));
            }
        }
    } // namespace

    AppIndex::~AppIndex()
    {
        Shutdown();
    }

    std::wstring AppIndex::CanonicalPathKey(const std::wstring& path)
    {
        std::wstring out = ToLowerFold(TrimCopyW(path));
        for (auto& ch : out)
        {
            if (ch == L'/')
                ch = L'\\';
        }
        return out;
    }

    std::wstring AppIndex::ResolveExecutableFriendlyName(const std::wstring& exePath)
    {
        if (exePath.empty())
            return {};

        DWORD handle = 0;
        const DWORD sz = GetFileVersionInfoSizeW(exePath.c_str(), &handle);
        if (sz > 0)
        {
            std::vector<BYTE> buf(sz);
            if (GetFileVersionInfoW(exePath.c_str(), handle, sz, buf.data()))
            {
                struct LangCp { WORD lang; WORD cp; };
                LangCp* trans = nullptr;
                UINT transLen = 0;
                std::vector<DWORD> probes;
                if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
                    reinterpret_cast<void**>(&trans), &transLen) &&
                    trans && transLen >= sizeof(LangCp))
                {
                    probes.push_back(MAKELONG(trans[0].lang, trans[0].cp));
                }
                probes.push_back(MAKELONG(0x0409, 0x04B0));
                probes.push_back(MAKELONG(0x0409, 0x04E4));
                probes.push_back(MAKELONG(0x0000, 0x04B0));

                for (DWORD lcp : probes)
                {
                    const std::wstring prod = ReadVerString(buf.data(), lcp, L"ProductName");
                    const std::wstring desc = ReadVerString(buf.data(), lcp, L"FileDescription");
                    if (!desc.empty() && (prod.empty() || IsGenericWindowsProductName(prod)))
                        return desc;
                    if (!prod.empty() && !IsGenericWindowsProductName(prod))
                        return prod;
                    if (!desc.empty())
                        return desc;
                }
            }
        }

        return FileStemPart(exePath);
    }

    std::wstring AppIndex::FindFriendlyNameForExe(const std::wstring& exePath) const
    {
        const std::wstring key = CanonicalPathKey(exePath);
        if (key.empty())
            return {};
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_exeToFriendlyName.find(key);
        if (it != m_exeToFriendlyName.end())
            return it->second;
        return {};
    }

    void AppIndex::StartAsyncRefresh()
    {
        if (m_stopRequested.load(std::memory_order_acquire))
            return;
        if (m_refreshing.exchange(true, std::memory_order_acq_rel))
            return;

        if (m_workerThread)
        {
            CloseHandle(m_workerThread);
            m_workerThread = nullptr;
        }

        m_workerThread = CreateThread(nullptr, 0, [](void* param) -> DWORD {
            static_cast<AppIndex*>(param)->RefreshWorker();
            return 0;
        }, this, 0, nullptr);

        if (!m_workerThread)
            m_refreshing.store(false, std::memory_order_release);
    }

    void AppIndex::EnsureFreshAsync()
    {
        const ULONGLONG last = m_lastRefreshTick.load(std::memory_order_acquire);
        const ULONGLONG now = GetTickCount64();
        if (last == 0 || (now - last) >= kRefreshIntervalMs)
            StartAsyncRefresh();
    }

    void AppIndex::Shutdown()
    {
        m_stopRequested.store(true, std::memory_order_release);
        if (m_workerThread)
        {
            WaitForSingleObject(m_workerThread, 1500);
            CloseHandle(m_workerThread);
            m_workerThread = nullptr;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
        m_exeToFriendlyName.clear();
        m_iconCache.clear();
    }

    void AppIndex::RefreshWorker()
    {
        const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        std::vector<IndexedApp> collected;
        std::unordered_set<std::wstring> seenTargets;
        std::unordered_set<std::wstring> seenNames;
        std::unordered_map<std::wstring, std::wstring> exeToName;

        auto scanKnownDir = [&](REFKNOWNFOLDERID kfid) {
            PWSTR rawPath = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(kfid, 0, nullptr, &rawPath)) && rawPath)
            {
                const std::wstring dir(rawPath);
                CoTaskMemFree(rawPath);
                ScanStartMenuFolder(dir, collected, seenTargets, seenNames, exeToName, m_stopRequested);
            }
        };

        // Start Menu shortcuts preserve .lnk launch semantics & clean exe mappings; scan them first,
        // then append UWP / Store / MSIX apps from shell:AppsFolder.
        scanKnownDir(FOLDERID_StartMenu);
        scanKnownDir(FOLDERID_CommonStartMenu);
        ScanAppsFolder(collected, seenTargets, seenNames, m_stopRequested);

        if (!m_stopRequested.load(std::memory_order_acquire))
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_entries = std::move(collected);
            m_exeToFriendlyName = std::move(exeToName);
        }

        m_lastRefreshTick.store(GetTickCount64(), std::memory_order_release);
        m_refreshing.store(false, std::memory_order_release);

        if (SUCCEEDED(hrCo))
            CoUninitialize();
    }

    Microsoft::WRL::ComPtr<IWICBitmapSource> AppIndex::LoadAppIconWic(
        const IndexedApp& app, IWICImagingFactory* wicFactory)
    {
        if (!wicFactory || app.launchTarget.empty())
            return nullptr;

        const std::wstring key = ToLowerFold(app.launchTarget);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (auto it = m_iconCache.find(key); it != m_iconCache.end())
                return it->second;
        }

        Microsoft::WRL::ComPtr<IWICBitmapSource> result;
        Microsoft::WRL::ComPtr<IShellItem> shellItem;

        if (!app.aumid.empty())
        {
            Microsoft::WRL::ComPtr<IShellItem> appsFolder;
            if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr,
                IID_PPV_ARGS(appsFolder.GetAddressOf()))) && appsFolder)
            {
                Microsoft::WRL::ComPtr<IBindCtx> bindCtx;
                CreateBindCtx(0, bindCtx.GetAddressOf());
                SHCreateItemFromRelativeName(appsFolder.Get(), app.aumid.c_str(), bindCtx.Get(),
                    IID_PPV_ARGS(shellItem.GetAddressOf()));
            }
        }
        else
        {
            SHCreateItemFromParsingName(app.launchTarget.c_str(), nullptr,
                IID_PPV_ARGS(shellItem.GetAddressOf()));
        }

        if (shellItem)
        {
            Microsoft::WRL::ComPtr<IShellItemImageFactory> imgFactory;
            if (SUCCEEDED(shellItem.As(&imgFactory)) && imgFactory)
            {
                HBITMAP hbm = nullptr;
                if (SUCCEEDED(imgFactory->GetImage(SIZE{ 64, 64 },
                    SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbm)) && hbm)
                {
                    result = HBitmapToWicPbgra(wicFactory, hbm);
                    DeleteObject(hbm);
                }
            }
        }

        if (!result && app.aumid.empty())
        {
            SHFILEINFOW shfi{};
            if (SHGetFileInfoW(app.launchTarget.c_str(), 0, &shfi, sizeof(shfi),
                SHGFI_ICON | SHGFI_LARGEICON) && shfi.hIcon)
            {
                Microsoft::WRL::ComPtr<IWICBitmap> wicBmp;
                if (SUCCEEDED(wicFactory->CreateBitmapFromHICON(shfi.hIcon, wicBmp.GetAddressOf())) && wicBmp)
                {
                    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
                    if (SUCCEEDED(wicFactory->CreateFormatConverter(converter.GetAddressOf())) && converter &&
                        SUCCEEDED(converter->Initialize(wicBmp.Get(), GUID_WICPixelFormat32bppPBGRA,
                            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
                    {
                        result = converter;
                    }
                }
                DestroyIcon(shfi.hIcon);
            }
        }

        if (result)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_iconCache.size() > 256)
                m_iconCache.clear();
            m_iconCache.emplace(key, result);
        }
        return result;
    }

    std::vector<IndexedApp> AppIndex::Search(
        const std::wstring& query,
        const std::unordered_set<std::wstring>& excludeTargets,
        const std::unordered_set<std::wstring>& excludeNames,
        size_t maxResults,
        IWICImagingFactory* wicFactory)
    {
        if (TrimCopyW(query).empty() || maxResults == 0)
            return {};

        EnsureFreshAsync();

        struct ScoredCandidate
        {
            int tier = 99;
            IndexedApp app;
        };
        std::vector<ScoredCandidate> matches;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& entry : m_entries)
            {
                const std::wstring lowName = ToLowerFold(entry.name);
                if (excludeNames.count(lowName))
                    continue;
                if (!entry.normalizedTarget.empty() && excludeTargets.count(entry.normalizedTarget))
                    continue;

                const int tier = ScoreAliases(entry.aliases, query);
                if (tier >= 0)
                    matches.push_back(ScoredCandidate{ tier, entry });
            }
        }

        std::sort(matches.begin(), matches.end(), [](const ScoredCandidate& a, const ScoredCandidate& b) {
            if (a.tier != b.tier)
                return a.tier < b.tier;
            if (a.app.name.size() != b.app.name.size())
                return a.app.name.size() < b.app.name.size();
            return _wcsicmp(a.app.name.c_str(), b.app.name.c_str()) < 0;
        });

        std::vector<IndexedApp> out;
        const size_t limit = (std::min)(maxResults, matches.size());
        out.reserve(limit);
        for (size_t i = 0; i < limit; ++i)
        {
            IndexedApp item = std::move(matches[i].app);
            if (!item.iconBitmap && wicFactory)
                item.iconBitmap = LoadAppIconWic(item, wicFactory);
            out.push_back(std::move(item));
        }
        return out;
    }
} // namespace winrings
