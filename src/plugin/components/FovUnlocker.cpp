#include "plugin/components/FovUnlocker.hpp"
#include "plugin/Helper.hpp"
#include "util/ExponentialFilter.hpp"
#include "util/win/Loader.hpp"

#include <wil/result.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cwchar>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <thread>

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

void ResumeUpstreamFov(
    void* instance, const float gameFov, const bool instantKnownExit) noexcept {
    previousInstance = instance;
    previousFov = gameFov;
    setFovCount = 0;
    isPreviousFov = false;
    // Diagnostic ONLY: eliminate the fade on a recognised burst exit. The
    // upstream block is eligible on this same call because previousFov above
    // equals the incoming value. Unknown exits keep the previous behaviour.
    // Do not change the time constant used for menus/focus/manual toggles.
    filter.SetInitialValue(instantKnownExit ? static_cast<float>(targetFov) : gameFov);
}

// Diagnostic recorder. Producers are serialized by the existing FOV mutex.
// A bounded SPSC queue moves formatting and disk writes to a worker; no
// extra game hooks, stack walks, memory scans, key polling or network I/O.
struct TimingRecord {
    LONGLONG enterTicks = 0;
    LONGLONG leaveTicks = 0;
    uintptr_t caller = 0;
    uintptr_t instance = 0;
    DWORD thread = 0;
    unsigned int episode = 0;
    float input = 0;
    float output = 0;
    int target = 0;
    unsigned int transitions = 0;
    bool active = false;
    bool enabled = false;
    bool reenabled = false;
    bool bypassBefore = false;
    bool bypassAfter = false;
    bool armed = false;
    const char* event = "sample";
    const char* path = "upstream";
};

class TimingRecorder {
    static constexpr unsigned int CAPACITY = 8192;
    static constexpr std::uint64_t MAX_FILE_BYTES = 64ull * 1024 * 1024;
    std::array<TimingRecord, CAPACITY> records {};
    std::atomic<unsigned int> readIndex { 0 }, writeIndex { 0 };
    std::atomic<std::uint64_t> dropped { 0 };
    std::atomic<bool> accepting { false }, stopping { false };
    std::thread worker;
    HANDLE file = INVALID_HANDLE_VALUE;
    LONGLONG zeroTicks = 0, frequency = 1;
    std::uint64_t bytesWritten = 0;

    bool Write(const char* data, const DWORD length) noexcept {
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD offset = 0;
        while (offset < length) {
            DWORD written = 0;
            if (!::WriteFile(file, data + offset, length - offset, &written,
                             nullptr) || written == 0) return false;
            offset += written;
        }
        bytesWritten += length;
        return true;
    }

