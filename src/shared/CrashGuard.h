#pragma once

#include <Windows.h>
#include <array>
#include <cstdint>

namespace winrings
{
    inline constexpr DWORD kCrashLoopThreshold = 3;
    inline constexpr ULONGLONG kCrashLoopWindow100ns = UINT64_C(180) * UINT64_C(10000000);

    struct CrashGuardState
    {
        std::array<ULONGLONG, kCrashLoopThreshold> abnormalExitTimes{};
        DWORD abnormalExitCount = 0;
        bool disabled = false;
    };

    constexpr CrashGuardState AdvanceCrashGuard(CrashGuardState state,
        bool countAbnormalExit, ULONGLONG now) noexcept
    {
        if (state.disabled)
            return state;

        std::array<ULONGLONG, kCrashLoopThreshold> retained{};
        DWORD retainedCount = 0;
        for (DWORD index = 0; index < state.abnormalExitCount && index < state.abnormalExitTimes.size(); ++index)
        {
            const ULONGLONG timestamp = state.abnormalExitTimes[index];
            if (timestamp && (now < timestamp || now - timestamp <= kCrashLoopWindow100ns))
                retained[retainedCount++] = timestamp;
        }
        state.abnormalExitTimes = retained;
        state.abnormalExitCount = retainedCount;

        if (countAbnormalExit)
        {
            if (state.abnormalExitCount < state.abnormalExitTimes.size())
            {
                state.abnormalExitTimes[state.abnormalExitCount++] = now;
            }
            else
            {
                for (size_t index = 1; index < state.abnormalExitTimes.size(); ++index)
                    state.abnormalExitTimes[index - 1] = state.abnormalExitTimes[index];
                state.abnormalExitTimes.back() = now;
            }
        }
        if (state.abnormalExitCount >= kCrashLoopThreshold)
            state.disabled = true;
        return state;
    }

    inline constexpr CrashGuardState kEmptyCrashGuard{};
    inline constexpr CrashGuardState kCrashOne = AdvanceCrashGuard(kEmptyCrashGuard, true, 100);
    inline constexpr CrashGuardState kCrashTwo = AdvanceCrashGuard(kCrashOne, true, 101);
    inline constexpr CrashGuardState kCrashThree = AdvanceCrashGuard(kCrashTwo, true, 102);
    static_assert(!kCrashTwo.disabled && kCrashThree.disabled,
        "three abnormal Explorer exits inside the guard window must disable injection");

    inline constexpr CrashGuardState kCrashExpired = AdvanceCrashGuard(kCrashTwo, true,
        101 + kCrashLoopWindow100ns + 1);
    static_assert(!kCrashExpired.disabled && kCrashExpired.abnormalExitCount == 1,
        "expired Explorer failures must be pruned before appending the new one");

    inline constexpr CrashGuardState kCrashWithCleanExit = AdvanceCrashGuard(kCrashTwo, false, 101);
    static_assert(AdvanceCrashGuard(kCrashWithCleanExit, true, 102).disabled,
        "a clean maintenance exit must not erase earlier abnormal timestamps");
    static_assert(AdvanceCrashGuard(kCrashOne, false, 99).abnormalExitCount == 1,
        "a backward wall-clock correction must not weaken the crash guard");
} // namespace winrings
