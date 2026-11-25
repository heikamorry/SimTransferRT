#include "v2/ProducerWait.h"
#include "core/SamplingClock.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if defined(_M_X64) || defined(_M_IX86)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace rt::v2 {
ProducerStop::ProducerStop() {
#ifdef _WIN32
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
#endif
}

ProducerStop::~ProducerStop() {
#ifdef _WIN32
    if (event_)
        CloseHandle(event_);
#endif
}

void ProducerStop::request() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_.store(true, std::memory_order_release);
#ifdef _WIN32
        if (event_)
            SetEvent(event_);
#endif
    }
    changed_.notify_all();
}

void ProducerStop::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
#ifdef _WIN32
    if (event_ && !ResetEvent(event_)) {
        CloseHandle(event_);
        event_ = nullptr; // Subsequent sessions can still use the stop-aware condition variable.
    }
#endif
    stopped_.store(false, std::memory_order_release);
}

bool ProducerStop::waitUntil(std::uint64_t targetUs) {
    std::unique_lock<std::mutex> lock(mutex_);
    // Convert the remaining interval into the steady_clock domain. On Windows
    // the slot's QPC epoch need not equal steady_clock's epoch.
    const auto base = std::chrono::steady_clock::now();
    const auto now = rt::monotonicUs();
    if (now >= targetUs)
        return !stopped_.load(std::memory_order_acquire);
    return !changed_.wait_until(lock, base + std::chrono::microseconds(targetUs - now), [this] {
        return stopped_.load(std::memory_order_acquire);
    });
}

ProducerWait::ProducerWait(ProducerStop &stop, ProducerWaitStats &stats, unsigned hz,
                           [[maybe_unused]] Timer timer)
    : stop_(stop), stats_(stats) {
    if (hz < 1 || hz > 1000)
        throw std::invalid_argument("V2 source rate must be 1..1000 Hz");
    // At 50/60 Hz the cap is 500 us. Even at 1 kHz the intended spin window
    // occupies at most one eighth of a period. Begin with the previous 80 us lead.
    stats_.leadCapUs = std::min<std::uint64_t>(500, 1000000u / hz / 8);
    stats_.backend = "condition_variable";
#ifdef _WIN32
    if (timer == Timer::Automatic && !stop_.event_)
        ++stats_.fallbacks;
    if (stop_.event_ && timer == Timer::Automatic) {
        timer_ =
            CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (timer_)
            stats_.backend = "windows_high_resolution";
        else {
            timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
            if (timer_)
                stats_.backend = "windows_waitable";
            ++stats_.fallbacks;
        }
    }
#endif
}

ProducerWait::~ProducerWait() {
#ifdef _WIN32
    if (timer_)
        CloseHandle(timer_);
#endif
}

bool ProducerWait::sleepUntil(std::uint64_t targetUs) {
#ifdef _WIN32
    if (timer_) {
        const auto now = rt::monotonicUs();
        if (now >= targetUs)
            return !stop_.flag().load(std::memory_order_acquire);
        LARGE_INTEGER relative{};
        relative.QuadPart = -static_cast<LONGLONG>((targetUs - now) * 10);
        if (SetWaitableTimer(timer_, &relative, 0, nullptr, nullptr, FALSE)) {
            HANDLE handles[]{stop_.event_, timer_};
            const auto result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (result == WAIT_OBJECT_0)
                return false;
            if (result == WAIT_OBJECT_0 + 1)
                return true;
        }
        CloseHandle(timer_);
        timer_ = nullptr;
        ++stats_.fallbacks;
        stats_.backend = "condition_variable";
    }
#endif
    return stop_.waitUntil(targetUs);
}

bool ProducerWait::until(std::uint64_t slotUs, std::uint64_t &sampledUs) {
    if (stop_.flag().load(std::memory_order_acquire)) {
        ++stats_.cancelled;
        return false;
    }
    auto now = rt::monotonicUs();
    const auto target = slotUs > stats_.leadUs ? slotUs - stats_.leadUs : 0;
    if (now < target) {
        ++stats_.timedWaitSlots;
        if (!sleepUntil(target)) {
            ++stats_.cancelled;
            return false;
        }
        now = rt::monotonicUs();
        // An early OS return must not turn the entire remaining period into a spin.
        while (now < target) {
            ++stats_.earlyReturns;
            if (!stop_.waitUntil(target)) {
                ++stats_.cancelled;
                return false;
            }
            now = rt::monotonicUs();
        }
        const auto overshoot = now - target;
        if (overshoot > stats_.overshootMaxUs || !stats_.worstTargetUs) {
            stats_.overshootMaxUs = overshoot;
            stats_.worstSlotUs = slotUs;
            stats_.worstTargetUs = target;
            stats_.worstReturnUs = now;
        }
        if (overshoot >= 1000)
            ++stats_.overshootGe1ms;
        if (now > slotUs)
            ++stats_.returnsPastSlot;
        // Absorb observed wake lateness promptly; decay the lead over subsequent
        // returns. A 16 us margin covers timer rearm/clock granularity.
        const auto desired = std::clamp<std::uint64_t>(overshoot + 16, 80, stats_.leadCapUs);
        if (desired >= stats_.leadUs)
            stats_.leadUs = desired;
        else
            stats_.leadUs -= (stats_.leadUs - desired + 31) / 32;
    } else if (now < slotUs)
        ++stats_.spinOnly;
    else
        ++stats_.alreadyDue;
    const auto spinBegin = now;
    while (now < slotUs && !stop_.flag().load(std::memory_order_acquire)) {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
        _mm_pause();
#else
        std::this_thread::yield();
#endif
        now = rt::monotonicUs();
    }
    stats_.spinWallUs += now - spinBegin;
    if (stop_.flag().load(std::memory_order_acquire)) {
        ++stats_.cancelled;
        return false;
    }
    sampledUs = rt::monotonicUs();
    return true;
}
} // namespace rt::v2
