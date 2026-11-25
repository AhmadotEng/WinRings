#include "AcrylicBackdrop.h"

#include <Unknwn.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <dwmapi.h>
#include <roapi.h>

#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.Graphics.Effects.h>

#include <windows.ui.composition.interop.h>
#include <windows.graphics.effects.interop.h>
#include <DispatcherQueue.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace winrings
{
    namespace
    {
        namespace wuc = winrt::Windows::UI::Composition;
        namespace wge = winrt::Windows::Graphics::Effects;
        namespace wf = winrt::Windows::Foundation;
        using ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING;
        using ABI::Windows::Graphics::Effects::IGraphicsEffectD2D1Interop;

        using CreateDispatcherQueueControllerFn = HRESULT (WINAPI*)(
            DispatcherQueueOptions options,
            PDISPATCHERQUEUECONTROLLER* dispatcherQueueController);

        struct WinCompAttrData
        {
            DWORD attrib;
            PVOID pvData;
            SIZE_T cbData;
        };
        using SetWindowCompositionAttributeFn = BOOL (WINAPI*)(HWND, WinCompAttrData*);

        void EnableHostBackdropForWindow(HWND hwnd) noexcept
        {
            const BOOL useHostBackdrop = TRUE;
            DwmSetWindowAttribute(hwnd, 17 /* DWMWA_USE_HOSTBACKDROPBRUSH */, &useHostBackdrop, sizeof(useHostBackdrop));
            const MARGINS fullGlass{ -1, -1, -1, -1 };
            DwmExtendFrameIntoClientArea(hwnd, &fullGlass);

            if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
            {
                if (auto setComp = reinterpret_cast<SetWindowCompositionAttributeFn>(
                    GetProcAddress(user32, "SetWindowCompositionAttribute")))
                {
                    BOOL forceActive = TRUE;
                    WinCompAttrData activeData{ 15 /* WCA_FORCE_ACTIVEWINDOW_APPEARANCE */, &forceActive, sizeof(forceActive) };
                    setComp(hwnd, &activeData);
                }
            }
        }

        struct EffectPropSpec
        {
            const wchar_t* name;
            GRAPHICS_EFFECT_PROPERTY_MAPPING mapping;
            wf::IPropertyValue value;
        };

        class D2D1InteropEffect : public winrt::implements<
            D2D1InteropEffect,
            wge::IGraphicsEffect,
            wge::IGraphicsEffectSource,
            IGraphicsEffectD2D1Interop>
        {
        public:
            D2D1InteropEffect(const GUID& effectId, std::wstring name,
                std::vector<EffectPropSpec> props, std::vector<wge::IGraphicsEffectSource> sources)
                : m_effectId(effectId)
                , m_name(std::move(name))
                , m_props(std::move(props))
                , m_sources(std::move(sources))
            {
            }

            winrt::hstring Name() const { return winrt::hstring(m_name); }
            void Name(winrt::hstring const& value) { m_name = value.c_str(); }

            IFACEMETHODIMP GetEffectId(GUID* id) noexcept override
            {
                if (!id) return E_POINTER;
                *id = m_effectId;
                return S_OK;
            }

            IFACEMETHODIMP GetNamedPropertyMapping(
                LPCWSTR name,
                UINT* index,
                GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) noexcept override
            {
                if (!name || !index || !mapping)
                    return E_INVALIDARG;
                for (UINT i = 0; i < static_cast<UINT>(m_props.size()); ++i)
                {
                    if (_wcsicmp(name, m_props[i].name) == 0)
                    {
                        *index = i;
                        *mapping = m_props[i].mapping;
                        return S_OK;
                    }
                }
                return E_INVALIDARG;
            }

            IFACEMETHODIMP GetPropertyCount(UINT* count) noexcept override
            {
                if (!count) return E_POINTER;
                *count = static_cast<UINT>(m_props.size());
                return S_OK;
            }

            IFACEMETHODIMP GetProperty(
                UINT index,
                ABI::Windows::Foundation::IPropertyValue** value) noexcept override
            {
                if (!value) return E_POINTER;
                *value = nullptr;
                if (index >= m_props.size() || !m_props[index].value)
                    return E_INVALIDARG;
                return winrt::get_unknown(m_props[index].value)->QueryInterface(
                    __uuidof(ABI::Windows::Foundation::IPropertyValue),
                    reinterpret_cast<void**>(value));
            }

            IFACEMETHODIMP GetSource(
                UINT index,
                ABI::Windows::Graphics::Effects::IGraphicsEffectSource** source) noexcept override
            {
                if (!source) return E_POINTER;
                *source = nullptr;
                if (index >= m_sources.size() || !m_sources[index])
                    return E_INVALIDARG;
                return winrt::get_unknown(m_sources[index])->QueryInterface(
                    __uuidof(ABI::Windows::Graphics::Effects::IGraphicsEffectSource),
                    reinterpret_cast<void**>(source));
            }

            IFACEMETHODIMP GetSourceCount(UINT* count) noexcept override
            {
                if (!count) return E_POINTER;
                *count = static_cast<UINT>(m_sources.size());
                return S_OK;
            }

        private:
            GUID m_effectId{};
            std::wstring m_name;
            std::vector<EffectPropSpec> m_props;
            std::vector<wge::IGraphicsEffectSource> m_sources;
        };

        wf::IPropertyValue MakeSingle(float v)
        {
            return wf::PropertyValue::CreateSingle(v).as<wf::IPropertyValue>();
        }

        wf::IPropertyValue MakeUInt32(uint32_t v)
        {
            return wf::PropertyValue::CreateUInt32(v).as<wf::IPropertyValue>();
        }

        wf::IPropertyValue MakeSingle4(float a, float b, float c, float d)
        {
            const std::array<float, 4> arr{ a, b, c, d };
            return wf::PropertyValue::CreateSingleArray(arr).as<wf::IPropertyValue>();
        }

        bool ParamsDiffer(const AcrylicMaterialParams& a, const AcrylicMaterialParams& b) noexcept
        {
            constexpr float kEps = 1e-3f;
            return std::abs(a.blurSigma - b.blurSigma) > kEps ||
                   std::abs(a.saturation - b.saturation) > kEps ||
                   std::abs(a.luminosityR - b.luminosityR) > kEps ||
                   std::abs(a.luminosityG - b.luminosityG) > kEps ||
                   std::abs(a.luminosityB - b.luminosityB) > kEps ||
                   std::abs(a.luminosityAlpha - b.luminosityAlpha) > kEps ||
                   std::abs(a.tintR - b.tintR) > kEps ||
                   std::abs(a.tintG - b.tintG) > kEps ||
                   std::abs(a.tintB - b.tintB) > kEps ||
                   std::abs(a.tintAlpha - b.tintAlpha) > kEps;
        }
    } // namespace

    struct AcrylicBackdrop::Impl
    {
        bool active = false;
        bool roInitialized = false;
        bool hasParams = false;
        AcrylicMaterialParams lastParams{};

        winrt::com_ptr<ABI::Windows::System::IDispatcherQueueController> dispatcherController;
        wuc::Compositor compositor{ nullptr };
        winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget target{ nullptr };
        wuc::ContainerVisual root{ nullptr };
        wuc::SpriteVisual backdropVisual{ nullptr };
        wuc::SpriteVisual foregroundVisual{ nullptr };
        wuc::CompositionBackdropBrush hostBackdrop{ nullptr };
        wuc::CompositionSurfaceBrush maskSurfaceBrush{ nullptr };
        wuc::CompositionSurfaceBrush foregroundSurfaceBrush{ nullptr };
        wuc::CompositionMaskBrush maskBrush{ nullptr };
        wuc::CompositionEffectBrush currentEffectBrush{ nullptr };
    };

    AcrylicBackdrop::AcrylicBackdrop()
        : m_impl(std::make_unique<Impl>())
    {
    }

    AcrylicBackdrop::~AcrylicBackdrop()
    {
        Shutdown();
    }

    bool AcrylicBackdrop::Initialize(
        HWND hwnd,
        IDXGISwapChain1* foregroundSwapChain,
        IDXGISwapChain1* maskSwapChain,
        int widthPx,
        int heightPx)
    {
        Shutdown();
        if (!hwnd || !foregroundSwapChain || !maskSwapChain)
            return false;

        try
        {
            const HRESULT roHr = RoInitialize(RO_INIT_SINGLETHREADED);
            if (roHr == S_OK || roHr == S_FALSE)
                m_impl->roInitialized = true;

            if (!winrt::Windows::System::DispatcherQueue::GetForCurrentThread())
            {
                HMODULE coreMsg = LoadLibraryExW(L"CoreMessaging.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!coreMsg)
                {
                    Shutdown();
                    return false;
                }
                auto createDq = reinterpret_cast<CreateDispatcherQueueControllerFn>(
                    GetProcAddress(coreMsg, "CreateDispatcherQueueController"));
                if (!createDq)
                {
                    FreeLibrary(coreMsg);
                    Shutdown();
                    return false;
                }
                DispatcherQueueOptions opts{};
                opts.dwSize = sizeof(opts);
                opts.threadType = DQTYPE_THREAD_CURRENT;
                opts.apartmentType = DQTAT_COM_NONE;
                const HRESULT dqHr = createDq(opts, m_impl->dispatcherController.put());
                FreeLibrary(coreMsg);
                if (FAILED(dqHr))
                {
                    Shutdown();
                    return false;
                }
            }

            m_impl->compositor = wuc::Compositor();
            if (!m_impl->compositor)
            {
                Shutdown();
                return false;
            }

            EnableHostBackdropForWindow(hwnd);

            auto desktopInterop = m_impl->compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
            winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget target{ nullptr };
            winrt::check_hresult(desktopInterop->CreateDesktopWindowTarget(
                hwnd,
                TRUE,
                reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(winrt::put_abi(target))));
            m_impl->target = std::move(target);

            auto compInterop = m_impl->compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>();
            wuc::ICompositionSurface fgSurface{ nullptr };
            wuc::ICompositionSurface maskSurface{ nullptr };
            winrt::check_hresult(compInterop->CreateCompositionSurfaceForSwapChain(
                foregroundSwapChain,
                reinterpret_cast<ABI::Windows::UI::Composition::ICompositionSurface**>(winrt::put_abi(fgSurface))));
            winrt::check_hresult(compInterop->CreateCompositionSurfaceForSwapChain(
                maskSwapChain,
                reinterpret_cast<ABI::Windows::UI::Composition::ICompositionSurface**>(winrt::put_abi(maskSurface))));

            m_impl->hostBackdrop = m_impl->compositor.CreateHostBackdropBrush();
            m_impl->foregroundSurfaceBrush = m_impl->compositor.CreateSurfaceBrush(fgSurface);
            m_impl->foregroundSurfaceBrush.Stretch(wuc::CompositionStretch::Fill);
            m_impl->maskSurfaceBrush = m_impl->compositor.CreateSurfaceBrush(maskSurface);
            m_impl->maskSurfaceBrush.Stretch(wuc::CompositionStretch::Fill);

            m_impl->maskBrush = m_impl->compositor.CreateMaskBrush();
            m_impl->maskBrush.Mask(m_impl->maskSurfaceBrush);

            m_impl->root = m_impl->compositor.CreateContainerVisual();
            m_impl->backdropVisual = m_impl->compositor.CreateSpriteVisual();
            m_impl->backdropVisual.Brush(m_impl->maskBrush);
            m_impl->foregroundVisual = m_impl->compositor.CreateSpriteVisual();
            m_impl->foregroundVisual.Brush(m_impl->foregroundSurfaceBrush);

            m_impl->root.Children().InsertAtTop(m_impl->backdropVisual);
            m_impl->root.Children().InsertAtTop(m_impl->foregroundVisual);
            m_impl->target.Root(m_impl->root);

            m_impl->active = true;
            Resize(widthPx, heightPx);
            UpdateMaterial(AcrylicMaterialParams{});
            if (!m_impl->currentEffectBrush)
            {
                Shutdown();
                return false;
            }
            return true;
        }
        catch (...)
        {
            Shutdown();
            return false;
        }
    }

    void AcrylicBackdrop::Shutdown()
    {
        if (!m_impl)
            return;
        try
        {
            if (m_impl->target)
            {
                m_impl->target.Root(nullptr);
                m_impl->target.Close();
            }
            if (m_impl->maskBrush)
            {
                m_impl->maskBrush.Source(nullptr);
                m_impl->maskBrush.Mask(nullptr);
            }
            m_impl->currentEffectBrush = nullptr;
            m_impl->maskBrush = nullptr;
            m_impl->maskSurfaceBrush = nullptr;
            m_impl->foregroundSurfaceBrush = nullptr;
            m_impl->hostBackdrop = nullptr;
            m_impl->backdropVisual = nullptr;
            m_impl->foregroundVisual = nullptr;
            m_impl->root = nullptr;
            m_impl->target = nullptr;
            m_impl->compositor = nullptr;
            m_impl->dispatcherController = nullptr;
        }
        catch (...)
        {
        }
        m_impl->active = false;
        m_impl->hasParams = false;
        if (m_impl->roInitialized)
        {
            RoUninitialize();
            m_impl->roInitialized = false;
        }
    }

    bool AcrylicBackdrop::IsActive() const noexcept
    {
        return m_impl && m_impl->active;
    }

    void AcrylicBackdrop::Resize(int widthPx, int heightPx)
    {
        if (!IsActive())
            return;
        try
        {
            const wf::Numerics::float2 sz{
                static_cast<float>((std::max)(1, widthPx)),
                static_cast<float>((std::max)(1, heightPx))
            };
            if (m_impl->root) m_impl->root.Size(sz);
            if (m_impl->backdropVisual) m_impl->backdropVisual.Size(sz);
            if (m_impl->foregroundVisual) m_impl->foregroundVisual.Size(sz);
        }
        catch (...)
        {
        }
    }

    void AcrylicBackdrop::UpdateMaterial(const AcrylicMaterialParams& params)
    {
        if (!IsActive() || !m_impl->compositor || !m_impl->hostBackdrop || !m_impl->maskBrush)
            return;
        if (m_impl->hasParams && !ParamsDiffer(m_impl->lastParams, params))
            return;

        auto buildAndApply = [&](bool withLuminosity) -> bool
        {
            try
            {
                wuc::CompositionEffectSourceParameter backdropParam(L"backdrop");
                wge::IGraphicsEffectSource backdropStage = backdropParam.as<wge::IGraphicsEffectSource>();

                if (params.blurSigma > 0.1f)
                {
                    auto blur = winrt::make<D2D1InteropEffect>(
                        CLSID_D2D1GaussianBlur,
                        L"Blur",
                        std::vector<EffectPropSpec>{
                            { L"StandardDeviation", ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT, MakeSingle(params.blurSigma) },
                            { L"Optimization", ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT, MakeUInt32(static_cast<uint32_t>(D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED)) },
                            { L"BorderMode", ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT, MakeUInt32(static_cast<uint32_t>(D2D1_BORDER_MODE_HARD)) },
                        },
                        std::vector<wge::IGraphicsEffectSource>{ backdropStage });

                    auto sat = winrt::make<D2D1InteropEffect>(
                        CLSID_D2D1Saturation,
                        L"Saturation",
                        std::vector<EffectPropSpec>{
                            { L"Saturation", ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT, MakeSingle(params.saturation) },
                        },
                        std::vector<wge::IGraphicsEffectSource>{ blur });
                    backdropStage = sat;
                }
                else if (std::abs(params.saturation - 1.0f) > 0.01f)
                {
                    auto sat = winrt::make<D2D1InteropEffect>(
                        CLSID_D2D1Saturation,
                        L"Saturation",
                        std::vector<EffectPropSpec>{
                            { L"Saturation", ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT, MakeSingle(params.saturation) },
                        },
                        std::vector<wge::IGraphicsEffectSource>{ backdropStage });
                    backdropStage = sat;
                }

                // Windows.UI.Composition (dwmcore) requires a strict tree effect graph (no shared parent nodes).
                // Empirically verified on Windows 11 dwmcore:
                //   1) CLSID_D2D1Flood expects STRAIGHT RGBA (dwmcore premultiplies RGB by A internally).
                //   2) CLSID_D2D1Blend takes D2D1_BLEND_MODE_COLOR with { backdropStage, lumFlood } to keep
                //      the backdrop's hue & saturation while applying lumFlood's luminosity at lumA opacity
                //      (matching Direct2D's D2D1_BLEND_MODE_LUMINOSITY with { backdropStage, premulLumFlood } 1:1).
                const float lumA = std::clamp(params.luminosityAlpha, 0.0f, 1.0f);
                if (withLuminosity && lumA > 0.01f)
                {
                    auto lumFlood = winrt::make<D2D1InteropEffect>(
                        CLSID_D2D1Flood,
                        L"LuminosityFlood",
                        std::vector<EffectPropSpec>{
                            { L"Color", ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT,
                              MakeSingle4(params.luminosityR, params.luminosityG, params.luminosityB, lumA) },
                        },
                        std::vector<wge::IGraphicsEffectSource>{});

                    auto lumBlend = winrt::make<D2D1InteropEffect>(
                        CLSID_D2D1Blend,
                        L"LuminosityBlend",
                        std::vector<EffectPropSpec>{
                            { L"Mode", ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT,
                              MakeUInt32(static_cast<uint32_t>(D2D1_BLEND_MODE_COLOR)) },
                        },
                        std::vector<wge::IGraphicsEffectSource>{ backdropStage, lumFlood });
                    backdropStage = lumBlend;
                }

                // Pass STRAIGHT RGBA to CLSID_D2D1Flood because dwmcore premultiplies Flood Color internally.
                const float a = std::clamp(params.tintAlpha, 0.0f, 1.0f);
                auto tintFlood = winrt::make<D2D1InteropEffect>(
                    CLSID_D2D1Flood,
                    L"TintFlood",
                    std::vector<EffectPropSpec>{
                        { L"Color", ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT,
                          MakeSingle4(params.tintR, params.tintG, params.tintB, a) },
                    },
                    std::vector<wge::IGraphicsEffectSource>{});

                auto composite = winrt::make<D2D1InteropEffect>(
                    CLSID_D2D1Composite,
                    L"TintOverBackdrop",
                    std::vector<EffectPropSpec>{
                        { L"Mode", ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT,
                          MakeUInt32(static_cast<uint32_t>(D2D1_COMPOSITE_MODE_SOURCE_OVER)) },
                    },
                    std::vector<wge::IGraphicsEffectSource>{ backdropStage, tintFlood });

                auto factory = m_impl->compositor.CreateEffectFactory(composite.as<wge::IGraphicsEffect>());
                if (!factory || FAILED(factory.ExtendedError()))
                    return false;
                const auto loadStatus = factory.LoadStatus();
                if (loadStatus != wuc::CompositionEffectFactoryLoadStatus::Success &&
                    loadStatus != wuc::CompositionEffectFactoryLoadStatus::Pending)
                    return false;

                auto brush = factory.CreateBrush();
                if (!brush)
                    return false;
                brush.SetSourceParameter(L"backdrop", m_impl->hostBackdrop);
                m_impl->maskBrush.Source(brush);
                m_impl->currentEffectBrush = brush;
                m_impl->lastParams = params;
                m_impl->hasParams = true;
                return true;
            }
            catch (...)
            {
                return false;
            }
        };

        if (!buildAndApply(true))
            buildAndApply(false);
    }
} // namespace winrings
