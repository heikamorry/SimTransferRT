#pragma once
#include "v2/Config.h"
#include "network/IUdpTransport.h"
#include "protocol/V2PacketCodec.h"
#include "core/Diagnostics.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace rt::v2 {
struct EncodedPayload {
    explicit EncodedPayload(const Frame &);
    std::size_t byteSize;
    std::uint32_t crc;
    std::vector<Datagram> packets;
};

struct PreparedFrame {
    std::uint8_t moduleId = 0;
    std::uint32_t frameId = 0;
    std::uint64_t timestampUs = 0;
    std::uint64_t plannedSampleUs = 0;
    std::uint64_t sampleUs = 0;
    std::uint64_t readyUs = 0;
    std::uint64_t sourceCounter = 0;
    bool hardwareTimestampVerified = false;
    std::shared_ptr<const EncodedPayload> payload;
    std::optional<DataStamp> stamp;
    Datagram packet(std::uint16_t index, bool retry) const;
};

using PreparedPtr = std::shared_ptr<const PreparedFrame>;
PreparedPtr prepareFrame(Frame frame, std::uint64_t planned, std::uint64_t sampled,
                         std::uint64_t ready = 0, std::uint64_t sourceCounter = 0);

class StaticEpPayload {
  public:
    explicit StaticEpPayload(Bytes);
    PreparedPtr sample(std::uint32_t id, std::uint64_t planned, std::uint64_t sampled) const;

  private:
    std::shared_ptr<const EncodedPayload> payload_;
};

struct Ledger {
    std::uint64_t count = 0;
    std::uint64_t idSum = 0;
    std::uint64_t crcSum = 0;
    std::uint64_t stampSum = 0;
    void add(const PreparedFrame &);
};

struct ModuleStats {
    std::uint64_t accepted = 0;
    std::uint64_t initialComplete = 0;
    std::uint64_t initialIncomplete = 0;
    std::uint64_t deadlineMisses = 0;
    std::uint64_t datagrams = 0;
    std::uint64_t bytes = 0;
    std::uint64_t retransmissions = 0;
    std::uint64_t sendErrors = 0;
    std::uint64_t unsentAtStop = 0;
    std::uint64_t unsentEvicted = 0;
    std::uint64_t retryCancelled = 0;
    std::uint64_t nacks = 0;
    std::uint64_t invalidNacks = 0;
    std::uint64_t cacheMisses = 0;
    std::uint64_t requestedFragments = 0;
    std::uint64_t retryQueueDrops = 0;
    std::uint64_t duplicatePending = 0;
    std::uint64_t injectedDrops = 0;
    std::uint64_t samplingViolations = 0;
    std::uint64_t gapViolations = 0;
    std::uint64_t originalPlanViolations = 0;
    std::uint64_t cacheHighWater = 0;
    std::uint64_t retryHighWater = 0;
    Distribution initialDuration;
    Distribution packetGaps;
    Distribution packetGapDeviation;
    Distribution originalPlanLateness;
    Distribution retryWait;
    Distribution readyToFirst;
    Ledger submitted;
};

// All times use the sender's monotonic clock. The producer issues its notification
// after releasing the mailbox lock, so the sender can dequeue before this stamp exists.
struct FrameHandoff {
    std::uint64_t enqueuedUs = 0;
    std::uint64_t waitBeginUs = 0;
    std::uint64_t waitReturnUs = 0;
    std::uint64_t senderObservedUs = 0;
    std::uint64_t dequeuedUs = 0;
    std::uint64_t submittedUs = 0;
    const char *waitKind = nullptr;
    const char *waitResult = nullptr;
    std::shared_ptr<const std::atomic<std::uint64_t>> notifyIssueUs;
};

struct DispatchCounts {
    std::uint64_t ndi = 0;
    std::uint64_t ep = 0;
    std::uint64_t status = 0;
    std::uint64_t retry = 0;
};

struct DispatchIdentity {
    std::uint8_t moduleId = 0; // Zero until the first dispatch.
    std::uint32_t frameId = 0;
    std::uint16_t index = 0;
    std::uint64_t startUs = 0;
    std::uint64_t endUs = 0;
    bool retry = false;
};

struct PendingHead {
    std::uint8_t moduleId = 0; // Zero if this module had no competing initial packet.
    std::uint32_t frameId = 0;
    std::uint64_t deadlineUs = 0;
    std::size_t nextFragment = 0;
    std::size_t fragmentsRemaining = 0; // Original submission order, excluding duplicates.
    bool duplicatePending = false;
};

