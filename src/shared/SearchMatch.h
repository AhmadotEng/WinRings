#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace winrings
{
    inline std::wstring ToLowerFold(std::wstring_view s)
    {
        std::wstring out;
        out.reserve(s.size());
        for (wchar_t ch : s)
            out.push_back(static_cast<wchar_t>(std::towlower(ch)));
        return out;
    }

    inline std::wstring TrimCopyW(std::wstring_view s)
    {
        size_t first = 0;
        while (first < s.size() && std::iswspace(s[first]))
            ++first;
        size_t last = s.size();
        while (last > first && std::iswspace(s[last - 1]))
            --last;
        return std::wstring(s.substr(first, last - first));
    }

    /// Beckon-compatible 4-tier search scorer (returns 0..3 for match, -1 for no match):
    ///   0 = candidate starts with query (prefix)
    ///   1 = any word in candidate starts with query (handles spaces, delimiters, and CamelCase)
    ///   2 = acronym/initials match (when query length >= 2, e.g. "fe" -> "File Explorer", "ps" -> "PowerShell")
    ///   3 = substring match
    inline int ScoreSearchMatch(std::wstring_view candidate, std::wstring_view query)
    {
        const std::wstring q = ToLowerFold(TrimCopyW(query));
        if (q.empty())
            return -1;
        const std::wstring rawC = TrimCopyW(candidate);
        const std::wstring c = ToLowerFold(rawC);
        if (c.empty() || c.size() < q.size())
            return -1;

        // Tier 0: Exact prefix match
        if (c.rfind(q, 0) == 0)
            return 0;

        auto isSep = [](wchar_t ch) noexcept -> bool {
            return std::iswspace(ch) != 0 || ch == L'-' || ch == L'_' || ch == L'.' ||
                   ch == L'/' || ch == L'\\' || ch == L'(' || ch == L')';
        };

        std::wstring initials;
        initials.reserve(8);
        bool atWordStart = true;
        bool tier1 = false;
        wchar_t prevRaw = L'\0';

        for (size_t i = 0; i < c.size(); ++i)
        {
            const wchar_t rawCh = rawC[i];
            const wchar_t ch = c[i];
            if (isSep(rawCh))
            {
                atWordStart = true;
                prevRaw = rawCh;
                continue;
            }
            const bool camelBoundary = !atWordStart && i > 0 &&
                std::iswlower(prevRaw) != 0 && std::iswupper(rawCh) != 0;
            if (atWordStart || camelBoundary)
            {
                initials.push_back(ch);
                if (i > 0 && c.size() - i >= q.size() && c.compare(i, q.size(), q) == 0)
                    tier1 = true;
                atWordStart = false;
            }
            prevRaw = rawCh;
        }
        if (tier1)
            return 1;

        // Tier 2: Acronym / initials match (for queries >= 2 chars)
        if (q.size() >= 2 && initials.size() >= q.size())
        {
            if (initials.find(q) != std::wstring::npos)
                return 2;

            size_t qi = 0;
            for (wchar_t ic : initials)
            {
                if (ic == q[qi])
                {
                    ++qi;
                    if (qi == q.size())
                        return 2;
                }
            }
        }

        // Tier 3: General substring match
        if (c.find(q) != std::wstring::npos)
            return 3;

        return -1;
    }

    inline int ScoreAliases(const std::vector<std::wstring>& aliases, std::wstring_view query)
    {
        int bestTier = -1;
        for (const auto& alias : aliases)
        {
            if (alias.empty())
                continue;
            const int tier = ScoreSearchMatch(alias, query);
            if (tier >= 0 && (bestTier < 0 || tier < bestTier))
            {
                bestTier = tier;
                if (bestTier == 0)
                    break;
            }
        }
        return bestTier;
    }

    /// Scores primary identity aliases in tiers 0..3 and contextual secondary aliases
    /// (window titles, folder contents, parent folders) in tiers 4..7 so a primary match
    /// on a hidden pin or installed app always outranks contextual file/window hits.
    inline int ScoreSlotAliases(
        const std::vector<std::wstring>& primaryAliases,
        const std::vector<std::wstring>& secondaryAliases,
        std::wstring_view query)
    {
        const int primaryTier = ScoreAliases(primaryAliases, query);
        if (primaryTier >= 0)
            return primaryTier;
        const int secondaryTier = ScoreAliases(secondaryAliases, query);
        if (secondaryTier >= 0)
            return 4 + secondaryTier;
        return -1;
    }

    inline std::wstring CanonicalSearchKey(std::wstring_view pathOrUrl)
    {
        std::wstring s = TrimCopyW(pathOrUrl);
        if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"')
            s = TrimCopyW(std::wstring_view(s).substr(1, s.size() - 2));
        if (s.empty())
            return {};

        const std::wstring low = ToLowerFold(s);
        const bool isUrl =
            low.rfind(L"http://", 0) == 0 ||
            low.rfind(L"https://", 0) == 0 ||
            low.rfind(L"ms-settings:", 0) == 0 ||
            low.rfind(L"shell:", 0) == 0 ||
            low.rfind(L"winrings://", 0) == 0;
        if (isUrl)
            return low;

        std::wstring out = low;
        std::replace(out.begin(), out.end(), L'/', L'\\');
        while (out.size() > 3 && out.back() == L'\\')
            out.pop_back();
        return out;
    }

    inline std::vector<std::wstring> BuildBasicSearchAliases(std::wstring_view label, std::wstring_view pathOrUrl)
    {
        std::vector<std::wstring> aliases;
        auto pushUnique = [&](std::wstring v) {
            v = TrimCopyW(v);
            if (v.empty())
                return;
            const std::wstring folded = ToLowerFold(v);
            for (const auto& existing : aliases)
            {
                if (ToLowerFold(existing) == folded)
                    return;
            }
            aliases.push_back(std::move(v));
        };

        pushUnique(std::wstring(label));

        std::wstring target = TrimCopyW(pathOrUrl);
        if (target.size() >= 2 && target.front() == L'"' && target.back() == L'"')
            target = TrimCopyW(std::wstring_view(target).substr(1, target.size() - 2));
        if (target.empty())
            return aliases;

        const std::wstring lowTarget = ToLowerFold(target);
        if (lowTarget.rfind(L"http://", 0) == 0 || lowTarget.rfind(L"https://", 0) == 0)
        {
            const size_t schemePos = target.find(L"://");
            std::wstring rest = (schemePos != std::wstring::npos) ? target.substr(schemePos + 3) : target;
            if (ToLowerFold(rest).rfind(L"www.", 0) == 0)
                rest = rest.substr(4);
            const size_t stop = rest.find_first_of(L"/?#");
            const std::wstring host = (stop != std::wstring::npos) ? rest.substr(0, stop) : rest;
            pushUnique(host);
            const size_t dot = host.find(L'.');
            if (dot != std::wstring::npos && dot > 0)
                pushUnique(host.substr(0, dot));
            return aliases;
        }

        if (lowTarget.rfind(L"ms-settings:", 0) == 0 ||
            lowTarget.rfind(L"shell:", 0) == 0 ||
            lowTarget.rfind(L"winrings://", 0) == 0)
        {
            return aliases;
        }

        while (target.size() > 3 && (target.back() == L'\\' || target.back() == L'/'))
            target.pop_back();
        const size_t slash = target.find_last_of(L"\\/");
        const std::wstring fileName = (slash != std::wstring::npos) ? target.substr(slash + 1) : target;
        pushUnique(fileName);
        const size_t dot = fileName.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0)
            pushUnique(fileName.substr(0, dot));

        return aliases;
    }

    struct SearchRankKey
    {
        int tier = 99;           // 0..3 primary identity, 4..7 secondary contextual (window title/folder file/parent dir)
        int sourcePriority = 2;  // 0 = visible Ring slot, 1 = custom Search-Only item, 2 = system AppIndex item
        size_t nameLen = 0;
    };

    inline bool CompareSearchRankKey(const SearchRankKey& a, const SearchRankKey& b) noexcept
    {
        if (a.tier != b.tier)
            return a.tier < b.tier;
        if (a.sourcePriority != b.sourcePriority)
            return a.sourcePriority < b.sourcePriority;
        return a.nameLen < b.nameLen;
    }
} // namespace winrings
