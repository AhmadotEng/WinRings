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
} // namespace winrings
