#pragma once

#include <cstdint>

namespace rt::v2 {

struct ReceiveLoopStats {
    std::uint64_t extraBatches = 0;
    std::uint64_t extraDatagrams = 0;
    std::uint64_t forcedTimerPolls = 0;
    std::uint64_t nackBatchesReadable = 0;
    std::uint64_t nackBatchesNotReadable = 0;
    std::uint64_t nackBatchesReadabilityUnknown = 0;
};

// readOne is nonblocking and counts every dequeued datagram, even a rejected one.
// Observed gaps are handled by readOne immediately. Only wall-clock timers wait
// for a potential extra batch. They still run under continuous unrelated traffic.
template <class Clock, class Stop, class ReadOne, class PollTimers>
void serviceReceiveBatch(ReceiveLoopStats &stats, Clock now, Stop stopped, ReadOne readOne,
                         PollTimers pollTimers) {
    constexpr unsigned maxPackets = 64;
    constexpr std::uint64_t budgetUs = 500;
    for (unsigned batch = 0; batch < 2; ++batch) {
        const auto start = now();
        for (unsigned count = 0; count < maxPackets; ++count) {
            if (stopped())
                return;
            if (!readOne()) {
                if (!stopped())
                    pollTimers();
                return;
            }
            if (batch) {
                if (!count)
                    ++stats.extraBatches;
                ++stats.extraDatagrams;
            }
            if (now() - start >= budgetUs)
                break;
        }
        // On the next iteration a direct read also detects an empty queue.
    }
    if (!stopped()) {
        ++stats.forcedTimerPolls;
        pollTimers();
    }
}

} // namespace rt::v2
