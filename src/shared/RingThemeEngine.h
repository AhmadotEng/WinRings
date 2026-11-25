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
        SurfaceTexture surfaceTexture = SurfaceTexture::A4SAcrylic;

        // Live HostBackdrop + D2D1 effect graph recipe (used by AcrylicBackdrop)
        float blurSigma = 30.0f;
        float saturation = 1.15f;
        float luminosityR = 38.0f / 255.0f;
        float luminosityG = 38.0f / 255.0f;
        float luminosityB = 38.0f / 255.0f;
        float luminosityAlpha = 0.90f;
        float tintR = 32.0f / 255.0f;
        float tintG = 32.0f / 255.0f;
        float tintB = 32.0f / 255.0f;
        float tintAlpha = 128.0f / 255.0f;
        float noiseOpacity = 0.012f;

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

        // 3D Glass bezel & WCA edge-bridge stops
        D2D1_COLOR_F edgeBridgeColor{};
        D2D1_COLOR_F outerBezelTop{};
        D2D1_COLOR_F outerBezelMid{};
        D2D1_COLOR_F outerBezelBot{};
        D2D1_COLOR_F innerBezelTop{};
        D2D1_COLOR_F innerBezelMid{};
        D2D1_COLOR_F innerBezelBot{};

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

        inline BYTE ClampByte(float v) noexcept
        {
            return static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(v)), 0, 255));
        }

        inline bool ParseHexColor(const std::wstring& hex, BYTE& r, BYTE& g, BYTE& b, BYTE defR, BYTE defG, BYTE defB) noexcept
        {
            r = defR;
            g = defG;
            b = defB;
            if (hex.empty())
                return false;
            const wchar_t* p = (hex[0] == L'#') ? (hex.c_str() + 1) : hex.c_str();
            const size_t len = wcslen(p);
            unsigned int val = 0;
            if ((len == 6 && swscanf_s(p, L"%06x", &val) == 1) ||
                (len == 8 && swscanf_s(p, L"%08x", &val) == 1))
            {
                r = static_cast<BYTE>((val >> 16) & 0xFF);
                g = static_cast<BYTE>((val >> 8) & 0xFF);
                b = static_cast<BYTE>(val & 0xFF);
                return true;
            }
            return false;
        }

        inline void ParseAccentHex(const std::wstring& hex, BYTE& r, BYTE& g, BYTE& b) noexcept
        {
            ParseHexColor(hex, r, g, b, 0, 120, 212);
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
        m.surfaceTexture = cfg.surfaceTexture;

        const float safeDpiScale = static_cast<float>((std::max)(1.0, dpiScale));
        const float hairlineDip = (std::min)(0.85f, 1.0f / safeDpiScale);
        m.ringStrokeWidth = hairlineDip;
        m.hairlineWidth = hairlineDip;

        // 1. Base surface color (completely decoupled from texture!)
        float baseR = 32.0f, baseG = 32.0f, baseB = 32.0f;
        if (cfg.useCustomSurfaceColor)
        {
            BYTE sr = 32, sg = 32, sb = 32;
            detail::ParseHexColor(cfg.surfaceColor, sr, sg, sb, 32, 32, 32);
            baseR = detail::LerpFloat(static_cast<float>(sr), 242.0f, t * 0.82f);
            baseG = detail::LerpFloat(static_cast<float>(sg), 242.0f, t * 0.82f);
            baseB = detail::LerpFloat(static_cast<float>(sb), 242.0f, t * 0.82f);
            const float luma = 0.2126f * baseR + 0.7152f * baseG + 0.0722f * baseB;
            m.isLightText = (luma < 142.0f);
        }
        else if (cfg.surfaceTexture == SurfaceTexture::TaskbarGlass)
        {
            baseR = detail::LerpFloat(28.0f, 240.0f, t);
            baseG = detail::LerpFloat(30.0f, 243.0f, t);
            baseB = detail::LerpFloat(34.0f, 248.0f, t);
            m.isLightText = (t < 0.52f);
        }
        else
        {
            const float v = detail::LerpFloat(32.0f, 243.0f, t);
            baseR = v;
            baseG = v;
            baseB = v;
            m.isLightText = (t < 0.52f);
        }

        m.tintR = baseR / 255.0f;
        m.tintG = baseG / 255.0f;
        m.tintB = baseB / 255.0f;

        // 2. Genuine per-texture optical parameters
        if (cfg.surfaceTexture == SurfaceTexture::TaskbarGlass)
        {
            // Windows 11 Taskbar / HostBackdrop recipe: wide blur, vibrant wallpaper bleed, deep luminosity wash
            m.tintAlpha = 0.12f + g * 0.76f;
            m.blurSigma = (g <= 0.70f)
                ? detail::LerpFloat(28.0f, 54.0f, g / 0.70f)
                : (std::max)(0.0f, 54.0f * (1.0f - std::pow((g - 0.70f) / 0.30f, 1.4f)));
            m.saturation = detail::LerpFloat(1.38f, 1.12f, g);
            m.luminosityR = (std::min)(255.0f, baseR + 8.0f) / 255.0f;
            m.luminosityG = (std::min)(255.0f, baseG + 8.0f) / 255.0f;
            m.luminosityB = (std::min)(255.0f, baseB + 10.0f) / 255.0f;
            m.luminosityAlpha = (g <= 0.70f)
                ? detail::LerpFloat(0.35f, 0.84f, g / 0.70f)
                : detail::LerpFloat(0.84f, 0.0f, (g - 0.70f) / 0.30f);
            m.noiseOpacity = (1.0f - g * 0.5f) * 0.010f;
        }
        else if (cfg.surfaceTexture == SurfaceTexture::PureMatte)
        {
            // Pure Matte Tint: zero blur, zero noise, clean controllable opacity
            m.tintAlpha = 0.14f + g * 0.84f;
            m.blurSigma = 0.0f;
            m.saturation = 1.00f;
            m.luminosityR = m.tintR;
            m.luminosityG = m.tintG;
            m.luminosityB = m.tintB;
            m.luminosityAlpha = 0.0f;
            m.noiseOpacity = 0.0f;
        }
        else // SurfaceTexture::A4SAcrylic (Default — calibrated 1:1 to Windows 11 WCA ACCENT_ENABLE_ACRYLICBLURBEHIND)
        {
            m.tintAlpha = (g <= 0.5f)
                ? detail::LerpFloat(0.08f, 128.0f / 255.0f, g / 0.5f)
                : detail::LerpFloat(128.0f / 255.0f, 0.96f, (g - 0.5f) / 0.5f);
            m.blurSigma = (g <= 0.5f)
                ? detail::LerpFloat(18.0f, 30.0f, g / 0.5f)
                : (std::max)(0.0f, 30.0f * (1.0f - std::pow((g - 0.5f) / 0.5f, 1.5f)));
            m.saturation = detail::LerpFloat(1.30f, 1.00f, g);

            if (cfg.useCustomSurfaceColor)
            {
                m.luminosityR = (std::min)(255.0f, baseR + 6.0f) / 255.0f;
                m.luminosityG = (std::min)(255.0f, baseG + 6.0f) / 255.0f;
                m.luminosityB = (std::min)(255.0f, baseB + 6.0f) / 255.0f;
            }
            else
            {
                const float lv = detail::LerpFloat(38.0f, 246.0f, t) / 255.0f;
                m.luminosityR = lv;
                m.luminosityG = lv;
                m.luminosityB = lv;
            }
            const float peakLumA = detail::LerpFloat(0.90f, 0.88f, t);
            m.luminosityAlpha = (g <= 0.5f)
                ? detail::LerpFloat(0.24f, peakLumA, g / 0.5f)
                : detail::LerpFloat(peakLumA, 0.0f, (g - 0.5f) / 0.5f);
            m.noiseOpacity = (g <= 0.5f)
                ? detail::LerpFloat(0.008f, 0.012f, g / 0.5f)
                : detail::LerpFloat(0.012f, 0.0f, (g - 0.5f) / 0.5f);
        }

        // 3. Accent, strokes, and 3D glass bezel tokens
        m.accent = detail::RgbaF(255, accR, accG, accB);
        m.hoverRevealColor = (isDefaultAccent && !cfg.useCustomSurfaceColor && cfg.surfaceTexture == SurfaceTexture::A4SAcrylic)
            ? detail::RgbaF(255, 52, 99, 141)
            : m.accent;
        m.accentColorRef = RGB(accR, accG, accB);

        if (cfg.useCustomSurfaceColor)
        {
            const float rimShift = m.isLightText ? 54.0f : -56.0f;
            const BYTE rimR = detail::ClampByte(baseR + rimShift);
            const BYTE rimG = detail::ClampByte(baseG + rimShift);
            const BYTE rimB = detail::ClampByte(baseB + rimShift);
            m.ringStroke = detail::RgbaF(185, rimR, rimG, rimB);
            m.innerRimStroke = detail::RgbaF(130, rimR, rimG, rimB);
            m.borderColorRef = RGB(rimR, rimG, rimB);
            m.centerBorder = detail::RgbaF(175, rimR, rimG, rimB);
            m.searchBorder = detail::RgbaF(185, rimR, rimG, rimB);

            m.slotSelectedFill = m.isLightText ? detail::RgbaF(46, 255, 255, 255) : detail::RgbaF(30, 0, 0, 0);
            m.hoverRim = m.isLightText ? detail::RgbaF(110, 255, 255, 255) : detail::RgbaF(75, 0, 0, 0);
            m.sectorStroke = detail::RgbaF(230, accR, accG, accB);

            m.edgeBridgeColor = D2D1_COLOR_F{ m.tintR, m.tintG, m.tintB, 0.90f };
            if (m.isLightText)
            {
                const float hiR = (std::min)(1.0f, m.tintR * 0.5f + 0.5f);
                const float hiG = (std::min)(1.0f, m.tintG * 0.5f + 0.5f);
                const float hiB = (std::min)(1.0f, m.tintB * 0.5f + 0.5f);
                m.outerBezelTop = D2D1_COLOR_F{ hiR, hiG, hiB, 0.44f };
                m.outerBezelMid = m.ringStroke;
                m.outerBezelBot = D2D1_COLOR_F{ m.tintR, m.tintG, m.tintB, 0.22f };
                m.innerBezelTop = D2D1_COLOR_F{ m.tintR, m.tintG, m.tintB, 0.16f };
                m.innerBezelMid = m.innerRimStroke;
                m.innerBezelBot = D2D1_COLOR_F{ hiR, hiG, hiB, 0.34f };
            }
            else
            {
                m.outerBezelTop = D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.85f };
                m.outerBezelMid = m.ringStroke;
                m.outerBezelBot = D2D1_COLOR_F{ rimR / 255.0f, rimG / 255.0f, rimB / 255.0f, 0.45f };
                m.innerBezelTop = D2D1_COLOR_F{ rimR / 255.0f, rimG / 255.0f, rimB / 255.0f, 0.36f };
                m.innerBezelMid = m.innerRimStroke;
                m.innerBezelBot = D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.72f };
            }
        }
        else
        {
            // Approved A4S neutral dark/light strokes & 3D glass rims
            const BYTE rimV = detail::LerpByteVal(62.0f, 184.0f, t);
            const BYTE rimB = detail::LerpByteVal(62.0f, 190.0f, t);
            m.ringStroke = detail::RgbaF(185, rimV, rimV, rimB);
            m.innerRimStroke = detail::RgbaF(135, rimV, rimV, rimB);
            m.borderColorRef = RGB(rimV, rimV, rimB);
            m.centerBorder = detail::RgbaF(170, rimV, rimV, rimB);

            const BYTE sbV = detail::LerpByteVal(81.0f, 165.0f, t);
            const BYTE sbB = detail::LerpByteVal(81.0f, 172.0f, t);
            m.searchBorder = detail::RgbaF(185, sbV, sbV, sbB);

            m.slotSelectedFill = m.isLightText ? detail::RgbaF(37, 255, 255, 255) : detail::RgbaF(26, 0, 0, 0);
            m.hoverRim = m.isLightText ? detail::RgbaF(85, 255, 255, 255) : detail::RgbaF(55, 0, 0, 0);
            m.sectorStroke = detail::RgbaF(220, accR, accG, accB);

            m.edgeBridgeColor = m.isLightText
                ? D2D1_COLOR_F{ 34.0f / 255.0f, 34.0f / 255.0f, 36.0f / 255.0f, 0.90f }
                : D2D1_COLOR_F{ 236.0f / 255.0f, 236.0f / 255.0f, 240.0f / 255.0f, 0.90f };

            m.outerBezelTop = m.isLightText
                ? D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.25f }
                : D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.85f };
            m.outerBezelMid = m.isLightText
                ? D2D1_COLOR_F{ 118.0f / 255.0f, 118.0f / 255.0f, 124.0f / 255.0f, 0.42f }
                : D2D1_COLOR_F{ 170.0f / 255.0f, 174.0f / 255.0f, 182.0f / 255.0f, 0.55f };
            m.outerBezelBot = m.isLightText
                ? D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.09f }
                : D2D1_COLOR_F{ 120.0f / 255.0f, 126.0f / 255.0f, 138.0f / 255.0f, 0.42f };

            m.innerBezelTop = m.isLightText
                ? D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.07f }
                : D2D1_COLOR_F{ 110.0f / 255.0f, 116.0f / 255.0f, 126.0f / 255.0f, 0.34f };
            m.innerBezelMid = m.isLightText
                ? D2D1_COLOR_F{ 108.0f / 255.0f, 108.0f / 255.0f, 114.0f / 255.0f, 0.32f }
                : D2D1_COLOR_F{ 170.0f / 255.0f, 174.0f / 255.0f, 182.0f / 255.0f, 0.45f };
            m.innerBezelBot = m.isLightText
                ? D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.20f }
                : D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.72f };
        }

        const BYTE fillR = detail::ClampByte(m.tintR * 255.0f);
        const BYTE fillG = detail::ClampByte(m.tintG * 255.0f);
        const BYTE fillB = detail::ClampByte(m.tintB * 255.0f);
        const BYTE fillA = detail::ClampByte(m.tintAlpha * 255.0f);
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

    // Generates a seamless 256x256 32bpp PBGRA velvety monochrome fine-grain texture
    // (triangular-peaked around mid-gray) matching Windows 11 Acrylic's subtle anti-banding frost.
    inline std::vector<uint8_t> GenerateAcrylicNoiseBgra256()
    {
        constexpr int kSize = 256;
        std::vector<uint8_t> buf(static_cast<size_t>(kSize) * kSize * 4u, 0);
        uint32_t state = 0x6D2B79F5u;

        for (int i = 0; i < kSize * kSize; ++i)
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            const uint32_t h = state * 0x27d4eb2du;
            const uint32_t u1 = (h >> 24) & 0xFFu;
            const uint32_t u2 = (h >> 16) & 0xFFu;
            const uint8_t luma = static_cast<uint8_t>((u1 + u2) >> 1);
            constexpr uint8_t alpha = 140u;
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
