#pragma once
#include "v2/Config.h"
#include "protocol/V2PacketCodec.h"
#include "core/Diagnostics.h"
#include <atomic>
#include <deque>
#include <memory>
#include <set>

namespace rt::v2 {
struct EncodedPayload {
    explicit EncodedPayload(const Frame&);
    std::size_t byteSize;
    std::uint32_t crc;
    std::vector<Datagram> packets;
};
struct PreparedFrame {
    std::uint8_t moduleId=0;
    std::uint32_t frameId=0;
    std::uint64_t timestampUs=0;
    std::uint64_t plannedSampleUs=0, sampleUs=0, readyUs=0, sourceCounter=0;
    bool hardwareTimestampVerified=false;
    std::shared_ptr<const EncodedPayload> payload;
    std::optional<DataStamp> stamp;
    Datagram packet(std::uint16_t index,bool retry)const;
};
using PreparedPtr=std::shared_ptr<const PreparedFrame>;
PreparedPtr prepareFrame(Frame frame,std::uint64_t planned,std::uint64_t sampled,
                         std::uint64_t ready=0,std::uint64_t sourceCounter=0);
class StaticEpPayload {
public:
    explicit StaticEpPayload(Bytes);
    PreparedPtr sample(std::uint32_t id,std::uint64_t planned,std::uint64_t sampled)const;
private:
    std::shared_ptr<const EncodedPayload> payload_;
};
struct Ledger {
    std::uint64_t count=0,idSum=0,crcSum=0,stampSum=0;
    void add(const PreparedFrame&);
};
struct ModuleStats {
    std::uint64_t accepted=0,initialComplete=0,initialIncomplete=0,deadlineMisses=0;
    std::uint64_t datagrams=0,bytes=0,retransmissions=0,sendErrors=0;
    std::uint64_t unsentAtStop=0,unsentEvicted=0,retryCancelled=0,nacks=0,invalidNacks=0,cacheMisses=0;
    std::uint64_t requestedFragments=0,retryQueueDrops=0,duplicatePending=0,injectedDrops=0;
    std::uint64_t samplingViolations=0,gapViolations=0,originalPlanViolations=0;
    std::uint64_t cacheHighWater=0,retryHighWater=0;
    Distribution initialDuration,packetGaps,packetGapDeviation,originalPlanLateness,retryWait;
    Distribution readyToFirst;
    Ledger submitted;
};
// All times use the sender's monotonic clock. The producer issues its notification
// after releasing the mailbox lock, so the sender can dequeue before this stamp exists.
struct FrameHandoff {
    std::uint64_t enqueuedUs=0,waitBeginUs=0,waitReturnUs=0,senderObservedUs=0,dequeuedUs=0,submittedUs=0;
    const char* waitKind=nullptr;
    const char* waitResult=nullptr;
    std::shared_ptr<const std::atomic<std::uint64_t>> notifyIssueUs;
};
struct DispatchCounts {
    std::uint64_t ndi=0,ep=0,status=0,retry=0;
};
struct DispatchIdentity {
    std::uint8_t moduleId=0; // Zero until the first dispatch.
    std::uint32_t frameId=0;
    std::uint16_t index=0;
    std::uint64_t startUs=0,endUs=0;
    bool retry=false;
};
struct PendingHead {
    std::uint8_t moduleId=0; // Zero if this module had no competing initial packet.
    std::uint32_t frameId=0;
    std::uint64_t deadlineUs=0;
    std::size_t nextFragment=0,fragmentsRemaining=0; // Original submission order, excluding duplicates.
    bool duplicatePending=false;
};
struct FirstDispatchEvent {
    std::uint8_t moduleId=0;
    std::uint32_t frameId=0;
    std::uint64_t sampleUs=0,readyUs=0,selectedUs=0,firstAttemptUs=0;
    std::uint64_t admissionPacingDueUs=0,pacingDueUs=0;
    FrameHandoff handoff;
    std::array<PendingHead,2> headsAtSubmit{};
    DispatchIdentity previousAtSubmit,previousAtFirst;
    DispatchCounts intervening;
    std::uint64_t readyToFirstUs()const{return firstAttemptUs>readyUs?firstAttemptUs-readyUs:0;}
};
struct PacketEvent {
    std::uint8_t moduleId=0;std::uint32_t frameId=0;std::uint16_t index=0;
    std::uint64_t plannedSampleUs=0,sampleUs=0,readyUs=0,originalDueUs=0,adjustedDueUs=0;
    std::uint64_t sendStartUs=0,sendEndUs=0,deadlineUs=0,scoreUs=0;
    bool retry=false,success=false;
};
struct InitialDeadlineEvent {
    std::uint8_t moduleId;
    std::uint32_t frameId;
    std::uint64_t plannedSampleUs, sampleUs, readyUs, firstAttemptUs, endUs, deadlineUs;
    bool complete;
    std::optional<FirstDispatchEvent> firstDispatch;
    std::uint64_t overrunUs() const { return endUs-deadlineUs; }
};
struct FragmentDispatchCounts {
    std::uint64_t ndi=0,ep=0,status=0,retry=0,duplicate=0;
};
struct FragmentInterval {
    std::uint16_t index=0;
    std::uint64_t originalDueUs=0,pacingDueUs=0,attemptUs=0,callStartUs=0,endUs=0;
    std::uint64_t pacingGapUs=0;
    bool called=false,success=false;
};
struct FragmentSpan {
    std::uint8_t moduleId=0;
    std::uint32_t frameId=0;
    std::uint64_t sampleUs=0,readyUs=0,firstAttemptUs=0,endUs=0;
    // These four disjoint parts sum to spanUs(). An all-dropped frame uses
    // beforeFirstCallUs for its entire span, as there was no transport call.
    std::uint64_t ownCallUs=0,beforeFirstCallUs=0,betweenCallsUs=0,afterLastCallUs=0;
    std::uint64_t pacingGapUs=0,callCount=0;
    FragmentDispatchCounts intervening;
    bool complete=false;
    std::vector<FragmentInterval> fragments;
    std::uint64_t spanUs()const{return endUs>firstAttemptUs?endUs-firstAttemptUs:0;}
    std::uint64_t readyToFirstUs()const{return firstAttemptUs>readyUs?firstAttemptUs-readyUs:0;}
    std::uint64_t readyToEndUs()const{return endUs>readyUs?endUs-readyUs:0;}
};
struct GapPacket {
    std::uint8_t moduleId=0;
    std::uint32_t id=0; // Frame ID for DATA; independent sequence for STATUS.
    std::uint16_t index=0;
    const char* kind=nullptr;
    std::uint64_t planUs=0,startUs=0,endUs=0;
    bool success=false;
};
struct WorstGap {
    GapPacket previous,current;
    std::uint64_t actualUs=0,expectedUs=0,deviationUs=0;
};
struct EngineSnapshot {
    // Index zero is the full dispatch stream; module IDs index their original DATA streams.
    std::array<WorstGap,3> worstPacketGaps{};
    std::array<ModuleStats,2> modules;
    std::uint64_t statusAccepted=0,statusSent=0,statusDropped=0,statusSendErrors=0;
    std::uint64_t statusCancelled=0,statusQueueHighWater=0,statusMaximumLatencyUs=0;
    std::uint64_t invalidFeedback=0,sourceRejected=0,globalGapViolations=0,rateChanges=0;
    std::uint32_t nextStatusSequence=1;
    unsigned effectiveMbps=1000;
    Distribution globalPacketGaps,globalPacketGapDeviation;
    std::vector<PacketEvent> worstEvents;
    std::vector<InitialDeadlineEvent> initialDeadlineEvents;
    std::array<std::vector<FirstDispatchEvent>,2> worstFirstDispatch;
    std::array<std::shared_ptr<const FragmentSpan>,2> worstFragmentSpan;
    std::array<Distribution,2> fragmentSpanDistribution{};
    std::shared_ptr<const FragmentSpan> worstNdiWait,worstNdiWaitEp,worstNdiReadyToEnd;
};
// One owner on the send thread. Producers only hand over immutable PreparedFrames.
class Engine {
public:
    explicit Engine(Config,unsigned liveLinkMbps=0);
    void submit(PreparedPtr,FrameHandoff handoff={});
    bool submitStatus(Bytes,std::uint64_t confirmedAtUs);
    void feedback(const std::uint8_t*,std::size_t,std::uint64_t nowUs);
    void rejectFeedbackSource(){++sourceRejected_;}
    void setEffectiveMbps(unsigned,std::uint64_t nowUs);
    void setNextSample(std::uint8_t module,std::uint64_t plannedUs);
    bool step(std::uint64_t nowUs,const SendFunction&,const std::function<std::uint64_t()>& clock);
    std::uint64_t nextDueUs() const;
    bool pendingInitial()const;
    void stop();
    EngineSnapshot snapshot()const;
private:
    struct Slot {
        PreparedPtr p;std::size_t next=0,successful=0;
        bool duplicatePending=false;
        std::uint16_t duplicateIndex=0;
        std::set<std::uint16_t> pendingRetries;
        bool finalized=false;
        bool yieldedToNdi=false;
        std::uint64_t firstAttemptUs=0;
        DispatchCounts countsAtSubmit;
        std::unique_ptr<FirstDispatchEvent> firstDispatch;
        std::shared_ptr<FragmentSpan> fragmentTrace;
        FragmentDispatchCounts fragmentCountsAtFirst;
        std::uint64_t lastCallEndUs=0;
        std::shared_ptr<const FragmentSpan> finishedSpan;
    };
    struct Retry {std::shared_ptr<Slot> slot;std::uint16_t index=0;std::uint64_t requestedUs=0;};
    struct QueuedStatus {Datagram packet;std::uint64_t confirmedUs=0;std::uint32_t sequence=0;};
    Config config_;unsigned rate_;
    std::array<std::deque<std::shared_ptr<Slot>>,2> caches_;
    std::array<ModuleStats,2> stats_;
    std::array<TimingHistogram,2> duration_,gaps_,gapDeviation_,planLateness_,retryWait_;
    std::array<TimingHistogram,2> readyToFirst_;
    std::array<std::vector<FirstDispatchEvent>,2> worstFirstDispatch_;
    DispatchCounts dispatchCounts_;
    DispatchIdentity previousDispatch_;
    FragmentDispatchCounts fragmentCounts_;
    std::array<GapPacket,3> previousGapPacket_{};
    std::array<WorstGap,3> worstPacketGaps_{};
    std::vector<TimingHistogram> fragmentSpanHistograms_;
    std::array<std::shared_ptr<const FragmentSpan>,2> worstFragmentSpan_;
    std::shared_ptr<const FragmentSpan> worstNdiWait_,worstNdiWaitEp_,worstNdiReadyToEnd_;
    std::weak_ptr<Slot> pendingNdiWaitEp_;
    std::array<std::uint64_t,2> lastStart_{},lastPlan_{},nextSample_{};
    std::deque<Retry> retries_;
    std::deque<QueuedStatus> statuses_;
    std::uint64_t allowedTick_=0,originalTick_=0,lastGlobalStart_=0,lastGlobalPlan_=0;
    TimingHistogram globalGaps_,globalDeviation_;
    std::uint64_t statusAccepted_=0,statusSent_=0,statusDropped_=0,statusSendErrors_=0,statusCancelled_=0,statusHigh_=0,statusLatency_=0;
    std::uint64_t invalidFeedback_=0,sourceRejected_=0,globalViolations_=0,rateChanges_=0;
    std::uint32_t nextStatus_;
    static constexpr std::size_t MaxPacketEvents=32;
    std::vector<PacketEvent> worst_;
    std::size_t lowestPacketEvent_=0;
    std::vector<InitialDeadlineEvent> initialDeadlineEvents_;
    void finalize(Slot&,std::uint64_t ended);
    void cancel(Slot&,bool stopping);
    void recordEvent(const PacketEvent&);
    void recordFirstDispatch(Slot&,std::uint64_t pacingDueUs);
    bool canServeNdiAtEpBoundary(const Slot& ndi,const Slot& ep,std::uint64_t now)const;
    void recordFragment(Slot&,FragmentInterval);
    void finishFragmentSpan(Slot&);
};
}
