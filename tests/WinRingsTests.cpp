#include <Windows.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

#include "../src/shared/CrashGuard.h"
#include "../src/shared/RadialMath.h"
#include "../src/shared/RingThemeEngine.h"
#include "../src/shared/SearchMatch.h"
#include "../src/shared/SettingsLayoutMath.h"
#include "../src/shared/SlotMatching.h"
#include "../src/shared/TriggerModel.h"
#include "../src/shared/WinRingsConfig.h"
#include "../src/shared/WinRingsDeltaIpc.h"

using namespace winrings;

static int g_failures = 0;

#define CHECK(cond)                                                                      \
    do                                                                                   \
    {                                                                                    \
        if (!(cond))                                                                     \
        {                                                                                \
            std::printf("FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond);                \
            ++g_failures;                                                                \
        }                                                                                \
    } while (0)

static void TestRadialMath()
{
    CHECK(CalculateSectorIndex(10.0, -10.0, 6, 18.0) == -1);
    CHECK(CalculateSectorIndex(0.0, -24.0, 8, 18.0) == 0);
    CHECK(CalculateSectorIndex(0.0, -80.0, 4, 18.0) == 0);
    CHECK(CalculateSectorIndex(80.0, 0.0, 4, 18.0) == 1);
    CHECK(CalculateSectorIndex(0.0, 80.0, 4, 18.0) == 2);
    CHECK(CalculateSectorIndex(-80.0, 0.0, 4, 18.0) == 3);
    CHECK(CalculateSectorIndex(100.0, 0.0, 0, 18.0) == -1);

    CHECK(StepIndex(-1, 6, -120) == 0);
    CHECK(StepIndex(-1, 6, 120) == 5);
    CHECK(StepIndex(5, 6, -120) == 0);
    CHECK(StepIndex(0, 6, 120) == 5);

    const auto arc = GetOuterArcCenters(340.0, 340.0, -kPi / 2.0, 200.0, 14);
    CHECK(arc.size() == 9);
    CHECK(std::abs(arc[0].x - 340.0) < 0.1);
    CHECK(std::abs(arc[0].y - 140.0) < 0.1);

    const PointD origin{ 340.0, 260.0 };
    const RectD submenu{ 280.0, 80.0, 120.0, 80.0 };
    CHECK(IsPointInHoverCorridor({ 340.0, 200.0 }, origin, submenu));
    CHECK(IsPointInHoverCorridor({ 320.0, 110.0 }, origin, submenu));
    CHECK(!IsPointInHoverCorridor({ 340.0, 360.0 }, origin, submenu));
    // Must not overshoot far past the submenu target rect.
    CHECK(!IsPointInHoverCorridor({ 340.0, 10.0 }, origin, submenu));
    // When origin is inside the expanded rect, must not project a 360-degree wraparound cone outside it.
    CHECK(!IsPointInHoverCorridor({ 340.0, 220.0 }, { 340.0, 120.0 }, submenu));
    CHECK(!IsPointInHoverCorridor({ 100.0, 120.0 }, { 340.0, 120.0 }, submenu));

    const RectD workArea{ 0.0, 30.0, 1920.0, 1050.0 };
    const auto clamped = ClampRingCenter(20.0, 10.0, 160.0, workArea);
    CHECK(std::abs(clamped.x - 160.0) < 0.01);
    CHECK(std::abs(clamped.y - 190.0) < 0.01);

    CHECK(std::abs(EaseOutCubic(0.0) - 0.0) < 1e-6);
    CHECK(std::abs(EaseOutCubic(1.0) - 1.0) < 1e-6);
    CHECK(EaseOutCubic(0.5) > 0.5);
}

static bool RectIntersectsCircle(RectD r, PointD c, double radius) noexcept
{
    const double closestX = std::clamp(c.x, r.left, r.Right());
    const double closestY = std::clamp(c.y, r.top, r.Bottom());
    return std::hypot(closestX - c.x, closestY - c.y) < radius;
}

static void TestDownloadsAndSubmenuHoverTransitions()
{
    constexpr double kOverlay = 680.0;
    constexpr PointD kCenter{ 340.0, 340.0 };
    constexpr double kDeadzone = 18.0;
    constexpr double kRingR = 112.0;
    constexpr double kIconSize = 42.0;
    const double mainRingOuterR = MainRingOuterRadius(kRingR, kIconSize);
    CHECK(std::abs(mainRingOuterR - 145.0) < 0.01);

    const RingGeometry a4sGeo = ComputeRingGeometry(8, 115.0, 44.0, 18.0);
    CHECK(std::abs(a4sGeo.ringRadius - 115.0) < 0.01);
    CHECK(std::abs(a4sGeo.outerRadius - 149.0) < 0.01);
    CHECK(a4sGeo.holeRadius > 18.0 && a4sGeo.holeRadius < a4sGeo.bandRadius);

    const IconArcLayout topSearchArc = ComputeIconArcLayout(kCenter.x, kCenter.y, -kPi * 0.5, a4sGeo.outerRadius, 6);
    CHECK(topSearchArc.rects.size() == 6);
    CHECK(std::abs(topSearchArc.trackWidthDip - 56.0) < 0.01);
    CHECK(std::abs((topSearchArc.startAngleRad + topSearchArc.endAngleRad) * 0.5 - (-kPi * 0.5)) < 0.01);

    // 1. Exact Bug 1 reproduction: Downloads (Folder, slot 1) with 8 items in 3-, 6-, 8-, and 14-slot rings.
    const int ringSizes[] = { 3, 6, 8, 14 };
    for (int slotCount : ringSizes)
    {
        const int downloadsSlot = 1;
        const double dlAngle = GetSlotAngle(downloadsSlot, slotCount);
        const auto cards = ComputeFolderSubmenuRects(kCenter.x, kCenter.y, dlAngle, kRingR, kIconSize, 8);
        CHECK(cards.size() == 8);
        const RectD bounds = ComputeUnionBounds(cards);

        // No submenu card may intersect the main ring.
        for (const auto& card : cards)
            CHECK(!RectIntersectsCircle(card, kCenter, mainRingOuterR));

        // Test transitions starting both from barely outside deadzone (r=22) AND from the Downloads icon (r=112).
        const PointD origins[2] = {
            { kCenter.x + 22.0 * std::cos(dlAngle), kCenter.y + 22.0 * std::sin(dlAngle) },
            GetSlotCenter(kCenter.x, kCenter.y, kRingR, downloadsSlot, slotCount)
        };

        for (const PointD& origin : origins)
        {
            // Moving to EVERY other main-ring slot icon (and inner/outer ring points in its sector) MUST switch immediately.
            for (int other = 0; other < slotCount; ++other)
            {
                if (other == downloadsSlot)
                    continue;
                const double otherAngle = GetSlotAngle(other, slotCount);
                const PointD testPts[3] = {
                    GetSlotCenter(kCenter.x, kCenter.y, kRingR, other, slotCount),
                    { kCenter.x + 55.0 * std::cos(otherAngle), kCenter.y + 55.0 * std::sin(otherAngle) },
                    { kCenter.x + (mainRingOuterR - 2.0) * std::cos(otherAngle), kCenter.y + (mainRingOuterR - 2.0) * std::sin(otherAngle) }
                };
                for (const PointD& pt : testPts)
                {
                    const auto res = ResolveRingHover(
                        pt, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
                        downloadsSlot, downloadsSlot, origin, cards, bounds, false);
                    CHECK(res.selectedSlot == other);
                    CHECK(res.selectedSubIndex == -1);
                    CHECK(!res.keepExistingSubmenu);
                }
            }

            // Moving into center deadzone MUST immediately deselect and collapse submenu.
            const PointD deadzonePts[4] = {
                kCenter,
                { kCenter.x + 10.0, kCenter.y - 6.0 },
                { kCenter.x, kCenter.y - kDeadzone },
                { kCenter.x + (kDeadzone - 0.5) * std::cos(dlAngle), kCenter.y + (kDeadzone - 0.5) * std::sin(dlAngle) }
            };
            for (const PointD& pt : deadzonePts)
            {
                const auto res = ResolveRingHover(
                    pt, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
                    downloadsSlot, downloadsSlot, origin, cards, bounds, false);
                CHECK(res.selectedSlot == -1);
                CHECK(res.selectedSubIndex == -1);
                CHECK(!res.keepExistingSubmenu);
            }
        }

        // Gliding straight out from Downloads icon across the gap into card 0 keeps submenu open and selects card 0.
        const PointD dlCenter = GetSlotCenter(kCenter.x, kCenter.y, kRingR, downloadsSlot, slotCount);
        const PointD bridgePt{ kCenter.x + 165.0 * std::cos(dlAngle), kCenter.y + 165.0 * std::sin(dlAngle) };
        const auto bridgeRes = ResolveRingHover(
            bridgePt, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
            downloadsSlot, downloadsSlot, dlCenter, cards, bounds, false);
        CHECK(bridgeRes.selectedSlot == downloadsSlot);
        CHECK(bridgeRes.selectedSubIndex == -1);
        CHECK(bridgeRes.keepExistingSubmenu);

        // Hovering every submenu card selects that exact card; gliding in inter-card gaps keeps submenu open.
        for (size_t i = 0; i < cards.size(); ++i)
        {
            const PointD cardCenter{ cards[i].left + cards[i].width * 0.5, cards[i].top + cards[i].height * 0.5 };
            const auto cardRes = ResolveRingHover(
                cardCenter, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
                downloadsSlot, downloadsSlot, dlCenter, cards, bounds, false);
            CHECK(cardRes.selectedSlot == downloadsSlot);
            CHECK(cardRes.selectedSubIndex == static_cast<int>(i));
            CHECK(cardRes.keepExistingSubmenu);
        }

        // Gap between adjacent items along the continuous half-ring track keeps Downloads open.
        const auto dlArc = ComputeIconArcLayout(kCenter.x, kCenter.y, dlAngle, mainRingOuterR, 8);
        CHECK(dlArc.angles.size() == 8);
        const double gapAngle = 0.5 * (dlArc.angles[0] + dlArc.angles[1]);
        const PointD gapPt{ kCenter.x + dlArc.arcRadius * std::cos(gapAngle), kCenter.y + dlArc.arcRadius * std::sin(gapAngle) };
        const auto gapRes = ResolveRingHover(
            gapPt, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
            downloadsSlot, downloadsSlot, dlCenter, cards, bounds, false);
        CHECK(gapRes.selectedSlot == downloadsSlot);
        CHECK(gapRes.selectedSubIndex == -1);
        CHECK(gapRes.keepExistingSubmenu);

        // Moving back onto Downloads icon clears stale sub-selection while keeping Downloads open & updating origin.
        const auto backRes = ResolveRingHover(
            dlCenter, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
            downloadsSlot, downloadsSlot, origins[0], cards, bounds, false);
        CHECK(backRes.selectedSlot == downloadsSlot);
        CHECK(backRes.selectedSubIndex == -1);
        CHECK(backRes.keepExistingSubmenu);
        CHECK(backRes.updateCorridorOrigin);

        // Moving far outside the opposite side of the ring MUST NOT select an unhovered icon / invisible PiP.
        const int oppositeSlot = (downloadsSlot + slotCount / 2) % slotCount;
        const double oppAngle = GetSlotAngle(oppositeSlot, slotCount);
        const PointD farOpp{ kCenter.x + 230.0 * std::cos(oppAngle), kCenter.y + 230.0 * std::sin(oppAngle) };
        const auto oppRes = ResolveRingHover(
            farOpp, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
            downloadsSlot, downloadsSlot, dlCenter, cards, bounds, false);
        CHECK(oppRes.selectedSlot == -1);
        CHECK(oppRes.selectedSubIndex == -1);
        CHECK(!oppRes.keepExistingSubmenu);

        // Bounded outer reach: sticky (12 DIP) rejects r = mainRingOuterR + 40, while holding reach (48 DIP) selects oppositeSlot.
        const PointD midOutOpp{
            kCenter.x + (mainRingOuterR + 40.0) * std::cos(oppAngle),
            kCenter.y + (mainRingOuterR + 40.0) * std::sin(oppAngle)
        };
        const auto stickyFarRes = ResolveRingHover(
            midOutOpp, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
            downloadsSlot, downloadsSlot, dlCenter, cards, bounds, false, 12.0);
        CHECK(stickyFarRes.selectedSlot == -1);
        const auto holdFarRes = ResolveRingHover(
            midOutOpp, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
            downloadsSlot, downloadsSlot, dlCenter, cards, bounds, false, 48.0);
        CHECK(holdFarRes.selectedSlot == oppositeSlot);
        CHECK(!holdFarRes.keepExistingSubmenu);
    }

    // 2. Multi-window App in OuterArc mode across thumbnailScale = 0.5, 1.0, 1.5 (up to 9 windows).
    const double scales[] = { 0.5, 1.0, 1.5 };
    for (double scale : scales)
    {
        const int slotCount = 8;
        const int appSlot = 2;
        const double baseAngle = GetSlotAngle(appSlot, slotCount);
        const auto cards = ComputeWindowOuterArcRects(kCenter.x, kCenter.y, baseAngle, kRingR, kIconSize, scale, 9);
        CHECK(cards.size() == 9);
        const RectD bounds = ComputeUnionBounds(cards);
        const PointD origin = GetSlotCenter(kCenter.x, kCenter.y, kRingR, appSlot, slotCount);

        for (const auto& card : cards)
            CHECK(!RectIntersectsCircle(card, kCenter, mainRingOuterR));

        // Direct hit on outermost window card (index 8) works.
        const PointD lastCard{ cards[8].left + cards[8].width * 0.5, cards[8].top + cards[8].height * 0.5 };
        const auto subRes = ResolveRingHover(
            lastCard, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
            appSlot, appSlot, origin, cards, bounds, false);
        CHECK(subRes.selectedSlot == appSlot);
        CHECK(subRes.selectedSubIndex == 8);
        CHECK(subRes.keepExistingSubmenu);

        // Moving onto ANY other main-ring slot icon immediately switches away.
        for (int other = 0; other < slotCount; ++other)
        {
            if (other == appSlot)
                continue;
            const PointD otherPt = GetSlotCenter(kCenter.x, kCenter.y, kRingR, other, slotCount);
            const auto res = ResolveRingHover(
                otherPt, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
                appSlot, appSlot, origin, cards, bounds, false);
            CHECK(res.selectedSlot == other);
            CHECK(!res.keepExistingSubmenu);
        }
    }

    // 3. Multi-window App in TitleColumn mode (1, 5, 9 windows) at all 8 compass slots.
    const int counts[] = { 1, 5, 9 };
    for (int count : counts)
    {
        const int slotCount = 8;
        for (int appSlot = 0; appSlot < slotCount; ++appSlot)
        {
            const double baseAngle = GetSlotAngle(appSlot, slotCount);
            const auto cards = ComputeTitleColumnRects(kCenter.x, kCenter.y, kOverlay, baseAngle, kRingR, kIconSize, count);
            CHECK(static_cast<int>(cards.size()) == count);
            const RectD bounds = ComputeUnionBounds(cards);
            const PointD origin = GetSlotCenter(kCenter.x, kCenter.y, kRingR, appSlot, slotCount);

            // TitleColumn cards must stay inside overlay and must NEVER overlap the main ring.
            for (const auto& card : cards)
            {
                CHECK(card.left >= 0.0 && card.Right() <= kOverlay);
                CHECK(card.top >= 0.0 && card.Bottom() <= kOverlay);
                CHECK(!RectIntersectsCircle(card, kCenter, mainRingOuterR));
            }

            // Hovering every row selects that exact row.
            for (int r = 0; r < count; ++r)
            {
                const PointD rowCenter{ cards[static_cast<size_t>(r)].left + cards[static_cast<size_t>(r)].width * 0.5,
                                        cards[static_cast<size_t>(r)].top + cards[static_cast<size_t>(r)].height * 0.5 };
                const auto rowRes = ResolveRingHover(
                    rowCenter, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
                    appSlot, appSlot, origin, cards, bounds, true);
                CHECK(rowRes.selectedSlot == appSlot);
                CHECK(rowRes.selectedSubIndex == r);
                CHECK(rowRes.keepExistingSubmenu);
            }

            // Gliding strictly outside the main ring straight from origin toward TitleColumn center keeps it open.
            const PointD colCenter{ bounds.left + bounds.width * 0.5, bounds.top + bounds.height * 0.5 };
            const double dirX = colCenter.x - origin.x;
            const double dirY = colCenter.y - origin.y;
            const double dirLen = std::hypot(dirX, dirY);
            if (dirLen > 1.0)
            {
                // Step along segment origin -> colCenter until outside mainRingOuterR + 4 and before entering bounds.
                for (double t = 0.1; t < 0.95; t += 0.05)
                {
                    const PointD p{ origin.x + t * dirX, origin.y + t * dirY };
                    if (std::hypot(p.x - kCenter.x, p.y - kCenter.y) > mainRingOuterR + 4.0 && !bounds.Contains(p))
                    {
                        const auto corrRes = ResolveRingHover(
                            p, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
                            appSlot, appSlot, origin, cards, bounds, true);
                        CHECK(corrRes.selectedSlot == appSlot);
                        CHECK(corrRes.keepExistingSubmenu);
                        break;
                    }
                }
            }

            // Moving to ANY other main-ring slot icon or center deadzone immediately collapses TitleColumn.
            for (int other = 0; other < slotCount; ++other)
            {
                if (other == appSlot)
                    continue;
                const PointD otherPt = GetSlotCenter(kCenter.x, kCenter.y, kRingR, other, slotCount);
                const auto res = ResolveRingHover(
                    otherPt, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
                    appSlot, appSlot, origin, cards, bounds, true);
                CHECK(res.selectedSlot == other);
                CHECK(!res.keepExistingSubmenu);
            }

            const auto deadRes = ResolveRingHover(
                kCenter, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
                appSlot, appSlot, origin, cards, bounds, true);
            CHECK(deadRes.selectedSlot == -1);
            CHECK(!deadRes.keepExistingSubmenu);
        }
    }

    // 4. End-to-end multi-step trajectory simulating RingOverlay::UpdatePointer with Downloads + other icons.
    {
        constexpr int slotCount = 6;
        constexpr int downloadsSlot = 1; // -30 deg (2 o'clock)
        constexpr int explorerSlot = 0;  // -90 deg (12 o'clock)
        constexpr int recycleSlot = 2;   // +30 deg (4 o'clock)
        constexpr int collectionSlot = 4; // +150 deg (8 o'clock)

        int selectedSlot = -1;
        int expandedSlot = -1;
        int selectedSub = -1;
        PointD corridorOrigin = kCenter;
        std::vector<RectD> subCards;
        RectD subBounds{};

        auto stepPointer = [&](PointD pt) {
            const auto res = ResolveRingHover(
                pt, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
                selectedSlot, expandedSlot, corridorOrigin, subCards, subBounds, false);
            if (res.selectedSlot != selectedSlot)
            {
                selectedSlot = res.selectedSlot;
                selectedSub = -1;
                if (selectedSlot == downloadsSlot || selectedSlot == collectionSlot)
                {
                    expandedSlot = selectedSlot;
                    corridorOrigin = pt;
                    const int itemCnt = (selectedSlot == downloadsSlot) ? 8 : 5;
                    subCards = ComputeFolderSubmenuRects(
                        kCenter.x, kCenter.y, GetSlotAngle(selectedSlot, slotCount), kRingR, kIconSize, itemCnt);
                    subBounds = ComputeUnionBounds(subCards);
                }
                else
                {
                    expandedSlot = -1;
                    subCards.clear();
                    subBounds = {};
                }
            }
            else
            {
                if (!res.keepExistingSubmenu)
                {
                    expandedSlot = -1;
                    subCards.clear();
                    subBounds = {};
                }
                if (res.updateCorridorOrigin)
                    corridorOrigin = pt;
                selectedSub = res.selectedSubIndex;
            }
        };

        // Enter Downloads just outside deadzone -> Downloads expands!
        const double dlAngle = GetSlotAngle(downloadsSlot, slotCount);
        stepPointer({ kCenter.x + 24.0 * std::cos(dlAngle), kCenter.y + 24.0 * std::sin(dlAngle) });
        CHECK(selectedSlot == downloadsSlot && expandedSlot == downloadsSlot && selectedSub == -1);
        // Verify that the naive AABB of Downloads' 8-item outer arc actually encloses Explorer and center!
        CHECK(subBounds.Contains(GetSlotCenter(kCenter.x, kCenter.y, kRingR, explorerSlot, slotCount)) ||
              IsPointInHoverCorridor(GetSlotCenter(kCenter.x, kCenter.y, kRingR, explorerSlot, slotCount), corridorOrigin, subBounds));

        // Move out to Downloads icon -> origin updates, still expanded.
        const PointD dlIcon = GetSlotCenter(kCenter.x, kCenter.y, kRingR, downloadsSlot, slotCount);
        stepPointer(dlIcon);
        CHECK(selectedSlot == downloadsSlot && expandedSlot == downloadsSlot);
        CHECK(std::abs(corridorOrigin.x - dlIcon.x) < 0.01 && std::abs(corridorOrigin.y - dlIcon.y) < 0.01);

        // Move into Downloads outer-arc card #0, then glide via gap to card #5.
        const PointD card0{ subCards[0].left + subCards[0].width * 0.5, subCards[0].top + subCards[0].height * 0.5 };
        stepPointer(card0);
        CHECK(selectedSlot == downloadsSlot && expandedSlot == downloadsSlot && selectedSub == 0);

        const auto simArc = ComputeIconArcLayout(kCenter.x, kCenter.y, dlAngle, mainRingOuterR, 8);
        const double simGapAngle = 0.5 * (simArc.angles[4] + simArc.angles[5]);
        const PointD card5{ subCards[5].left + subCards[5].width * 0.5, subCards[5].top + subCards[5].height * 0.5 };
        stepPointer({ kCenter.x + simArc.arcRadius * std::cos(simGapAngle), kCenter.y + simArc.arcRadius * std::sin(simGapAngle) });
        CHECK(selectedSlot == downloadsSlot && expandedSlot == downloadsSlot && selectedSub == -1);
        stepPointer(card5);
        CHECK(selectedSlot == downloadsSlot && expandedSlot == downloadsSlot && selectedSub == 5);

        // Now move directly onto Recycle Bin icon -> MUST immediately leave Downloads and select Recycle Bin!
        stepPointer(GetSlotCenter(kCenter.x, kCenter.y, kRingR, recycleSlot, slotCount));
        CHECK(selectedSlot == recycleSlot && expandedSlot == -1 && selectedSub == -1);

        // Re-hover Downloads, then move directly onto Explorer icon -> MUST immediately switch to Explorer!
        stepPointer(dlIcon);
        CHECK(selectedSlot == downloadsSlot && expandedSlot == downloadsSlot);
        stepPointer(GetSlotCenter(kCenter.x, kCenter.y, kRingR, explorerSlot, slotCount));
        CHECK(selectedSlot == explorerSlot && expandedSlot == -1 && selectedSub == -1);

        // Hover Collection (slot 4), expand it, then move directly onto Downloads -> switches cleanly!
        stepPointer(GetSlotCenter(kCenter.x, kCenter.y, kRingR, collectionSlot, slotCount));
        CHECK(selectedSlot == collectionSlot && expandedSlot == collectionSlot && subCards.size() == 5);
        stepPointer(dlIcon);
        CHECK(selectedSlot == downloadsSlot && expandedSlot == downloadsSlot && subCards.size() == 8);

        // Move back to center deadzone -> clears everything!
        stepPointer(kCenter);
        CHECK(selectedSlot == -1 && expandedSlot == -1 && selectedSub == -1);
    }
}

static void TestCrashGuard()
{
    CrashGuardState s{};
    s = AdvanceCrashGuard(s, true, 1000);
    CHECK(!s.disabled && s.abnormalExitCount == 1);
    s = AdvanceCrashGuard(s, true, 2000);
    CHECK(!s.disabled && s.abnormalExitCount == 2);
    s = AdvanceCrashGuard(s, false, 2001);
    CHECK(!s.disabled && s.abnormalExitCount == 2);
    s = AdvanceCrashGuard(s, true, 1000 + kCrashLoopWindow100ns + 5000);
    CHECK(!s.disabled && s.abnormalExitCount == 1);
    s = AdvanceCrashGuard(s, true, 1000 + kCrashLoopWindow100ns + 6000);
    s = AdvanceCrashGuard(s, true, 1000 + kCrashLoopWindow100ns + 7000);
    CHECK(s.disabled && s.abnormalExitCount == 3);
}

static void TestSearchScoring()
{
    CHECK(ScoreSearchMatch(L"Visual Studio Code", L"vis") == 0);
    CHECK(ScoreSearchMatch(L"Visual Studio Code", L"code") == 1);
    CHECK(ScoreSearchMatch(L"PowerShell", L"ps") == 2);
    CHECK(ScoreSearchMatch(L"Visual Studio Code", L"vs") == 2);
    CHECK(ScoreSearchMatch(L"File Explorer", L"fe") == 2);
    CHECK(ScoreSearchMatch(L"Notepad", L"tepa") == 3);
    CHECK(ScoreSearchMatch(L"Notepad", L"xyz") == -1);
    CHECK(ScoreSearchMatch(L"Helium", L"") == -1);

    const std::vector<std::wstring> heliumAliases = { L"chrome", L"Helium", L"Chromium" };
    CHECK(ScoreAliases(heliumAliases, L"hel") == 0);
    CHECK(ScoreAliases(heliumAliases, L"chr") == 0);
    CHECK(ScoreAliases(heliumAliases, L"ium") == 3);
    CHECK(ScoreAliases(heliumAliases, L"nomatch") == -1);
}

static void TestConfigRoundTrip()
{
    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(_countof(tempDir), tempDir);
    const std::wstring testDir = std::wstring(tempDir) + L"WinRingsTest_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(testDir.c_str(), nullptr);

    const std::wstring cfgPath = testDir + L"\\config.json";
    const std::wstring profDir = testDir + L"\\profiles";
    SwitcherConfig orig = SwitcherConfig::CreateDefault();
    CHECK(orig.version == 6);
    CHECK(orig.profiles.size() == 2);
    CHECK(orig.FindProfile(L"default") != nullptr);
    const RingProfileConfig* smenu = orig.FindProfile(L"SMenu");
    CHECK(smenu != nullptr);
    CHECK(!smenu->includeRunningApps);
    CHECK(smenu->pinnedSlots.size() == 6);
    CHECK(smenu->pinnedSlots[0].kind == SlotKind::Collection && smenu->pinnedSlots[0].items.size() == 11);
    CHECK(smenu->pinnedSlots[1].runAsAdmin == true);
    CHECK(smenu->pinnedSlots[2].runAsAdmin == true);
    CHECK(smenu->pinnedSlots[3].kind == SlotKind::Collection && smenu->pinnedSlots[3].items.size() == 3);
    CHECK(smenu->pinnedSlots[3].pathOrUrl.empty());
    CHECK(smenu->pinnedSlots[3].items[0].id == L"smenu-restart");
    CHECK(smenu->pinnedSlots[3].items[1].id == L"smenu-shutdown");
    CHECK(smenu->pinnedSlots[3].items[2].id == L"smenu-switch-user");
    const IconArcLayout bottomPowerArc = ComputeIconArcLayout(340.0, 340.0, kPi * 0.5, 149.0, 3);
    CHECK(bottomPowerArc.rects.size() == 3);
    const double restartCenterX = bottomPowerArc.rects[0].left + bottomPowerArc.rects[0].width * 0.5;
    const double shutCenterX = bottomPowerArc.rects[1].left + bottomPowerArc.rects[1].width * 0.5;
    const double switchCenterX = bottomPowerArc.rects[2].left + bottomPowerArc.rects[2].width * 0.5;
    CHECK(restartCenterX > 340.0 && std::abs(shutCenterX - 340.0) < 0.1 && switchCenterX < 340.0);
    CHECK(smenu->pinnedSlots[4].pathOrUrl == L"ms-settings:");
    CHECK(smenu->pinnedSlots[5].pathOrUrl.find(L"control.exe") != std::wstring::npos);
    CHECK(orig.surfaceTexture == SurfaceTexture::A4SAcrylic);
    CHECK(!orig.useCustomSurfaceColor);
    CHECK(orig.surfaceColor == L"#202020");
    CHECK(orig.glassLevel == 50);
    CHECK(orig.lightDarkMode == 0);
    CHECK(orig.accentColor == L"#0078D4");
    CHECK(orig.triggers.size() == 2);
    CHECK(orig.SaveToFile(cfgPath));
    SwitcherConfig loadedDefault;
    CHECK(SwitcherConfig::LoadFromFile(cfgPath, loadedDefault));
    CHECK(loadedDefault.version == 6);
    CHECK(loadedDefault.profiles.size() == 2);
    CHECK(loadedDefault.surfaceTexture == SurfaceTexture::A4SAcrylic);
    CHECK(loadedDefault.glassLevel == 50);
    CHECK(loadedDefault.lightDarkMode == 0);
    CHECK(loadedDefault.triggers.size() == 2);
    CHECK(loadedDefault.triggers[0].kind == TriggerKind::MouseButton);
    CHECK(loadedDefault.triggers[1].kind == TriggerKind::KeyChord);

    orig.surfaceTexture = SurfaceTexture::PureMatte;
    orig.useCustomSurfaceColor = true;
    orig.surfaceColor = L"#0F3B2E";
    orig.glassLevel = 86;
    orig.lightDarkMode = 15;
    orig.accentColor = L"#FF5522";
    orig.autoLightDark = true;
    orig.autoWindowsAccent = true;
    orig.gestureTapOpenClose = false;
    orig.gestureHoldReleaseSwitch = true;
    orig.triggers = {
        TriggerBinding{ TriggerKind::SoloModifier, MouseTriggerButton::Disabled, 0, VK_LWIN, L"work" },
        TriggerBinding{ TriggerKind::KeyChord, MouseTriggerButton::Disabled, MOD_ALT, VK_TAB, L"default" }
    };
    orig.SyncLegacyTriggerMirrors();
    orig.pinnedSlots.push_back(PinnedSlotConfig{
        L"col-1", SlotKind::Collection, L"Dev", L"", L"", 8,
        { PinnedSlotConfig{ L"site-1", SlotKind::Website, L"Example", L"https://example.com", L"icons\\example.ico", 8, {} } }
    });
    orig.SyncDefaultProfileMirror();
    orig.profiles.push_back(RingProfileConfig{
        L"work", L"Work", false,
        { PinnedSlotConfig{ L"w-cmd", SlotKind::App, L"Cmd Admin", L"cmd.exe", L"", 8, {}, true, L"/k echo hi" } }
    });

    CHECK(orig.SaveToFile(cfgPath));
    SwitcherConfig loaded;
    CHECK(SwitcherConfig::LoadFromFile(cfgPath, loaded));
    CHECK(loaded.surfaceTexture == SurfaceTexture::PureMatte);
    CHECK(loaded.useCustomSurfaceColor == true);
    CHECK(loaded.surfaceColor == L"#0F3B2E");
    CHECK(loaded.glassLevel == 86);
    CHECK(loaded.lightDarkMode == 15);
    CHECK(loaded.accentColor == L"#FF5522");
    CHECK(loaded.autoLightDark == true);
    CHECK(loaded.autoWindowsAccent == true);
    CHECK(loaded.gestureTapOpenClose == false);
    CHECK(loaded.gestureHoldReleaseSwitch == true);
    CHECK(loaded.triggers.size() == 2);
    CHECK(loaded.triggers[0].kind == TriggerKind::SoloModifier);
    CHECK(loaded.triggers[0].vk == VK_LWIN);
    CHECK(loaded.triggers[0].profileId == L"work");
    CHECK(loaded.triggers[1].kind == TriggerKind::KeyChord);
    CHECK(loaded.triggers[1].modifiers == MOD_ALT && loaded.triggers[1].vk == VK_TAB);
    CHECK(loaded.pinnedSlots.size() == 4);
    CHECK(loaded.pinnedSlots[3].kind == SlotKind::Collection);
    CHECK(loaded.pinnedSlots[3].items.size() == 1);
    CHECK(loaded.pinnedSlots[3].items[0].pathOrUrl == L"https://example.com");
    CHECK(loaded.profiles.size() == 3);
    const RingProfileConfig* loadedWork = loaded.FindProfile(L"work");
    CHECK(loadedWork != nullptr && !loadedWork->includeRunningApps);
    CHECK(loadedWork->pinnedSlots.size() == 1 && loadedWork->pinnedSlots[0].runAsAdmin);
    CHECK(loadedWork->pinnedSlots[0].arguments == L"/k echo hi");

    // Migrating v4 config preserves user pins in "default" and adds built-in "SMenu"
    SwitcherConfig v4Mig;
    CHECK(SwitcherConfig::ParseJsonUtf8(
        "{\"version\":4,\"pinnedSlots\":[{\"id\":\"p1\",\"kind\":\"App\",\"label\":\"Notepad\",\"pathOrUrl\":\"notepad.exe\"}]}",
        v4Mig));
    CHECK(v4Mig.profiles.size() == 2);
    CHECK(v4Mig.profiles[0].id == L"default" && v4Mig.profiles[0].pinnedSlots.size() == 1);
    CHECK(v4Mig.profiles[0].pinnedSlots[0].label == L"Notepad");
    CHECK(v4Mig.FindProfile(L"SMenu") != nullptr);

    SwitcherConfig clamped;
    CHECK(SwitcherConfig::ParseJsonUtf8(
        "{\"version\":3,\"theme\":\"DarkWood\",\"glassLevel\":150,\"lightDarkMode\":-25}",
        clamped));
    CHECK(clamped.surfaceTexture == SurfaceTexture::PureMatte);
    CHECK(clamped.glassLevel == 100);
    CHECK(clamped.lightDarkMode == 0);

    SwitcherConfig legacyBlue;
    CHECK(SwitcherConfig::ParseJsonUtf8(
        "{\"version\":3,\"theme\":\"Blue\",\"mouseTrigger\":\"Middle\",\"interceptWinTab\":true}",
        legacyBlue));
    CHECK(legacyBlue.surfaceTexture == SurfaceTexture::TaskbarGlass);
    CHECK(legacyBlue.useCustomSurfaceColor == true);
    CHECK(legacyBlue.surfaceColor == L"#142A52");
    CHECK(legacyBlue.triggers.size() == 3);
    CHECK(legacyBlue.triggers[0].mouseButton == MouseTriggerButton::Middle);
    CHECK(legacyBlue.triggers[2].modifiers == MOD_WIN && legacyBlue.triggers[2].vk == VK_TAB);

    SwitcherConfig legacyAll5;
    CHECK(SwitcherConfig::ParseJsonUtf8(
        "{\"version\":3,\"mouseTrigger\":\"SideForward\",\"hotkeyEnabled\":true,\"hotkeyModifiers\":1,"
        "\"hotkeyVk\":32,\"interceptAltTab\":true,\"interceptWinTab\":true,\"menuKeyEnabled\":true}",
        legacyAll5));
    CHECK(legacyAll5.triggers.size() == 5);
    CHECK(legacyAll5.triggers[0].kind == TriggerKind::MouseButton && legacyAll5.triggers[0].mouseButton == MouseTriggerButton::SideForward);
    CHECK(legacyAll5.triggers[1].kind == TriggerKind::KeyChord && legacyAll5.triggers[1].modifiers == MOD_ALT && legacyAll5.triggers[1].vk == VK_SPACE);
    CHECK(legacyAll5.triggers[2].kind == TriggerKind::KeyChord && legacyAll5.triggers[2].modifiers == MOD_ALT && legacyAll5.triggers[2].vk == VK_TAB);
    CHECK(legacyAll5.triggers[3].kind == TriggerKind::KeyChord && legacyAll5.triggers[3].modifiers == MOD_WIN && legacyAll5.triggers[3].vk == VK_TAB);
    CHECK(legacyAll5.triggers[4].kind == TriggerKind::KeyChord && legacyAll5.triggers[4].modifiers == 0 && legacyAll5.triggers[4].vk == VK_APPS);

    for (const char* legacyJson : {
        "{\"version\":2,\"theme\":\"LiquidGlass\"}",
        "{\"version\":2,\"theme\":\"Glass\"}",
        "{\"version\":2,\"theme\":\"Aura\"}"
    })
    {
        SwitcherConfig legacyCfg;
        CHECK(SwitcherConfig::ParseJsonUtf8(legacyJson, legacyCfg));
        CHECK(legacyCfg.surfaceTexture == SurfaceTexture::A4SAcrylic);
        CHECK(!legacyCfg.useCustomSurfaceColor);
    }

    SwitcherConfig bad;
    CHECK(!SwitcherConfig::ParseJsonUtf8("{ not valid json", bad));

    DeleteFileW((profDir + L"\\default.json").c_str());
    DeleteFileW((profDir + L"\\SMenu.json").c_str());
    DeleteFileW((profDir + L"\\work.json").c_str());
    RemoveDirectoryW(profDir.c_str());
    DeleteFileW((testDir + L"\\search.json").c_str());
    DeleteFileW(cfgPath.c_str());
    RemoveDirectoryW(testDir.c_str());
}

static void TestPerProfileConfigFiles()
{
    CHECK(SwitcherConfig::SanitizeProfileId(L"  My:Work/Prof*?  ") == L"MyWorkProf");
    CHECK(SwitcherConfig::SanitizeProfileId(L"... ") == L"profile");
    CHECK(SwitcherConfig::SanitizeProfileId(L"") == L"profile");
    CHECK(SwitcherConfig::SanitizeProfileId(L"VeryLongProfileIdentifierNameOverTwentyEightChars").size() == 28);

    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(_countof(tempDir), tempDir);
    const std::wstring testDir = std::wstring(tempDir) + L"WinRingsProfTest_" + std::to_wstring(GetCurrentProcessId());
    const std::wstring cfgPath = testDir + L"\\config.json";
    const std::wstring profDir = testDir + L"\\profiles";
    CreateDirectoryW(testDir.c_str(), nullptr);

    SwitcherConfig cfg = SwitcherConfig::CreateDefault();
    cfg.profiles.push_back(RingProfileConfig{
        L"work", L"Work", false,
        { PinnedSlotConfig{ L"w-app", SlotKind::App, L"Notepad", L"notepad.exe", L"", 8, {}, false, L"" } }
    });
    CHECK(cfg.SaveToFile(cfgPath));

    // config.json must stay clean global config (without inline profiles/pinnedSlots bloat)
    HANDLE hCfg = CreateFileW(cfgPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(hCfg != INVALID_HANDLE_VALUE);
    if (hCfg != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER sz{};
        GetFileSizeEx(hCfg, &sz);
        std::string raw(static_cast<size_t>(sz.QuadPart), '\0');
        DWORD rd = 0;
        ReadFile(hCfg, raw.data(), static_cast<DWORD>(raw.size()), &rd, nullptr);
        CloseHandle(hCfg);
        CHECK(raw.find("\"profiles\"") == std::string::npos);
        CHECK(raw.find("\"pinnedSlots\"") == std::string::npos);
    }

    // Standalone profile files must exist and load individually via the exact same schema
    RingProfileConfig defDisk, smenuDisk, workDisk;
    CHECK(RingProfileConfig::LoadFromFile(SwitcherConfig::ProfileFilePath(L"default", cfgPath), defDisk));
    CHECK(RingProfileConfig::LoadFromFile(SwitcherConfig::ProfileFilePath(L"SMenu", cfgPath), smenuDisk));
    CHECK(RingProfileConfig::LoadFromFile(SwitcherConfig::ProfileFilePath(L"work", cfgPath), workDisk));
    CHECK(defDisk.id == L"default" && defDisk.includeRunningApps);
    CHECK(smenuDisk.id == L"SMenu" && smenuDisk.pinnedSlots.size() == 6);
    CHECK(workDisk.id == L"work" && workDisk.pinnedSlots.size() == 1);

    // Dropping a brand-new standalone profile JSON file into profiles/ must be discovered cleanly
    RingProfileConfig gaming;
    gaming.id = L"gaming";
    gaming.name = L"Gaming";
    gaming.includeRunningApps = false;
    gaming.pinnedSlots.push_back(PinnedSlotConfig{ L"g-1", SlotKind::Website, L"Steam", L"https://store.steampowered.com", L"", 8, {} });
    CHECK(gaming.SaveToFile(SwitcherConfig::ProfileFilePath(L"gaming", cfgPath)));

    SwitcherConfig reloaded;
    CHECK(SwitcherConfig::LoadFromFile(cfgPath, reloaded));
    CHECK(reloaded.profiles.size() == 4);
    CHECK(reloaded.profiles[0].id == L"default");
    CHECK(reloaded.profiles[1].id == L"SMenu");
    const RingProfileConfig* loadedGaming = reloaded.FindProfile(L"gaming");
    CHECK(loadedGaming != nullptr && loadedGaming->pinnedSlots.size() == 1);
    CHECK(loadedGaming->pinnedSlots[0].pathOrUrl == L"https://store.steampowered.com");

    // Deleting profile file removes it cleanly on next load
    CHECK(SwitcherConfig::DeleteProfileFile(L"gaming", cfgPath));
    SwitcherConfig afterDel;
    CHECK(SwitcherConfig::LoadFromFile(cfgPath, afterDel));
    CHECK(afterDel.profiles.size() == 3);
    CHECK(afterDel.FindProfile(L"gaming") == nullptr);

    // Old SMenu.json Power order ([switch-user, shutdown, restart] + shutdown parent URL) upgrades automatically on load
    RingProfileConfig legacySMenu = SwitcherConfig::CreateBuiltInSMenuProfile();
    legacySMenu.pinnedSlots[3].pathOrUrl = L"winrings://power/shutdown";
    std::swap(legacySMenu.pinnedSlots[3].items[0], legacySMenu.pinnedSlots[3].items[2]);
    CHECK(legacySMenu.SaveToFile(SwitcherConfig::ProfileFilePath(L"SMenu", cfgPath)));
    SwitcherConfig upgraded;
    CHECK(SwitcherConfig::LoadFromFile(cfgPath, upgraded));
    const RingProfileConfig* upSMenu = upgraded.FindProfile(L"SMenu");
    CHECK(upSMenu != nullptr && upSMenu->pinnedSlots[3].pathOrUrl.empty());
    CHECK(upSMenu->pinnedSlots[3].items[0].id == L"smenu-restart");
    CHECK(upSMenu->pinnedSlots[3].items[2].id == L"smenu-switch-user");

    DeleteFileW((profDir + L"\\default.json").c_str());
    DeleteFileW((profDir + L"\\SMenu.json").c_str());
    DeleteFileW((profDir + L"\\work.json").c_str());
    RemoveDirectoryW(profDir.c_str());
    DeleteFileW((testDir + L"\\search.json").c_str());
    DeleteFileW(cfgPath.c_str());
    RemoveDirectoryW(testDir.c_str());
}

static void TestPointerGlideSafety()
{
    const RectD primaryMon{ 0.0, 0.0, 1920.0, 1080.0 };
    const RectD primaryWork{ 0.0, 0.0, 1920.0, 1032.0 };
    const RectD virtDesktop{ -1920.0, 0.0, 3840.0, 1080.0 };

    // 1. Classic -32000 minimized bug: must NEVER resolve near (-32000,-32000) or (0,0)
    const RectD iconicSentinel{ -32000.0, -32000.0, 160.0, 28.0 };
    const RectD normalPos{ 400.0, 180.0, 900.0, 620.0 };
    const RectD restored = ResolveRestoredWindowRect(
        true, iconicSentinel, true, normalPos, false, primaryMon, primaryWork, primaryWork);
    CHECK(restored.left >= 0.0 && restored.top >= 0.0);
    const PointerGlidePlan restoredPlan = ComputeSafePointerGlideTarget(
        { 100.0, 100.0 }, restored, primaryWork, virtDesktop);
    CHECK(restoredPlan.shouldGlide);
    CHECK(std::abs(restoredPlan.target.x - 850.0) < 0.1);
    CHECK(std::abs(restoredPlan.target.y - 490.0) < 0.1);

    // 2. Restored-to-maximized minimized window -> monitor work area center when cursor is on another monitor, no jump if already inside
    const RectD maxRestored = ResolveRestoredWindowRect(
        true, iconicSentinel, true, normalPos, true, primaryMon, primaryWork, primaryWork);
    CHECK(!ComputeSafePointerGlideTarget({ 50.0, 50.0 }, maxRestored, primaryWork, virtDesktop).shouldGlide);
    const PointerGlidePlan maxPlan = ComputeSafePointerGlideTarget(
        { -300.0, 200.0 }, maxRestored, primaryWork, virtDesktop);
    CHECK(maxPlan.shouldGlide);
    CHECK(std::abs(maxPlan.target.x - 960.0) < 0.1);
    CHECK(std::abs(maxPlan.target.y - 516.0) < 0.1);

    // 3. Top taskbar workspace translation (workArea.top = 48)
    const RectD topBarWork{ 0.0, 48.0, 1920.0, 1032.0 };
    const RectD shifted = ResolveRestoredWindowRect(
        true, iconicSentinel, true, RectD{ 200.0, 100.0, 600.0, 400.0 }, false,
        primaryMon, topBarWork, topBarWork);
    CHECK(std::abs(shifted.left - 200.0) < 0.1 && std::abs(shifted.top - 148.0) < 0.1);

    // 4. Secondary monitor at negative X coordinates stays on secondary monitor
    const RectD leftMonWork{ -1920.0, 0.0, 1920.0, 1040.0 };
    const RectD leftWin{ -1500.0, 200.0, 800.0, 600.0 };
    const PointerGlidePlan leftPlan = ComputeSafePointerGlideTarget(
        { 300.0, 300.0 }, leftWin, leftMonWork, virtDesktop);
    CHECK(leftPlan.shouldGlide);
    CHECK(std::abs(leftPlan.target.x - (-1100.0)) < 0.1);
    CHECK(std::abs(leftPlan.target.y - 500.0) < 0.1);

    // 5. Cursor already inside visible window -> no glide
    const PointerGlidePlan insidePlan = ComputeSafePointerGlideTarget(
        { 700.0, 400.0 }, normalPos, primaryWork, virtDesktop);
    CHECK(!insidePlan.shouldGlide);

    // 6. Degenerate / tiny / offscreen window -> never glides to (0,0)
    CHECK(!ComputeSafePointerGlideTarget({ 500.0, 500.0 }, RectD{ -32000.0, -32000.0, 160.0, 28.0 }, primaryWork, virtDesktop).shouldGlide);
    CHECK(!ComputeSafePointerGlideTarget({ 500.0, 500.0 }, RectD{ 10.0, 10.0, 8.0, 8.0 }, primaryWork, virtDesktop).shouldGlide);
    CHECK(!ComputeSafePointerGlideTarget({ 500.0, 500.0 }, RectD{ 2200.0, 100.0, 500.0, 400.0 }, primaryWork, virtDesktop).shouldGlide);

    // 7. Partially offscreen window clamps glide to visible intersection center
    const RectD partOff{ 1600.0, 700.0, 800.0, 600.0 }; // intersects primaryWork to [1600..1920, 700..1032]
    const PointerGlidePlan partPlan = ComputeSafePointerGlideTarget(
        { 200.0, 200.0 }, partOff, primaryWork, virtDesktop);
    CHECK(partPlan.shouldGlide);
    CHECK(partPlan.target.x >= 1600.0 && partPlan.target.x <= 1912.0);
    CHECK(partPlan.target.y >= 700.0 && partPlan.target.y <= 1024.0);

    // 8. User pointer override check avoids self-canceling SetCursorPos steps
    CHECK(!IsUserPointerOverride({ 500.0, 500.0 }, { 480.0, 490.0 }, { 500.0, 500.0 }, 10.0));
    CHECK(!IsUserPointerOverride({ 483.0, 492.0 }, { 480.0, 490.0 }, { 540.0, 540.0 }, 10.0));
    CHECK(IsUserPointerOverride({ 650.0, 300.0 }, { 480.0, 490.0 }, { 500.0, 500.0 }, 10.0));
}

static void TestThemeMaterialEngine()
{
    // Default A4S native WCA material values must remain bit-calibrated
    SwitcherConfig a4s = SwitcherConfig::CreateDefault();
    const RingMaterialSpec mA4s = ComputeRingMaterial(a4s, 1.0);
    CHECK(mA4s.surfaceTexture == SurfaceTexture::A4SAcrylic);
    CHECK(std::abs(mA4s.tintR - (32.0f / 255.0f)) < 0.002f);
    CHECK(std::abs(mA4s.tintAlpha - (128.0f / 255.0f)) < 0.002f);
    CHECK(std::abs(mA4s.blurSigma - 30.0f) < 0.05f);
    CHECK(std::abs(mA4s.luminosityR - (38.0f / 255.0f)) < 0.002f);
    CHECK(mA4s.luminosityAlpha >= 0.88f);
    CHECK(mA4s.noiseOpacity > 0.008f && mA4s.noiseOpacity <= 0.016f);
    CHECK(mA4s.borderColorRef == RGB(62, 62, 62));
    CHECK(mA4s.ringStrokeWidth <= 0.85f);
    CHECK(mA4s.isLightText);
    CHECK(std::abs(mA4s.hoverRevealColor.r - (52.0f / 255.0f)) < 0.005f);
    CHECK(std::abs(mA4s.hoverRevealColor.g - (99.0f / 255.0f)) < 0.005f);
    CHECK(std::abs(mA4s.hoverRevealColor.b - (141.0f / 255.0f)) < 0.005f);

    SwitcherConfig a4sLight = a4s;
    a4sLight.lightDarkMode = 100;
    const RingMaterialSpec mLight = ComputeRingMaterial(a4sLight, 1.0);
    CHECK(mLight.tintR > 0.90f && mLight.tintG > 0.90f && mLight.tintB > 0.90f);
    CHECK(!mLight.isLightText);
    CHECK(mLight.textColor.r < 0.20f);

    SwitcherConfig customAcc = a4s;
    customAcc.accentColor = L"#FF5522";
    const RingMaterialSpec mAcc = ComputeRingMaterial(customAcc, 1.0);
    CHECK(mAcc.accentColorRef == RGB(0xFF, 0x55, 0x22));
    CHECK(std::abs(mAcc.hoverRevealColor.r - 1.0f) < 0.005f);

    // Texture 2: Genuine Win11 Taskbar Glass (wide blur, higher saturation, distinct from A4S)
    SwitcherConfig taskbar = a4s;
    taskbar.surfaceTexture = SurfaceTexture::TaskbarGlass;
    const RingMaterialSpec mTaskbar = ComputeRingMaterial(taskbar, 1.0);
    CHECK(mTaskbar.surfaceTexture == SurfaceTexture::TaskbarGlass);
    CHECK(mTaskbar.blurSigma > 40.0f);
    CHECK(mTaskbar.saturation > mA4s.saturation);

    // Texture 3: Pure Matte (zero blur, zero noise)
    SwitcherConfig matte = a4s;
    matte.surfaceTexture = SurfaceTexture::PureMatte;
    matte.glassLevel = 86;
    const RingMaterialSpec mMatte = ComputeRingMaterial(matte, 1.0);
    CHECK(mMatte.blurSigma == 0.0f);
    CHECK(mMatte.noiseOpacity == 0.0f);
    CHECK(mMatte.tintAlpha > 0.80f);

    // Any texture + any custom surface color (e.g. Emerald Green & Light Alabaster)
    SwitcherConfig greenGlass = taskbar;
    greenGlass.useCustomSurfaceColor = true;
    greenGlass.surfaceColor = L"#0F3B2E";
    const RingMaterialSpec mGreen = ComputeRingMaterial(greenGlass, 1.0);
    CHECK(mGreen.tintG > mGreen.tintR && mGreen.tintG > mGreen.tintB);
    CHECK(mGreen.isLightText);
    CHECK(mGreen.blurSigma > 40.0f);
    CHECK(std::abs(mGreen.edgeBridgeColor.g - mGreen.tintG) < 0.01f);

    SwitcherConfig lightCustom = matte;
    lightCustom.useCustomSurfaceColor = true;
    lightCustom.surfaceColor = L"#F3F3F3";
    const RingMaterialSpec mLightCustom = ComputeRingMaterial(lightCustom, 1.0);
    CHECK(!mLightCustom.isLightText);
    CHECK(mLightCustom.textColor.r < 0.20f);

    const std::vector<uint8_t> noise = GenerateAcrylicNoiseBgra256();
    CHECK(noise.size() == 256u * 256u * 4u);
    uint8_t minLuma = 255, maxLuma = 0;
    bool premulOk = true;
    for (size_t i = 0; i < noise.size(); i += 4u)
    {
        if (noise[i + 0] > noise[i + 3] || noise[i + 1] > noise[i + 3] || noise[i + 2] > noise[i + 3])
            premulOk = false;
        if (noise[i + 0] < minLuma) minLuma = noise[i + 0];
        if (noise[i + 0] > maxLuma) maxLuma = noise[i + 0];
    }
    CHECK(premulOk);
    CHECK(maxLuma > minLuma + 80);
}

static void TestTriggerModelAndDeltaIpc()
{
    CHECK(NormalizeTriggerVk(VK_RWIN) == VK_LWIN);
    CHECK(NormalizeTriggerVk(VK_LMENU) == VK_MENU);
    CHECK(NormalizeTriggerVk(VK_RMENU) == VK_MENU);
    CHECK(NormalizeTriggerVk(VK_LCONTROL) == VK_CONTROL);
    CHECK(NormalizeTriggerVk(VK_RSHIFT) == VK_SHIFT);
    CHECK(IsModifierVk(VK_RWIN) && IsModifierVk(VK_LMENU) && !IsModifierVk(VK_TAB));

    TriggerBinding soloWin{ TriggerKind::SoloModifier, MouseTriggerButton::Disabled, 0, VK_LWIN, L"default" };
    CHECK(FormatTriggerBinding(soloWin) == L"Win");
    CHECK(AnalyzeTriggerConflict(soloWin).hasWarning);
    CHECK(AnalyzeTriggerConflict(soloWin).summary.find(L"Start Menu") != std::wstring::npos);

    TriggerBinding soloAlt{ TriggerKind::SoloModifier, MouseTriggerButton::Disabled, 0, VK_MENU, L"default" };
    CHECK(FormatTriggerBinding(soloAlt) == L"Alt");
    CHECK(AnalyzeTriggerConflict(soloAlt).hasWarning);

    TriggerBinding altTab{ TriggerKind::KeyChord, MouseTriggerButton::Disabled, MOD_ALT, VK_TAB, L"dev" };
    CHECK(FormatTriggerBinding(altTab) == L"Alt + Tab");
    CHECK(AnalyzeTriggerConflict(altTab).hasWarning);

    TriggerBinding winTab{ TriggerKind::KeyChord, MouseTriggerButton::Disabled, MOD_WIN, VK_TAB, L"default" };
    CHECK(FormatTriggerBinding(winTab) == L"Win + Tab");
    CHECK(AnalyzeTriggerConflict(winTab).hasWarning);

    TriggerBinding rlChord{ TriggerKind::RightLeftChord, MouseTriggerButton::RightLeftChord, 0, 0, L"default" };
    CHECK(FormatTriggerBinding(rlChord) == L"Right + Left Click");
    CHECK(!AnalyzeTriggerConflict(rlChord).hasWarning);

    // Surgical Delta IPC must mutate only its target section while preserving pinnedSlots
    SwitcherConfig cfg = SwitcherConfig::CreateDefault();
    const size_t initialPins = cfg.pinnedSlots.size();

    SwitcherConfig lookSrc = cfg;
    lookSrc.surfaceTexture = SurfaceTexture::TaskbarGlass;
    lookSrc.useCustomSurfaceColor = true;
    lookSrc.surfaceColor = L"#2A1B4E";
    lookSrc.glassLevel = 72;
    lookSrc.ringRadiusDip = 140.0;
    CHECK(ApplyDeltaToConfig(cfg, MakeLookDelta(lookSrc)));
    CHECK(cfg.surfaceTexture == SurfaceTexture::TaskbarGlass);
    CHECK(cfg.useCustomSurfaceColor);
    CHECK(cfg.surfaceColor == L"#2A1B4E");
    CHECK(cfg.glassLevel == 72);
    CHECK(std::abs(cfg.ringRadiusDip - 140.0) < 0.01);
    CHECK(cfg.pinnedSlots.size() == initialPins);

    SwitcherConfig trigSrc = cfg;
    trigSrc.triggers = { soloWin, altTab, rlChord };
    CHECK(ApplyDeltaToConfig(cfg, MakeTriggersDelta(trigSrc)));
    CHECK(cfg.triggers.size() == 3);
    CHECK(cfg.triggers[0].kind == TriggerKind::SoloModifier && cfg.triggers[0].vk == VK_LWIN);
    CHECK(cfg.triggers[1].profileId == L"dev");
    CHECK(cfg.interceptAltTab == true);
    CHECK(cfg.mouseTrigger == MouseTriggerButton::RightLeftChord);

    // Multi-trigger (>3) round-trip via Delta IPC (single non-modifier keys + multiple mouse buttons)
    TriggerBinding keyF1{ TriggerKind::KeyChord, MouseTriggerButton::Disabled, 0, VK_F1, L"default" };
    TriggerBinding keyTilde{ TriggerKind::KeyChord, MouseTriggerButton::Disabled, 0, VK_OEM_3, L"default" };
    TriggerBinding sideBack{ TriggerKind::MouseButton, MouseTriggerButton::SideBack, 0, 0, L"default" };
    TriggerBinding sideFwd{ TriggerKind::MouseButton, MouseTriggerButton::SideForward, 0, 0, L"default" };
    SwitcherConfig sixSrc = cfg;
    sixSrc.triggers = { sideBack, sideFwd, soloWin, altTab, keyF1, keyTilde };
    CHECK(ApplyDeltaToConfig(cfg, MakeTriggersDelta(sixSrc)));
    CHECK(cfg.triggers.size() == 6);
    CHECK(cfg.triggers[0].kind == TriggerKind::MouseButton && cfg.triggers[0].mouseButton == MouseTriggerButton::SideBack);
    CHECK(cfg.triggers[1].kind == TriggerKind::MouseButton && cfg.triggers[1].mouseButton == MouseTriggerButton::SideForward);
    CHECK(cfg.triggers[2].kind == TriggerKind::SoloModifier && cfg.triggers[2].vk == VK_LWIN);
    CHECK(cfg.triggers[3].kind == TriggerKind::KeyChord && cfg.triggers[3].modifiers == MOD_ALT && cfg.triggers[3].vk == VK_TAB);
    CHECK(cfg.triggers[4].kind == TriggerKind::KeyChord && cfg.triggers[4].modifiers == 0 && cfg.triggers[4].vk == VK_F1);
    CHECK(cfg.triggers[5].kind == TriggerKind::KeyChord && cfg.triggers[5].modifiers == 0 && cfg.triggers[5].vk == VK_OEM_3);

    SwitcherConfig gestSrc = cfg;
    gestSrc.gestureTapOpenClose = false;
    gestSrc.gestureHoldReleaseSwitch = true;
    gestSrc.doubleTapLastApp = false;
    gestSrc.pointerFollow = false;
    CHECK(ApplyDeltaToConfig(cfg, MakeGesturesDelta(gestSrc)));
    CHECK(!cfg.gestureTapOpenClose && cfg.gestureHoldReleaseSwitch);
    CHECK(!cfg.doubleTapLastApp && !cfg.pointerFollow);
    CHECK(cfg.pinnedSlots.size() == initialPins);

    const WinRingsDeltaPacket openEdit = MakeOpenProfileForEditDelta(420, 360, L"SMenu");
    CHECK(openEdit.op == DeltaOp::PreviewLiveRing && openEdit.intParam == 2u);
    CHECK(openEdit.anchorScreenX == 420 && openEdit.anchorScreenY == 360);
    CHECK(std::wstring(openEdit.previewProfileId) == L"SMenu");

    // Native Windows (Win + ...) chords must always pass through when Win is held
    for (UINT sysVk : { UINT('I'), UINT('R'), UINT('S'), UINT('D'), UINT('E'), UINT('X'), UINT('L'), UINT('V'),
                        UINT('1'), UINT(VK_LEFT), UINT(VK_RIGHT), UINT(VK_UP), UINT(VK_DOWN),
                        UINT(VK_TAB), UINT(VK_ESCAPE), UINT(VK_RETURN), UINT(VK_SPACE), UINT(VK_OEM_PERIOD), UINT(VK_OEM_MINUS) })
    {
        CHECK(ShouldPassThroughWinSystemChord(true, sysVk));
        CHECK(!ShouldPassThroughWinSystemChord(false, sysVk));
    }
    for (UINT modVk : { UINT(VK_LWIN), UINT(VK_RWIN), UINT(VK_LSHIFT), UINT(VK_RSHIFT), UINT(VK_LCONTROL), UINT(VK_LMENU) })
    {
        CHECK(!ShouldPassThroughWinSystemChord(true, modVk));
    }
    CHECK(ResolveWinKeyHeld(true, false, -1.0, false));
    CHECK(ResolveWinKeyHeld(false, false, -1.0, true));
    CHECK(ResolveWinKeyHeld(false, true, 12.0, false));
    CHECK(!ResolveWinKeyHeld(false, true, 250.0, false));
    CHECK(!ResolveWinKeyHeld(false, false, -1.0, false));
}

static void TestSlotMatchingAndPreviews()
{
    CHECK(IsExplorerExecutablePath(L"C:\\Windows\\explorer.exe"));
    CHECK(IsExplorerExecutablePath(L"C:/Windows/Explorer.EXE"));
    CHECK(IsExplorerExecutablePath(L"explorer.exe"));
    CHECK(!IsExplorerExecutablePath(L"C:\\Windows\\notepad.exe"));
    CHECK(!IsExplorerExecutablePath(L""));

    // Folder matching against Explorer window titles (leaf name, suffix strip, full path, negative cases)
    const std::wstring dlPath = L"C:\\Users\\Administrator\\Downloads";
    CHECK(ExplorerWindowMatchesFolder(L"Downloads - File Explorer", dlPath));
    CHECK(ExplorerWindowMatchesFolder(L"downloads - file explorer", dlPath + L"\\"));
    CHECK(ExplorerWindowMatchesFolder(L"Downloads", dlPath));
    CHECK(ExplorerWindowMatchesFolder(L"C:\\Users\\Administrator\\Downloads - File Explorer", L"C:/Users/Administrator/Downloads/"));
    CHECK(!ExplorerWindowMatchesFolder(L"Documents - File Explorer", dlPath));
    CHECK(!ExplorerWindowMatchesFolder(L"This PC - File Explorer", dlPath));
    CHECK(!ExplorerWindowMatchesFolder(L"Recycle Bin - File Explorer", dlPath));
    CHECK(!ExplorerWindowMatchesFolder(L"C:", L"C:\\"));
    CHECK(ExplorerWindowMatchesFolder(L"C:\\ - File Explorer", L"C:\\"));

    // Recycle Bin matching
    CHECK(ExplorerWindowMatchesRecycleBin(L"Recycle Bin - File Explorer"));
    CHECK(ExplorerWindowMatchesRecycleBin(L"recycle bin"));
    CHECK(ExplorerWindowMatchesRecycleBin(L"Corbeille - File Explorer", L"Corbeille"));
    CHECK(!ExplorerWindowMatchesRecycleBin(L"Downloads - File Explorer"));
    CHECK(!ExplorerWindowMatchesRecycleBin(L""));

    // Window PiP previews: App only; Folder and RecycleBin must never open DWM window previews
    CHECK(ShouldShowSlotWindowPreview(SlotKind::App, 1, true));
    CHECK(!ShouldShowSlotWindowPreview(SlotKind::App, 1, false));
    CHECK(ShouldShowSlotWindowPreview(SlotKind::App, 2, false));
    CHECK(!ShouldShowSlotWindowPreview(SlotKind::Folder, 1, true));
    CHECK(!ShouldShowSlotWindowPreview(SlotKind::Folder, 3, false));
    CHECK(!ShouldShowSlotWindowPreview(SlotKind::RecycleBin, 1, true));
    CHECK(!ShouldShowSlotWindowPreview(SlotKind::RecycleBin, 3, false));

    // Control Panel & Settings matching helpers
    CHECK(ExplorerWindowMatchesControlPanel(L"Control Panel"));
    CHECK(ExplorerWindowMatchesControlPanel(L"All Control Panel Items - File Explorer"));
    CHECK(!ExplorerWindowMatchesControlPanel(L"Downloads - File Explorer"));
    CHECK(IsSystemSettingsExePath(L"C:\\Windows\\ImmersiveControlPanel\\SystemSettings.exe"));
    CHECK(!IsSystemSettingsExePath(L"C:\\Windows\\notepad.exe"));

    // Run as administrator eligibility & custom icon display formatting
    CHECK(CanSlotToggleRunAsAdmin(SlotKind::App, L"C:\\Windows\\System32\\cmd.exe"));
    CHECK(CanSlotToggleRunAsAdmin(SlotKind::Script, L"D:\\tools\\run.ps1"));
    CHECK(CanSlotToggleRunAsAdmin(SlotKind::File, L"C:\\Windows\\System32\\eventvwr.msc"));
    CHECK(!CanSlotToggleRunAsAdmin(SlotKind::App, L"winrings://power/shutdown"));
    CHECK(!CanSlotToggleRunAsAdmin(SlotKind::File, L"ms-settings:about"));
    CHECK(!CanSlotToggleRunAsAdmin(SlotKind::Website, L"https://example.com"));
    CHECK(!CanSlotToggleRunAsAdmin(SlotKind::Folder, L"C:\\Users"));
    CHECK(!CanSlotToggleRunAsAdmin(SlotKind::Collection, L""));
    CHECK(!CanSlotToggleRunAsAdmin(SlotKind::RecycleBin, L""));

    CHECK(FormatCustomIconDisplayName(L"") == L"Default icon");
    CHECK(FormatCustomIconDisplayName(L"builtin:power") == L"Built-in icon");
    CHECK(FormatCustomIconDisplayName(L"C:\\Icons\\handy.png") == L"handy.png");
    CHECK(FormatCustomIconDisplayName(L"icons/example.ico") == L"example.ico");
}

static void TestDragSpringAndReorder()
{
    // Spring-loaded double-heartbeat timing: 500ms dwell + 420ms two-beat pulse + trigger
    const auto s0 = EvaluateSpringHover(0.0);
    CHECK(std::abs(s0.scaleMultiplier - 1.0) < 1e-6 && !s0.pulsing && !s0.shouldActivate);
    const auto s250 = EvaluateSpringHover(250.0);
    CHECK(std::abs(s250.scaleMultiplier - 1.0) < 1e-6 && !s250.pulsing && !s250.shouldActivate);
    const auto s499 = EvaluateSpringHover(499.0);
    CHECK(std::abs(s499.scaleMultiplier - 1.0) < 1e-6 && !s499.pulsing && !s499.shouldActivate);

    const auto b1Peak = EvaluateSpringHover(552.5);
    CHECK(b1Peak.pulsing && !b1Peak.shouldActivate && b1Peak.scaleMultiplier > 1.18);
    const auto b1Dip = EvaluateSpringHover(657.5);
    CHECK(b1Dip.pulsing && !b1Dip.shouldActivate && b1Dip.scaleMultiplier < 0.92);
    const auto b2Peak = EvaluateSpringHover(762.5);
    CHECK(b2Peak.pulsing && !b2Peak.shouldActivate && b2Peak.scaleMultiplier > 1.18);
    const auto b2Dip = EvaluateSpringHover(867.5);
    CHECK(b2Dip.pulsing && !b2Dip.shouldActivate && b2Dip.scaleMultiplier < 0.92);

    const auto trigger = EvaluateSpringHover(920.0);
    CHECK(!trigger.pulsing && trigger.shouldActivate && std::abs(trigger.scaleMultiplier - 1.0) < 1e-6);

    // Reorder target computation (8 slots total, 5 pinned)
    CHECK(ComputeReorderTargetSlotIndex(0.0, -90.0, 5, 8) == 0);   // 12 o'clock -> slot 0
    CHECK(ComputeReorderTargetSlotIndex(90.0, 0.0, 5, 8) == 2);    // 3 o'clock -> slot 2
    CHECK(ComputeReorderTargetSlotIndex(0.0, 90.0, 5, 8) == 4);    // 6 o'clock -> slot 4
    CHECK(ComputeReorderTargetSlotIndex(-90.0, 0.0, 5, 8) == 4);   // 9 o'clock (slot 6) clamped to pinnedCount - 1 (4)
    CHECK(ComputeReorderTargetSlotIndex(90.0, 0.0, 1, 8) == 0);    // <=1 pinned -> 0

    // Unpinned drag-to-pin target index (8 slots total, 5 pinned -> promotes into 0..5)
    CHECK(ComputeUnpinnedDragTargetIndex(0.0, -90.0, 5, 8) == 0);   // 12 o'clock -> slot 0 (before first pinned)
    CHECK(ComputeUnpinnedDragTargetIndex(0.0, 90.0, 5, 8) == 4);    // 6 o'clock -> slot 4
    CHECK(ComputeUnpinnedDragTargetIndex(-90.0, 90.0, 5, 8) == 5);  // 7:30 o'clock (slot 5) -> end of pinned list (5)
    CHECK(ComputeUnpinnedDragTargetIndex(-90.0, 0.0, 5, 8) == 5);   // 9 o'clock (slot 6) -> clamped to pinnedCount (5)
    CHECK(ComputeUnpinnedDragTargetIndex(0.0, 90.0, 0, 8) == 0);    // 0 pinned -> promotes to slot 0

    // Reorder vector element forward, backward, and no-op
    std::vector<int> v1{ 10, 20, 30, 40, 50 };
    ReorderVectorElement(v1, 0, 3);
    CHECK((v1 == std::vector<int>{ 20, 30, 40, 10, 50 }));

    std::vector<int> v2{ 10, 20, 30, 40, 50 };
    ReorderVectorElement(v2, 4, 1);
    CHECK((v2 == std::vector<int>{ 10, 50, 20, 30, 40 }));

    std::vector<int> v3{ 10, 20, 30, 40, 50 };
    ReorderVectorElement(v3, 2, 2);
    CHECK((v3 == std::vector<int>{ 10, 20, 30, 40, 50 }));
}

static void TestHiddenPinsSearchAndReorderMapping()
{
    // 1. Alias generation for portable app, website, and admin command (must NOT index parent directory names)
    const auto handyAliases = BuildBasicSearchAliases(L"Handy", L"D:\\Portable\\Tools\\handy.exe");
    CHECK(ScoreAliases(handyAliases, L"han") == 0);
    CHECK(ScoreAliases(handyAliases, L"handy") == 0);
    CHECK(ScoreAliases(handyAliases, L"portable") == -1);
    CHECK(ScoreAliases(handyAliases, L"tools") == -1);

    const auto siteAliases = BuildBasicSearchAliases(L"GitHub", L"https://www.github.com/org/repo");
    CHECK(ScoreAliases(siteAliases, L"git") == 0);
    CHECK(ScoreAliases(siteAliases, L"github.com") == 0);

    const auto cmdAliases = BuildBasicSearchAliases(L"Admin Shell", L"C:\\Windows\\System32\\cmd.exe");
    CHECK(ScoreAliases(cmdAliases, L"cmd") == 0);
    CHECK(ScoreAliases(cmdAliases, L"as") == 2);

    // Canonical keys normalize slashes, quotes, and trailing backslashes
    CHECK(CanonicalSearchKey(L"\"D:/Portable/Handy/handy.exe\"") == L"d:\\portable\\handy\\handy.exe");
    CHECK(CanonicalSearchKey(L"D:\\Portable\\Folder\\") == L"d:\\portable\\folder");
    CHECK(CanonicalSearchKey(L"HTTPS://Example.COM/app") == L"https://example.com/app");

    // 2. Source-priority tie-breaking & primary-vs-secondary alias scoring
    CHECK(CompareSearchRankKey(SearchRankKey{ 0, 2, 4 }, SearchRankKey{ 1, 0, 10 })); // lower tier always wins
    CHECK(CompareSearchRankKey(SearchRankKey{ 0, 0, 12 }, SearchRankKey{ 0, 1, 5 })); // Ring beats Hidden Pin on same tier
    CHECK(CompareSearchRankKey(SearchRankKey{ 0, 1, 12 }, SearchRankKey{ 0, 2, 5 })); // Hidden Pin beats System on same tier
    CHECK(CompareSearchRankKey(SearchRankKey{ 1, 1, 5 }, SearchRankKey{ 1, 1, 9 }));  // shorter name wins ties

    const std::vector<std::wstring> downloadsPrimary{ L"Downloads" };
    const std::vector<std::wstring> downloadsSecondary{ L"Handy-portable.zip", L"Handy - File Explorer" };
    const std::vector<std::wstring> handyPrimary{ L"Handy", L"handy.exe" };
    CHECK(ScoreSlotAliases(downloadsPrimary, downloadsSecondary, L"ha") == 4);
    CHECK(ScoreSlotAliases(downloadsPrimary, downloadsSecondary, L"han") == 4);
    CHECK(ScoreSlotAliases(handyPrimary, {}, L"ha") == 0);
    CHECK(ScoreSlotAliases(handyPrimary, {}, L"han") == 0);
    CHECK(CompareSearchRankKey(SearchRankKey{ 0, 1, 5 }, SearchRankKey{ 4, 0, 9 })); // Hidden Handy beats visible Downloads secondary match
    CHECK(CompareSearchRankKey(SearchRankKey{ 0, 1, 5 }, SearchRankKey{ 3, 0, 5 })); // Hidden Handy prefix beats visible primary substring
    CHECK(ScoreSlotAliases(downloadsPrimary, downloadsSecondary, L"portable") == 5);
    CHECK(ScoreSlotAliases(downloadsPrimary, downloadsSecondary, L"nomatch") == -1);

    // 3. Reorder index mapping with hidden pins + pointer search selection retention
    const std::vector<bool> hiddenFlags{ false, true, false, true, false }; // visible indices 0->cfg 0, 1->cfg 2, 2->cfg 4
    CHECK(MapVisiblePinnedIndexToConfigIndex(hiddenFlags, 0) == 0);
    CHECK(MapVisiblePinnedIndexToConfigIndex(hiddenFlags, 1) == 2);
    CHECK(MapVisiblePinnedIndexToConfigIndex(hiddenFlags, 2) == 4);
    CHECK(MapVisiblePinnedIndexToConfigIndex(hiddenFlags, 3) == static_cast<size_t>(-1));
    CHECK(MapVisiblePinnedInsertPosToConfigIndex(hiddenFlags, 0) == 0);
    CHECK(MapVisiblePinnedInsertPosToConfigIndex(hiddenFlags, 1) == 2);
    CHECK(MapVisiblePinnedInsertPosToConfigIndex(hiddenFlags, 2) == 4);
    CHECK(MapVisiblePinnedInsertPosToConfigIndex(hiddenFlags, 3) == 5);
    CHECK(ShouldKeepSearchSelectionOnPointer(-1, -1));
    CHECK(!ShouldKeepSearchSelectionOnPointer(2, -1));
    CHECK(!ShouldKeepSearchSelectionOnPointer(-1, 0));

    // 4. Unified hiddenFromRing persistence inside profiles/*.json + one-time legacy search.json migration
    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(_countof(tempDir), tempDir);
    const std::wstring testDir = std::wstring(tempDir) + L"WinRingsSearchTest_" + std::to_wstring(GetCurrentProcessId());
    const std::wstring cfgPath = testDir + L"\\config.json";
    const std::wstring profDir = testDir + L"\\profiles";
    const std::wstring legacySearchPath = testDir + L"\\search.json";
    CreateDirectoryW(testDir.c_str(), nullptr);

    SwitcherConfig cfg = SwitcherConfig::CreateDefault();
    PinnedSlotConfig handyPin{ L"pin-handy", SlotKind::App, L"Handy", L"D:\\Portable\\Handy\\handy.exe", L"C:\\Icons\\handy.png", 8, {}, true, L"--tray", true };
    PinnedSlotConfig hiddenFolder{ L"pin-notes", SlotKind::Folder, L"Notes", L"D:\\Notes", L"", 6, {}, false, L"", true };
    cfg.FindProfile(L"default")->pinnedSlots.push_back(handyPin);
    cfg.FindProfile(L"SMenu")->pinnedSlots.push_back(hiddenFolder);
    cfg.PullFromDefaultProfileMirror();
    CHECK(cfg.SaveToFile(cfgPath));
    CHECK(GetFileAttributesW(legacySearchPath.c_str()) == INVALID_FILE_ATTRIBUTES);

    SwitcherConfig inlineRoundTrip;
    CHECK(SwitcherConfig::ParseJsonUtf8(cfg.ToJsonUtf8(true), inlineRoundTrip));
    CHECK(inlineRoundTrip.FindProfile(L"default")->pinnedSlots.back().hiddenFromRing);

    SwitcherConfig loaded;
    CHECK(SwitcherConfig::LoadFromFile(cfgPath, loaded));
    const RingProfileConfig* defLoaded = loaded.FindProfile(L"default");
    const RingProfileConfig* smenuLoaded = loaded.FindProfile(L"SMenu");
    CHECK(defLoaded != nullptr && defLoaded->pinnedSlots.back().id == L"pin-handy");
    CHECK(defLoaded->pinnedSlots.back().hiddenFromRing == true);
    CHECK(defLoaded->pinnedSlots.back().runAsAdmin == true);
    CHECK(defLoaded->pinnedSlots.back().iconCacheFile == L"C:\\Icons\\handy.png");
    CHECK(smenuLoaded != nullptr && smenuLoaded->pinnedSlots.back().id == L"pin-notes");
    CHECK(smenuLoaded->pinnedSlots.back().hiddenFromRing == true);

    // Legacy search.json migrates automatically into default profile with hiddenFromRing = true
    const std::string legacyJson =
        "{\"version\":1,\"items\":[{\"id\":\"srch-1\",\"kind\":\"Website\",\"label\":\"Docs\",\"pathOrUrl\":\"https://example.com/docs\",\"iconCacheFile\":\"\",\"maxFolderItems\":8,\"items\":[]}]}";
    HANDLE hLegacy = CreateFileW(legacySearchPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(hLegacy != INVALID_HANDLE_VALUE);
    if (hLegacy != INVALID_HANDLE_VALUE)
    {
        DWORD wr = 0;
        WriteFile(hLegacy, legacyJson.data(), static_cast<DWORD>(legacyJson.size()), &wr, nullptr);
        CloseHandle(hLegacy);
    }
    SwitcherConfig migrated;
    CHECK(SwitcherConfig::LoadFromFile(cfgPath, migrated));
    const auto& migratedPins = migrated.FindProfile(L"default")->pinnedSlots;
    CHECK(migratedPins.back().label == L"Docs" && migratedPins.back().hiddenFromRing);

    DeleteFileW((profDir + L"\\default.json").c_str());
    DeleteFileW((profDir + L"\\SMenu.json").c_str());
    RemoveDirectoryW(profDir.c_str());
    DeleteFileW(legacySearchPath.c_str());
    DeleteFileW(cfgPath.c_str());
    RemoveDirectoryW(testDir.c_str());
}

static void TestSettingsStageAndPreviewMath()
{
    CHECK(!ShouldShowLiveRingForTab(0, false, true, false)); // 0 = General never shows live Ring
    CHECK(ShouldShowLiveRingForTab(1, false, true, false));  // 1 = Appearance shows Ring
    CHECK(ShouldShowLiveRingForTab(2, false, true, false));  // 2 = Profiles shows Ring
    CHECK(!ShouldShowLiveRingForTab(1, true, true, false));  // Minimized hides Ring
    CHECK(!ShouldShowLiveRingForTab(2, false, false, false)); // Inactive hides Ring
    CHECK(!ShouldShowLiveRingForTab(1, false, true, true));  // Paused hides Ring

    const POINT c100 = ComputeRingStageCenterPhysical(POINT{ 100, 50 }, 1.0, 488.0f, 16.0f, 784.0f, 508.0f);
    CHECK(c100.x == 736 && c100.y == 312);

    const POINT c150 = ComputeRingStageCenterPhysical(POINT{ 120, 80 }, 1.5, 488.0f, 16.0f, 784.0f, 508.0f);
    CHECK(c150.x == 1074 && c150.y == 473);

    const RectD work{ 0.0, 0.0, 1920.0, 1040.0 };
    const PointD interior = ComputePreviewRingClampedCenter(736.0, 312.0, 149.6, work);
    CHECK(std::abs(interior.x - 736.0) < 0.001 && std::abs(interior.y - 312.0) < 0.001);

    const PointD edge = ComputePreviewRingClampedCenter(20.0, 1030.0, 149.6, work);
    CHECK(std::abs(edge.x - 157.6) < 0.001);
    CHECK(std::abs(edge.y - (1040.0 - 157.6)) < 0.001);

    const double hitDefault = ComputePreviewOverlayHitRadiusDip(149.6, 380.0);
    CHECK(std::abs(hitDefault - 177.6) < 0.001);
    const double hitClampedMax = ComputePreviewOverlayHitRadiusDip(390.0, 380.0);
    CHECK(std::abs(hitClampedMax - 380.0) < 0.001);
    const double hitClampedMin = ComputePreviewOverlayHitRadiusDip(-20.0, 380.0);
    CHECK(std::abs(hitClampedMin - 24.0) < 0.001);

    CHECK(ShouldClipOverlayToActiveRegion(true, false));
    CHECK(!ShouldClipOverlayToActiveRegion(false, false));
    CHECK(!ShouldClipOverlayToActiveRegion(true, true));

    CHECK(!IsRingSlotPressWithinOuterRadius(20.0, 32.0, 149.6));
    CHECK(IsRingSlotPressWithinOuterRadius(96.0, 32.0, 149.6));
    CHECK(IsRingSlotPressWithinOuterRadius(160.0, 32.0, 149.6));
    CHECK(!IsRingSlotPressWithinOuterRadius(165.0, 32.0, 149.6));

    CHECK(std::abs(ResolveSidebarIconHoverAlpha(false, false) - 0.0f) < 0.001f);
    CHECK(std::abs(ResolveSidebarIconHoverAlpha(false, true) - 0.45f) < 0.001f);
    CHECK(std::abs(ResolveSidebarIconHoverAlpha(true, true) - 1.0f) < 0.001f);
    CHECK(std::abs(ResolveSidebarIconHoverAlpha(true, false) - 1.0f) < 0.001f);

    CHECK(ShouldFooterStatusClickTogglePause(true, false));
    CHECK(ShouldFooterStatusClickTogglePause(false, true));
    CHECK(!ShouldFooterStatusClickTogglePause(false, false));

    CHECK(std::abs(ComputeUnifiedProfilesCardBottomDip(184.0f, 0, 28.0f, 4.0f, 512.0f) - 184.0f) < 0.001f);
    CHECK(std::abs(ComputeUnifiedProfilesCardBottomDip(184.0f, 2, 28.0f, 4.0f, 512.0f) - 248.0f) < 0.001f);
    CHECK(std::abs(ComputeUnifiedProfilesCardBottomDip(184.0f, 4, 28.0f, 4.0f, 512.0f) - 304.0f) < 0.001f);
    CHECK(std::abs(ComputeUnifiedProfilesCardBottomDip(184.0f, 20, 28.0f, 4.0f, 512.0f) - 512.0f) < 0.001f);

    CHECK(!ShouldShowProfileDeleteAction(true, false));  // default: Reset only
    CHECK(!ShouldShowProfileDeleteAction(false, true));  // SMenu: Reset only
    CHECK(ShouldShowProfileDeleteAction(false, false));  // custom: Reset + Delete
}

int wmain()
{
    TestRadialMath();
    TestDownloadsAndSubmenuHoverTransitions();
    TestSearchScoring();
    TestCrashGuard();
    TestConfigRoundTrip();
    TestPerProfileConfigFiles();
    TestPointerGlideSafety();
    TestThemeMaterialEngine();
    TestTriggerModelAndDeltaIpc();
    TestSlotMatchingAndPreviews();
    TestDragSpringAndReorder();
    TestHiddenPinsSearchAndReorderMapping();
    TestSettingsStageAndPreviewMath();

    if (g_failures == 0)
    {
        std::printf("WinRingsTests: ALL PASSED\n");
        return 0;
    }
    std::printf("WinRingsTests: %d FAILURE(S)\n", g_failures);
    return 1;
}