struct FirstDispatchEvent {
    std::uint8_t moduleId = 0;
    std::uint32_t frameId = 0;
    std::uint64_t sampleUs = 0;
    std::uint64_t readyUs = 0;
    std::uint64_t selectedUs = 0;
    std::uint64_t firstAttemptUs = 0;
    std::uint64_t admissionPacingDueUs = 0;
    std::uint64_t pacingDueUs = 0;
    FrameHandoff handoff;
    std::array<PendingHead, 2> headsAtSubmit{};
    DispatchIdentity previousAtSubmit;
    DispatchIdentity previousAtFirst;
    DispatchCounts intervening;

    std::uint64_t readyToFirstUs() const {
        return firstAttemptUs > readyUs ? firstAttemptUs - readyUs : 0;
    }
};

struct PacketEvent {
    std::uint8_t moduleId = 0;
    std::uint32_t frameId = 0;
    std::uint16_t index = 0;
    std::uint64_t plannedSampleUs = 0;
    std::uint64_t sampleUs = 0;
    std::uint64_t readyUs = 0;
    std::uint64_t originalDueUs = 0;
    std::uint64_t adjustedDueUs = 0;
    std::uint64_t sendStartUs = 0;
    std::uint64_t sendEndUs = 0;
    std::uint64_t deadlineUs = 0;
    std::uint64_t scoreUs = 0;
    bool retry = false;
    bool success = false;
};

struct InitialDeadlineEvent {
    std::uint8_t moduleId;
    std::uint32_t frameId;
    std::uint64_t plannedSampleUs;
    std::uint64_t sampleUs;
    std::uint64_t readyUs;
    std::uint64_t firstAttemptUs;
    std::uint64_t endUs;
    std::uint64_t deadlineUs;
    bool complete;
    std::optional<FirstDispatchEvent> firstDispatch;

    std::uint64_t overrunUs() const {
        return endUs - deadlineUs;
    }
};

struct FragmentDispatchCounts {
    std::uint64_t ndi = 0;
    std::uint64_t ep = 0;
    std::uint64_t status = 0;
    std::uint64_t retry = 0;
    std::uint64_t duplicate = 0;
};

struct FragmentInterval {
    std::uint16_t index = 0;
    std::uint64_t originalDueUs = 0;
    std::uint64_t pacingDueUs = 0;
    std::uint64_t attemptUs = 0;
    std::uint64_t callStartUs = 0;
    std::uint64_t submitReturnUs = 0;
    std::uint64_t endUs = 0;
    std::uint64_t pacingGapUs = 0;
    bool called = false;
    bool success = false;
    bool pendingCompletion = false;
};

struct FragmentSpan {
    std::uint8_t moduleId = 0;
    std::uint32_t frameId = 0;
    std::uint64_t sampleUs = 0;
    std::uint64_t readyUs = 0;
    std::uint64_t enqueuedUs = 0;
    std::uint64_t dequeuedUs = 0;
    std::uint64_t firstAttemptUs = 0;
    std::uint64_t endUs = 0;
    // These four disjoint parts sum to spanUs(). An all-dropped frame uses
    // beforeFirstCallUs for its entire span, as there was no transport call.
    std::uint64_t ownCallUs = 0;
    std::uint64_t beforeFirstCallUs = 0;
    std::uint64_t betweenCallsUs = 0;
    std::uint64_t afterLastCallUs = 0;
    std::uint64_t pacingGapUs = 0;
    std::uint64_t callCount = 0;
    FragmentDispatchCounts intervening;
    bool complete = false;
    std::vector<FragmentInterval> fragments;

    std::uint64_t spanUs() const {
        return endUs > firstAttemptUs ? endUs - firstAttemptUs : 0;
    }

    std::uint64_t readyToFirstUs() const {
        return firstAttemptUs > readyUs ? firstAttemptUs - readyUs : 0;
    }

    std::uint64_t readyToEndUs() const {
        return endUs > readyUs ? endUs - readyUs : 0;
    }
};

struct GapPacket {
    std::uint8_t moduleId = 0;
    std::uint32_t id = 0; // Frame ID for DATA; independent sequence for STATUS.
    std::uint16_t index = 0;
    const char *kind = nullptr;
    std::uint64_t planUs = 0;
    std::uint64_t startUs = 0;
    std::uint64_t endUs = 0;
    bool success = false;
    std::uint64_t readyUs = 0;
    std::uint64_t enqueuedUs = 0;
    std::uint64_t dequeuedUs = 0;
    std::uint64_t callStartUs = 0;
    std::uint64_t submitReturnUs = 0;
};

