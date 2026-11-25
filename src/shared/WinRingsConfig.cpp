#include "WinRingsConfig.h"

#include <ShlObj.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <variant>

#include "SearchMatch.h"

namespace winrings
{
    std::wstring Utf8ToWide(const std::string& utf8)
    {
        if (utf8.empty())
            return {};
        const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (needed <= 0)
            return {};
        std::wstring wide(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
        return wide;
    }

    std::string WideToUtf8(const std::wstring& wide)
    {
        if (wide.empty())
            return {};
        const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (needed <= 0)
            return {};
        std::string utf8(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed, nullptr, nullptr);
        return utf8;
    }

    const wchar_t* ToString(ScrollWheelBehavior v) noexcept
    {
        switch (v)
        {
        case ScrollWheelBehavior::CycleSlotsOrWindows: return L"CycleSlotsOrWindows";
        case ScrollWheelBehavior::CycleSubmenuOnly: return L"CycleSubmenuOnly";
        case ScrollWheelBehavior::Disabled: return L"Disabled";
        }
        return L"CycleSlotsOrWindows";
    }

    const wchar_t* ToString(WindowViewMode v) noexcept
    {
        switch (v)
        {
        case WindowViewMode::OuterArc: return L"OuterArc";
        case WindowViewMode::TitleColumn: return L"TitleColumn";
        }
        return L"OuterArc";
    }

    const wchar_t* ToString(SurfaceTexture v) noexcept
    {
        switch (v)
        {
        case SurfaceTexture::A4SAcrylic: return L"A4SAcrylic";
        case SurfaceTexture::TaskbarGlass: return L"TaskbarGlass";
        case SurfaceTexture::PureMatte: return L"PureMatte";
        }
        return L"A4SAcrylic";
    }

    const wchar_t* ToString(RingTheme v) noexcept
    {
        switch (v)
        {
        case RingTheme::A4S: return L"A4S";
        case RingTheme::DarkWood: return L"DarkWood";
        case RingTheme::Blue: return L"Blue";
        }
        return L"A4S";
    }

    const wchar_t* ToString(SlotKind v) noexcept
    {
        switch (v)
        {
        case SlotKind::App: return L"App";
        case SlotKind::Website: return L"Website";
        case SlotKind::File: return L"File";
        case SlotKind::Folder: return L"Folder";
        case SlotKind::Script: return L"Script";
        case SlotKind::Collection: return L"Collection";
        case SlotKind::RecycleBin: return L"RecycleBin";
        }
        return L"App";
    }

    namespace
    {
        struct JsonValue;
        using JsonObject = std::map<std::string, JsonValue, std::less<>>;
        using JsonArray = std::vector<JsonValue>;

        struct JsonValue
        {
            std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject> data{ nullptr };

            bool IsObject() const noexcept { return std::holds_alternative<JsonObject>(data); }
            bool IsArray() const noexcept { return std::holds_alternative<JsonArray>(data); }
            bool IsString() const noexcept { return std::holds_alternative<std::string>(data); }
            bool IsBool() const noexcept { return std::holds_alternative<bool>(data); }
            bool IsNumber() const noexcept { return std::holds_alternative<double>(data); }

            const JsonObject* AsObject() const noexcept { return std::get_if<JsonObject>(&data); }
            const JsonArray* AsArray() const noexcept { return std::get_if<JsonArray>(&data); }
            const std::string* AsString() const noexcept { return std::get_if<std::string>(&data); }
        };

        class JsonParser
        {
        public:
            explicit JsonParser(const std::string& src) : m_src(src) {}

            bool ParseRoot(JsonValue& out)
            {
                SkipWs();
                if (!ParseValue(out))
                    return false;
                SkipWs();
                return m_pos == m_src.size();
            }

        private:
            const std::string& m_src;
            size_t m_pos = 0;

            void SkipWs() noexcept
            {
                while (m_pos < m_src.size())
                {
                    const unsigned char c = static_cast<unsigned char>(m_src[m_pos]);
                    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                        ++m_pos;
                    else
                        break;
                }
            }

            bool Consume(char expected) noexcept
            {
                SkipWs();
                if (m_pos < m_src.size() && m_src[m_pos] == expected)
                {
                    ++m_pos;
                    return true;
                }
                return false;
            }

            static void AppendUtf8CodePoint(std::string& dst, uint32_t cp)
            {
                if (cp <= 0x7F)
                {
                    dst.push_back(static_cast<char>(cp));
                }
                else if (cp <= 0x7FF)
                {
                    dst.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                else if (cp <= 0xFFFF)
                {
                    dst.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    dst.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                else
                {
                    dst.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                    dst.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                    dst.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
            }

            bool ParseHex4(uint32_t& out) noexcept
            {
                if (m_pos + 4 > m_src.size())
                    return false;
                uint32_t val = 0;
                for (int i = 0; i < 4; ++i)
                {
                    const char c = m_src[m_pos++];
                    val <<= 4;
                    if (c >= '0' && c <= '9') val |= static_cast<uint32_t>(c - '0');
                    else if (c >= 'a' && c <= 'f') val |= static_cast<uint32_t>(10 + c - 'a');
                    else if (c >= 'A' && c <= 'F') val |= static_cast<uint32_t>(10 + c - 'A');
                    else return false;
                }
                out = val;
                return true;
            }

            bool ParseString(std::string& out)
            {
                if (m_pos >= m_src.size() || m_src[m_pos] != '"')
                    return false;
                ++m_pos;
                out.clear();
                while (m_pos < m_src.size())
                {
                    const char c = m_src[m_pos++];
                    if (c == '"')
                        return true;
                    if (c == '\\')
                    {
                        if (m_pos >= m_src.size())
                            return false;
                        const char esc = m_src[m_pos++];
                        switch (esc)
                        {
                        case '"': out.push_back('"'); break;
                        case '\\': out.push_back('\\'); break;
                        case '/': out.push_back('/'); break;
                        case 'b': out.push_back('\b'); break;
                        case 'f': out.push_back('\f'); break;
                        case 'n': out.push_back('\n'); break;
                        case 'r': out.push_back('\r'); break;
                        case 't': out.push_back('\t'); break;
                        case 'u':
                        {
                            uint32_t cp = 0;
                            if (!ParseHex4(cp))
                                return false;
                            if (cp >= 0xD800 && cp <= 0xDBFF && m_pos + 6 <= m_src.size() &&
                                m_src[m_pos] == '\\' && m_src[m_pos + 1] == 'u')
                            {
                                m_pos += 2;
                                uint32_t low = 0;
                                if (!ParseHex4(low))
                                    return false;
                                if (low >= 0xDC00 && low <= 0xDFFF)
                                    cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                            }
                            AppendUtf8CodePoint(out, cp);
                            break;
                        }
                        default:
                            return false;
                        }
                    }
                    else
                    {
                        out.push_back(c);
                    }
                }
                return false;
            }

            bool ParseNumber(double& out) noexcept
            {
                const char* start = m_src.c_str() + m_pos;
                char* end = nullptr;
                out = std::strtod(start, &end);
                if (end == start)
                    return false;
                m_pos = static_cast<size_t>(end - m_src.c_str());
                return true;
            }

            bool ParseValue(JsonValue& out)
            {
                SkipWs();
                if (m_pos >= m_src.size())
                    return false;

                const char c = m_src[m_pos];
                if (c == '{')
                {
                    ++m_pos;
                    JsonObject obj;
                    SkipWs();
                    if (m_pos < m_src.size() && m_src[m_pos] == '}')
                    {
                        ++m_pos;
                        out.data = std::move(obj);
                        return true;
                    }
                    for (;;)
                    {
                        SkipWs();
                        std::string key;
                        if (!ParseString(key) || !Consume(':'))
                            return false;
                        JsonValue val;
                        if (!ParseValue(val))
                            return false;
                        obj.insert_or_assign(std::move(key), std::move(val));
                        SkipWs();
                        if (m_pos < m_src.size() && m_src[m_pos] == ',')
                        {
                            ++m_pos;
                            continue;
                        }
                        if (m_pos < m_src.size() && m_src[m_pos] == '}')
                        {
                            ++m_pos;
                            out.data = std::move(obj);
                            return true;
                        }
                        return false;
                    }
                }
                if (c == '[')
                {
                    ++m_pos;
                    JsonArray arr;
                    SkipWs();
                    if (m_pos < m_src.size() && m_src[m_pos] == ']')
                    {
                        ++m_pos;
                        out.data = std::move(arr);
                        return true;
                    }
                    for (;;)
                    {
                        JsonValue item;
                        if (!ParseValue(item))
                            return false;
                        arr.push_back(std::move(item));
                        SkipWs();
                        if (m_pos < m_src.size() && m_src[m_pos] == ',')
                        {
                            ++m_pos;
                            continue;
                        }
                        if (m_pos < m_src.size() && m_src[m_pos] == ']')
                        {
                            ++m_pos;
                            out.data = std::move(arr);
                            return true;
                        }
                        return false;
                    }
                }
                if (c == '"')
                {
                    std::string s;
                    if (!ParseString(s))
                        return false;
                    out.data = std::move(s);
                    return true;
                }
                if (m_src.compare(m_pos, 4, "true") == 0)
                {
                    m_pos += 4;
                    out.data = true;
                    return true;
                }
                if (m_src.compare(m_pos, 5, "false") == 0)
                {
                    m_pos += 5;
                    out.data = false;
                    return true;
                }
                if (m_src.compare(m_pos, 4, "null") == 0)
                {
                    m_pos += 4;
                    out.data = nullptr;
                    return true;
                }
                if (c == '-' || (c >= '0' && c <= '9'))
                {
                    double d = 0.0;
                    if (!ParseNumber(d))
                        return false;
                    out.data = d;
                    return true;
                }
                return false;
            }
        };

        void AppendEscapedJsonString(std::string& out, const std::string& s)
        {
            out.push_back('"');
            for (unsigned char c : s)
            {
                switch (c)
                {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20)
                    {
                        char buf[8]{};
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                        out += buf;
                    }
                    else
                    {
                        out.push_back(static_cast<char>(c));
                    }
                    break;
                }
            }
            out.push_back('"');
        }

        template <typename T>
        bool TryGetBool(const JsonObject& obj, const char* key, T& dst)
        {
            const auto it = obj.find(key);
            if (it != obj.end() && it->second.IsBool())
            {
                dst = std::get<bool>(it->second.data);
                return true;
            }
            return false;
        }

        bool TryGetInt(const JsonObject& obj, const char* key, int& dst)
        {
            const auto it = obj.find(key);
            if (it != obj.end() && it->second.IsNumber())
            {
                dst = static_cast<int>(std::llround(std::get<double>(it->second.data)));
                return true;
            }
            return false;
        }

        bool TryGetUInt(const JsonObject& obj, const char* key, UINT& dst)
        {
            const auto it = obj.find(key);
            if (it != obj.end() && it->second.IsNumber())
            {
                const double val = std::get<double>(it->second.data);
                dst = static_cast<UINT>((std::max)(0.0, std::round(val)));
                return true;
            }
            return false;
        }

        bool TryGetDouble(const JsonObject& obj, const char* key, double& dst)
        {
            const auto it = obj.find(key);
            if (it != obj.end() && it->second.IsNumber())
            {
                dst = std::get<double>(it->second.data);
                return true;
            }
            return false;
        }

        bool TryGetWString(const JsonObject& obj, const char* key, std::wstring& dst)
        {
            const auto it = obj.find(key);
            if (it != obj.end() && it->second.IsString())
            {
                dst = Utf8ToWide(*it->second.AsString());
                return true;
            }
            return false;
        }

        template <typename EnumT>
        EnumT ParseEnum(const std::wstring& value, EnumT fallback,
            std::initializer_list<std::pair<const wchar_t*, EnumT>> entries) noexcept
        {
            for (const auto& entry : entries)
            {
                if (_wcsicmp(value.c_str(), entry.first) == 0)
                    return entry.second;
            }
            return fallback;
        }

        PinnedSlotConfig ParseSlot(const JsonObject& obj)
        {
            PinnedSlotConfig slot;
            TryGetWString(obj, "id", slot.id);
            std::wstring kindStr;
            if (TryGetWString(obj, "kind", kindStr))
            {
                slot.kind = ParseEnum<SlotKind>(kindStr, SlotKind::App, {
                    { L"App", SlotKind::App },
                    { L"Website", SlotKind::Website },
                    { L"File", SlotKind::File },
                    { L"Folder", SlotKind::Folder },
                    { L"Script", SlotKind::Script },
                    { L"Collection", SlotKind::Collection },
                    { L"RecycleBin", SlotKind::RecycleBin }
                });
            }
            TryGetWString(obj, "label", slot.label);
            TryGetWString(obj, "pathOrUrl", slot.pathOrUrl);
            TryGetWString(obj, "iconCacheFile", slot.iconCacheFile);
            TryGetBool(obj, "runAsAdmin", slot.runAsAdmin);
            TryGetWString(obj, "arguments", slot.arguments);
            TryGetBool(obj, "hiddenFromRing", slot.hiddenFromRing);
            TryGetInt(obj, "maxFolderItems", slot.maxFolderItems);
            slot.maxFolderItems = std::clamp(slot.maxFolderItems, 1, 12);

            const auto itemsIt = obj.find("items");
            if (itemsIt != obj.end() && itemsIt->second.IsArray())
            {
                for (const auto& childVal : *itemsIt->second.AsArray())
                {
                    if (const auto* childObj = childVal.AsObject())
                        slot.items.push_back(ParseSlot(*childObj));
                }
            }
            return slot;
        }

        void SerializeSlot(std::string& out, const PinnedSlotConfig& slot, int indentLevel)
        {
            const std::string ind(static_cast<size_t>(indentLevel * 2), ' ');
            const std::string ind2(static_cast<size_t>((indentLevel + 1) * 2), ' ');
            out += ind + "{\n";
            out += ind2 + "\"id\": "; AppendEscapedJsonString(out, WideToUtf8(slot.id)); out += ",\n";
            out += ind2 + "\"kind\": "; AppendEscapedJsonString(out, WideToUtf8(ToString(slot.kind))); out += ",\n";
            out += ind2 + "\"label\": "; AppendEscapedJsonString(out, WideToUtf8(slot.label)); out += ",\n";
            out += ind2 + "\"pathOrUrl\": "; AppendEscapedJsonString(out, WideToUtf8(slot.pathOrUrl)); out += ",\n";
            out += ind2 + "\"iconCacheFile\": "; AppendEscapedJsonString(out, WideToUtf8(slot.iconCacheFile)); out += ",\n";
            if (slot.runAsAdmin)
                out += ind2 + "\"runAsAdmin\": true,\n";
            if (!slot.arguments.empty())
            {
                out += ind2 + "\"arguments\": "; AppendEscapedJsonString(out, WideToUtf8(slot.arguments)); out += ",\n";
            }
            if (slot.hiddenFromRing)
                out += ind2 + "\"hiddenFromRing\": true,\n";
            out += ind2 + "\"maxFolderItems\": " + std::to_string(slot.maxFolderItems) + ",\n";
            out += ind2 + "\"items\": [";
            if (!slot.items.empty())
            {
                out += "\n";
                for (size_t i = 0; i < slot.items.size(); ++i)
                {
                    SerializeSlot(out, slot.items[i], indentLevel + 2);
                    if (i + 1 < slot.items.size())
                        out += ",";
                    out += "\n";
                }
                out += ind2;
            }
            out += "]\n";
            out += ind + "}";
        }

        RingProfileConfig ParseProfile(const JsonObject& obj, const std::wstring& fallbackId = L"profile")
        {
            RingProfileConfig p;
            TryGetWString(obj, "id", p.id);
            TryGetWString(obj, "name", p.name);
            if (p.id.empty())
                p.id = p.name.empty() ? (fallbackId.empty() ? L"profile" : fallbackId) : p.name;
            if (p.name.empty())
                p.name = p.id;
            TryGetBool(obj, "includeRunningApps", p.includeRunningApps);
            const auto pinsIt = obj.find("pinnedSlots");
            if (pinsIt != obj.end() && pinsIt->second.IsArray())
            {
                for (const auto& val : *pinsIt->second.AsArray())
                {
                    if (const auto* slotObj = val.AsObject())
                        p.pinnedSlots.push_back(ParseSlot(*slotObj));
                }
            }
            return p;
        }

        void SerializeProfile(std::string& out, const RingProfileConfig& prof, int indentLevel, bool includeVersion = false)
        {
            const std::string ind(static_cast<size_t>(indentLevel * 2), ' ');
            const std::string ind2(static_cast<size_t>((indentLevel + 1) * 2), ' ');
            out += ind + "{\n";
            if (includeVersion)
                out += ind2 + "\"version\": 1,\n";
            out += ind2 + "\"id\": "; AppendEscapedJsonString(out, WideToUtf8(prof.id)); out += ",\n";
            out += ind2 + "\"name\": "; AppendEscapedJsonString(out, WideToUtf8(prof.name)); out += ",\n";
            out += ind2 + "\"includeRunningApps\": "; out += (prof.includeRunningApps ? "true" : "false"); out += ",\n";
            out += ind2 + "\"pinnedSlots\": [";
            if (!prof.pinnedSlots.empty())
            {
                out += "\n";
                for (size_t i = 0; i < prof.pinnedSlots.size(); ++i)
                {
                    SerializeSlot(out, prof.pinnedSlots[i], indentLevel + 2);
                    if (i + 1 < prof.pinnedSlots.size())
                        out += ",";
                    out += "\n";
                }
                out += ind2;
            }
            out += "]\n";
            out += ind + "}";
        }

        bool ReadUtf8FileBounded(const std::wstring& path, std::string& outBytes)
        {
            HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;

            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024)
            {
                CloseHandle(file);
                return false;
            }
            std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
            DWORD read = 0;
            const BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
            CloseHandle(file);
            if (!ok || read != bytes.size())
                return false;
            outBytes = std::move(bytes);
            return true;
        }

        bool WriteUtf8FileAtomic(const std::wstring& path, const std::string& json)
        {
            const size_t slash = path.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
            {
                const std::wstring dir = path.substr(0, slash);
                SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
            }

            const std::wstring tmpPath = path + L".tmp";
            HANDLE file = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;

            DWORD written = 0;
            const BOOL ok = WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written, nullptr)
                && written == json.size();
            FlushFileBuffers(file);
            CloseHandle(file);
            if (!ok)
            {
                DeleteFileW(tmpPath.c_str());
                return false;
            }

            if (!MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                DeleteFileW(tmpPath.c_str());
                return false;
            }
            return true;
        }

        std::wstring FileStemOfProfilePath(const std::wstring& fileName)
        {
            const size_t dot = fileName.find_last_of(L'.');
            return (dot != std::wstring::npos) ? fileName.substr(0, dot) : fileName;
        }

        bool UpgradeBuiltInSMenuProfile(RingProfileConfig& prof)
        {
            if (_wcsicmp(prof.id.c_str(), L"SMenu") != 0)
                return false;
            bool changed = false;
            for (auto& slot : prof.pinnedSlots)
            {
                if (slot.id == L"smenu-power")
                {
                    if (_wcsicmp(slot.pathOrUrl.c_str(), L"winrings://power/shutdown") == 0)
                    {
                        slot.pathOrUrl.clear();
                        changed = true;
                    }
                    if (slot.items.size() == 3 &&
                        slot.items[0].id == L"smenu-switch-user" &&
                        slot.items[2].id == L"smenu-restart")
                    {
                        std::swap(slot.items[0], slot.items[2]);
                        changed = true;
                    }
                    break;
                }
            }
            return changed;
        }
        TriggerBinding ParseTrigger(const JsonObject& obj)
        {
            TriggerBinding b;
            std::wstring s;
            if (TryGetWString(obj, "kind", s))
            {
                b.kind = ParseEnum<TriggerKind>(s, TriggerKind::Disabled, {
                    { L"Disabled", TriggerKind::Disabled },
                    { L"MouseButton", TriggerKind::MouseButton },
                    { L"RightLeftChord", TriggerKind::RightLeftChord },
                    { L"KeyChord", TriggerKind::KeyChord },
                    { L"SoloModifier", TriggerKind::SoloModifier }
                });
            }
            if (TryGetWString(obj, "mouseButton", s))
            {
                b.mouseButton = ParseEnum<MouseTriggerButton>(s, MouseTriggerButton::Disabled, {
                    { L"SideBack", MouseTriggerButton::SideBack },
                    { L"SideForward", MouseTriggerButton::SideForward },
                    { L"Middle", MouseTriggerButton::Middle },
                    { L"RightLeftChord", MouseTriggerButton::RightLeftChord },
                    { L"Disabled", MouseTriggerButton::Disabled }
                });
            }
            TryGetUInt(obj, "modifiers", b.modifiers);
            TryGetUInt(obj, "vk", b.vk);
            b.vk = NormalizeTriggerVk(b.vk);
            TryGetWString(obj, "profileId", b.profileId);
            if (b.profileId.empty())
                b.profileId = L"default";
            return b;
        }

        void SerializeTrigger(std::string& out, const TriggerBinding& b, int indentLevel)
        {
            const std::string ind(static_cast<size_t>(indentLevel * 2), ' ');
            const std::string ind2(static_cast<size_t>((indentLevel + 1) * 2), ' ');
            out += ind + "{\n";
            out += ind2 + "\"kind\": "; AppendEscapedJsonString(out, WideToUtf8(ToString(b.kind))); out += ",\n";
            out += ind2 + "\"mouseButton\": "; AppendEscapedJsonString(out, WideToUtf8(ToString(b.mouseButton))); out += ",\n";
            out += ind2 + "\"modifiers\": " + std::to_string(b.modifiers) + ",\n";
            out += ind2 + "\"vk\": " + std::to_string(b.vk) + ",\n";
            out += ind2 + "\"profileId\": "; AppendEscapedJsonString(out, WideToUtf8(b.profileId.empty() ? L"default" : b.profileId)); out += "\n";
            out += ind + "}";
        }
    } // namespace

    void SwitcherConfig::SyncLegacyTriggerMirrors() noexcept
    {
        mouseTrigger = MouseTriggerButton::Disabled;
        hotkeyEnabled = false;
        hotkeyModifiers = MOD_ALT;
        hotkeyVk = VK_SPACE;
        menuKeyEnabled = false;
        interceptWinTab = false;
        interceptAltTab = false;

        for (const auto& t : triggers)
        {
            if (t.kind == TriggerKind::RightLeftChord && mouseTrigger == MouseTriggerButton::Disabled)
                mouseTrigger = MouseTriggerButton::RightLeftChord;
            else if (t.kind == TriggerKind::MouseButton && mouseTrigger == MouseTriggerButton::Disabled)
                mouseTrigger = t.mouseButton;
            else if (t.kind == TriggerKind::KeyChord)
            {
                const UINT nvk = NormalizeTriggerVk(t.vk);
                if (t.modifiers == 0 && nvk == VK_APPS)
                    menuKeyEnabled = true;
                else if (t.modifiers == MOD_WIN && nvk == VK_TAB)
                    interceptWinTab = true;
                else if (t.modifiers == MOD_ALT && nvk == VK_TAB)
                    interceptAltTab = true;
                else if (!hotkeyEnabled && nvk != 0)
                {
                    hotkeyEnabled = true;
                    hotkeyModifiers = t.modifiers;
                    hotkeyVk = nvk;
                }
            }
        }
    }

    std::string RingProfileConfig::ToJsonUtf8() const
    {
        std::string out;
        out.reserve(1024);
        SerializeProfile(out, *this, 0, true);
        out += "\n";
        return out;
    }

    bool RingProfileConfig::ParseJsonUtf8(const std::string& jsonUtf8, RingProfileConfig& outProfile, const std::wstring& fallbackId)
    {
        JsonValue rootVal;
        JsonParser parser(jsonUtf8);
        if (!parser.ParseRoot(rootVal))
            return false;
        const auto* obj = rootVal.AsObject();
        if (!obj)
            return false;
        outProfile = ParseProfile(*obj, fallbackId);
        return true;
    }

    bool RingProfileConfig::LoadFromFile(const std::wstring& path, RingProfileConfig& outProfile, const std::wstring& fallbackId)
    {
        std::string bytes;
        if (!ReadUtf8FileBounded(path, bytes))
            return false;
        std::wstring stem = fallbackId;
        if (stem.empty())
        {
            const size_t slash = path.find_last_of(L"\\/");
            const std::wstring fn = (slash != std::wstring::npos) ? path.substr(slash + 1) : path;
            stem = FileStemOfProfilePath(fn);
        }
        return ParseJsonUtf8(bytes, outProfile, stem);
    }

    bool RingProfileConfig::SaveToFile(const std::wstring& path) const
    {
        return WriteUtf8FileAtomic(path, ToJsonUtf8());
    }

    std::wstring SwitcherConfig::AppDataDirectory()
    {
        PWSTR raw = nullptr;
        std::wstring base;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) && raw)
        {
            base = raw;
            CoTaskMemFree(raw);
        }
        else
        {
            wchar_t buf[MAX_PATH]{};
            const DWORD len = GetEnvironmentVariableW(L"APPDATA", buf, _countof(buf));
            if (len > 0 && len < _countof(buf))
                base = buf;
            else
                base = L".";
        }
        return base + L"\\WinRings";
    }

    std::wstring SwitcherConfig::ConfigPath()
    {
        return AppDataDirectory() + L"\\config.json";
    }

    namespace
    {
        std::wstring LegacySearchItemsPath(const std::wstring& configPath)
        {
            if (!configPath.empty())
            {
                const size_t slash = configPath.find_last_of(L"\\/");
                if (slash != std::wstring::npos && slash > 0)
                    return configPath.substr(0, slash) + L"\\search.json";
            }
            return SwitcherConfig::AppDataDirectory() + L"\\search.json";
        }

        bool LoadLegacySearchItemsFromFile(const std::wstring& path, std::vector<PinnedSlotConfig>& outItems)
        {
            std::string bytes;
            if (!ReadUtf8FileBounded(path, bytes))
                return false;
            JsonValue rootVal;
            JsonParser parser(bytes);
            if (!parser.ParseRoot(rootVal))
                return false;
            const auto* obj = rootVal.AsObject();
            if (!obj)
                return false;

            std::vector<PinnedSlotConfig> parsed;
            auto it = obj->find("items");
            if (it == obj->end())
                it = obj->find("searchItems");
            if (it != obj->end() && it->second.IsArray())
            {
                for (const auto& val : *it->second.AsArray())
                {
                    if (const auto* slotObj = val.AsObject())
                    {
                        PinnedSlotConfig s = ParseSlot(*slotObj);
                        s.hiddenFromRing = true;
                        parsed.push_back(std::move(s));
                    }
                }
            }
            outItems = std::move(parsed);
            return true;
        }
    } // namespace

    std::wstring SwitcherConfig::SanitizeProfileId(const std::wstring& rawName)
    {
        std::wstring out;
        out.reserve(rawName.size());
        for (wchar_t ch : rawName)
        {
            if (ch < 0x20 || ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' ||
                ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|')
            {
                continue;
            }
            out.push_back(ch);
        }
        while (!out.empty() && (out.front() == L' ' || out.front() == L'\t' || out.front() == L'.'))
            out.erase(out.begin());
        while (!out.empty() && (out.back() == L' ' || out.back() == L'\t' || out.back() == L'.'))
            out.pop_back();
        if (out.size() > 28)
            out.resize(28);
        while (!out.empty() && (out.back() == L' ' || out.back() == L'.'))
            out.pop_back();
        if (out.empty())
            out = L"profile";
        return out;
    }

    std::wstring SwitcherConfig::ProfilesDirectory()
    {
        return AppDataDirectory() + L"\\profiles";
    }

    std::wstring SwitcherConfig::ProfilesDirectoryForConfigPath(const std::wstring& configPath)
    {
        const size_t slash = configPath.find_last_of(L"\\/");
        if (slash != std::wstring::npos && slash > 0)
            return configPath.substr(0, slash) + L"\\profiles";
        return ProfilesDirectory();
    }

    std::wstring SwitcherConfig::ProfileFilePath(const std::wstring& profileId, const std::wstring& configPath)
    {
        const std::wstring dir = configPath.empty()
            ? ProfilesDirectory()
            : ProfilesDirectoryForConfigPath(configPath);
        const std::wstring safeId = SanitizeProfileId(profileId.empty() ? L"default" : profileId);
        return dir + L"\\" + safeId + L".json";
    }

    RingProfileConfig SwitcherConfig::CreateBuiltInSMenuProfile()
    {
        RingProfileConfig smenu;
        smenu.id = L"SMenu";
        smenu.name = L"SMenu";
        smenu.includeRunningApps = false;

        PinnedSlotConfig winMenu;
        winMenu.id = L"smenu-windows";
        winMenu.kind = SlotKind::Collection;
        winMenu.label = L"Windows";
        winMenu.pathOrUrl = L"winrings://system/start-menu";
        winMenu.iconCacheFile = L"builtin:windows";
        winMenu.maxFolderItems = 11;
        winMenu.items = {
            PinnedSlotConfig{ L"smenu-apps", SlotKind::File, L"Installed Apps", L"ms-settings:appsfeatures", L"builtin:apps", 8, {} },
            PinnedSlotConfig{ L"smenu-poweropts", SlotKind::File, L"Power Options", L"ms-settings:powersleep", L"builtin:power-opts", 8, {} },
            PinnedSlotConfig{ L"smenu-eventvwr", SlotKind::File, L"Event Viewer", L"%SystemRoot%\\System32\\eventvwr.msc", L"", 8, {} },
            PinnedSlotConfig{ L"smenu-system", SlotKind::File, L"System", L"ms-settings:about", L"builtin:system", 8, {} },
            PinnedSlotConfig{ L"smenu-devmgmt", SlotKind::File, L"Device Manager", L"%SystemRoot%\\System32\\devmgmt.msc", L"", 8, {} },
            PinnedSlotConfig{ L"smenu-ncpa", SlotKind::File, L"Network Connections", L"%SystemRoot%\\System32\\ncpa.cpl", L"", 8, {} },
            PinnedSlotConfig{ L"smenu-diskmgmt", SlotKind::File, L"Disk Management", L"%SystemRoot%\\System32\\diskmgmt.msc", L"", 8, {} },
            PinnedSlotConfig{ L"smenu-compmgmt", SlotKind::File, L"Computer Management", L"%SystemRoot%\\System32\\compmgmt.msc", L"", 8, {} },
            PinnedSlotConfig{ L"smenu-taskmgr", SlotKind::App, L"Task Manager", L"%SystemRoot%\\System32\\taskmgr.exe", L"", 8, {} },
            PinnedSlotConfig{ L"smenu-explorer", SlotKind::App, L"File Explorer", L"%SystemRoot%\\explorer.exe", L"", 8, {} },
            PinnedSlotConfig{ L"smenu-run", SlotKind::File, L"Run", L"winrings://system/run-dialog", L"builtin:run", 8, {} }
        };

        PinnedSlotConfig cmdAdmin{ L"smenu-cmd-admin", SlotKind::App, L"CMD (Admin)", L"%SystemRoot%\\System32\\cmd.exe", L"", 8, {}, true, L"" };
        PinnedSlotConfig psAdmin{ L"smenu-ps-admin", SlotKind::App, L"PowerShell (Admin)", L"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", L"", 8, {}, true, L"" };

        PinnedSlotConfig powerMenu;
        powerMenu.id = L"smenu-power";
        powerMenu.kind = SlotKind::Collection;
        powerMenu.label = L"Power";
        powerMenu.pathOrUrl = L"";
        powerMenu.iconCacheFile = L"builtin:power";
        powerMenu.maxFolderItems = 3;
        // At 6 o'clock (+pi/2), increasing angle sweeps right-to-left: Restart (right), Shut Down (center), Switch User (left).
        powerMenu.items = {
            PinnedSlotConfig{ L"smenu-restart", SlotKind::File, L"Restart", L"winrings://power/restart", L"builtin:restart", 8, {} },
            PinnedSlotConfig{ L"smenu-shutdown", SlotKind::File, L"Shut Down", L"winrings://power/shutdown", L"builtin:shutdown", 8, {} },
            PinnedSlotConfig{ L"smenu-switch-user", SlotKind::File, L"Switch User", L"winrings://power/switch-user", L"builtin:switch-user", 8, {} }
        };

        PinnedSlotConfig settingsSlot{ L"smenu-settings", SlotKind::File, L"Settings", L"ms-settings:", L"builtin:settings", 8, {} };
        PinnedSlotConfig controlSlot{ L"smenu-control", SlotKind::App, L"Control Panel", L"%SystemRoot%\\System32\\control.exe", L"", 8, {} };

        // Clockwise from 12 o'clock: Top=Windows, Right=CMD+PowerShell(Admin), Bottom=Power, Left=Settings+Control Panel
        smenu.pinnedSlots = {
            std::move(winMenu),
            std::move(cmdAdmin),
            std::move(psAdmin),
            std::move(powerMenu),
            std::move(settingsSlot),
            std::move(controlSlot)
        };
        return smenu;
    }

    RingProfileConfig* SwitcherConfig::FindProfile(const std::wstring& profileId) noexcept
    {
        const wchar_t* target = profileId.empty() ? L"default" : profileId.c_str();
        for (auto& p : profiles)
        {
            if (_wcsicmp(p.id.c_str(), target) == 0)
                return &p;
        }
        return nullptr;
    }

    const RingProfileConfig* SwitcherConfig::FindProfile(const std::wstring& profileId) const noexcept
    {
        const wchar_t* target = profileId.empty() ? L"default" : profileId.c_str();
        for (const auto& p : profiles)
        {
            if (_wcsicmp(p.id.c_str(), target) == 0)
                return &p;
        }
        return nullptr;
    }

    const RingProfileConfig& SwitcherConfig::ResolveProfile(const std::wstring& profileId) const noexcept
    {
        if (const RingProfileConfig* p = FindProfile(profileId))
            return *p;
        if (const RingProfileConfig* def = FindProfile(L"default"))
            return *def;
        if (!profiles.empty())
            return profiles.front();
        static const RingProfileConfig kEmptyFallback{};
        return kEmptyFallback;
    }

    void SwitcherConfig::SyncDefaultProfileMirror()
    {
        if (RingProfileConfig* def = FindProfile(L"default"))
        {
            def->pinnedSlots = pinnedSlots;
            def->includeRunningApps = includeUnpinnedRunningApps;
        }
        else
        {
            RingProfileConfig newDef;
            newDef.id = L"default";
            newDef.name = L"default";
            newDef.includeRunningApps = includeUnpinnedRunningApps;
            newDef.pinnedSlots = pinnedSlots;
            profiles.insert(profiles.begin(), std::move(newDef));
        }
    }

    void SwitcherConfig::PullFromDefaultProfileMirror()
    {
        if (const RingProfileConfig* def = FindProfile(L"default"))
        {
            pinnedSlots = def->pinnedSlots;
            includeUnpinnedRunningApps = def->includeRunningApps;
        }
    }

    SwitcherConfig SwitcherConfig::CreateDefault()
    {
        SwitcherConfig cfg;
        cfg.version = 6;
        cfg.triggers = {
            TriggerBinding{ TriggerKind::MouseButton, MouseTriggerButton::SideBack, 0, 0, L"default" },
            TriggerBinding{ TriggerKind::KeyChord, MouseTriggerButton::Disabled, MOD_ALT, VK_SPACE, L"default" }
        };
        cfg.SyncLegacyTriggerMirrors();
        cfg.pinnedSlots = {
            PinnedSlotConfig{ L"pin-explorer", SlotKind::App, L"Explorer", L"C:\\Windows\\explorer.exe", L"", 8, {} },
            PinnedSlotConfig{ L"pin-downloads", SlotKind::Folder, L"Downloads", L"%USERPROFILE%\\Downloads", L"", 8, {} },
            PinnedSlotConfig{ L"pin-trash", SlotKind::RecycleBin, L"Recycle Bin", L"", L"", 8, {} }
        };
        RingProfileConfig defProf;
        defProf.id = L"default";
        defProf.name = L"default";
        defProf.includeRunningApps = cfg.includeUnpinnedRunningApps;
        defProf.pinnedSlots = cfg.pinnedSlots;
        cfg.profiles = { std::move(defProf), CreateBuiltInSMenuProfile() };
        return cfg;
    }

    bool SwitcherConfig::ParseJsonUtf8(const std::string& jsonUtf8, SwitcherConfig& outConfig)
    {
        JsonValue rootVal;
        JsonParser parser(jsonUtf8);
        if (!parser.ParseRoot(rootVal))
            return false;
        const auto* obj = rootVal.AsObject();
        if (!obj)
            return false;

        SwitcherConfig cfg = CreateDefault();
        TryGetInt(*obj, "version", cfg.version);

        std::wstring s;
        if (TryGetWString(*obj, "mouseTrigger", s))
        {
            cfg.mouseTrigger = ParseEnum<MouseTriggerButton>(s, cfg.mouseTrigger, {
                { L"SideBack", MouseTriggerButton::SideBack },
                { L"SideForward", MouseTriggerButton::SideForward },
                { L"Middle", MouseTriggerButton::Middle },
                { L"RightLeftChord", MouseTriggerButton::RightLeftChord },
                { L"Disabled", MouseTriggerButton::Disabled }
            });
        }
        if (TryGetWString(*obj, "scrollWheelMode", s))
        {
            cfg.scrollWheelMode = ParseEnum<ScrollWheelBehavior>(s, cfg.scrollWheelMode, {
                { L"CycleSlotsOrWindows", ScrollWheelBehavior::CycleSlotsOrWindows },
                { L"CycleSubmenuOnly", ScrollWheelBehavior::CycleSubmenuOnly },
                { L"Disabled", ScrollWheelBehavior::Disabled }
            });
        }
        TryGetBool(*obj, "gestureTapOpenClose", cfg.gestureTapOpenClose);
        TryGetBool(*obj, "gestureHoldReleaseSwitch", cfg.gestureHoldReleaseSwitch);
        TryGetBool(*obj, "doubleTapLastApp", cfg.doubleTapLastApp);
        TryGetInt(*obj, "tapThresholdMs", cfg.tapThresholdMs);
        TryGetInt(*obj, "doubleTapWindowMs", cfg.doubleTapWindowMs);
        TryGetBool(*obj, "hotkeyEnabled", cfg.hotkeyEnabled);
        TryGetUInt(*obj, "hotkeyModifiers", cfg.hotkeyModifiers);
        TryGetUInt(*obj, "hotkeyVk", cfg.hotkeyVk);
        TryGetBool(*obj, "menuKeyEnabled", cfg.menuKeyEnabled);
        TryGetBool(*obj, "interceptWinTab", cfg.interceptWinTab);
        TryGetBool(*obj, "interceptAltTab", cfg.interceptAltTab);

        const auto triggersIt = obj->find("triggers");
        if (triggersIt != obj->end() && triggersIt->second.IsArray())
        {
            cfg.triggers.clear();
            for (const auto& val : *triggersIt->second.AsArray())
            {
                if (cfg.triggers.size() >= kMaxTriggerBindings)
                    break;
                if (const auto* trigObj = val.AsObject())
                {
                    TriggerBinding b = ParseTrigger(*trigObj);
                    if (b.IsActive())
                        cfg.triggers.push_back(std::move(b));
                }
            }
            cfg.SyncLegacyTriggerMirrors();
        }
        else
        {
            cfg.triggers.clear();
            auto pushUnique = [&](const TriggerBinding& candidate) {
                if (!candidate.IsActive() || cfg.triggers.size() >= kMaxTriggerBindings)
                    return;
                for (const auto& existing : cfg.triggers)
                {
                    if (AreTriggerBindingsEqual(existing, candidate))
                        return;
                }
                cfg.triggers.push_back(candidate);
            };
            if (cfg.mouseTrigger == MouseTriggerButton::RightLeftChord)
                pushUnique(TriggerBinding{ TriggerKind::RightLeftChord, MouseTriggerButton::RightLeftChord, 0, 0, L"default" });
            else if (cfg.mouseTrigger != MouseTriggerButton::Disabled)
                pushUnique(TriggerBinding{ TriggerKind::MouseButton, cfg.mouseTrigger, 0, 0, L"default" });
            if (cfg.hotkeyEnabled && cfg.hotkeyVk != 0)
                pushUnique(TriggerBinding{ TriggerKind::KeyChord, MouseTriggerButton::Disabled, cfg.hotkeyModifiers, NormalizeTriggerVk(cfg.hotkeyVk), L"default" });
            if (cfg.interceptAltTab)
                pushUnique(TriggerBinding{ TriggerKind::KeyChord, MouseTriggerButton::Disabled, MOD_ALT, VK_TAB, L"default" });
            if (cfg.interceptWinTab)
                pushUnique(TriggerBinding{ TriggerKind::KeyChord, MouseTriggerButton::Disabled, MOD_WIN, VK_TAB, L"default" });
            if (cfg.menuKeyEnabled)
                pushUnique(TriggerBinding{ TriggerKind::KeyChord, MouseTriggerButton::Disabled, 0, VK_APPS, L"default" });
        }

        TryGetDouble(*obj, "deadzoneDip", cfg.deadzoneDip);
        TryGetDouble(*obj, "ringRadiusDip", cfg.ringRadiusDip);
        TryGetDouble(*obj, "iconSizeDip", cfg.iconSizeDip);
        TryGetDouble(*obj, "thumbnailScale", cfg.thumbnailScale);
        TryGetBool(*obj, "disableOpeningAnimation", cfg.disableOpeningAnimation);
        if (TryGetWString(*obj, "windowViewMode", s))
        {
            cfg.windowViewMode = ParseEnum<WindowViewMode>(s, cfg.windowViewMode, {
                { L"OuterArc", WindowViewMode::OuterArc },
                { L"TitleColumn", WindowViewMode::TitleColumn }
            });
        }
        TryGetBool(*obj, "fanOutSingleWindow", cfg.fanOutSingleWindow);
        if (TryGetWString(*obj, "theme", s))
        {
            cfg.theme = ParseEnum<RingTheme>(s, cfg.theme, {
                { L"A4S", RingTheme::A4S },
                { L"A4SMenu", RingTheme::A4S },
                { L"DarkWood", RingTheme::DarkWood },
                { L"Blue", RingTheme::Blue },
                { L"LiquidGlass", RingTheme::A4S },
                { L"Glass", RingTheme::A4S },
                { L"Kiwi", RingTheme::A4S },
                { L"Aura", RingTheme::A4S },
                { L"Orbit", RingTheme::A4S },
                { L"Outline", RingTheme::A4S }
            });
        }

        const bool hasSurfaceTex = TryGetWString(*obj, "surfaceTexture", s);
        if (hasSurfaceTex)
        {
            cfg.surfaceTexture = ParseEnum<SurfaceTexture>(s, cfg.surfaceTexture, {
                { L"A4SAcrylic", SurfaceTexture::A4SAcrylic },
                { L"TaskbarGlass", SurfaceTexture::TaskbarGlass },
                { L"PureMatte", SurfaceTexture::PureMatte }
            });
        }
        else
        {
            // Honest v1-v3 migration from old RingTheme names
            if (cfg.theme == RingTheme::DarkWood)
            {
                cfg.surfaceTexture = SurfaceTexture::PureMatte;
                cfg.useCustomSurfaceColor = false;
            }
            else if (cfg.theme == RingTheme::Blue)
            {
                cfg.surfaceTexture = SurfaceTexture::TaskbarGlass;
                cfg.useCustomSurfaceColor = true;
                cfg.surfaceColor = L"#142A52";
            }
            else
            {
                cfg.surfaceTexture = SurfaceTexture::A4SAcrylic;
                cfg.useCustomSurfaceColor = false;
            }
        }

        TryGetBool(*obj, "useCustomSurfaceColor", cfg.useCustomSurfaceColor);
        TryGetWString(*obj, "surfaceColor", cfg.surfaceColor);
        TryGetBool(*obj, "autoLightDark", cfg.autoLightDark);
        TryGetBool(*obj, "autoWindowsAccent", cfg.autoWindowsAccent);
        TryGetInt(*obj, "glassLevel", cfg.glassLevel);
        TryGetInt(*obj, "lightDarkMode", cfg.lightDarkMode);
        TryGetWString(*obj, "accentColor", cfg.accentColor);
        TryGetBool(*obj, "pointerFollow", cfg.pointerFollow);
        TryGetBool(*obj, "shiftCenterOnActiveMonitor", cfg.shiftCenterOnActiveMonitor);
        TryGetBool(*obj, "includeUnpinnedRunningApps", cfg.includeUnpinnedRunningApps);
        TryGetInt(*obj, "maxSlots", cfg.maxSlots);

        const auto pinsIt = obj->find("pinnedSlots");
        if (pinsIt != obj->end() && pinsIt->second.IsArray())
        {
            cfg.pinnedSlots.clear();
            for (const auto& val : *pinsIt->second.AsArray())
            {
                if (const auto* slotObj = val.AsObject())
                    cfg.pinnedSlots.push_back(ParseSlot(*slotObj));
            }
        }

        const auto profsIt = obj->find("profiles");
        if (profsIt != obj->end() && profsIt->second.IsArray())
        {
            cfg.profiles.clear();
            for (const auto& val : *profsIt->second.AsArray())
            {
                if (const auto* profObj = val.AsObject())
                {
                    RingProfileConfig p = ParseProfile(*profObj);
                    if (!cfg.FindProfile(p.id))
                        cfg.profiles.push_back(std::move(p));
                }
            }
            if (!cfg.FindProfile(L"default"))
                cfg.SyncDefaultProfileMirror();
            else
                cfg.PullFromDefaultProfileMirror();
            if (cfg.version < 5 && !cfg.FindProfile(L"SMenu"))
                cfg.profiles.push_back(CreateBuiltInSMenuProfile());
        }
        else
        {
            cfg.profiles.clear();
            cfg.SyncDefaultProfileMirror();
            cfg.profiles.push_back(CreateBuiltInSMenuProfile());
        }

        cfg.deadzoneDip = std::clamp(cfg.deadzoneDip, 8.0, 48.0);
        cfg.ringRadiusDip = std::clamp(cfg.ringRadiusDip, 72.0, 240.0);
        cfg.iconSizeDip = std::clamp(cfg.iconSizeDip, 24.0, 72.0);
        cfg.thumbnailScale = std::clamp(cfg.thumbnailScale, 0.5, 2.0);
        cfg.glassLevel = std::clamp(cfg.glassLevel, 0, 100);
        cfg.lightDarkMode = std::clamp(cfg.lightDarkMode, 0, 100);
        cfg.maxSlots = std::clamp(cfg.maxSlots, 4, 24);

        outConfig = std::move(cfg);
        return true;
    }

    std::string SwitcherConfig::ToJsonUtf8(bool includeInlineProfiles) const
    {
        std::string out;
        out.reserve(2048);
        auto addBool = [&](const char* k, bool v)
        {
            out += "  \""; out += k; out += "\": "; out += (v ? "true" : "false"); out += ",\n";
        };
        auto addInt = [&](const char* k, int v)
        {
            out += "  \""; out += k; out += "\": "; out += std::to_string(v); out += ",\n";
        };
        auto addUInt = [&](const char* k, UINT v)
        {
            out += "  \""; out += k; out += "\": "; out += std::to_string(v); out += ",\n";
        };
        auto addDouble = [&](const char* k, double v)
        {
            char buf[64]{};
            std::snprintf(buf, sizeof(buf), "%.4g", v);
            out += "  \""; out += k; out += "\": "; out += buf; out += ",\n";
        };
        auto addWStr = [&](const char* k, const std::wstring& v, bool trailingComma = true)
        {
            out += "  \""; out += k; out += "\": "; AppendEscapedJsonString(out, WideToUtf8(v));
            out += trailingComma ? ",\n" : "\n";
        };

        out += "{\n";
        addInt("version", version);

        addWStr("mouseTrigger", ToString(mouseTrigger));
        addWStr("scrollWheelMode", ToString(scrollWheelMode));
        addBool("gestureTapOpenClose", gestureTapOpenClose);
        addBool("gestureHoldReleaseSwitch", gestureHoldReleaseSwitch);
        addBool("doubleTapLastApp", doubleTapLastApp);
        addInt("tapThresholdMs", tapThresholdMs);
        addInt("doubleTapWindowMs", doubleTapWindowMs);
        addBool("hotkeyEnabled", hotkeyEnabled);
        addUInt("hotkeyModifiers", hotkeyModifiers);
        addUInt("hotkeyVk", hotkeyVk);
        addBool("menuKeyEnabled", menuKeyEnabled);
        addBool("interceptWinTab", interceptWinTab);
        addBool("interceptAltTab", interceptAltTab);

        out += "  \"triggers\": [";
        if (!triggers.empty())
        {
            out += "\n";
            const size_t tLimit = (std::min)(triggers.size(), kMaxTriggerBindings);
            for (size_t i = 0; i < tLimit; ++i)
            {
                SerializeTrigger(out, triggers[i], 2);
                if (i + 1 < tLimit)
                    out += ",";
                out += "\n";
            }
            out += "  ";
        }
        out += "],\n";

        addDouble("deadzoneDip", deadzoneDip);
        addDouble("ringRadiusDip", ringRadiusDip);
        addDouble("iconSizeDip", iconSizeDip);
        addDouble("thumbnailScale", thumbnailScale);
        addBool("disableOpeningAnimation", disableOpeningAnimation);
        addWStr("windowViewMode", ToString(windowViewMode));
        addBool("fanOutSingleWindow", fanOutSingleWindow);
        addWStr("surfaceTexture", ToString(surfaceTexture));
        addBool("useCustomSurfaceColor", useCustomSurfaceColor);
        addWStr("surfaceColor", surfaceColor);
        addBool("autoLightDark", autoLightDark);
        addBool("autoWindowsAccent", autoWindowsAccent);
        addWStr("theme", ToString(theme));
        addInt("glassLevel", glassLevel);
        addInt("lightDarkMode", lightDarkMode);
        addWStr("accentColor", accentColor);
        addBool("pointerFollow", pointerFollow);
        addBool("shiftCenterOnActiveMonitor", shiftCenterOnActiveMonitor);
        addBool("includeUnpinnedRunningApps", includeUnpinnedRunningApps);
        if (includeInlineProfiles)
        {
            addInt("maxSlots", maxSlots);

            out += "  \"pinnedSlots\": [";
            if (!pinnedSlots.empty())
            {
                out += "\n";
                for (size_t i = 0; i < pinnedSlots.size(); ++i)
                {
                    SerializeSlot(out, pinnedSlots[i], 2);
                    if (i + 1 < pinnedSlots.size())
                        out += ",";
                    out += "\n";
                }
                out += "  ";
            }
            out += "],\n";

            out += "  \"profiles\": [";
            if (!profiles.empty())
            {
                out += "\n";
                for (size_t i = 0; i < profiles.size(); ++i)
                {
                    SerializeProfile(out, profiles[i], 2, false);
                    if (i + 1 < profiles.size())
                        out += ",";
                    out += "\n";
                }
                out += "  ";
            }
            out += "]\n";
        }
        else
        {
            out += "  \"maxSlots\": " + std::to_string(maxSlots) + "\n";
        }
        out += "}\n";
        return out;
    }

    bool SwitcherConfig::LoadFromFile(const std::wstring& path, SwitcherConfig& outConfig)
    {
        std::string bytes;
        if (!ReadUtf8FileBounded(path, bytes))
            return false;

        if (!ParseJsonUtf8(bytes, outConfig))
            return false;

        // Load standalone per-profile JSON files from <configDir>\profiles\*.json when present.
        const std::wstring profDir = ProfilesDirectoryForConfigPath(path);
        const std::wstring pattern = profDir + L"\\*.json";
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE)
        {
            std::vector<RingProfileConfig> fileProfiles;
            auto findLoaded = [&](const std::wstring& id) -> RingProfileConfig* {
                for (auto& existing : fileProfiles)
                {
                    if (_wcsicmp(existing.id.c_str(), id.c_str()) == 0)
                        return &existing;
                }
                return nullptr;
            };

            do
            {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                    continue;
                const std::wstring fileName = fd.cFileName;
                const std::wstring fullPath = profDir + L"\\" + fileName;
                const std::wstring stem = FileStemOfProfilePath(fileName);
                RingProfileConfig prof;
                if (RingProfileConfig::LoadFromFile(fullPath, prof, stem))
                {
                    if (prof.id.empty())
                        prof.id = stem;
                    if (prof.name.empty())
                        prof.name = prof.id;

                    if (RingProfileConfig* existing = findLoaded(prof.id))
                    {
                        if (_wcsicmp(SanitizeProfileId(prof.id).c_str(), stem.c_str()) == 0)
                            *existing = std::move(prof);
                    }
                    else
                    {
                        fileProfiles.push_back(std::move(prof));
                    }
                }
            } while (FindNextFileW(find, &fd));
            FindClose(find);

            if (!fileProfiles.empty())
            {
                std::sort(fileProfiles.begin(), fileProfiles.end(),
                    [](const RingProfileConfig& a, const RingProfileConfig& b) {
                        auto rank = [](const std::wstring& id) -> int {
                            if (_wcsicmp(id.c_str(), L"default") == 0) return 0;
                            if (_wcsicmp(id.c_str(), L"SMenu") == 0) return 1;
                            return 2;
                        };
                        const int ra = rank(a.id);
                        const int rb = rank(b.id);
                        if (ra != rb)
                            return ra < rb;
                        return _wcsicmp(a.id.c_str(), b.id.c_str()) < 0;
                    });

                outConfig.profiles = std::move(fileProfiles);
                if (!outConfig.FindProfile(L"default"))
                    outConfig.SyncDefaultProfileMirror();
                outConfig.PullFromDefaultProfileMirror();
            }
        }

        if (RingProfileConfig* sMenuProf = outConfig.FindProfile(L"SMenu"))
            UpgradeBuiltInSMenuProfile(*sMenuProf);

        // One-time migration: fold legacy search.json entries into default profile as hidden pins.
        std::vector<PinnedSlotConfig> legacySearch;
        if (LoadLegacySearchItemsFromFile(LegacySearchItemsPath(path), legacySearch) && !legacySearch.empty())
        {
            if (RingProfileConfig* defProf = outConfig.FindProfile(L"default"))
            {
                for (auto& item : legacySearch)
                {
                    item.hiddenFromRing = true;
                    const std::wstring newKey = CanonicalSearchKey(item.pathOrUrl);
                    bool dup = false;
                    if (!newKey.empty())
                    {
                        for (const auto& existing : defProf->pinnedSlots)
                        {
                            if (CanonicalSearchKey(existing.pathOrUrl) == newKey)
                            {
                                dup = true;
                                break;
                            }
                        }
                    }
                    if (!dup)
                        defProf->pinnedSlots.push_back(std::move(item));
                }
                outConfig.PullFromDefaultProfileMirror();
            }
        }

        return true;
    }

    bool SwitcherConfig::SaveToFile(const std::wstring& path) const
    {
        if (!WriteUtf8FileAtomic(path, ToJsonUtf8(false)))
            return false;

        const std::wstring profDir = ProfilesDirectoryForConfigPath(path);
        SHCreateDirectoryExW(nullptr, profDir.c_str(), nullptr);

        if (profiles.empty())
        {
            RingProfileConfig defProf;
            defProf.id = L"default";
            defProf.name = L"default";
            defProf.includeRunningApps = includeUnpinnedRunningApps;
            defProf.pinnedSlots = pinnedSlots;
            if (!defProf.SaveToFile(ProfileFilePath(L"default", path)))
                return false;
            return true;
        }

        for (const auto& p : profiles)
        {
            if (!p.SaveToFile(ProfileFilePath(p.id, path)))
                return false;
        }
        return true;
    }

    bool SwitcherConfig::SaveProfile(const std::wstring& profileId, const std::wstring& configPath) const
    {
        const RingProfileConfig* prof = FindProfile(profileId);
        if (!prof)
            return false;
        const std::wstring effectiveCfg = configPath.empty() ? ConfigPath() : configPath;
        return prof->SaveToFile(ProfileFilePath(prof->id, effectiveCfg));
    }

    bool SwitcherConfig::DeleteProfileFile(const std::wstring& profileId, const std::wstring& configPath)
    {
        if (profileId.empty() || _wcsicmp(profileId.c_str(), L"default") == 0)
            return false;
        const std::wstring effectiveCfg = configPath.empty() ? ConfigPath() : configPath;
        const std::wstring path = ProfileFilePath(profileId, effectiveCfg);
        return DeleteFileW(path.c_str()) != FALSE || GetLastError() == ERROR_FILE_NOT_FOUND;
    }

    SwitcherConfig SwitcherConfig::LoadOrCreate()
    {
        SwitcherConfig cfg;
        const std::wstring path = ConfigPath();
        if (LoadFromFile(path, cfg))
        {
            const bool missingDefaultProf =
                GetFileAttributesW(ProfileFilePath(L"default", path).c_str()) == INVALID_FILE_ATTRIBUTES;
            bool smenuNeedsUpgrade = false;
            if (RingProfileConfig* sMenuDisk = cfg.FindProfile(L"SMenu"))
            {
                RingProfileConfig diskCopy;
                if (RingProfileConfig::LoadFromFile(ProfileFilePath(L"SMenu", path), diskCopy))
                    smenuNeedsUpgrade = UpgradeBuiltInSMenuProfile(diskCopy);
            }
            if (cfg.version < 3)
            {
                cfg.surfaceTexture = SurfaceTexture::A4SAcrylic;
                cfg.useCustomSurfaceColor = false;
                cfg.theme = RingTheme::A4S;
                cfg.glassLevel = 50;
                cfg.lightDarkMode = 0;
                if (_wcsicmp(cfg.accentColor.c_str(), L"#3186FF") == 0)
                    cfg.accentColor = L"#0078D4";
                cfg.version = 6;
                cfg.SaveToFile(path);
            }
            else if (cfg.version < 6 || missingDefaultProf)
            {
                cfg.version = 6;
                cfg.SaveToFile(path);
            }
            else if (smenuNeedsUpgrade)
            {
                cfg.SaveProfile(L"SMenu", path);
            }
            const std::wstring legacySearch = LegacySearchItemsPath(path);
            if (GetFileAttributesW(legacySearch.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                cfg.SaveProfile(L"default", path);
                DeleteFileW(legacySearch.c_str());
            }
            return cfg;
        }

        cfg = CreateDefault();
        cfg.SaveToFile(path);
        return cfg;
    }

    bool SwitcherConfig::Save() const
    {
        return SaveToFile(ConfigPath());
    }
} // namespace winrings
