#include "plugin/components/FovUnlocker.hpp"
#include "plugin/Helper.hpp"
#include "util/ExponentialFilter.hpp"
#include "util/win/Loader.hpp"

#include <wil/result.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
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

using Clock = std::chrono::steady_clock;
constexpr auto BURST_FOV_BYPASS_DURATION = std::chrono::milliseconds { 2750 };
constexpr auto TRACE_DURATION = std::chrono::seconds { 5 };
bool wasBurstKeyDown = false;
bool isBurstFovBypass = false;
bool isTracing = false;
Clock::time_point burstFovBypassUntil {};
Clock::time_point traceStart {};
Clock::time_point traceUntil {};
uintptr_t gameModuleBase = 0;
HANDLE traceFile = INVALID_HANDLE_VALUE;

void WriteTraceHeader() noexcept {
    if (traceFile == INVALID_HANDLE_VALUE) {
        return;
    }
    static constexpr char header[] =
        "us,thread,caller_rva,instance,input_fov,output_fov,mode\r\n";
    DWORD written = 0;
    ::WriteFile(
        traceFile,
        header,
        static_cast<DWORD>(sizeof(header) - 1),
        &written,
        nullptr
    );
}

void TraceFov(
    void* instance,
    const float inputFov,
    const float outputFov,
    const char* mode,
    void* returnAddress
) noexcept {
    if (!isTracing || traceFile == INVALID_HANDLE_VALUE) {
        return;
    }

    const auto now = Clock::now();
    if (now >= traceUntil) {
        isTracing = false;
        ::FlushFileBuffers(traceFile);
        return;
    }

    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now - traceStart
    ).count();
    const auto caller = reinterpret_cast<uintptr_t>(returnAddress);
    const auto callerRva =
        (caller >= gameModuleBase) ? (caller - gameModuleBase) : caller;

    char line[256] {};
    const int length = std::snprintf(
        line,
        sizeof(line),
        "%lld,%lu,0x%llX,0x%llX,%.9g,%.9g,%s\r\n",
        static_cast<long long>(us),
        static_cast<unsigned long>(GetCurrentThreadId()),
        static_cast<unsigned long long>(callerRva),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(instance)),
        static_cast<double>(inputFov),
        static_cast<double>(outputFov),
        mode
    );
    if (length <= 0) {
        return;
    }

    DWORD written = 0;
    ::WriteFile(
        traceFile,
        line,
        static_cast<DWORD>((std::min)(length, static_cast<int>(sizeof(line) - 1))),
        &written,
        nullptr
    );
}
} // namespace

namespace z3lx::plugin {
FovUnlocker::FovUnlocker() noexcept = default;

FovUnlocker::~FovUnlocker() noexcept {
    std::lock_guard lock { mutex };
    hook = {};
    if (traceFile != INVALID_HANDLE_VALUE) {
        ::FlushFileBuffers(traceFile);
        ::CloseHandle(traceFile);
        traceFile = INVALID_HANDLE_VALUE;
    }
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
    const auto target = reinterpret_cast<void*>(
        gameModuleBase + offset
    );
    const auto detour = reinterpret_cast<void*>(
        HkSetFieldOfView
    );

    const auto tracePath =
        z3lx::util::GetCurrentModuleFilePath().parent_path() /
        "fov_trace.csv";

    std::lock_guard lock { mutex };
    hook = mmh::Hook<void, void*, float>::Create(target, detour);
    traceFile = ::CreateFileW(
        tracePath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    WriteTraceHeader();
}

void FovUnlocker::Update() {
    const auto& cursor = GetComponent<CursorState>();
    const auto& window = GetComponent<WindowState>();
    const bool shouldHook = window.IsFocused() && !cursor.IsVisible();
    Hook(shouldHook);

    // Diagnostic workaround for issue #75: temporarily let the game own FOV
    // while a burst cinematic is expected to be active.
    const bool burstKeyDown =
        shouldHook && ((GetAsyncKeyState('Q') & 0x8000) != 0);

    std::lock_guard lock { mutex };
    if (burstKeyDown && !wasBurstKeyDown && isEnabled) {
        const auto now = Clock::now();
        isBurstFovBypass = true;
        burstFovBypassUntil = now + BURST_FOV_BYPASS_DURATION;
        isTracing = true;
        traceStart = now;
        traceUntil = now + TRACE_DURATION;

        if (traceFile != INVALID_HANDLE_VALUE) {
            static constexpr char marker[] = "0,0,0x0,0x0,0,0,Q_PRESSED\r\n";
            DWORD written = 0;
            ::WriteFile(
                traceFile,
                marker,
                static_cast<DWORD>(sizeof(marker) - 1),
                &written,
                nullptr
            );
        }
    }
    wasBurstKeyDown = burstKeyDown;
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
    const float inputValue = value;
    void* const returnAddress = _ReturnAddress();

    std::lock_guard lock { mutex };
    if (!hook.IsCreated()) {
        return;
    }

    if (isBurstFovBypass) {
        if (Clock::now() < burstFovBypassUntil) {
            TraceFov(instance, inputValue, value, "BYPASS", returnAddress);
            hook.CallOriginal(instance, value);
            return;
        }

        isBurstFovBypass = false;
        previousInstance = instance;
        previousFov = value;
        setFovCount = 0;
        isPreviousFov = false;
        filter.SetInitialValue(value);
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

    TraceFov(instance, inputValue, value, "NORMAL", returnAddress);
    hook.CallOriginal(instance, value);
} catch (...) {
    // Should never happen
    hook.CallOriginal(instance, value);
}
} // namespace