    void Run() noexcept {
        unsigned int read = readIndex.load(std::memory_order_relaxed);
        bool healthy = true;
        while (true) {
            const auto available = writeIndex.load(std::memory_order_acquire);
            if (read == available) {
                if (stopping.load(std::memory_order_acquire)) break;
                ::Sleep(50);
                continue;
            }
            // Copy a small batch, then free queue slots BEFORE writing to disk.
            std::array<TimingRecord, 64> batch {};
            unsigned int count = 0;
            while (read != available && count < batch.size()) {
                batch[count++] = records[read];
                read = (read + 1) % CAPACITY;
            }
            readIndex.store(read, std::memory_order_release);
            if (!healthy) continue;

            char buffer[32768] {};
            std::size_t used = 0;
            for (unsigned int i = 0; i < count; ++i) {
                const auto& r = batch[i];
                const auto toUs = [&](const LONGLONG ticks) {
                    return static_cast<long long>(
                        (static_cast<long double>(ticks - zeroTicks) *
                         1000000.0L) / static_cast<long double>(frequency));
                };
                const int n = std::snprintf(buffer + used, sizeof(buffer) - used,
                    "%lld,%lld,%u,%lu,0x%llX,0x%llX,%.9g,%.9g,0x%08X,0x%08X,"
                    "%s,%s,%d,%d,%d,%d,%d,%d,%u,%d,%llu\r\n",
                    toUs(r.enterTicks), toUs(r.leaveTicks), r.episode,
                    static_cast<unsigned long>(r.thread),
                    static_cast<unsigned long long>(r.caller),
                    static_cast<unsigned long long>(r.instance),
                    static_cast<double>(r.input), static_cast<double>(r.output),
                    static_cast<unsigned int>(std::bit_cast<std::uint32_t>(r.input)),
                    static_cast<unsigned int>(std::bit_cast<std::uint32_t>(r.output)),
                    r.event, r.path, static_cast<int>(r.active),
                    static_cast<int>(r.enabled), static_cast<int>(r.reenabled),
                    static_cast<int>(r.bypassBefore), static_cast<int>(r.bypassAfter),
                    static_cast<int>(r.armed), r.transitions, r.target,
                    static_cast<unsigned long long>(dropped.load()));
                if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(buffer) - used) {
                    healthy = false;
                    accepting.store(false, std::memory_order_release);
                    break;
                }
                used += static_cast<std::size_t>(n);
            }
            if (bytesWritten + used >= MAX_FILE_BYTES) {
                static constexpr char cap[] = "# logging_stopped=file_size_limit\r\n";
                Write(cap, static_cast<DWORD>(sizeof(cap) - 1));
                healthy = false;
            } else if (!Write(buffer, static_cast<DWORD>(used))) {
                healthy = false;
            }
            if (!healthy) accepting.store(false, std::memory_order_release);
        }
        // Flush only on the writer thread, never inside the game FOV hook.
        ::FlushFileBuffers(file);
    }

public:
    static LONGLONG NowTicks() noexcept {
        LARGE_INTEGER t {};
        ::QueryPerformanceCounter(&t);
        return t.QuadPart;
    }

    void Start() noexcept try {
        LARGE_INTEGER f {};
        if (!::QueryPerformanceFrequency(&f) || f.QuadPart <= 0) return;
        frequency = f.QuadPart;
        zeroTicks = NowTicks();
        FILETIME utc {};
        ::GetSystemTimePreciseAsFileTime(&utc);
        SYSTEMTIME time {};
        if (!::FileTimeToSystemTime(&utc, &time)) return;
        wchar_t name[128] {};
        const int n = std::swprintf(name, std::size(name),
            L"fov_timing_%04u%02u%02u_%02u%02u%02u_%lu.csv",
            static_cast<unsigned int>(time.wYear),
            static_cast<unsigned int>(time.wMonth),
            static_cast<unsigned int>(time.wDay),
            static_cast<unsigned int>(time.wHour),
            static_cast<unsigned int>(time.wMinute),
            static_cast<unsigned int>(time.wSecond),
            static_cast<unsigned long>(::GetCurrentProcessId()));
        if (n <= 0) return;
        const auto path = z3lx::util::GetCurrentModuleFilePath().parent_path() / name;
        file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        const auto utc100ns = (static_cast<std::uint64_t>(utc.dwHighDateTime) << 32) |
                             utc.dwLowDateTime;
        char header[1024] {};
        const int h = std::snprintf(header, sizeof(header),
            "# build=issue75-instant-timing-v1\r\n"
            "# qpc_frequency=%lld,qpc_zero=%lld,utc_filetime_100ns_at_start=%llu\r\n"
            "# caller_and_instance_are_session_addresses;times_are_not_present_times\r\n"
            "us,done_us,episode,thread,caller,instance,input_fov,output_fov,"
            "input_bits,output_bits,event,path,active,enabled,reenabled,"
            "bypass_before,bypass_after,armed,alternations,target_fov,dropped_total\r\n",
            static_cast<long long>(frequency), static_cast<long long>(zeroTicks),
            static_cast<unsigned long long>(utc100ns));
        if (h <= 0 || h >= static_cast<int>(sizeof(header)) ||
            !Write(header, static_cast<DWORD>(h))) {
            ::CloseHandle(file); file = INVALID_HANDLE_VALUE; return;
        }
        stopping.store(false);
        worker = std::thread([this] { Run(); });
        accepting.store(true, std::memory_order_release);
    } catch (...) {
        accepting.store(false);
        if (file != INVALID_HANDLE_VALUE) {
            ::CloseHandle(file); file = INVALID_HANDLE_VALUE;
        }
        // Diagnostics must not stop the plugin if log creation fails.
    }

    // Called only while the existing FOV mutex is held. Never waits for disk.
    void Push(const TimingRecord& r) noexcept {
        if (!accepting.load(std::memory_order_acquire)) return;
        const auto write = writeIndex.load(std::memory_order_relaxed);
        const auto next = (write + 1) % CAPACITY;
        if (next == readIndex.load(std::memory_order_acquire)) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        records[write] = r;
        writeIndex.store(next, std::memory_order_release);
    }

    void Stop() noexcept {
        accepting.store(false, std::memory_order_release);
        stopping.store(true, std::memory_order_release);
        if (worker.joinable()) worker.join();
        if (file != INVALID_HANDLE_VALUE) {
            ::CloseHandle(file); file = INVALID_HANDLE_VALUE;
        }
    }
    ~TimingRecorder() noexcept { Stop(); }
};
TimingRecorder timingRecorder {};
unsigned int timingEpisode = 0;

