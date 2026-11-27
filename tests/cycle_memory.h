#pragma once

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <fstream>
#include <sstream>
#include <string>
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace cycle_memory {
struct Reading {
    std::int64_t workingSetBytes;
    std::int64_t privateBytes;
};

inline Reading readCurrent() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                              sizeof(counters))) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "GetProcessMemoryInfo");
    }
    return {static_cast<std::int64_t>(counters.WorkingSetSize),
            static_cast<std::int64_t>(counters.PrivateUsage)};
#else
    std::ifstream status("/proc/self/status");
    std::int64_t rss = -1, anonymous = -1;
    std::string line;
    while (std::getline(status, line)) {
        if (line.compare(0, 6, "VmRSS:") != 0 && line.compare(0, 8, "RssAnon:") != 0)
            continue;
        std::istringstream values(line);
        std::string field, unit;
        std::int64_t kib = 0;
        if (values >> field >> kib >> unit && unit == "kB")
            (field == "VmRSS:" ? rss : anonymous) = kib * 1024;
        if (rss >= 0 && anonymous >= 0)
            return {rss, anonymous};
    }
    throw std::runtime_error("VmRSS or RssAnon unavailable in /proc/self/status");
#endif
}

struct Peaks {
    std::int64_t workingSetBytes = 0;
    std::int64_t privateBytes = 0;
    std::int64_t samples = 0;
    static constexpr int intervalMs = 20;
};

// Scope one sampler to one load/convert/unload cycle. Windows' native peak
// working set counter is cumulative over the entire life of the process.
class Sampler {
  public:
    Sampler() {
        record();
        worker_ = std::thread(&Sampler::sampleUntilStopped, this);
    }

    ~Sampler() {
        stop();
    }

    Sampler(const Sampler &) = delete;
    Sampler &operator=(const Sampler &) = delete;

    Peaks finish() {
        if (worker_.joinable()) {
            stop();
            if (!error_) {
                try {
                    record();
                } catch (...) {
                    error_ = std::current_exception();
                }
            }
        }
        if (error_)
            std::rethrow_exception(error_);
        return peaks_;
    }

  private:
    void record() {
        const auto sample = readCurrent();
        peaks_.workingSetBytes = (std::max)(peaks_.workingSetBytes, sample.workingSetBytes);
        peaks_.privateBytes = (std::max)(peaks_.privateBytes, sample.privateBytes);
        ++peaks_.samples;
    }

    void sampleUntilStopped() noexcept {
        try {
            std::unique_lock<std::mutex> lock(mutex_);
            while (!wake_.wait_for(lock, std::chrono::milliseconds(Peaks::intervalMs), [this] {
                return stopping_;
            })) {
                record();
            }
        } catch (...) {
            error_ = std::current_exception();
        }
    }

    void stop() {
        if (!worker_.joinable())
            return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        worker_.join();
    }

    Peaks peaks_;
    std::exception_ptr error_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::thread worker_;
};
} // namespace cycle_memory
