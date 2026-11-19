#include "core/SamplingClock.h"
#include <chrono>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#else
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif
#endif
namespace rt {
    std::uint64_t monotonicUs() noexcept {
#ifdef _WIN32
        static const std::uint64_t frequency=[]() {
            LARGE_INTEGER f {
            };
            QueryPerformanceFrequency(&f);
            return static_cast<std::uint64_t>(f.QuadPart);
        }
        ();
        LARGE_INTEGER n {
        };
        QueryPerformanceCounter(&n);
        const auto ticks=static_cast<std::uint64_t>(n.QuadPart);
        return (ticks/frequency)*1000000u+(ticks%frequency)*1000000u/frequency;
#else
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
    }
    void waitBrieflyUntil(std::uint64_t deadline) {
        const auto now=monotonicUs();
        if(deadline<=now)return;
        const auto wait=deadline-now;
        // Long waits yield the CPU. Short sub-100us waits spin on this dedicated worker only.
        // This is best-effort user-space pacing, NOT a hard real-time guarantee.
        if(wait>180) {
#ifdef _WIN32
            struct TimerHandle {
                HANDLE handle=CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_ALL_ACCESS);
                ~TimerHandle() {
                    if(handle)CloseHandle(handle);
                }
            };
            thread_local TimerHandle timer;
            LARGE_INTEGER interval {
            };
            interval.QuadPart=-static_cast<LONGLONG>((wait-80)*10u);
            if(timer.handle&&SetWaitableTimer(timer.handle,&interval,0,nullptr,nullptr,FALSE))WaitForSingleObject(timer.handle,INFINITE);
            else std::this_thread::sleep_for(std::chrono::microseconds(wait-80));
#else
            std::this_thread::sleep_for(std::chrono::microseconds(wait-80));
#endif
        }
        while(monotonicUs()<deadline) {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
            _mm_pause();
#else
            std::this_thread::yield();
#endif
        }
    }
}
