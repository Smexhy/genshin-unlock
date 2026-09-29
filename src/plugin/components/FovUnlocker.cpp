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
unsigned int alternatingTransitions = 0;
bool detectorArmed = false;
bool isBurstFovBypass = false;

constexpr unsigned int ALTERNATIONS_TO_ARM = 8;

uintptr_t GetCallerRva(void* returnAddress) noexcept {
    const auto caller = reinterpret_cast<uintptr_t>(returnAddress);
    return (caller >= gameModuleBase) ? (caller - gameModuleBase) : caller;
}

void ResetFovStateForGameplay(void* instance, float gameFov) noexcept {
    previousInstance = instance;
    previousFov = gameFov;
    setFovCount = 0;
    isPreviousFov = false;

    // The cinematic is already over here. Resume the configured gameplay FOV
    // immediately instead of spending another second smoothing from 45.
    filter.SetInitialValue(
        (isHooked && isEnabled) ? static_cast<float>(targetFov) : gameFov
    );
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

    // If we are already inside a detected cinematic, leave its FOV untouched.
    // The first different caller marks the return of the normal A/B sequence.
    if (isBurstFovBypass) {
        if (callerRva == burstCallerRva) {
            hook.CallOriginal(instance, value);
            return;
        }

        isBurstFovBypass = false;
        burstCallerRva = 0;
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
        // only during the burst cinematic.
        if (detectorArmed && isHooked && isEnabled) {
            isBurstFovBypass = true;
            burstCallerRva = callerRva;
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
