#pragma once

#include <Windows.h>
#include <dxgi1_2.h>
#include <memory>

#include "../shared/RingThemeEngine.h"

namespace winrings
{
    struct AcrylicMaterialParams
    {
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
    };

    inline AcrylicMaterialParams AcrylicParamsFromMaterial(const RingMaterialSpec& m) noexcept
    {
        AcrylicMaterialParams p{};
        p.blurSigma = m.blurSigma;
        p.saturation = m.saturation;
        p.luminosityR = m.luminosityR;
        p.luminosityG = m.luminosityG;
        p.luminosityB = m.luminosityB;
        p.luminosityAlpha = m.luminosityAlpha;
        p.tintR = m.tintR;
        p.tintG = m.tintG;
        p.tintB = m.tintB;
        p.tintAlpha = m.tintAlpha;
        return p;
    }

    /// Live environment-reactive Windows 11 acrylic layer using Windows.UI.Composition
    /// HostBackdropBrush + D2D1 effect graph masked by an antialiased DXGI alpha mask swap chain.
    /// Falls back cleanly to caller's IDCompositionTarget if unavailable or disabled.
    class AcrylicBackdrop
    {
    public:
        AcrylicBackdrop();
        ~AcrylicBackdrop();

        AcrylicBackdrop(const AcrylicBackdrop&) = delete;
        AcrylicBackdrop& operator=(const AcrylicBackdrop&) = delete;

        bool Initialize(
            HWND hwnd,
            IDXGISwapChain1* foregroundSwapChain,
            IDXGISwapChain1* maskSwapChain,
            int widthPx,
            int heightPx);

        void Shutdown();
        bool IsActive() const noexcept;
        void Resize(int widthPx, int heightPx);
        void UpdateMaterial(const AcrylicMaterialParams& params);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace winrings
