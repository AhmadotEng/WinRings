#pragma once

#include <Windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../shared/RadialMath.h"
#include "../shared/WinRingsConfig.h"
#include "DropTarget.h"
#include "DwmThumbPool.h"
#include "SlotModel.h"
#include "WindowTracker.h"

namespace winrings
{
    class RingOverlay
    {
    public:
        struct Callbacks
        {
            std::function<void(const std::wstring& exeArgs)> launchTools;
            std::function<void(const SwitcherConfig& updatedConfig)> saveAndReloadConfig;
            std::function<void()> onToggleTopBarRequested;
            std::function<void()> onExitRequested;
            std::function<void()> onOverlayClosed;
        };

        RingOverlay() = default;
        ~RingOverlay();

        RingOverlay(const RingOverlay&) = delete;
        RingOverlay& operator=(const RingOverlay&) = delete;

        bool Initialize(HINSTANCE instance, HMODULE shellModule, WindowTracker* tracker, Callbacks callbacks);
        void Shutdown();

        void SetConfig(const SwitcherConfig& config);
        const SwitcherConfig& Config() const noexcept { return m_config; }

        HWND Hwnd() const noexcept { return m_hwnd; }
        bool IsOpen() const noexcept { return m_isOpen; }
        bool IsLockedOpen() const noexcept { return m_isLockedOpen; }
        double DpiScale() const noexcept { return m_dpiScale; }
        bool IsPointInActiveBounds(POINT screenPt) const noexcept;

        void OpenAt(int physicalX, int physicalY, std::vector<RingSlot> slots, bool lockOpen);
        void LockOpen() noexcept { m_isLockedOpen = true; }
        void UpdatePointer(int physicalX, int physicalY);
        void HandleScrollWheel(int wheelDelta);
        void CommitSelectionAndClose();
        void Dismiss();

        void AppendFilterChar(wchar_t ch);
        void BackspaceFilter();

    private:
        struct SubmenuCard
        {
            RectD boundsDip;
            const WindowEntry* window = nullptr;
            const RingSlot* subSlot = nullptr;
            double currentScale = 1.0;
            double targetScale = 1.0;
        };

        struct ThemeColors
        {
            D2D1_COLOR_F ringFill{};
            D2D1_COLOR_F auraOuter{};
            bool useAuraGradient = false;
            D2D1_COLOR_F ringStroke{};
            float ringStrokeWidth = 1.5f;
            D2D1_COLOR_F innerFill{};
            D2D1_COLOR_F slotIdleFill{};
            D2D1_COLOR_F slotSelectedFill{};
            D2D1_COLOR_F sectorFill{};
            D2D1_COLOR_F sectorStroke{};
            D2D1_COLOR_F centerFill{};
            D2D1_COLOR_F centerBorder{};
            D2D1_COLOR_F textColor{};
            D2D1_COLOR_F accent{};
            COLORREF accentColorRef = RGB(49, 134, 255);
            COLORREF borderColorRef = RGB(70, 78, 92);
        };

        static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
        LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

        bool CreateGraphicsDevices();
        void ReleaseGraphicsDevices();
        bool EnsureSwapChainSize(int widthPx, int heightPx);
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> GetOrCreateD2DBitmap(IWICBitmapSource* wicSource);

        void UpdateDpiAndPosition(int physicalX, int physicalY);
        ThemeColors BuildThemeColors() const noexcept;
        void RebuildLayoutAfterFilter();
        bool SlotHasSubmenuItems(const RingSlot& slot) const noexcept;
        void OpenSubmenuForSlot(int slotIndex, PointD originDip);
        void ClearSubmenu();
        int HitTestSubmenu(PointD localDip) const noexcept;
        void SyncDwmThumbnails();
        void StepAnimations();
        void Render();
        void ActivateSlotOrWindow(const RingSlot* slot, const WindowEntry* win, bool shiftHeld);
        void ShowContextMenuAt(POINT screenPt);
        void ShowDropActionsMenu(const std::vector<std::wstring>& files, POINT screenPt);
        std::wstring BuildFilterSummary() const;

        void OnDragEnterOverlay();
        void OnDragOverOverlay(POINT screenPt);
        void OnDragLeaveOverlay();
        void OnDropOverlay(POINT screenPt, std::vector<std::wstring> files, std::wstring text);

        HINSTANCE m_instance = nullptr;
        HMODULE m_shellModule = nullptr;
        WindowTracker* m_tracker = nullptr;
        Callbacks m_callbacks;
        SwitcherConfig m_config;

        HWND m_hwnd = nullptr;
        bool m_classRegistered = false;
        RingDropTarget* m_dropTarget = nullptr;
        DwmThumbPool m_thumbPool;

        // Direct3D / Direct2D / DirectComposition
        Microsoft::WRL::ComPtr<ID3D11Device> m_d3dDevice;
        Microsoft::WRL::ComPtr<IDXGIDevice> m_dxgiDevice;
        Microsoft::WRL::ComPtr<ID2D1Factory1> m_d2dFactory;
        Microsoft::WRL::ComPtr<ID2D1Device> m_d2dDevice;
        Microsoft::WRL::ComPtr<ID2D1DeviceContext> m_d2dContext;
        Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_titleFormat;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_smallFormat;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_glyphFormat;
        Microsoft::WRL::ComPtr<IDCompositionDevice> m_dcompDevice;
        Microsoft::WRL::ComPtr<IDCompositionTarget> m_dcompTarget;
        Microsoft::WRL::ComPtr<IDCompositionVisual> m_dcompVisual;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_targetBitmap;
        int m_swapWidthPx = 0;
        int m_swapHeightPx = 0;

        std::unordered_map<IWICBitmapSource*, Microsoft::WRL::ComPtr<ID2D1Bitmap1>> m_d2dBitmapCache;

        // Runtime ring state
        std::vector<RingSlot> m_allSlots;
        std::vector<RingSlot> m_visibleSlots;
        std::vector<double> m_slotScales;
        std::vector<SubmenuCard> m_submenuCards;
        std::vector<RectD> m_submenuCardRects;

        double m_dpiScale = 1.0;
        PointD m_centerScreenPhysical{};
        int m_selectedIndex = -1;
        int m_expandedSlotIndex = -1;
        int m_selectedSubIndex = -1;
        RectD m_submenuBoundsDip{};
        PointD m_corridorOriginDip{};
        bool m_submenuIsTitleColumn = false;
        bool m_hasLastPointer = false;
        POINT m_lastPointerPhysical{};
        std::wstring m_filterQuery;

        bool m_isOpen = false;
        bool m_isLockedOpen = false;
        bool m_contextMenuOpen = false;
        bool m_dragActive = false;
        int m_dragHoverBadge = 0; // 0=none, 1=Convert, 2=Actions
        double m_openProgress = 1.0;
        LONGLONG m_openStartQpc = 0;
        LONGLONG m_qpcFreq = 1;
        bool m_animTimerRunning = false;
    };
} // namespace winrings
