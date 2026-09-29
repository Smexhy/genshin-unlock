#include "plugin/components/FovUnlocker.hpp"
#include "plugin/Helper.hpp"
#include "util/ExponentialFilter.hpp"

#include <wil/result.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <intrin.h>

#include <Windows.h>

import mmh;

namespace {
constexpr uintptr_t OFFSET_OS = 0x1454150;
constexpr uintptr_t OFFSET_CN = 0x1455150;

void HkSetFieldOfView(void* instance, float value) noexcept;

std::mutex mutex {};
mmh::Hook<void, void*, float> hook {};
z3lx::util::ExponentialFilter<float> filter {};

bool isHooked = false;
bool isEnabled = false;
bool isEnabledOnce = false;
int targetFov = 45;

int setFovCount = 0;
void* previousInstance = nullptr;
float previousFov = 45.0f;
bool isPreviousFov = false;

// Burst/cinematic detector.
//
// In normal gameplay, the FOV setter is called from two call sites that
// alternate every frame (A, B, A, B...). During the affected burst
// cinematic, the secondary call disappears and the remaining call site
// repeats (A, A, A...). When B returns, normal gameplay has resumed.
//
// Learn the pattern dynamically instead of hard-coding caller addresses so
// the detector is not tied to the RVAs observed in a single game build.
uintptr_t gameModuleBase = 0;
uintptr_t lastCallerRva = 0;
uintptr_t burstCallerRva = 0;
uintptr_t burstExitLastCallerRva = 0;
unsigned int alternatingTransitions = 0;
unsigned int burstExitTransitions = 0;
bool detectorArmed = false;
bool isBurstFovBypass = false;

constexpr unsigned int ALTERNATIONS_TO_ARM = 8;
constexpr unsigned int ALTERNATIONS_TO_CONFIRM_BURST_EXIT = 4;

uintptr_t GetCallerRva(void* returnAddress) noexcept {
    const auto caller = reinterpret_cast<uintptr_t>(returnAddress);
    return (caller >= gameModuleBase) ? (caller - gameModuleBase) : caller;
}

void ResetFovStateForGameplay(void* instance, float gameFov) noexcept {
    previousInstance = instance;
    previousFov = gameFov;
    setFovCount = 0;
    isPreviousFov = false;

    // Resume from the game's current FOV and let the existing configured
    // smoothing handle the return to the user's gameplay FOV.
    filter.SetInitialValue(gameFov);
}

void ResetBurstDetector(const uintptr_t callerRva = 0) noexcept {
    lastCallerRva = callerRva;
    burstCallerRva = 0;
    burstExitLastCallerRva = 0;
    alternatingTransitions = 0;
    burstExitTransitions = 0;
    detectorArmed = false;
    isBurstFovBypass = false;
}
} // namespace

namespace z3lx::plugin {
FovUnlocker::FovUnlocker() noexcept = default;

FovUnlocker::~FovUnlocker() noexcept {
    std::lock_guard lock { mutex };
    hook = {};
}

void FovUnlocker::Start() {
    const auto [module, region] = GetGameModuleContext();
    const uintptr_t offset = [region] {
        switch (region) {
        case GameRegion::OS: return OFFSET_OS;
        case GameRegion::CN: return OFFSET_CN;
        default: THROW_WIN32(ERROR_NOT_SUPPORTED);
        }
    }();

    gameModuleBase = reinterpret_cast<uintptr_t>(module);
    const auto target = reinterpret_cast<void*>(gameModuleBase + offset);
    const auto detour = reinterpret_cast<void*>(HkSetFieldOfView);

    std::lock_guard lock { mutex };
    hook = mmh::Hook<void, void*, float>::Create(target, detour);
}

void FovUnlocker::Update() {
    const auto& cursor = GetComponent<CursorState>();
    const auto& window = GetComponent<WindowState>();
    Hook(window.IsFocused() && !cursor.IsVisible());
}

bool FovUnlocker::IsHooked() const noexcept {
    return isHooked;
}

void FovUnlocker::Hook(const bool hook) {
    if (hook == isHooked) {
        return;
    }
    std::lock_guard lock { mutex };
    if (hook) {
        ::hook.Enable(true);
        isEnabledOnce = true;
    } else {
        // ::hook.Enable(false);
    }
    isHooked = hook;
}

bool FovUnlocker::IsEnabled() const noexcept {
    return isEnabled;
}

void FovUnlocker::Enable(const bool enable) noexcept {
    isEnabled = enable;
}

int FovUnlocker::GetTargetFov() const noexcept {
    return targetFov;
}

void FovUnlocker::SetTargetFov(const int targetFov) noexcept {
    ::targetFov = targetFov;
}

float FovUnlocker::GetSmoothing() const noexcept {
    return filter.GetTimeConstant();
}

void FovUnlocker::SetSmoothing(const float smoothing) noexcept {
    filter.SetTimeConstant(smoothing);
}
} // namespace z3lx::plugin

