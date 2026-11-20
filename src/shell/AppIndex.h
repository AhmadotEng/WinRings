#pragma once

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace winrings
{
    struct IndexedApp
    {
        std::wstring name;
        std::wstring launchTarget;      // .lnk/.exe path or "shell:AppsFolder\\<AUMID>"
        std::wstring normalizedTarget;  // lowercase resolved exe path or lowercase aumid
        std::wstring aumid;             // non-empty for shell:AppsFolder items
        std::vector<std::wstring> aliases;
        Microsoft::WRL::ComPtr<IWICBitmapSource> iconBitmap;
    };

    class AppIndex
    {
    public:
        AppIndex() = default;
        ~AppIndex();

        AppIndex(const AppIndex&) = delete;
        AppIndex& operator=(const AppIndex&) = delete;

        void StartAsyncRefresh();
        void EnsureFreshAsync();
        void Shutdown();

        /// Searches cached system apps (excluding keys/names already on the main ring).
        /// Results are sorted by (tier, nameLength, name) and include loaded WIC icons.
        std::vector<IndexedApp> Search(
            const std::wstring& query,
            const std::unordered_set<std::wstring>& excludeTargets,
            const std::unordered_set<std::wstring>& excludeNames,
            size_t maxResults,
            IWICImagingFactory* wicFactory);

        /// Resolves a user-friendly application name for an executable via PE VersionInfo
        /// (ProductName / FileDescription), falling back to meaningful parent folder / exe stem.
        static std::wstring ResolveExecutableFriendlyName(const std::wstring& exePath);

        /// Looks up a friendly Start Menu display name for an executable path if indexed.
        std::wstring FindFriendlyNameForExe(const std::wstring& exePath) const;

        static std::wstring CanonicalPathKey(const std::wstring& path);

    private:
        void RefreshWorker();
        Microsoft::WRL::ComPtr<IWICBitmapSource> LoadAppIconWic(
            const IndexedApp& app, IWICImagingFactory* wicFactory);

        mutable std::mutex m_mutex;
        std::vector<IndexedApp> m_entries;
        std::unordered_map<std::wstring, std::wstring> m_exeToFriendlyName;
        std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<IWICBitmapSource>> m_iconCache;

        std::atomic<bool> m_refreshing{ false };
        std::atomic<bool> m_stopRequested{ false };
        std::atomic<ULONGLONG> m_lastRefreshTick{ 0 };
        HANDLE m_workerThread = nullptr;
    };
} // namespace winrings
