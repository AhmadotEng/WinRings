#pragma once

#include <Windows.h>
#include <d2d1.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "WinRingsConfig.h"

namespace winrings
{
    struct RingMaterialSpec
    {
        // Live HostBackdrop + D2D1 effect graph recipe (used identically by AcrylicBackdrop and Settings Preview)
        float blurSigma = 30.0f;
        float saturation = 1.14f;
        float luminosityR = 36.0f / 255.0f;
        float luminosityG = 36.0f / 255.0f;
        float luminosityB = 36.0f / 255.0f;
        float luminosityAlpha = 0.34f;
        float tintR = 32.0f / 255.0f;
        float tintG = 32.0f / 255.0f;
        float tintB = 32.0f / 255.0f;
        float tintAlpha = 128.0f / 255.0f;
        float noiseOpacity = 0.038f;

        // Foreground & fallback colors
        D2D1_COLOR_F ringFill{};
        D2D1_COLOR_F centerFill{};
        D2D1_COLOR_F searchFill{};
        D2D1_COLOR_F ringStroke{};
        D2D1_COLOR_F innerRimStroke{};
        D2D1_COLOR_F centerBorder{};
        D2D1_COLOR_F searchBorder{};
        D2D1_COLOR_F slotIdleFill{};
        D2D1_COLOR_F slotSelectedFill{};
        D2D1_COLOR_F hoverRim{};
        D2D1_COLOR_F sectorFill{};
        D2D1_COLOR_F sectorStroke{};
        D2D1_COLOR_F innerFill{};
        D2D1_COLOR_F accent{};
        D2D1_COLOR_F hoverRevealColor{};
        D2D1_COLOR_F textColor{};
        D2D1_COLOR_F mutedTextColor{};
        D2D1_COLOR_F idleDotColor{};

        float ringStrokeWidth = 0.85f;
        float hairlineWidth = 0.85f;
        bool isLightText = true;
        COLORREF accentColorRef = RGB(0, 120, 212);
        COLORREF borderColorRef = RGB(62, 62, 62);
    };

    namespace detail
    {
        constexpr D2D1_COLOR_F RgbaF(BYTE a, BYTE r, BYTE g, BYTE b) noexcept
        {
            return D2D1_COLOR_F{ r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f };
        }

        inline float LerpFloat(float a, float b, float x) noexcept
        {
            return a + (b - a) * x;
        }