namespace {
void HkSetFieldOfView(void* instance, float value) noexcept try {
    void* const returnAddress = _ReturnAddress();
    const uintptr_t callerRva = GetCallerRva(returnAddress);

    std::lock_guard lock { mutex };
    if (!hook.IsCreated()) {
        return;
    }

    // Cursor/menu/dialog transitions intentionally disable the FOV hook at the
    // component level. Do not let the cinematic detector interfere with the
    // original smoothing behavior in those states.
    if (!isHooked || !isEnabled) {
        ResetBurstDetector(callerRva);
    } else if (isBurstFovBypass) {
        // Stay on native FOV until the normal alternating caller sequence has
        // genuinely returned. One different caller can arrive slightly before
        // the cinematic is visually finished, so require several alternating
        // transitions rather than ending the bypass on the first one.
        if (burstExitTransitions == 0) {
            if (callerRva == burstCallerRva) {
                hook.CallOriginal(instance, value);
                return;
            }

            burstExitLastCallerRva = callerRva;
            burstExitTransitions = 1;
            hook.CallOriginal(instance, value);
            return;
        }

        if (callerRva != burstExitLastCallerRva) {
            burstExitLastCallerRva = callerRva;
            ++burstExitTransitions;
        } else {
            // The candidate normal sequence broke again. Treat it as still
            // cinematic and wait for a fresh alternating sequence.
            burstExitTransitions = 0;
            burstExitLastCallerRva = 0;
            hook.CallOriginal(instance, value);
            return;
        }

        if (burstExitTransitions < ALTERNATIONS_TO_CONFIRM_BURST_EXIT) {
            hook.CallOriginal(instance, value);
            return;
        }

        isBurstFovBypass = false;
        burstCallerRva = 0;
        burstExitLastCallerRva = 0;
        burstExitTransitions = 0;
        alternatingTransitions = 1;
        lastCallerRva = callerRva;
        ResetFovStateForGameplay(instance, value);
    } else if (lastCallerRva == 0) {
        lastCallerRva = callerRva;
    } else if (callerRva != lastCallerRva) {
        lastCallerRva = callerRva;
        if (alternatingTransitions < ALTERNATIONS_TO_ARM) {
            ++alternatingTransitions;
        }
        if (alternatingTransitions >= ALTERNATIONS_TO_ARM) {
            detectorArmed = true;
        }
    } else {
        // Two consecutive calls from the same site means the normal A/B
        // alternation has broken. In the captured Vesna trace this occurs
        // during the burst cinematic.
        if (detectorArmed) {
            isBurstFovBypass = true;
            burstCallerRva = callerRva;
            burstExitLastCallerRva = 0;
            burstExitTransitions = 0;
            alternatingTransitions = 0;
            hook.CallOriginal(instance, value);
            return;
        }

        alternatingTransitions = 0;
    }

    ++setFovCount;
    if (const bool isDefaultFov = value == 45.0f;
        instance == previousInstance &&
        (value == previousFov || isDefaultFov)) {
        if (isDefaultFov) {
            previousInstance = instance;
            previousFov = value;
        }

        if (setFovCount > 8) {
            filter.SetInitialValue(value);
        }
        setFovCount = 0;

        if (isEnabledOnce) {
            isEnabledOnce = false;
            filter.Update(value);
        }
        const float target = (isHooked && isEnabled) ?
            static_cast<float>(targetFov) : previousFov;
        const float filtered = filter.Update(target);

        if ((isHooked && isEnabled) || !isPreviousFov) {
            isPreviousFov = std::abs(previousFov - filtered) < 0.1f;
            value = filtered;
        } else if (!isHooked) {
            isPreviousFov = false;
            hook.Enable(false);
        }
    } else {
        const auto rep = std::bit_cast<std::uint32_t>(value);
        value = std::bit_cast<float>(rep + 1); // marker value
        previousInstance = instance;
        previousFov = value;
    }

    hook.CallOriginal(instance, value);
} catch (...) {
    // Should never happen
    hook.CallOriginal(instance, value);
}
} // namespace
