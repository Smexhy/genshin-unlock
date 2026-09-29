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

// Experimental camera-call heuristic, not an internal burst-state signal.
// Observe only the existing FOV hook. No keys, timers or new game hooks.
// Learn a specific pair on a single camera; a third caller or camera change
// discards the old pattern rather than treating arbitrary changes as A/B.
void* detectorInstance = nullptr;
uintptr_t callerA = 0;
uintptr_t callerB = 0;
uintptr_t lastCaller = 0;
uintptr_t burstCaller = 0;
unsigned int alternatingTransitions = 0;
bool detectorArmed = false;
bool isBurstFovBypass = false;
constexpr unsigned int ALTERNATIONS_TO_ARM = 8;

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
}

void ResumeUpstreamFov(void* instance, const float gameFov) noexcept {
    previousInstance = instance;
    previousFov = gameFov;
    setFovCount = 0;
    isPreviousFov = false;
    // Do not force the target FOV or alter the configured time constant.
    // The upstream filter below resumes from the current native value.
    filter.SetInitialValue(gameFov);
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


    // Upstream lifecycle takes priority. Inactive/re-enabled states must
    // reach the original smoothing and eventual hook-disable path below;
    // the cinematic pass-through must never short-circuit that path.
    if (!isHooked || !isEnabled || isEnabledOnce || caller == 0) {
        ResetBurstDetector();
    } else if (instance != detectorInstance) {
        ResetBurstDetector(instance, caller);
    } else if (isBurstFovBypass) {
        if (caller == burstCaller) {
            hook.CallOriginal(instance, value);
            return;
        }

        // Stop bypassing on this first changed caller. In particular, the
        // learned partner resumes upstream handling on the SAME call,
        // without an extra exit-confirmation counter or a timed hold.
        // An unknown caller also abandons the heuristic, but is not proof
        // that the burst has visually ended.
        const bool isKnownCaller = caller == callerA || caller == callerB;
        if (isKnownCaller) {
            isBurstFovBypass = false;
            burstCaller = 0;
            lastCaller = caller;
            alternatingTransitions = 0;
            detectorArmed = false;
        } else {
            ResetBurstDetector(instance, caller);
        }
        ResumeUpstreamFov(instance, value);
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