        inline BYTE LerpByteVal(float a, float b, float x) noexcept
        {
            return static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(LerpFloat(a, b, x))), 0, 255));
        }

        inline void ParseAccentHex(const std::wstring& hex, BYTE& r, BYTE& g, BYTE& b) noexcept
        {
            r = 0;
            g = 120;
            b = 212;
            if (hex.empty())
                return;
            const wchar_t* p = (hex[0] == L'#') ? (hex.c_str() + 1) : hex.c_str();
            const size_t len = wcslen(p);
            unsigned int val = 0;
            if ((len == 6 && swscanf_s(p, L"%06x", &val) == 1) ||
                (len == 8 && swscanf_s(p, L"%08x", &val) == 1))
            {
                r = static_cast<BYTE>((val >> 16) & 0xFF);
                g = static_cast<BYTE>((val >> 8) & 0xFF);
                b = static_cast<BYTE>(val & 0xFF);
            }
        }
    } // namespace detail

    inline RingMaterialSpec ComputeRingMaterial(const SwitcherConfig& cfg, double dpiScale) noexcept
    {
        const float g = static_cast<float>(std::clamp(cfg.glassLevel, 0, 100)) / 100.0f;
        const float t = static_cast<float>(std::clamp(cfg.lightDarkMode, 0, 100)) / 100.0f;

        BYTE accR = 0, accG = 120, accB = 212;
        detail::ParseAccentHex(cfg.accentColor, accR, accG, accB);
        const bool isDefaultAccent = (accR == 0 && accG == 120 && accB == 212);

        RingMaterialSpec m{};
        m.isLightText = (t < 0.52f);

        const float safeDpiScale = static_cast<float>((std::max)(1.0, dpiScale));
        const float hairlineDip = (std::min)(0.85f, 1.0f / safeDpiScale);
        m.ringStrokeWidth = hairlineDip;
        m.hairlineWidth = hairlineDip;

        if (cfg.theme == RingTheme::Blue)
        {
            const float tr = detail::LerpFloat(20.0f, 220.0f, t);
            const float tg = detail::LerpFloat(42.0f, 236.0f, t);
            const float tb = detail::LerpFloat(82.0f, 255.0f, t);
            m.tintR = tr / 255.0f;
            m.tintG = tg / 255.0f;
            m.tintB = tb / 255.0f;
            m.tintAlpha = (g <= 0.45f)
                ? detail::LerpFloat(0.10f, 0.48f, g / 0.45f)
                : detail::LerpFloat(0.48f, 0.94f, (g - 0.45f) / 0.55f);

            m.blurSigma = (g <= 0.45f)
                ? detail::LerpFloat(16.0f, 28.0f, g / 0.45f)
                : (std::max)(0.0f, 28.0f * (1.0f - std::pow((g - 0.45f) / 0.55f, 1.5f)));
            m.saturation = detail::LerpFloat(1.38f, 1.08f, g);
            m.luminosityR = detail::LerpFloat(26.0f, 228.0f, t) / 255.0f;
            m.luminosityG = detail::LerpFloat(50.0f, 240.0f, t) / 255.0f;
            m.luminosityB = detail::LerpFloat(96.0f, 255.0f, t) / 255.0f;
            m.luminosityAlpha = (g <= 0.45f)
                ? detail::LerpFloat(0.08f, 0.26f, g / 0.45f)
                : detail::LerpFloat(0.26f, 0.0f, (g - 0.45f) / 0.55f);
            m.noiseOpacity = (g <= 0.45f)
                ? detail::LerpFloat(0.020f, 0.034f, g / 0.45f)
                : detail::LerpFloat(0.034f, 0.0f, (g - 0.45f) / 0.55f);

            const BYTE blueR = isDefaultAccent ? 49 : accR;
            const BYTE blueG = isDefaultAccent ? 134 : accG;
            const BYTE blueB = isDefaultAccent ? 255 : accB;
            m.accent = detail::RgbaF(255, blueR, blueG, blueB);
            m.hoverRevealColor = m.accent;
            m.accentColorRef = RGB(blueR, blueG, blueB);

            const BYTE rimR = detail::LerpByteVal(72.0f, 36.0f, t);
            const BYTE rimG = detail::LerpByteVal(148.0f, 116.0f, t);
            const BYTE rimB = 255;
            m.ringStroke = detail::RgbaF(185, rimR, rimG, rimB);
            m.innerRimStroke = detail::RgbaF(125, rimR, rimG, rimB);
            m.borderColorRef = RGB(rimR, rimG, rimB);
            m.centerBorder = detail::RgbaF(175, rimR, rimG, rimB);
            m.searchBorder = detail::RgbaF(185, detail::LerpByteVal(72.0f, 49.0f, t), detail::LerpByteVal(148.0f, 134.0f, t), 255);
            m.slotSelectedFill = detail::RgbaF(68, blueR, blueG, blueB);
            m.hoverRim = detail::RgbaF(210, 96, 165, 250);
            m.sectorStroke = detail::RgbaF(225, blueR, blueG, blueB);
        }
        else if (cfg.theme == RingTheme::DarkWood)
        {
            const float tr = detail::LerpFloat(32.0f, 238.0f, t);
            const float tg = detail::LerpFloat(32.0f, 236.0f, t);
            const float tb = detail::LerpFloat(32.0f, 232.0f, t);
            m.tintR = tr / 255.0f;
            m.tintG = tg / 255.0f;
            m.tintB = tb / 255.0f;
            m.tintAlpha = 0.08f + g * 0.88f;

            m.blurSigma = (std::max)(0.0f, (1.0f - g) * 14.0f);
            m.saturation = 1.02f;
            m.luminosityR = m.tintR;
            m.luminosityG = m.tintG;
            m.luminosityB = m.tintB;
            m.luminosityAlpha = (std::max)(0.0f, (1.0f - g) * 0.22f);
            m.noiseOpacity = (std::max)(0.0f, (1.0f - g) * 0.024f);

            m.accent = detail::RgbaF(255, accR, accG, accB);
            m.hoverRevealColor = m.accent;
            m.accentColorRef = RGB(accR, accG, accB);

            const BYTE rimV = detail::LerpByteVal(78.0f, 154.0f, t);
            m.ringStroke = detail::RgbaF(static_cast<BYTE>(150 + g * 45), rimV, rimV, rimV);
            m.innerRimStroke = detail::RgbaF(110, rimV, rimV, rimV);
            m.borderColorRef = RGB(rimV, rimV, rimV);
            m.centerBorder = detail::RgbaF(170, rimV, rimV, rimV);
            const BYTE srV = detail::LerpByteVal(90.0f, 168.0f, t);
            m.searchBorder = detail::RgbaF(185, srV, srV, static_cast<BYTE>((std::min)(255, static_cast<int>(srV) + 4)));
            m.slotSelectedFill = m.isLightText ? detail::RgbaF(44, 255, 255, 255) : detail::RgbaF(30, 0, 0, 0);
            m.hoverRim = detail::RgbaF(200, accR, accG, accB);
            m.sectorStroke = detail::RgbaF(255, accR, accG, accB);
        }
        else // RingTheme::A4S (Default — A4S-Menu / Windows 11 Acrylic spec)
        {
            // A4SMenuProfile.cpp:213, 301: RGB(32,32,32) tint, BackdropAlpha = 128 (~0.502) at glassLevel = 50
            const float tv = detail::LerpFloat(32.0f, 243.0f, t) / 255.0f;
            m.tintR = tv;
            m.tintG = tv;
            m.tintB = tv;
            m.tintAlpha = (g <= 0.5f)
                ? detail::LerpFloat(0.08f, 128.0f / 255.0f, g / 0.5f)
                : detail::LerpFloat(128.0f / 255.0f, 0.96f, (g - 0.5f) / 0.5f);

            m.blurSigma = (g <= 0.5f)
                ? detail::LerpFloat(18.0f, 30.0f, g / 0.5f)
                : (std::max)(0.0f, 30.0f * (1.0f - std::pow((g - 0.5f) / 0.5f, 1.5f)));
            m.saturation = detail::LerpFloat(1.28f, 1.00f, g);

            const float lv = detail::LerpFloat(36.0f, 246.0f, t) / 255.0f;
            m.luminosityR = lv;
            m.luminosityG = lv;
            m.luminosityB = lv;
            m.luminosityAlpha = (g <= 0.5f)
                ? detail::LerpFloat(0.08f, 0.34f, g / 0.5f)
                : detail::LerpFloat(0.34f, 0.0f, (g - 0.5f) / 0.5f);

            m.noiseOpacity = (g <= 0.5f)
                ? detail::LerpFloat(0.022f, 0.038f, g / 0.5f)
                : detail::LerpFloat(0.038f, 0.0f, (g - 0.5f) / 0.5f);

            m.accent = detail::RgbaF(255, accR, accG, accB);
            m.hoverRevealColor = isDefaultAccent ? detail::RgbaF(255, 52, 99, 141) : m.accent;
            m.accentColorRef = RGB(accR, accG, accB);

            // MenuContainer.cpp:112 (#3E3E3E = RGB(62,62,62) outer border), MenuPaint.cpp:151 (RGB(81,81,81) search box)
            const BYTE rimV = detail::LerpByteVal(62.0f, 184.0f, t);
            const BYTE rimB = detail::LerpByteVal(62.0f, 190.0f, t);
            m.ringStroke = detail::RgbaF(175, rimV, rimV, rimB);
            m.innerRimStroke = detail::RgbaF(108, rimV, rimV, rimB);
            m.borderColorRef = RGB(rimV, rimV, rimB);
            m.centerBorder = detail::RgbaF(160, rimV, rimV, rimB);

            const BYTE sbV = detail::LerpByteVal(81.0f, 165.0f, t);
            const BYTE sbB = detail::LerpByteVal(81.0f, 172.0f, t);
            m.searchBorder = detail::RgbaF(185, sbV, sbV, sbB);

            // MenuPaint.cpp:241 (overlayAlpha = 37)
            m.slotSelectedFill = m.isLightText ? detail::RgbaF(37, 255, 255, 255) : detail::RgbaF(26, 0, 0, 0);
            m.hoverRim = m.isLightText ? detail::RgbaF(85, 255, 255, 255) : detail::RgbaF(55, 0, 0, 0);
            m.sectorStroke = detail::RgbaF(220, accR, accG, accB);
        }

        const BYTE fillR = static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(m.tintR * 255.0f)), 0, 255));
        const BYTE fillG = static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(m.tintG * 255.0f)), 0, 255));
        const BYTE fillB = static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(m.tintB * 255.0f)), 0, 255));
        const BYTE fillA = static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(m.tintAlpha * 255.0f)), 0, 255));
        const BYTE centerA = static_cast<BYTE>((std::min)(246, static_cast<int>(fillA) + 28));

        m.ringFill = detail::RgbaF(fillA, fillR, fillG, fillB);
        m.centerFill = detail::RgbaF(centerA, fillR, fillG, fillB);
        m.searchFill = detail::RgbaF(centerA, fillR, fillG, fillB);

        m.innerFill = detail::RgbaF(0, 0, 0, 0);
        m.slotIdleFill = detail::RgbaF(0, 0, 0, 0);
        m.sectorFill = detail::RgbaF(0, 0, 0, 0);

        m.textColor = m.isLightText ? detail::RgbaF(255, 255, 255, 255) : detail::RgbaF(255, 24, 24, 27);
        m.mutedTextColor = m.isLightText ? detail::RgbaF(255, 153, 153, 153) : detail::RgbaF(255, 102, 102, 110);
        m.idleDotColor = m.isLightText ? detail::RgbaF(115, 255, 255, 255) : detail::RgbaF(110, 24, 24, 27);

        return m;
    }

    // Generates a seamless 256x256 32bpp PBGRA monochrome frosted micro-grain texture
    // for banding-free Windows 11 Acrylic appearance across live overlay and settings preview.
    inline std::vector<uint8_t> GenerateAcrylicNoiseBgra256()
    {
        constexpr int kSize = 256;
        std::vector<uint8_t> buf(static_cast<size_t>(kSize) * kSize * 4u, 0);
        uint32_t state = 0x6D2B79F5u;

        for (int i = 0; i < kSize * kSize; ++i)
        {
            // Fast 32-bit xorshift/splitmix-style PRNG
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            const uint32_t h = state * 0x27d4eb2du;
            const uint8_t luma = static_cast<uint8_t>(h >> 24);
            const uint8_t alpha = static_cast<uint8_t>(120u + (((h >> 16) & 0xFFu) * 100u) / 255u);
            const uint8_t pm = static_cast<uint8_t>((static_cast<uint32_t>(luma) * alpha + 127u) / 255u);
            const size_t off = static_cast<size_t>(i) * 4u;
            buf[off + 0] = pm;
            buf[off + 1] = pm;
            buf[off + 2] = pm;
            buf[off + 3] = alpha;
        }
        return buf;
    }
} // namespace winrings
