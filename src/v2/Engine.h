#pragma once
#include "v2/Config.h"
#include "protocol/V2PacketCodec.h"
#include "core/Diagnostics.h"
#include <deque>
#include <memory>
#include <set>

namespace rt::v2 {
struct PreparedFrame {
    Frame frame;
    std::uint64_t plannedSampleUs=0, sampleUs=0, readyUs=0, sourceCounter=0;
    bool hardwareTimestampVerified=false;
    std::vector<Datagram> packets;
    std::uint32_t payloadCrc=0;
};
using PreparedPtr=std::shared_ptr<const PreparedFrame>;
PreparedPtr prepareFrame(Frame frame,std::uint64_t planned,std::uint64_t sampled,
                         std::uint64_t ready=0,std::uint64_t sourceCounter=0);
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
    Ledger submitted;
};
struct PacketEvent {
    std::uint8_t moduleId=0;std::uint32_t frameId=0;std::uint16_t index=0;
    std::uint64_t plannedSampleUs=0,sampleUs=0,readyUs=0,originalDueUs=0,adjustedDueUs=0;
    std::uint64_t sendStartUs=0,sendEndUs=0,deadlineUs=0,scoreUs=0;
    bool retry=false,success=false;
};
struct EngineSnapshot {
    std::array<ModuleStats,2> modules;
    std::uint64_t statusAccepted=0,statusSent=0,statusDropped=0,statusSendErrors=0;
    std::uint64_t statusCancelled=0,statusQueueHighWater=0,statusMaximumLatencyUs=0;
    std::uint64_t invalidFeedback=0,sourceRejected=0,globalGapViolations=0,rateChanges=0;
    std::uint32_t nextStatusSequence=1;
    unsigned effectiveMbps=1000;
    Distribution globalPacketGaps,globalPacketGapDeviation;
    std::vector<PacketEvent> worstEvents;
};
// One owner on the send thread. Producers only hand over immutable PreparedFrames.
class Engine {
public:
    explicit Engine(Config);
    void submit(PreparedPtr);
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
    };
    struct Retry {std::shared_ptr<Slot> slot;std::uint16_t index=0;std::uint64_t requestedUs=0;};
    struct QueuedStatus {Datagram packet;std::uint64_t confirmedUs=0;};
    Config config_;unsigned rate_;
    std::array<std::deque<std::shared_ptr<Slot>>,2> caches_;
    std::array<ModuleStats,2> stats_;
    std::array<TimingHistogram,2> duration_,gaps_,gapDeviation_,planLateness_,retryWait_;
    std::array<std::uint64_t,2> lastStart_{},lastPlan_{},nextSample_{};
    std::deque<Retry> retries_;
    std::deque<QueuedStatus> statuses_;
    std::uint64_t allowedTick_=0,originalTick_=0,lastGlobalStart_=0,lastGlobalPlan_=0;
    TimingHistogram globalGaps_,globalDeviation_;
    std::uint64_t statusAccepted_=0,statusSent_=0,statusDropped_=0,statusSendErrors_=0,statusCancelled_=0,statusHigh_=0,statusLatency_=0;
    std::uint64_t invalidFeedback_=0,sourceRejected_=0,globalViolations_=0,rateChanges_=0;
    std::uint32_t nextStatus_;
    std::vector<PacketEvent> worst_;
    void finalize(Slot&,std::uint64_t ended);
    void cancel(Slot&,bool stopping);
    void recordEvent(PacketEvent);
};
}