// Constructed after the existing lock, so destruction/queue submission also
// happens under that lock. Output is the value supplied to CallOriginal, not
// a claim about the FOV eventually displayed by the renderer.
struct TimingScope {
    float& output;
    TimingRecord record;
    TimingScope(void* instance, const uintptr_t caller, float& value) noexcept
        : output(value) {
        record.enterTicks = TimingRecorder::NowTicks();
        record.caller = caller;
        record.instance = reinterpret_cast<uintptr_t>(instance);
        record.thread = ::GetCurrentThreadId();
        record.input = value;
        record.active = isHooked;
        record.enabled = isEnabled;
        record.reenabled = isEnabledOnce;
        record.bypassBefore = isBurstFovBypass;
    }
    ~TimingScope() noexcept {
        record.leaveTicks = TimingRecorder::NowTicks();
        record.output = output;
        record.target = targetFov;
        record.episode = timingEpisode;
        record.bypassAfter = isBurstFovBypass;
        record.armed = detectorArmed;
        record.transitions = alternatingTransitions;
        timingRecorder.Push(record);
    }
};
} // namespace

namespace z3lx::plugin {
FovUnlocker::FovUnlocker() noexcept = default;

FovUnlocker::~FovUnlocker() noexcept {
    {
        std::lock_guard lock { mutex };
        hook = {};
    }
    timingRecorder.Stop();
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
    // The hook is not enabled yet. This creates a local diagnostic log only.
    timingRecorder.Start();
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
    TimingScope trace { instance, caller, value };

    // Upstream lifecycle takes priority. Inactive/re-enabled states must
    // reach the original smoothing and eventual hook-disable path below;
    // the cinematic pass-through must never short-circuit that path.
    if (!isHooked || !isEnabled || isEnabledOnce || caller == 0) {
        trace.record.event = isBurstFovBypass ? "abort_inactive" : "inactive";
        ResetBurstDetector();
    } else if (instance != detectorInstance) {
        trace.record.event = isBurstFovBypass ? "abort_camera" : "camera_reset";
        ResetBurstDetector(instance, caller);
    } else if (isBurstFovBypass) {
        if (caller == burstCaller) {
            trace.record.event = "native_bypass";
            trace.record.path = "native_bypass";
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
        trace.record.event = isKnownCaller ? "exit_known_instant" : "exit_unknown_upstream";
        ResumeUpstreamFov(instance, value, isKnownCaller);
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
        ++timingEpisode;
        trace.record.event = "enter_bypass";
        trace.record.path = "native_bypass";
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
        trace.record.path = "upstream_eligible";
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
            trace.record.path = (isHooked && isEnabled) ? "override" : "restore";
            isPreviousFov = std::abs(previousFov - filtered) < 0.1f;
            value = filtered;
        } else if (!isHooked) {
            trace.record.path = "disable_hook";
            isPreviousFov = false;
            hook.Enable(false);
        }
    } else {
        trace.record.path = "marker";
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
