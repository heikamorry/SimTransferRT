#pragma once
#include "protocol/V2PacketCodec.h"
#include "sources/AuroraCodec.h"
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace rt::v2 {
struct ReceiveLedger {
    std::uint64_t count = 0;
    std::uint64_t idSum = 0;
    std::uint64_t crcSum = 0;
    std::uint64_t stampSum = 0;
};

// Exactly one outcome is recorded for each decoded DATA datagram, before any
// frame-level consumption decision. A retry can supply the first usable copy.
struct FragmentOutcomes {
    std::uint64_t accepted = 0;
    std::uint64_t duplicate = 0;
    std::uint64_t afterComplete = 0;
    std::uint64_t afterExpiry = 0;
    std::uint64_t afterConflict = 0;
    std::uint64_t outsideWindow = 0;
    std::uint64_t conflict = 0;
};

enum class ExpiryReason { RetryExhausted, WindowEviction };

struct ExpiredFrameSample {
    std::uint32_t frameId = 0;
    std::size_t received = 0;
    std::size_t total = 0;
    unsigned rounds = 0;
    ExpiryReason reason = ExpiryReason::RetryExhausted;
};

struct ReceiveModuleStats {
    std::uint64_t datagrams = 0;
    std::uint64_t bytes = 0;
    std::uint64_t completed = 0;
    std::uint64_t consumed = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t late = 0;
    std::uint64_t conflicts = 0;
    std::uint64_t expired = 0;
    std::uint64_t wholeMissing = 0;
    std::uint64_t businessErrors = 0;
    std::uint64_t retries = 0;
    std::uint64_t nackRounds = 0;
    std::uint64_t nackPackets = 0;
    std::uint64_t requestedFragments = 0;
    std::uint64_t tailTriggers = 0;
    std::uint64_t gapTriggers = 0;
    std::uint64_t maxCompletionUs = 0;
    std::uint64_t maxNackLatenessUs = 0;
    std::uint64_t maxRetryIntervalUs = 0;
    std::uint64_t retryIntervalCount = 0;
    std::uint64_t minRetryIntervalUs = 0;
    std::size_t activeFrames = 0;
    std::size_t highWaterFrames = 0;
    std::array<FragmentOutcomes, 2> fragmentOutcomes{}; // original, retransmission
    std::uint64_t firstTailIdle = 0;
    std::uint64_t firstTailNewFrame = 0;
    std::array<ExpiredFrameSample, 8> expirySamples{};
    std::size_t expirySampleCount = 0;
    ReceiveLedger ledger;
};

struct ReceiveView {
    std::shared_ptr<const Frame> frame;
    std::uint64_t hostCompletedUs = 0;
    bool stale = true;
};

struct ReceiverSnapshot {
    std::array<ReceiveModuleStats, 2> modules{};
    std::array<ReceiveView, 2> latest{};
    rt::aurora::Reply ndi;
    Status status;
    bool hasStatus = false;
    std::uint64_t statusApplied = 0;
    std::uint64_t statusOld = 0;
    std::uint64_t invalidPackets = 0;
    std::uint64_t unexpectedNacks = 0;
    std::uint64_t statusAppliedUs = 0;
    std::uint64_t nearestTimestampDeltaUs = 0;
    std::uint32_t associatedNdiId = 0;
    std::uint32_t associatedEpId = 0;
    bool associationAvailable = false;
    // A matching timestamp is not proof of a verified common hardware clock.
    bool hardwareClockVerified = false;
};

class Receiver {
  public:
    using Clock = std::function<std::uint64_t()>;
    using FrameConsumer = std::function<void(const Frame &, std::uint64_t)>;
    using StatusConsumer = std::function<void(const Status &, std::uint64_t)>;
    explicit Receiver(bool decodeAurora = true, Clock clock = {});
    void receive(const std::uint8_t *, std::size_t, std::uint64_t nowUs);
    // While draining queued datagrams, only a newly observed gap/new frame
    // should trigger feedback. Service elapsed timers with poll at batch end.
    std::vector<rt::Datagram> receiveAndPollObserved(const std::uint8_t *, std::size_t,
                                                     std::uint64_t nowUs);
    std::vector<rt::Datagram> poll(std::uint64_t nowUs);
    ReceiverSnapshot snapshot(std::uint64_t nowUs) const;

    void setFrameConsumer(FrameConsumer f) {
        frameConsumer_ = std::move(f);
    }

    void setStatusConsumer(StatusConsumer f) {
        statusConsumer_ = std::move(f);
    }

    // Caller must isolate old endpoint traffic/drain its socket before reset.
    void reset();

  private:
    struct Assembly {
        Frame frame;
        std::vector<bool> seen;
        std::size_t received = 0;
        std::uint64_t firstUs = 0;
        std::uint64_t lastUs = 0;
        std::uint64_t nextRequestUs = 0;
        std::uint64_t lastRequestUs = 0;
        unsigned rounds = 0;
        bool discovered = false;
        bool tail = false;
        std::uint16_t highest = 0;
        std::uint16_t contiguous = 0;
    };

    enum class Retirement { Completed, Expired, Conflict };

    struct RetiredFrame {
        std::uint32_t id;
        Retirement reason;
    };

    struct Module {
        std::deque<Assembly> active;
        std::deque<RetiredFrame> retired;
        std::optional<std::uint32_t> highest;
        std::deque<std::pair<std::uint32_t, std::uint64_t>> consumedTimes;
    };

    bool decodeAurora_;
    Clock clock_;
    std::array<Module, 2> state_;
    ReceiverSnapshot report_;
    FrameConsumer frameConsumer_;
    StatusConsumer statusConsumer_;
    std::optional<std::uint8_t> ingest(const std::uint8_t *, std::size_t, std::uint64_t);
    std::vector<rt::Datagram> pollRequests(std::uint64_t,
                                           std::optional<std::uint8_t> observedModule);
    void retire(std::size_t, std::uint32_t, Retirement);
    void expire(std::size_t, const Assembly &, ExpiryReason);
    void complete(std::size_t, Assembly &, std::uint64_t);
};

std::string receiverJson(const ReceiverSnapshot &);
} // namespace rt::v2
