#include "plugin/components/FovUnlocker.hpp"
#include "plugin/Helper.hpp"
#include "util/ExponentialFilter.hpp"

#include <wil/result.h>

#include <algorithm>
#include <chrono>
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

// Experimental fixed-timing alternative. Reuse the existing caller-pair
// detector for START only; no HUD, stack walking, character lookup or logging.
// A 1500 ms cap starts the return about 500 ms before the observed ~2 s
// Vesna handover. This is a prediction, NOT a universal burst-end signal.
using BurstClock = std::chrono::steady_clock;
constexpr auto BURST_NATIVE_LIMIT = std::chrono::milliseconds { 1500 };
constexpr float BURST_RETURN_SECONDS = 0.200f;
constexpr unsigned int ALTERNATIONS_TO_ARM = 8;

void* detectorInstance = nullptr;
uintptr_t callerA = 0;
uintptr_t callerB = 0;
uintptr_t lastCaller = 0;
uintptr_t burstCaller = 0;
unsigned int alternatingTransitions = 0;
bool detectorArmed = false;
// Remains true through the early return until the partner has returned.
// Otherwise the still-repeating caller could re-trigger native FOV.
bool isBurstFovBypass = false;
bool burstReturnStarted = false;
bool burstPartnerReturned = false;
BurstClock::time_point burstStart {};
BurstClock::time_point burstReturnStart {};
float burstReturnFrom = 45.0f;

void ResetBurstDetector(
    void* instance = nullptr, const uintptr_t caller = 0) noexcept {
    detectorInstance = instance;
    callerA = caller;
    callerB = 0;
    lastCaller = caller;
    burstCaller = 0;
    alternatingTransitions = 0;
    detectorArmed = false;
    isBurstFovBypass = false;
    burstReturnStarted = false;
    burstPartnerReturned = false;
}

void TrackBurstFov(
    void* instance, const float nativeFov, const float outputFov) noexcept {
    // Keep upstream restoration in sync with what we just sent. Its normal
    // time constant and hook-disable logic are not changed by the burst ramp.
    previousInstance = instance;
    previousFov = nativeFov;
    setFovCount = 0;
    isPreviousFov = false;
    filter.SetInitialValue(outputFov);
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
    const auto target = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(module) + offset
    );
    const auto detour = reinterpret_cast<void*>(
        HkSetFieldOfView
    );

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
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    std::lock_guard lock { mutex };
    if (!hook.IsCreated()) {
        return;
    }


    // Original cursor/focus/toggle lifecycle always takes priority. Never
    // keep an early-return override running in a menu or after re-enabling.
    if (!isHooked || !isEnabled || isEnabledOnce || caller == 0) {
        ResetBurstDetector();
    } else if (instance != detectorInstance) {
        ResetBurstDetector(instance, caller);
    } else if (isBurstFovBypass) {
        if (caller != callerA && caller != callerB) {
            // Unexpected path: stop this experiment and use upstream handling.
            ResetBurstDetector(instance, caller);
        } else {
            const auto now = BurstClock::now();
            if (caller != burstCaller) {
                burstPartnerReturned = true;
            }
            if (!burstReturnStarted &&
                (burstPartnerReturned || now - burstStart >= BURST_NATIVE_LIMIT)) {
                burstReturnStarted = true;
                burstReturnStart = now;
                burstReturnFrom = value;
            }

            const float nativeFov = value;
            bool returnFinished = false;
            if (burstReturnStarted) {
                const float elapsed = std::chrono::duration<float>(
                    now - burstReturnStart).count();
                const float t = std::clamp(
                    elapsed / BURST_RETURN_SECONDS, 0.0f, 1.0f);
                const float blend = t * t * (3.0f - 2.0f * t);
                const float target = static_cast<float>(targetFov);
                value = (t >= 1.0f) ? target :
                    burstReturnFrom + (target - burstReturnFrom) * blend;
                returnFinished = t >= 1.0f;
            }
            TrackBurstFov(instance, nativeFov, value);
            if (returnFinished && burstPartnerReturned) {
                ResetBurstDetector(instance, caller);
            }
            hook.CallOriginal(instance, value);
            return;
        }
    } else if (lastCaller == 0) {
        ResetBurstDetector(instance, caller);
    } else if (caller != lastCaller) {
        if (callerB == 0) {
            callerB = caller;
        }
        if (caller != callerA && caller != callerB) {
            ResetBurstDetector(instance, caller);
        } else {
            lastCaller = caller;
            if (alternatingTransitions < ALTERNATIONS_TO_ARM) {
                ++alternatingTransitions;
            }
            detectorArmed = alternatingTransitions >= ALTERNATIONS_TO_ARM;
        }
    } else if (detectorArmed) {
        isBurstFovBypass = true;
        burstCaller = caller;
        detectorArmed = false;
        alternatingTransitions = 0;
        burstStart = BurstClock::now();
        burstReturnStarted = false;
        burstPartnerReturned = false;
        TrackBurstFov(instance, value, value);
        hook.CallOriginal(instance, value);
        return;
    } else {
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
