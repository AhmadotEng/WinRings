#pragma once

#include <Windows.h>
#include <algorithm>
#include <cmath>

#include "RadialMath.h"

namespace winrings
{
    inline bool ShouldShowLiveRingForTab(
        int activeTab,
        bool isMinimized,
        bool isWindowActive,
        bool isPaused) noexcept
    {
        return (activeTab == 1 || activeTab == 2) && !isMinimized && isWindowActive && !isPaused;
    }

    inline POINT ComputeRingStageCenterPhysical(
        POINT clientOriginScreenPx,
        double dpiScale,
        float stageLeftDip,
        float stageTopDip,
        float stageRightDip,
        float stageBottomDip) noexcept
    {
        const double scale = (std::max)(0.5, dpiScale);
        const double centerDipX = (static_cast<double>(stageLeftDip) + static_cast<double>(stageRightDip)) * 0.5;
        const double centerDipY = (static_cast<double>(stageTopDip) + static_cast<double>(stageBottomDip)) * 0.5;
        return POINT{
            static_cast<LONG>(std::llround(static_cast<double>(clientOriginScreenPx.x) + centerDipX * scale)),
            static_cast<LONG>(std::llround(static_cast<double>(clientOriginScreenPx.y) + centerDipY * scale))
        };
    }

    inline PointD ComputePreviewRingClampedCenter(
        double targetDipX,
        double targetDipY,
        double outerRadiusDip,
        const RectD& workAreaDip) noexcept
    {
        const double pad = (std::min)(
            outerRadiusDip + 8.0,
            (std::min)(workAreaDip.width, workAreaDip.height) * 0.45);
        return ClampRingCenter(targetDipX, targetDipY, pad, workAreaDip);
    }

    inline double ComputePreviewOverlayHitRadiusDip(
        double outerRadiusDip,
        double maxCenterDip) noexcept
    {
        return std::clamp(outerRadiusDip + 28.0, 24.0, (std::max)(24.0, maxCenterDip));
    }

    inline bool ShouldClipOverlayToActiveRegion(
        bool isOpen,
        bool isDragOrReorderActive) noexcept
    {
        return isOpen && !isDragOrReorderActive;
    }

    inline bool IsRingSlotPressWithinOuterRadius(
        double distDip,
        double deadzoneDip,
        double outerRadiusDip) noexcept
    {
        return distDip > deadzoneDip && distDip <= outerRadiusDip + 12.0;
    }

    inline float ResolveSidebarIconHoverAlpha(
        bool isIconHovered,
        bool isRowHovered) noexcept
    {
        if (isIconHovered)
            return 1.0f;
        if (isRowHovered)
            return 0.45f;
        return 0.0f;
    }

    inline bool ShouldFooterStatusClickTogglePause(
        bool isActive,
        bool isPaused) noexcept
    {
        return isActive || isPaused;
    }

    inline float ComputeUnifiedProfilesCardBottomDip(
        float headerBottomDip,
        size_t visibleProfileCount,
        float rowPitchDip,
        float bottomPadDip,
        float maxBottomDip) noexcept
    {
        if (visibleProfileCount == 0)
            return headerBottomDip;
        const float bottom = headerBottomDip + 4.0f +
            static_cast<float>(visibleProfileCount) * rowPitchDip + bottomPadDip;
        return (std::min)(maxBottomDip, bottom);
    }

    inline bool ShouldShowProfileDeleteAction(
        bool isDefault,
        bool isSMenu) noexcept
    {
        return !isDefault && !isSMenu;
    }
} // namespace winrings
