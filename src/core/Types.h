#pragma once
#include "protocol/ProtocolTypes.h"
#include "core/Diagnostics.h"
#include "core/TimingTrace.h"
#include <functional>
#include <optional>
#include <set>
namespace rt {
    enum class CachePolicy {
        SpecModulo, FifoErratum
    };
    struct ModuleConfig {
        std::uint8_t id=0;
        bool online=true;
        std::uint32_t dataBytes=65531;
        std::uint8_t seed=0;
        std::string file;
        // Empty => deterministic synthetic opaque bytes.
    };
    struct FaultConfig {
        std::set<std::uint16_t> dropFirst, dropAlways, corruptFirst, duplicateFirst;
        bool reverseInitial=false;
        bool any()const {
            return reverseInitial||!dropFirst.empty()||!dropAlways.empty()||!corruptFirst.empty()||!duplicateFirst.empty();
        }
    };
    struct StreamConfig {
        std::string runId;
        std::string targetIp="127.0.0.1", bindIp="127.0.0.1", allowedNackIp="127.0.0.1";
        std::uint16_t targetPort=5650, commandPort=5500, allowedNackPort=0;
        std::uint32_t initialLogicId=1;
        std::uint64_t frameLimit=0;
        // 0 = continuous; local test control, never a wire field.
        unsigned linkMbps=1000;
        bool allowOversubscribed=false;
        // Bounded local diagnostics only; never changes DATA/NACK bytes or pacing.
        bool timingTraceEnabled=true;
        CachePolicy cachePolicy=CachePolicy::SpecModulo;
        std::vector<ModuleConfig> modules {
            ModuleConfig {
            }
        };
        FaultConfig faults;
    };
    struct FragmentStatus {
        std::uint16_t id=0;
        std::uint32_t bytes=0, retries=0;
        std::uint64_t lastSendUs=0;
        std::string state="Queued";
    };
    struct FrameStatus {
        std::uint32_t id=0, bytes=0, mask=0;
        std::uint64_t timestampUs=0;
        std::uint16_t count=0;
        bool initialFinished=false;
        std::vector<FragmentStatus> fragments;
    };
    struct Statistics {
        std::uint64_t sampled=0, built=0, skippedNoModules=0, skippedOverrun=0, buildRejected=0;
        std::uint64_t firstAttempts=0, datagrams=0, bytesSent=0, retransmissions=0, injectedDrops=0, injectedCorrupt=0;
        std::uint64_t nacks=0, invalidNacks=0, cacheMisses=0, duplicatePending=0, queueDrops=0, sourceRejected=0;
        std::uint64_t unsentExpired=0, sendErrors=0, deadlineMisses=0, maxSampleLatenessUs=0, maxSendLatenessUs=0;
        std::uint64_t lastInitialDurationUs=0, elapsedUs=0;
        std::uint64_t initialFinishedFrames=0, initialFullySubmittedFrames=0, initialIncompleteFrames=0;
        std::uint64_t unsentAtStop=0, initialPendingFragments=0, initialFragmentsPlanned=0;
        std::uint64_t lastSampleToInitialEndUs=0, actualInitialDeadlineMisses=0;
        std::uint64_t samplingSpanUs=0, samplingActiveUs=0, firstSampleUs=0, lastSampleUs=0;
        std::uint64_t maxPlannedSendLatenessUs=0, nackDatagramsReceived=0, nackParsed=0, cacheHits=0, requestedFragments=0;
        std::uint64_t retryQueueHighWater=0, retryMaxWaitUs=0, retriesCancelled=0, commandQueueHighWater=0, commandMaxWaitUs=0;
        std::uint64_t feedbackBudgetHits=0, commandBudgetHits=0;
        std::uint64_t detailedSnapshots=0, lightSnapshots=0, deferredSnapshots=0, maxSnapshotDeferralUs=0;
        // Complete-population counters, independent of bounded/optional trace.
        std::uint64_t samplingIntervalViolations=0,initialPacketGapViolations=0;
        std::uint64_t socketSendCalls=0,initialFirstCopySendCalls=0,initialDuplicateSendCalls=0,retrySendCalls=0;
        std::uint64_t initialGapSegments=0,initialInjectedDropAttempts=0,retryInjectedDropAttempts=0;
        std::uint64_t minInitialNominalGapUs=0,maxInitialNominalGapUs=0;
        std::uint64_t minObservedDatagramBytes=0,maxObservedDatagramBytes=0;
        StageProfile receiveProfile,decodeProfile,commandProfile,buildProfile,encodeCrcProfile,sendProfile,publishProfile;
    };
    struct CacheMissEvent {
        std::uint32_t requested=0, latest=0;
        std::int64_t distance=0;
        std::uint64_t localUs=0;
        std::vector<std::uint32_t> cached;
    };
    struct Snapshot {
        bool running=false, draining=false;
        // A lightweight heartbeat intentionally omits frame/cache-miss detail.
        bool detailsIncluded=true;
        std::string detail="Idle; receiver delivery unknown";
        Statistics stats;
        std::vector<FrameStatus> frames;
        std::uint32_t nextLogicId=1;
        std::uint16_t localPort=0;
        bool dfEnabled=false;
        std::string runId, transport;
        StreamConfig config;
        int sendBufferBytes=0,receiveBufferBytes=0,connResetError=0;
        Distribution initialDuration,initialCallSpan,samplingIntervals,samplingDeviation;
        Distribution initialPacketGaps,initialPacketGapDeviation,retryPacketGaps,socketCallGaps;
        std::vector<CacheMissEvent> cacheMissEvents;
        std::uint64_t cacheMissEventsDropped=0;
        std::uint64_t workerThreadId=0;
        TimingTraceReport timingTrace;
    };
    using SendFunction=std::function<bool(const std::uint8_t*,std::size_t)>;
    std::set<std::uint16_t> parseFragmentIds(const std::string& text);
    std::uint64_t conservativeWireBytes(std::size_t frameBytes);
}
