#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace rt::v2 {
// A persistent stop signal is shared by both producers. Only reset it after joining them.
class ProducerStop {
  public:
    ProducerStop();
    ~ProducerStop();

    const std::atomic<bool> &flag() const {
        return stopped_;
    }

    void request() noexcept;
    void reset();

  private:
    friend class ProducerWait;
    bool waitUntil(std::uint64_t targetUs);
    std::atomic<bool> stopped_{false};
    std::mutex mutex_;
    std::condition_variable changed_;
#ifdef _WIN32
    void *event_ = nullptr;
#endif
};

struct ProducerWaitStats {
    const char *backend = "inactive";
    std::uint64_t timedWaitSlots = 0;
    std::uint64_t fallbacks = 0;
    std::uint64_t earlyReturns = 0;
    std::uint64_t returnsPastSlot = 0;
    std::uint64_t spinOnly = 0;
    std::uint64_t alreadyDue = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t spinWallUs = 0;
    std::uint64_t overshootMaxUs = 0;
    std::uint64_t overshootGe1ms = 0;
    std::uint64_t worstSlotUs = 0;
    std::uint64_t worstTargetUs = 0;
    std::uint64_t worstReturnUs = 0;
    std::uint64_t leadUs = 80;
    std::uint64_t leadFloorUs = 0;
    std::uint64_t leadCapUs = 0;
    bool priorityRaised = false;
    unsigned priorityError = 0;
};

class ProducerWait {
  public:
    enum class Timer {
        Automatic,
        ConditionVariable
    };
    ProducerWait(ProducerStop &, ProducerWaitStats &, unsigned hz, Timer = Timer::Automatic,
                 unsigned leadCapUs = 2000);
    ~ProducerWait();
    ProducerWait(const ProducerWait &) = delete;
    ProducerWait &operator=(const ProducerWait &) = delete;
    // On success sampledUs is the observed time at or after the original slot.
    bool until(std::uint64_t slotUs, std::uint64_t &sampledUs);

  private:
    bool sleepUntil(std::uint64_t targetUs);
    ProducerStop &stop_;
    ProducerWaitStats &stats_;
#ifdef _WIN32
    void *timer_ = nullptr;
    int previousPriority_ = 0;
#endif
};
} // namespace rt::v2