struct WorstGap {
    GapPacket previous;
    GapPacket current;
    std::uint64_t actualUs = 0;
    std::uint64_t expectedUs = 0;
    std::uint64_t deviationUs = 0;
    std::optional<std::size_t> previousEpFrameIndex{};
};

struct GapEpFrame {
    std::uint32_t frameId = 0;
    bool awaitingFinalize = false;
    std::shared_ptr<const FragmentSpan> span;
};

inline constexpr std::size_t MaxGlobalGapViolationPairs = 64;

struct EngineSnapshot {
    // Index zero is the full dispatch stream; module IDs index their original DATA streams.
    std::array<WorstGap, 3> worstPacketGaps{};
    std::vector<WorstGap> globalGapViolationPairs;
    std::vector<GapEpFrame> gapPreviousEpFrames;
    std::array<ModuleStats, 2> modules;
    std::uint64_t statusAccepted = 0;
    std::uint64_t statusSent = 0;
    std::uint64_t statusDropped = 0;
    std::uint64_t statusSendErrors = 0;
    std::uint64_t statusCancelled = 0;
    std::uint64_t statusQueueHighWater = 0;
    std::uint64_t statusMaximumLatencyUs = 0;
    std::uint64_t invalidFeedback = 0;
    std::uint64_t sourceRejected = 0;
    std::uint64_t globalGapViolations = 0;
    std::uint64_t rateChanges = 0;
    std::uint32_t nextStatusSequence = 1;
    unsigned effectiveMbps = 1000;
    Distribution globalPacketGaps;
    Distribution globalPacketGapDeviation;
    std::vector<PacketEvent> worstEvents;
    std::vector<InitialDeadlineEvent> initialDeadlineEvents;
    std::array<std::vector<FirstDispatchEvent>, 2> worstFirstDispatch;
    std::array<std::shared_ptr<const FragmentSpan>, 2> worstFragmentSpan;
    std::array<Distribution, 2> fragmentSpanDistribution{};
    std::shared_ptr<const FragmentSpan> worstNdiWait;
    std::shared_ptr<const FragmentSpan> worstNdiWaitEp;
    std::shared_ptr<const FragmentSpan> worstNdiReadyToEnd;
};

// One owner on the send thread. Producers only hand over immutable PreparedFrames.
class Engine {
  public:
    explicit Engine(Config, unsigned liveLinkMbps = 0);
    void submit(PreparedPtr, FrameHandoff handoff = {});
    bool submitStatus(Bytes, std::uint64_t confirmedAtUs);
    void feedback(const std::uint8_t *, std::size_t, std::uint64_t nowUs);

    void rejectFeedbackSource() {
        ++sourceRejected_;
    }

    void setEffectiveMbps(unsigned, std::uint64_t nowUs);
    void setNextSample(std::uint8_t module, std::uint64_t plannedUs);
    bool step(std::uint64_t nowUs, const SendFunction &,
              const std::function<std::uint64_t()> &clock);
    // Same selection and accounting as step(); an asynchronous transport is
    // observed to completion on this thread, so initiation is never success.
    bool stepTransport(std::uint64_t nowUs, IUdpTransport &,
                       const std::function<std::uint64_t()> &clock);
    std::uint64_t nextDueUs() const;
    bool pendingInitial() const;
    void stop();
    EngineSnapshot snapshot() const;

  private:
    struct Slot {
        PreparedPtr p;
        std::size_t next = 0;
        std::size_t successful = 0;
        bool duplicatePending = false;
        std::uint16_t duplicateIndex = 0;
        std::set<std::uint16_t> pendingRetries;
        bool finalized = false;
        bool yieldedToNdi = false;
        std::uint64_t firstAttemptUs = 0;
        DispatchCounts countsAtSubmit;
        std::unique_ptr<FirstDispatchEvent> firstDispatch;
        std::shared_ptr<FragmentSpan> fragmentTrace;
        FragmentDispatchCounts fragmentCountsAtFirst;
        std::uint64_t lastCallEndUs = 0;
        std::shared_ptr<const FragmentSpan> finishedSpan;
        std::optional<std::size_t> gapEpFrameIndex;
    };

    struct Retry {
        std::shared_ptr<Slot> slot;
        std::uint16_t index = 0;
        std::uint64_t requestedUs = 0;
    };

