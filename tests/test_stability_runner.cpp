// Deterministic run-loop tests only. No sockets, sleeps, or real-time acceptance.
#include "application/StreamRunner.h"
#include "core/SamplingClock.h"
#include "protocol/PacketCodec.h"
#include <algorithm>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {
    using namespace rt;
    void require(bool condition,const char* message) {
        if(!condition)throw std::runtime_error(message);
    }

    struct VirtualClock {
        // CommandDispatcher stamps real monotonic time. Keep the injected clock
        // in that epoch but ahead of real time; queue-wait metrics are not tested.
        std::uint64_t start=monotonicUs()+3600000000ull;
        std::uint64_t now=start;
        std::size_t waits=0;
        std::function<void()> onWait;
        RunnerHooks hooks() {
            return {
                [this]{return now;},
                [this](std::uint64_t deadline) {
                    now=std::max(now+1,deadline);
                    require(++waits<2000000,"virtual runner did not make bounded progress");
                    require(now-start<2000000,"virtual runner exceeded two second test guard");
                    if(onWait)onWait();
                }
            };
        }
    };

    struct FakeUdp final: IUdpTransport {
        explicit FakeUdp(VirtualClock& virtualClock):clock(virtualClock){}
        VirtualClock& clock;
        std::thread::id owner;
        bool opened=false,flood=false,delayFirstTail=false;
        std::uint64_t sendCostUs=2,receiveCostUs=5,initialPackets=0,retryPackets=0;
        std::uint32_t newestLogic=0;
        std::set<std::uint32_t> initialLogicIds,masks;
        std::uint64_t firstTailFinishedUs=0;
        void open(const StreamConfig&)override {
            owner=std::this_thread::get_id();opened=true;
        }
        void sameOwner()const {
            require(opened&&owner==std::this_thread::get_id(),"transport used outside its owning thread");
        }
        bool send(const std::uint8_t* bytes,std::size_t size)override {
            sameOwner();
            const auto packet=decodeData(bytes,size);
            newestLogic=packet.logicId;
            if(packet.retransmission)++retryPackets;
            else {
                ++initialPackets;
                initialLogicIds.insert(packet.logicId);
                masks.insert(packet.moduleMask);
            }
            if(delayFirstTail&&!packet.retransmission&&packet.logicId==1&&
               packet.fragmentIndex+1==packet.fragmentCount) {
                clock.now+=4800;
                firstTailFinishedUs=clock.now;
            } else clock.now+=sendCostUs;
            return true;
        }
        bool receive(IncomingDatagram& datagram)override {
            sameOwner();
            if(!flood||!newestLogic)return false;
            clock.now+=receiveCostUs;
            const auto packet=encodeNack(Nack{newestLogic,{{0,1}}});
            datagram.bytes.assign(packet.bytes.begin(),packet.bytes.begin()+packet.size);
            datagram.sourceIp="127.0.0.1";
            datagram.sourcePort=5650;
            return true;
        }
        std::uint16_t localPort()const override{return 5500;}
        bool dfEnabled()const override{return true;}
        std::string lastError()const override{return {};}
        int sendBufferBytes()const override{return 65536;}
        int receiveBufferBytes()const override{return 4194304;}
        std::string name()const override{return "virtual stability transport";}
    };

    struct Observation {
        std::uint64_t at;
        Snapshot snapshot;
    };
    StreamConfig configuration(std::uint64_t frames) {
        StreamConfig config;
        config.runId="virtual-stability";
        config.frameLimit=frames;
        config.modules[0].dataBytes=1048571; // Full frame remains 1 MiB / 731 fragments.
        return config;
    }
    Snapshot run(StreamConfig config,FakeUdp& udp,CommandDispatcher& commands,
                 VirtualClock& clock,std::vector<Observation>& observations) {
        return runStream(config,udp,commands,[&](const Snapshot& snapshot) {
            observations.push_back({clock.now,snapshot});
        },[](const std::string&){},clock.hooks());
    }
    void requireFinal(const Snapshot& result,const std::vector<Observation>& observations) {
        require(!result.running,"runner must stop after the finite run or Stop command");
        require(result.detailsIncluded,"returned final snapshot must always contain details");
        require(!observations.empty()&&!observations.back().snapshot.running,
                "final stopped snapshot must be published");
        require(observations.back().snapshot.detailsIncluded,"published final snapshot must contain details");
        require(result.stats.initialFragmentsPlanned==result.stats.firstAttempts+
                result.stats.unsentExpired+result.stats.unsentAtStop+result.stats.initialPendingFragments,
                "snapshot policy must preserve initial-fragment accounting");
    }
    void testSafeDetailedSnapshots() {
        VirtualClock clock;FakeUdp udp(clock);CommandDispatcher commands;
        std::vector<Observation> observations;
        const auto result=run(configuration(20),udp,commands,clock,observations);
        requireFinal(result,observations);
        require(result.stats.built==20&&result.stats.initialIncompleteFrames==0,
                "normal virtual run must complete every initial frame");
        std::size_t runningDetails=0;
        std::uint64_t previous=0;
        for(const auto& observation:observations) {
            const auto& snapshot=observation.snapshot;
            if(!snapshot.running)continue;
            require(observation.at!=previous,"only one running snapshot may publish in one turn");
            previous=observation.at;
            if(!snapshot.detailsIncluded) {
                require(snapshot.frames.empty()&&snapshot.cacheMissEvents.empty(),
                        "heartbeat must not carry detailed frame or cache-event vectors");
                continue;
            }
            ++runningDetails;
            require(snapshot.stats.initialPendingFragments==0,"detailed snapshot interrupted initial sending");
            if(!snapshot.draining) {
                const auto nextSample=snapshot.stats.lastSampleUs+SamplingPeriodUs;
                require(nextSample>=observation.at&&nextSample-observation.at>=1000,
                        "detailed snapshot consumed the final millisecond before sampling");
            }
        }
        require(runningDetails>=3,"normal run must publish periodic details, not freeze at the first-send phase");
        require(!result.frames.empty(),"final detail must include cached frames");
    }
    void testNearSamplingBoundaryDefersDetails() {
        VirtualClock clock;FakeUdp udp(clock);udp.delayFirstTail=true;
        CommandDispatcher commands;std::vector<Observation> observations;
        const auto result=run(configuration(3),udp,commands,clock,observations);
        requireFinal(result,observations);
        require(udp.firstTailFinishedUs>clock.start+19000&&udp.firstTailFinishedUs<clock.start+20000,
                "fixture must finish first initial send within the unsafe final millisecond");
        require(!observations.empty()&&observations.front().at>=clock.start+20000,
                "runner must defer a due detailed snapshot across the unsafe sampling boundary");
        require(observations.front().snapshot.detailsIncluded,
                "a safe detailed snapshot should be sent before the deferred heartbeat deadline");
        require(observations.front().snapshot.stats.built>=2,"safe detailed snapshot must follow the next frame");
    }
    void testOverloadStillPublishesBoundedHeartbeats() {
        VirtualClock clock;FakeUdp udp(clock);udp.sendCostUs=100;
        CommandDispatcher commands;std::vector<Observation> observations;
        const auto result=run(configuration(20),udp,commands,clock,observations);
        requireFinal(result,observations);
        require(result.stats.unsentExpired>0,"overload fixture must leave initial fragments unfinished");
        std::vector<std::uint64_t> heartbeats;
        std::uint64_t previousBuilt=0;
        for(const auto& observation:observations) {
            const auto& snapshot=observation.snapshot;
            if(!snapshot.running)continue;
            require(!snapshot.detailsIncluded,"continuously pending initial frames must suppress detail publication");
            require(snapshot.frames.empty()&&snapshot.cacheMissEvents.empty(),"heartbeat carried detail allocations");
            require(snapshot.config.runId=="virtual-stability"&&snapshot.runId=="virtual-stability",
                    "heartbeat must retain configuration and run identity");
            require(snapshot.stats.built>=previousBuilt,"heartbeat counters must remain live and monotonic");
            require(snapshot.stats.firstAttempts>0,"heartbeat must preserve useful send counters");
            previousBuilt=snapshot.stats.built;
            heartbeats.push_back(observation.at);
        }
        require(heartbeats.size()>=2,"overload must not starve heartbeat publication");
        require(heartbeats.front()-clock.start<=101000,"first heartbeat exceeded the bounded deferral");
        for(std::size_t i=1;i<heartbeats.size();++i)
            require(heartbeats[i]-heartbeats[i-1]<=201000,"overload heartbeat gap exceeded publish plus deferral budgets");
        require(!result.frames.empty(),"final snapshot must remain complete after overload");
    }
    void testFeedbackMaskAndStopRemainFair() {
        VirtualClock clock;FakeUdp udp(clock);udp.flood=true;
        CommandDispatcher commands;std::vector<Observation> observations;
        auto config=configuration(0);
        ModuleConfig alternative;alternative.id=1;alternative.online=false;alternative.dataBytes=1048571;
        config.modules.push_back(alternative);
        bool maskPosted=false,stopPosted=false;
        std::uint64_t stopAt=0;
        clock.onWait=[&] {
            const auto elapsed=clock.now-clock.start;
            if(!maskPosted&&elapsed>=45000) {
                Command mask;mask.kind=Command::Kind::SetMask;mask.mask=2;
                require(commands.post(mask),"SetMask must enter the bounded command queue");
                maskPosted=true;
            }
            if(!stopPosted&&elapsed>=140000) {
                // Fill the ordinary queue before posting priority Stop. This is
                // deterministic producer interleaving, not a thread-race test.
                for(int i=0;i<512;++i) {
                    Command nack;nack.kind=Command::Kind::NackFeedback;
                    nack.nack=Nack{udp.newestLogic,{{0,1}}};commands.post(std::move(nack));
                }
                Command stop;stop.kind=Command::Kind::Stop;
                require(commands.post(stop),"Stop must be accepted even with a full ordinary queue");
                stopAt=clock.now;stopPosted=true;
            }
        };
        const auto result=run(config,udp,commands,clock,observations);
        requireFinal(result,observations);
        require(maskPosted&&stopPosted,"fixture did not exercise mask and Stop commands");
        require(clock.now-stopAt<=2000,"feedback flood delayed priority Stop beyond a bounded turn");
        require(result.stats.built>=7&&udp.initialLogicIds.size()>=7,"feedback starved newer initial frames");
        require(udp.masks.count(1)&&udp.masks.count(2),"SetMask did not affect subsequent sampled frames");
        require(result.stats.nackParsed>0&&result.stats.nacks>0&&udp.retryPackets>0,
                "fairness must process legitimate NACKs and submit retries");
        require(result.stats.retryQueueHighWater<=3u*1461u,"retransmission queue exceeded its fixed bound");
        require(result.timingTrace.triggersByReason[static_cast<std::size_t>(TraceReason::WaitLateness)]==0,
                "an already-due retry wake sentinel must not be reported as a wait oversleep");
    }
    void testAllOfflineFinalSnapshotIsExplicitlyComplete() {
        VirtualClock clock;FakeUdp udp(clock);CommandDispatcher commands;
        std::vector<Observation> observations;
        auto config=configuration(3);config.modules[0].online=false;
        const auto result=run(config,udp,commands,clock,observations);
        requireFinal(result,observations);
        require(result.frames.empty()&&result.stats.built==0&&result.stats.skippedNoModules==3,
                "an empty complete final snapshot must be distinguishable from a heartbeat");
    }
    void testControlPollStopsBeforeFirstPublication() {
        VirtualClock clock;FakeUdp udp(clock);CommandDispatcher commands;
        std::vector<Observation> observations;
        bool posted=false;
        std::uint64_t postedAt=0;
        auto hooks=clock.hooks();
        hooks.pollControl=[&] {
            if(!posted&&clock.now-clock.start>=1000) {
                require(observations.empty(),"control poll must not depend on a prior snapshot callback");
                require(udp.initialPackets>0,"fixture must stop during the first initial transmission");
                for(int i=0;i<512;++i) {
                    Command nack;nack.kind=Command::Kind::NackFeedback;
                    nack.nack=Nack{udp.newestLogic,{{0,1}}};commands.post(std::move(nack));
                }
                Command stop;stop.kind=Command::Kind::Stop;
                require(commands.post(stop),"control-poll Stop must bypass the full ordinary queue");
                postedAt=clock.now;posted=true;
            }
        };
        const auto result=runStream(configuration(0),udp,commands,[&](const Snapshot& snapshot) {
            observations.push_back({clock.now,snapshot});
        },[](const std::string&){},hooks);
        requireFinal(result,observations);
        require(posted&&clock.now-postedAt<=1000,"control-poll Stop did not take effect in a bounded turn");
        require(observations.size()==1,"Stop before the first safe publish should emit only final detail");
        require(result.stats.built==1&&result.stats.unsentAtStop>0,
                "control Stop must preserve first-frame cancellation accounting");
        require(result.stats.nacks==0,"priority Stop must precede ordinary feedback queued by the control poll");
    }
    void testFutureWaitOversleepIsMeasuredOnce() {
        VirtualClock clock;FakeUdp udp(clock);CommandDispatcher commands;
        std::vector<Observation> observations;
        auto hooks=clock.hooks();
        const auto ordinaryWait=hooks.waitUntil;
        bool overslept=false;
        std::uint64_t plannedWake=0;
        hooks.waitUntil=[&](std::uint64_t deadline) {
            const auto waitStart=clock.now;
            ordinaryWait(deadline);
            if(!overslept&&deadline>waitStart) {
                plannedWake=deadline;
                clock.now=deadline+1500;
                overslept=true;
            }
        };
        const auto result=runStream(configuration(2),udp,commands,[&](const Snapshot& snapshot) {
            observations.push_back({clock.now,snapshot});
        },[](const std::string&){},hooks);
        requireFinal(result,observations);
        require(overslept,"fixture must oversleep a genuinely future wait deadline");
        require(result.timingTrace.triggersByReason[static_cast<std::size_t>(TraceReason::WaitLateness)]==1,
                "one real future-deadline oversleep must produce exactly one wait-lateness trigger");
        const auto found=std::find_if(result.timingTrace.windows.begin(),result.timingTrace.windows.end(),
            [](const TimingTraceWindow& window){return window.reason==TraceReason::WaitLateness;});
        require(found!=result.timingTrace.windows.end(),"first wait oversleep must retain its incident window");
        require(found->value==1500&&found->triggerUs==plannedWake+1500,
                "wait-lateness trigger must measure the 1500us oversleep, not the host epoch");
        require(found->beforeCount>=2,"captured wait incident must include wait begin/end evidence");
        const auto& begin=found->events[found->beforeCount-2];
        const auto& end=found->events[found->beforeCount-1];
        require(begin.kind==TraceKind::WaitBegin&&end.kind==TraceKind::WaitEnd&&
                begin.value==plannedWake&&end.value==plannedWake&&begin.atUs<plannedWake&&
                end.atUs==plannedWake+1500,"wait trace must preserve the future deadline and actual return time");
    }
}

int main() {
    const std::vector<std::pair<const char*,std::function<void()>>> tests={
        {"safe detailed snapshots and final detail",testSafeDetailedSnapshots},
        {"less than one millisecond before sampling defers detail",testNearSamplingBoundaryDefersDetails},
        {"overload keeps bounded lightweight heartbeats",testOverloadStillPublishesBoundedHeartbeats},
        {"NACK SetMask and priority Stop fairness",testFeedbackMaskAndStopRemainFair},
        {"all-offline final snapshot is complete",testAllOfflineFinalSnapshotIsExplicitlyComplete},
        {"control poll stops before the first snapshot",testControlPollStopsBeforeFirstPublication},
        {"one future wait oversleep records its actual lateness",testFutureWaitOversleepIsMeasuredOnce},
    };
    std::size_t failures=0;
    for(const auto& test:tests) {
        try {test.second();std::cout<<"PASS "<<test.first<<'\n';}
        catch(const std::exception& error) {++failures;std::cerr<<"FAIL "<<test.first<<": "<<error.what()<<'\n';}
    }
    std::cout<<tests.size()-failures<<'/'<<tests.size()<<" deterministic runner tests passed\n";
    return failures?1:0;
}
