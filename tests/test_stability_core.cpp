#include "core/RealtimeStream.h"
#include "core/TimingTrace.h"
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace rt;
namespace {
    void require(bool value,const char* message="assertion failed") {
        if(!value)throw std::runtime_error(message);
    }
    StreamConfig small(std::uint32_t bytes=5000) {
        StreamConfig c;c.modules[0].dataBytes=bytes;return c;
    }
    bool accept(const std::uint8_t*,std::size_t){return true;}
    const FragmentStatus& fragment(const Snapshot& s,std::uint16_t index) {
        return s.frames.at(0).fragments.at(index);
    }
    void requireGapAccounting(const Snapshot& s) {
        const auto& stats=s.stats;
        require(stats.socketSendCalls==stats.initialFirstCopySendCalls+stats.initialDuplicateSendCalls+stats.retrySendCalls,
                "socket-call category accounting failed");
        require(stats.initialFirstCopySendCalls==stats.initialGapSegments+s.initialPacketGaps.count,
                "first-copy segment/gap denominator accounting failed");
        require(s.initialPacketGaps.count==s.initialPacketGapDeviation.count,"initial gap/deviation population differs");
        require(s.socketCallGaps.count==(stats.socketSendCalls?stats.socketSendCalls-1:0),"socket-call gap denominator failed");
        require(s.retryPacketGaps.count==(stats.retrySendCalls?stats.retrySendCalls-1:0),"retry-call gap denominator failed");
        require(stats.samplingIntervalViolations==s.samplingDeviation.overLimit,"sampling violation histogram/counter mismatch");
        require(stats.initialPacketGapViolations==s.initialPacketGapDeviation.overLimit,"packet gap violation histogram/counter mismatch");
        require(s.samplingIntervals.count==(stats.sampled?stats.sampled-1:0),"sampling interval denominator failed");
    }
}
int main() {
    using Test=std::pair<const char*,std::function<void()>>;
    const std::vector<Test> tests{
        {"disabled trace retains no events or incidents",[] {
            TimingTrace trace(false);
            for(unsigned i=0;i<1000;++i) {
                trace.record(TraceKind::SendBegin,i,7,i);
                trace.trigger(TraceReason::InitialDeadline,i,7,15001);
            }
            trace.finish();const auto r=trace.report();
            require(!r.enabled&&r.totalEvents==0&&r.totalTriggers==0&&r.windows.empty());
        }},
        {"trace preserves chronological wrapped history and partial stop",[] {
            TimingTrace trace;
            for(unsigned i=0;i<600;++i)trace.record(TraceKind::WaitBegin,i,42,i+100);
            trace.trigger(TraceReason::WaitLateness,600,42,1111);
            for(unsigned i=0;i<3;++i)trace.record(TraceKind::WaitEnd,601+i,42);
            trace.finish();const auto r=trace.report();
            require(r.totalEvents==603&&r.totalTriggers==1&&r.eventsOverwritten==91);
            require(r.windows.size()==1&&r.triggersNotCaptured==0);
            const auto& w=r.windows[0];
            require(w.beforeCount==512&&w.afterCount==3&&!w.complete&&w.events.size()==515);
            require(w.triggerSequence==600&&w.value==1111&&w.logicId==42);
            require(w.events.front().sequence==89&&w.events.front().atUs==88);
            require(w.events.back().sequence==603&&w.events.back().atUs==603);
        }},
        {"trace bounds incident count and accounts for every suppressed trigger",[] {
            TimingTrace trace;
            for(unsigned window=0;window<10;++window) {
                trace.record(TraceKind::Sample,window*1000,window);
                trace.trigger(TraceReason::SamplingDeviation,window*1000,window,1000);
                if(window<8)trace.trigger(TraceReason::InitialAbandoned,window*1000,window,3);
                for(unsigned i=0;i<128;++i)trace.record(TraceKind::SendEnd,window*1000+i,window,i);
            }
            const auto r=trace.report();
            require(r.windows.size()==8&&r.totalTriggers==18&&r.triggersNotCaptured==10);
            require(r.triggersByReason[0]==10&&r.notCapturedByReason[0]==2);
            require(r.triggersByReason[2]==8&&r.notCapturedByReason[2]==8);
            for(const auto& w:r.windows)require(w.complete&&w.afterCount==128&&w.events.size()<=640);
        }},
        {"compact states preserve existing fault, retry and UI strings",[] {
            auto c=small();c.faults.dropFirst.insert(0);c.faults.duplicateFirst.insert(1);
            RealtimeStream stream(c,0);
            auto send=[](const std::uint8_t* p,std::size_t n){return decodeData(p,n).fragmentIndex!=2;};
            for(std::uint64_t now=0;now<=60;now+=20)stream.tick(now,send);
            auto s=stream.snapshot(60);
            require(fragment(s,0).state=="Injected drop");
            require(fragment(s,1).state=="Sent locally (unconfirmed)");
            require(fragment(s,2).state=="Local send error");
            require(fragment(s,3).state=="Sent locally (unconfirmed)");
            require(s.stats.firstAttempts==4&&s.stats.datagrams==3&&s.stats.sendErrors==1);
            require(s.frames[0].initialFinished&&s.frames[0].count==4);
            require(fragment(s,0).bytes==1436&&fragment(s,3).bytes==697);
            Nack nack;nack.logicId=1;nack.ranges.push_back({0,3});stream.acceptNack(nack);
            for(std::uint64_t now=100;now<=120;now+=10)stream.tick(now,accept);
            s=stream.snapshot(120);
            for(unsigned i=0;i<3;++i)require(fragment(s,static_cast<std::uint16_t>(i)).state=="Retransmitted (unconfirmed)"&&fragment(s,static_cast<std::uint16_t>(i)).retries==1);
            require(s.stats.retransmissions==3);
        }},
        {"compact metadata mirrors modulo and FIFO cache across uint32 wrap",[] {
            for(auto policy:{CachePolicy::SpecModulo,CachePolicy::FifoErratum}) {
                auto c=small(10);c.initialLogicId=0xfffffffeu;c.cachePolicy=policy;
                RealtimeStream stream(c,0);
                for(std::uint64_t now=0;now<=60000;now+=20000)stream.tick(now,accept);
                const auto s=stream.snapshot(60000);
                require(s.frames.size()==3&&s.frames[0].id==1&&s.frames[1].id==0);
                require(s.frames[2].id==(policy==CachePolicy::SpecModulo?0xfffffffeu:0xffffffffu));
                for(const auto& f:s.frames)require(stream.cache().find(f.id)&&f.fragments.size()==1&&f.fragments[0].retries==0);
            }
        }},
        {"reused metadata resets retries and handles module-mask size changes",[] {
            auto c=small(10);c.modules.push_back(ModuleConfig{1,false,5000,0,{}});
            RealtimeStream stream(c,0);stream.tick(0,accept);
            Nack nack;nack.logicId=1;nack.ranges.push_back({0,1});stream.acceptNack(nack);stream.tick(10,accept);
            stream.setModuleMask(3);
            for(std::uint64_t now=20000;now<60100;now+=20)stream.tick(now,accept);
            const auto s=stream.snapshot(60100);
            require(s.frames.size()==3&&s.frames[0].id==4&&s.frames[0].mask==3&&s.frames[0].bytes==5020);
            for(const auto& f:s.frames)for(const auto& p:f.fragments)require(p.retries==0&&p.state=="Sent locally (unconfirmed)");
        }},
        {"light snapshots omit details and preserve statistics and configuration",[] {
            auto c=small();c.runId="compatibility";
            RealtimeStream stream(c,0);stream.tick(0,accept);
            Nack nack;nack.logicId=1234;nack.ranges.push_back({0,1});stream.acceptNack(nack);
            const auto full=stream.snapshot(100),light=stream.snapshot(100,false);
            require(full.detailsIncluded&&!light.detailsIncluded&&full.frames.size()==1&&light.frames.empty());
            require(full.cacheMissEvents.size()==1&&light.cacheMissEvents.empty());
            require(light.stats.built==full.stats.built&&light.stats.firstAttempts==full.stats.firstAttempts&&light.stats.cacheMisses==1);
            require(light.runId==full.runId&&light.config.modules[0].dataBytes==full.config.modules[0].dataBytes);
            require(light.nextLogicId==full.nextLogicId&&light.stats.initialPendingFragments==3);
            require(light.timingTrace.windows.empty()&&full.timingTrace.windows.empty());
        }},
        {"deadline trace uses actual completion and leaves legacy counters intact",[] {
            std::uint64_t now=0;auto c=small(10);
            RealtimeStream stream(c,0,[&]{return now;});
            stream.tick(0,[&](const std::uint8_t*,std::size_t){now=15001;return true;});
            const auto s=stream.snapshot(now);const auto r=stream.timingTrace().report();
            require(s.stats.actualInitialDeadlineMisses==1&&s.stats.deadlineMisses==0);
            require(s.stats.lastSampleToInitialEndUs==15001&&s.stats.initialFullySubmittedFrames==1);
            require(r.windows.size()==1&&r.windows[0].reason==TraceReason::InitialDeadline);
            require(r.windows[0].triggerUs==15001&&r.windows[0].value==15001);
            require(r.windows[0].events.back().kind==TraceKind::InitialEnd);
        }},
        {"one millisecond period deviation triggers while metrics retain original meaning",[] {
            RealtimeStream stream(small(10),0);
            stream.tick(0,accept);stream.tick(21000,accept);
            const auto r=stream.timingTrace().report();const auto s=stream.snapshot(21000);
            require(s.samplingDeviation.count==1&&s.samplingDeviation.maxUs==1000,"period deviation metric changed");
            require(r.triggersByReason[0]==1&&r.windows.size()==1&&r.windows[0].value==1000,"missing exact-threshold trigger");
            const auto& window=r.windows[0];
            require(window.beforeCount>0&&window.afterCount==4,"normal packet stages are filtered; all four build/initial boundary events remain");
            const auto& triggerEvent=window.events.at(window.beforeCount-1);
            require(triggerEvent.kind==TraceKind::Sample&&triggerEvent.value==20000&&triggerEvent.atUs==21000,
                    "trigger must preserve planned and actual sample times");
            require(triggerEvent.sequence==window.triggerSequence&&triggerEvent.logicId==2,"trigger event identity mismatch");
            require(window.events.at(window.beforeCount).kind==TraceKind::BuildBegin&&window.events.back().kind==TraceKind::InitialEnd,
                    "post-trigger window must include continuing build and initial send");
            require(stream.nextSamplingDeadlineUs()==40000);
        }},
        {"abandoned initial fragments are counted and captured without suppressing new frames",[] {
            RealtimeStream stream(small(),0);stream.tick(0,accept);stream.tick(20000,accept);
            const auto s=stream.snapshot(20000);const auto r=stream.timingTrace().report();
            require(s.stats.built==2&&s.stats.unsentExpired==3&&s.stats.initialIncompleteFrames==1);
            require(r.triggersByReason[2]==1&&r.windows[0].logicId==1&&r.windows[0].value==3);
            stream.stop();stream.timingTrace().finish();
            require(stream.snapshot(20000).stats.unsentAtStop==3);
            require(stream.timingTrace().report().totalTriggers==2);
        }},
        {"abandon and explicit stop timestamps follow already-observed build and command times",[] {
            std::uint64_t now=0;
            RealtimeStream stream(small(),0,[&]{return now+=5;});
            stream.tick(0,accept);now=20000;stream.tick(now,accept);
            stream.timingTrace().record(TraceKind::CommandsBegin,now+=10,stream.currentLogicId());
            stream.stop(now+=10);stream.timingTrace().finish();
            const auto report=stream.timingTrace().report();
            require(report.windows.size()==1);
            const auto& events=report.windows[0].events;
            for(std::size_t i=1;i<events.size();++i)require(events[i].atUs>=events[i-1].atUs,"trace observation timestamps moved backwards");
            require(events.back().kind==TraceKind::Stop&&events.back().atUs==now);
            require(stream.snapshot(now).frames[0].timestampUs==20000,"trace timing changed original sample TS");
        }},
        {"p999 includes the tail and preserves original histogram quantiles",[] {
            TimingHistogram histogram;
            for(unsigned i=0;i<998;++i)histogram.add(100);
            histogram.add(250);histogram.add(10000);
            const auto d=histogram.summary();
            require(d.count==1000&&d.p50Us==100&&d.p95Us==100&&d.p99Us==100&&d.p999Us==300&&d.maxUs==10000);
        }},
        {"slow stage filter preserves the exact 100us boundary and original observation times",[] {
            TimingTrace trace;
            trace.recordSpanIfSlow(TraceKind::EncodeBegin,TraceKind::EncodeEnd,100,199,8,3);
            require(trace.report().totalEvents==0,"99us normal stage must be filtered");
            trace.recordSpanIfSlow(TraceKind::EncodeBegin,TraceKind::EncodeEnd,200,300,8,3);
            trace.trigger(TraceReason::InitialDeadline,300,8,15001);
            trace.recordSpanIfSlow(TraceKind::SendBegin,TraceKind::SendEnd,301,501,8,3);
            const auto r=trace.report();const auto& window=r.windows.at(0);
            require(r.totalEvents==4&&window.beforeCount==2&&window.afterCount==2);
            require(window.events[0].kind==TraceKind::EncodeBegin&&window.events[0].atUs==200);
            require(window.events[1].kind==TraceKind::EncodeEnd&&window.events[1].atUs==300);
            require(window.events[2].kind==TraceKind::SendBegin&&window.events[2].atUs==301);
            require(window.events[3].kind==TraceKind::SendEnd&&window.events[3].atUs==501);
            for(const auto& event:window.events)require(event.logicId==8&&event.value==3);
            TimingTrace disabled(false);
            disabled.recordSpanIfSlow(TraceKind::SendBegin,TraceKind::SendEnd,100,10000,8,3);
            require(disabled.report().totalEvents==0);
        }},
        {"trace switch preserves wire bytes, scheduling counters and fragment status",[] {
            auto enabled=small();enabled.initialLogicId=0xffffffffu;
            enabled.faults.dropFirst.insert(2);enabled.faults.duplicateFirst.insert(0);
            enabled.faults.corruptFirst.insert(1);enabled.faults.reverseInitial=true;
            auto disabled=enabled;disabled.timingTraceEnabled=false;
            RealtimeStream a(enabled,0),b(disabled,0);
            std::vector<Bytes> aWire,bWire;
            for(std::uint64_t now=0;now<=21000;now+=10) {
                if(now==1000) {
                    Nack nack;nack.logicId=0xffffffffu;nack.ranges.push_back({0,4});
                    a.acceptNack(nack);b.acceptNack(nack);
                }
                a.tick(now,[&](const std::uint8_t* p,std::size_t n){aWire.emplace_back(p,p+n);return true;});
                b.tick(now,[&](const std::uint8_t* p,std::size_t n){bWire.emplace_back(p,p+n);return true;});
            }
            const auto x=a.snapshot(21000),y=b.snapshot(21000);
            require(!aWire.empty()&&aWire==bWire,"trace switch changed DATA bytes or send order");
#define SAME_STAT(name) require(x.stats.name==y.stats.name,"trace switch changed " #name);
            SAME_STAT(sampled) SAME_STAT(built) SAME_STAT(firstAttempts) SAME_STAT(datagrams)
            SAME_STAT(bytesSent) SAME_STAT(retransmissions) SAME_STAT(nacks) SAME_STAT(cacheMisses)
            SAME_STAT(injectedDrops) SAME_STAT(injectedCorrupt) SAME_STAT(unsentExpired)
            SAME_STAT(deadlineMisses) SAME_STAT(actualInitialDeadlineMisses) SAME_STAT(maxSampleLatenessUs)
            SAME_STAT(maxSendLatenessUs) SAME_STAT(lastSampleToInitialEndUs) SAME_STAT(initialFragmentsPlanned)
            SAME_STAT(initialFullySubmittedFrames) SAME_STAT(initialIncompleteFrames) SAME_STAT(initialPendingFragments)
            SAME_STAT(samplingIntervalViolations) SAME_STAT(initialPacketGapViolations) SAME_STAT(socketSendCalls)
            SAME_STAT(initialFirstCopySendCalls) SAME_STAT(initialDuplicateSendCalls) SAME_STAT(retrySendCalls)
            SAME_STAT(initialGapSegments) SAME_STAT(initialInjectedDropAttempts) SAME_STAT(retryInjectedDropAttempts)
            SAME_STAT(minInitialNominalGapUs) SAME_STAT(maxInitialNominalGapUs)
            SAME_STAT(minObservedDatagramBytes) SAME_STAT(maxObservedDatagramBytes)
#undef SAME_STAT
            requireGapAccounting(x);requireGapAccounting(y);
            require(x.initialPacketGaps.count==y.initialPacketGaps.count&&x.initialPacketGaps.maxUs==y.initialPacketGaps.maxUs);
            require(x.socketCallGaps.count==y.socketCallGaps.count&&x.socketCallGaps.maxUs==y.socketCallGaps.maxUs);
            require(x.frames.size()==y.frames.size()&&x.nextLogicId==y.nextLogicId);
            for(std::size_t f=0;f<x.frames.size();++f) {
                require(x.frames[f].id==y.frames[f].id&&x.frames[f].timestampUs==y.frames[f].timestampUs);
                require(x.frames[f].fragments.size()==y.frames[f].fragments.size());
                for(std::size_t i=0;i<x.frames[f].fragments.size();++i) {
                    const auto& p=x.frames[f].fragments[i];const auto& q=y.frames[f].fragments[i];
                    require(p.state==q.state&&p.retries==q.retries&&p.lastSendUs==q.lastSendUs);
                }
            }
            require(a.timingTrace().report().totalEvents>0&&b.timingTrace().report().totalEvents==0);
        }},
        {"sampling violation counts include equality without relying on trace",[] {
            auto c=small(10);c.timingTraceEnabled=false;
            RealtimeStream stream(c,0);
            stream.tick(0,accept);stream.tick(20999,accept);stream.tick(41999,accept);
            const auto s=stream.snapshot(41999);requireGapAccounting(s);
            require(s.samplingDeviation.count==2&&s.samplingDeviation.maxUs==1000&&s.samplingDeviation.overLimit==1);
            require(s.stats.samplingIntervalViolations==1&&stream.timingTrace().report().totalEvents==0);
            require(s.initialPacketGaps.count==0&&s.stats.initialGapSegments==3,
                    "one-packet frames must never create an inter-frame initial gap");
        }},
        {"packet gap 999us passes and 1000us violates even when local sends fail",[] {
            auto c=small(2870);c.timingTraceEnabled=false; // 3 fragments, nominal gap20us.
            RealtimeStream stream(c,0);
            const auto reject=[](const std::uint8_t*,std::size_t){return false;};
            stream.tick(0,reject);stream.tick(1019,reject);stream.tick(2039,reject);
            const auto s=stream.snapshot(2039);requireGapAccounting(s);
            require(s.initialPacketGaps.count==2&&s.initialPacketGaps.maxUs==1020);
            require(s.initialPacketGapDeviation.maxUs==1000&&s.initialPacketGapDeviation.overLimit==1);
            require(s.stats.initialPacketGapViolations==1&&s.stats.sendErrors==3&&s.stats.datagrams==0);
            require(s.stats.socketSendCalls==3&&s.stats.initialFirstCopySendCalls==3&&s.stats.initialGapSegments==1);
            require(s.stats.minInitialNominalGapUs==20&&s.stats.maxInitialNominalGapUs==20);
            require(s.stats.minObservedDatagramBytes==39&&s.stats.maxObservedDatagramBytes==1472,
                    "observed datagram length includes actual failed call attempts");
        }},
        {"injected first or middle drops split gap adjacency and all drops have no observations",[] {
            const std::vector<std::set<std::uint16_t>> drops{{0},{1},{0,1,2,3}};
            const std::uint64_t expectedCalls[]{3,3,0},expectedGaps[]{2,1,0},expectedSegments[]{1,2,0};
            for(std::size_t caseIndex=0;caseIndex<drops.size();++caseIndex) {
                auto c=small();c.faults.dropFirst=drops[caseIndex];
                RealtimeStream stream(c,0);
                for(std::uint64_t now=0;now<=60;now+=20)stream.tick(now,accept);
                const auto s=stream.snapshot(60);requireGapAccounting(s);
                require(s.stats.firstAttempts==4&&s.stats.initialInjectedDropAttempts==drops[caseIndex].size());
                require(s.stats.socketSendCalls==expectedCalls[caseIndex]&&s.initialPacketGaps.count==expectedGaps[caseIndex]);
                require(s.stats.initialGapSegments==expectedSegments[caseIndex]&&s.initialPacketGapDeviation.maxUs==0);
                if(caseIndex==1)require(s.socketCallGaps.maxUs==40&&s.initialPacketGaps.maxUs==20,
                                      "gap across missing initial send must appear only in all-call distribution");
                if(caseIndex==2)require(s.stats.minObservedDatagramBytes==0&&s.stats.maxObservedDatagramBytes==0);
            }
        }},
        {"duplicates reverse order and uint32 wrap preserve first-copy gap population",[] {
            auto c=small();c.initialLogicId=0xffffffffu;c.faults.reverseInitial=true;c.faults.duplicateFirst.insert(1);
            RealtimeStream stream(c,0);
            for(std::uint64_t frame=0;frame<2;++frame)
                for(std::uint64_t ordinal=0;ordinal<4;++ordinal)stream.tick(frame*20000+ordinal*20,accept);
            const auto s=stream.snapshot(20060);requireGapAccounting(s);
            require(s.stats.initialFirstCopySendCalls==8&&s.stats.initialDuplicateSendCalls==2&&s.stats.socketSendCalls==10);
            require(s.initialPacketGaps.count==6&&s.stats.initialGapSegments==2&&s.initialPacketGaps.maxUs==20);
            require(s.initialPacketGapDeviation.maxUs==0&&s.socketCallGaps.maxUs==19940);
            require(s.nextLogicId==1&&s.frames[0].id==0);
            require(s.stats.minObservedDatagramBytes==733&&s.stats.maxObservedDatagramBytes==1472);
        }},
        {"unfinished frame replacement resets adjacency but preserves missing-fragment counts",[] {
            RealtimeStream stream(small(),0);
            stream.tick(0,accept);stream.tick(20,accept);stream.tick(20000,accept);
            const auto s=stream.snapshot(20000);requireGapAccounting(s);
            require(s.stats.unsentExpired==2&&s.stats.initialIncompleteFrames==1&&s.stats.initialPendingFragments==3);
            require(s.stats.initialFirstCopySendCalls==3&&s.stats.initialGapSegments==2&&s.initialPacketGaps.count==1);
            require(s.initialPacketGaps.maxUs==20&&s.socketCallGaps.maxUs==19980);
        }},
        {"retry-call gaps include idle while injected retry drops create no socket call",[] {
            auto c=small();c.faults.dropAlways.insert(0);RealtimeStream stream(c,0);
            for(std::uint64_t now=0;now<=60;now+=20)stream.tick(now,accept);
            Nack nack;nack.logicId=1;nack.ranges.push_back({0,3});stream.acceptNack(nack);
            for(std::uint64_t now=100;now<=120;now+=10)stream.tick(now,accept);
            for(std::uint64_t now=20000;now<=20060;now+=20)stream.tick(now,accept);
            nack.ranges={{3,1}};stream.acceptNack(nack);stream.tick(20100,accept);
            const auto s=stream.snapshot(20100);requireGapAccounting(s);
            require(s.stats.retrySendCalls==3&&s.stats.retryInjectedDropAttempts==1&&s.retryPacketGaps.count==2);
            require(s.retryPacketGaps.maxUs==19980&&s.initialPacketGaps.maxUs==20);
            require(s.stats.initialInjectedDropAttempts==2&&s.stats.initialFirstCopySendCalls==6&&s.initialPacketGaps.count==4);
            require(s.stats.socketSendCalls==9&&s.stats.retransmissions==3);
        }},
        {"initial deviation uses the actual 1MiB and 2MiB pacing target",[] {
            for(const auto bytes:{std::uint32_t(1048576),std::uint32_t(2097152)}) {
                auto c=small(bytes-5);c.linkMbps=10000;
                RealtimeStream stream(c,0);const auto nominal=bytes==1048576?20u:10u;
                for(unsigned ordinal=0;ordinal<3;++ordinal)stream.tick(std::uint64_t(ordinal)*nominal,accept);
                const auto s=stream.snapshot(2*nominal);requireGapAccounting(s);
                require(s.initialPacketGaps.count==2&&s.initialPacketGaps.maxUs==nominal&&s.initialPacketGapDeviation.maxUs==0);
                require(s.stats.minInitialNominalGapUs==nominal&&s.stats.maxInitialNominalGapUs==nominal);
            }
        }}
    };
    unsigned failures=0;
    for(const auto& test:tests)try{test.second();std::cout<<"PASS "<<test.first<<'\n';}
    catch(const std::exception& e){++failures;std::cerr<<"FAIL "<<test.first<<": "<<e.what()<<'\n';}
    std::cout<<tests.size()-failures<<'/'<<tests.size()<<" stability core tests passed\n";
    return failures?1:0;
}