    struct QueuedStatus {
        Datagram packet;
        std::uint64_t confirmedUs = 0;
        std::uint32_t sequence = 0;
    };

    struct Dispatch {
        std::shared_ptr<Slot> slot;
        std::uint16_t index;
        bool retry, status, duplicate, dropped;
        std::size_t bytes;
        std::uint64_t requestedUs, anchorUs, planUs, adjustedUs, startUs, callStartUs, submitReturnUs;
    };

    std::optional<Dispatch> inFlight_;
    bool pollSend(IUdpTransport &, const std::function<std::uint64_t()> &clock);
    void finishDispatch(const Dispatch &, bool success, std::uint64_t end,
                        bool pendingCompletion = false);

    Config config_;
    unsigned rate_;
    std::array<std::deque<std::shared_ptr<Slot>>, 2> caches_;
    std::array<ModuleStats, 2> stats_;
    std::array<TimingHistogram, 2> duration_;
    std::array<TimingHistogram, 2> gaps_;
    std::array<TimingHistogram, 2> gapDeviation_;
    std::array<TimingHistogram, 2> planLateness_;
    std::array<TimingHistogram, 2> retryWait_;
    std::array<TimingHistogram, 2> readyToFirst_;
    std::array<std::vector<FirstDispatchEvent>, 2> worstFirstDispatch_;
    DispatchCounts dispatchCounts_;
    DispatchIdentity previousDispatch_;
    FragmentDispatchCounts fragmentCounts_;
    std::array<GapPacket, 3> previousGapPacket_{};
    std::array<WorstGap, 3> worstPacketGaps_{};
    std::vector<WorstGap> globalGapViolationPairs_;
    std::vector<GapEpFrame> gapPreviousEpFrames_;
    std::shared_ptr<Slot> lastEpDataSlot_;
    std::vector<TimingHistogram> fragmentSpanHistograms_;
    std::array<std::shared_ptr<const FragmentSpan>, 2> worstFragmentSpan_;
    std::shared_ptr<const FragmentSpan> worstNdiWait_;
    std::shared_ptr<const FragmentSpan> worstNdiWaitEp_;
    std::shared_ptr<const FragmentSpan> worstNdiReadyToEnd_;
    std::weak_ptr<Slot> pendingNdiWaitEp_;
    std::array<std::uint64_t, 2> lastStart_{};
    std::array<std::uint64_t, 2> lastPlan_{};
    std::array<std::uint64_t, 2> nextSample_{};
    std::deque<Retry> retries_;
    std::deque<QueuedStatus> statuses_;
    std::uint64_t allowedTick_ = 0;
    std::uint64_t originalTick_ = 0;
    std::uint64_t lastGlobalStart_ = 0;
    std::uint64_t lastGlobalPlan_ = 0;
    TimingHistogram globalGaps_;
    TimingHistogram globalDeviation_;
    std::uint64_t statusAccepted_ = 0;
    std::uint64_t statusSent_ = 0;
    std::uint64_t statusDropped_ = 0;
    std::uint64_t statusSendErrors_ = 0;
    std::uint64_t statusCancelled_ = 0;
    std::uint64_t statusHigh_ = 0;
    std::uint64_t statusLatency_ = 0;
    std::uint64_t invalidFeedback_ = 0;
    std::uint64_t sourceRejected_ = 0;
    std::uint64_t globalViolations_ = 0;
    std::uint64_t rateChanges_ = 0;
    std::uint32_t nextStatus_;
    std::uint64_t asyncToken_ = 0;
    static constexpr std::size_t MaxPacketEvents = 32;
    std::vector<PacketEvent> worst_;
    std::size_t lowestPacketEvent_ = 0;
    std::vector<InitialDeadlineEvent> initialDeadlineEvents_;
    void finalize(Slot &, std::uint64_t ended);
    void cancel(Slot &, bool stopping);
    void recordEvent(const PacketEvent &);
    void recordFirstDispatch(Slot &, std::uint64_t pacingDueUs);
    bool stepInternal(std::uint64_t nowUs,
                      const std::function<SendDisposition(const std::uint8_t *, std::size_t)> &,
                      const std::function<std::uint64_t()> &clock, bool allowDefer);
    bool canServeNdiAtEpBoundary(const Slot &ndi, const Slot &ep, std::uint64_t now) const;
    void recordFragment(Slot &, FragmentInterval);
    void finishFragmentSpan(Slot &);
};
} // namespace rt::v2
