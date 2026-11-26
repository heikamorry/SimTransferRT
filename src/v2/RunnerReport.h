#pragma once

// Private snapshots shared by the runner and its report serializer.
// Session's public interface is declared separately in Runner.h.
#include "v2/Engine.h"
#include "v2/ProducerWait.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace rt::v2::runner_detail {
struct TraceClock {
    std::uint64_t processId = 0;
    std::uint64_t senderTid = 0;
    std::uint64_t frequency = 0;
    std::uint64_t beforeTicks = 0;
    std::uint64_t afterTicks = 0;
    std::uint64_t utcFiletime = 0;
};

struct SourceStats {
    ProducerWaitStats wait;
    std::uint64_t producerTid = 0;
    std::uint64_t sampled = 0;
    std::uint64_t built = 0;
    std::uint64_t skipped = 0;
    std::uint64_t queueDrops = 0;
    std::uint64_t errors = 0;
    std::uint64_t queueHighWater = 0;
    std::uint64_t firstSampleUs = 0;
    std::uint64_t lastSampleUs = 0;
    std::uint64_t duplicateSource = 0;
    std::uint64_t prepareMaxUs = 0;
    std::uint64_t newSourceFrames = 0;
    std::uint64_t sourceCounterGaps = 0;
    std::uint64_t sourceCounterResets = 0;
    std::uint64_t mixedCounterFrames = 0;
    std::uint64_t semanticChanges = 0;
    std::uint64_t unmappedSourceFrames = 0;
    std::uint64_t clockErrorBoundUs = 0;
    std::uint64_t cancelledAttempts = 0;
    double actualDeviceHz = 0;
    double counterHz = 0;
    bool commonClockMapped = false;
    std::uint32_t nextId = 1;
    std::size_t payloadBytes = 0;
    Ledger ledger;
    Distribution intervals;
    Distribution deviation;
    Distribution lateness;
    std::string error;
    std::string kind = "simulation";
};

struct State {
    Config config;
    Preflight capacity;
    LinkObservation link;
    TraceClock traceClock;
    std::array<SourceStats, 2> sources;
    EngineSnapshot engine;
    std::uint64_t startedUs = 0;
    std::uint64_t endedUs = 0;
    std::uint64_t elapsedUs = 0;
    std::uint16_t localPort = 0;
    bool df = false;
    bool finished = false;
    std::string transport;
    std::string sendApi;
    OperationStorageStats operationStorage;
    std::string boostVersion;
    std::string error;
    std::string endedReason;
    int sendBuffer = 0;
    int receiveBuffer = 0;
    int resetError = 0;
    bool asynchronous = false;
    bool senderPriorityRaised = false;
    unsigned senderPriorityError = 0;
    std::uint64_t ndiGuardWindows = 0;
    std::uint64_t ndiGuardHits = 0;
    std::uint64_t ndiGuardWaitWallUs = 0;
    std::uint64_t ndiReadyToDequeueMaxUs = 0;
    std::uint64_t feedbackPollCalls = 0;
    std::uint64_t feedbackEmptyPolls = 0;
    std::uint64_t feedbackReadErrors = 0;
    std::string lastFeedbackReadError;
    std::uint64_t statusIngressAccepted = 0;
    std::uint64_t statusIngressRejected = 0;
    std::uint64_t statusIngressCancelled = 0;
    std::uint64_t statusIngressPending = 0;
};

// Pure serialization of a published snapshot; no scheduler or socket access.
std::string report(const State &state);
} // namespace rt::v2::runner_detail
