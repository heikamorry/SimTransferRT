#include "v2/Engine.h"
#include "protocol/V2PacketCodec.h"
#include <iostream>
#include <stdexcept>
using namespace rt::v2;
namespace {
void require(bool b,const char* text){if(!b)throw std::runtime_error(text);}
PreparedPtr make(unsigned module,unsigned id,std::size_t n=1437,std::uint64_t sampled=1000){Frame f;f.moduleId=static_cast<std::uint8_t>(module);f.frameId=id;f.timestampUs=sampled;f.bytes.resize(n,37);return prepareFrame(std::move(f),sampled,sampled,sampled);}
void testPriority(){Config c;Engine e(c);e.submit(make(1,1));e.submit(make(2,1));e.submitStatus({1,2},1000);std::vector<rt::Bytes> out;std::uint64_t clock=1000;auto send=[&](const auto*p,std::size_t n){out.emplace_back(p,p+n);return true;};auto time=[&]{return clock;};
    for(unsigned i=0;i<5;++i){require(e.step(clock,send,time),"packet expected");clock=std::max(clock+1,e.nextDueUs());}
    require(out[0][6]==3,"status must go first");require(decodeData(out[1].data(),out[1].size()).moduleId==1,"earlier NDI deadline");require(e.snapshot().modules[0].initialComplete==1&&e.snapshot().modules[1].initialComplete==1,"independent completion");
    Nack n;n.moduleId=2;n.frameId=1;n.indices={0};auto nack=encodeNack(n);e.feedback(nack.bytes.data(),nack.size,clock);e.submit(make(1,2,1,clock));
    require(e.step(clock,send,time),"initial pending");require(decodeData(out.back().data(),out.back().size()).moduleId==1,"initial ahead of retry");clock=std::max(clock+1,e.nextDueUs());e.step(clock,send,time);auto p=decodeData(out.back().data(),out.back().size());require(p.moduleId==2&&p.retransmission,"retry correct module");}
void testBudget(){for(unsigned rate:{100u,1000u,2500u}){Config c;c.linkMbps=rate;Engine e(c);e.submit(make(2,1,204800));std::uint64_t clock=1000;unsigned packets=0;auto send=[&](const auto*,std::size_t){++packets;return true;};while(e.pendingInitial()){if(!e.step(clock,send,[&]{return clock;}))throw std::runtime_error("scheduler stalled");clock=std::max(clock,e.nextDueUs());}require(packets==143,"143 EP fragments");const auto d=e.snapshot().modules[1].initialDuration;require(d.maxUs<=18000,"capacity timing");require(d.maxUs>=(rate==100?17000:rate==1000?1700:680),"selected profile actually paces");}}
void testCacheStopAndLate(){Config c;Engine e(c);for(unsigned i=0;i<5;++i)e.submit(make(1,i));require(e.snapshot().modules[0].cacheHighWater==4,"four-frame bound");Nack n;n.moduleId=1;n.frameId=0;n.indices={0};auto d=encodeNack(n);e.feedback(d.bytes.data(),d.size,1000);require(e.snapshot().modules[0].cacheMisses==1,"old cache eviction");std::uint64_t now=100000;e.step(now,[](auto*,auto){return true;},[&]{return now;});require(e.snapshot().worstEvents.front().originalDueUs==1000,"original plan not moved after stall");e.stop();auto m=e.snapshot().modules[0];require(m.initialIncomplete==5,"all incomplete accounted");require(m.unsentEvicted==2&&m.unsentAtStop==7,"unsent counted");}
void testPreflight(){Config c;c.linkMbps=100;c.ndiTools=40;auto p=preflight(c);require(p.feasible&&p.synchronousWireUs>17700&&p.synchronousWireUs<17703,"100M budget");c.ndiTools=255;require(!preflight(c).feasible,"synchronous overload rejected");c.linkMbps=2500;c.observedLinkMbps=100;require(effectiveMbps(c)==100,"local lower link wins");c.pathLimitMbps=50;require(effectiveMbps(c)==50,"path lower cap wins");}
void testLateStatusAndFaults(){Config c;c.faults.reverseInitial=true;c.faults.duplicateFirst.insert(1);Engine e(c);e.submit(make(2,3));e.submitStatus({7},10000);std::uint64_t now=10000;std::vector<rt::Bytes> packets;auto send=[&](const auto*p,std::size_t n){packets.emplace_back(p,p+n);return true;};e.step(now,send,[&]{return now;});for(unsigned k=0;k<3;++k){now=std::max(now+1,e.nextDueUs());require(e.step(now,send,[&]{return now;}),"fault packet");}require(packets.size()==4,"duplicate emitted");require(decodeData(packets[1].data(),packets[1].size()).fragmentIndex==1,"reverse first");require(decodeData(packets[2].data(),packets[2].size()).fragmentIndex==1,"duplicate fragment");require(e.snapshot().modules[1].originalPlanLateness.maxUs>=9000,"late status cannot hide original data lateness");}
void testRetryDuringDelayedAcquisition(){
    Engine engine(Config{});
    engine.submit(make(2,8,1));
    std::vector<rt::Bytes> output;
    auto send=[&](const auto* bytes,std::size_t size){output.emplace_back(bytes,bytes+size);return true;};
    std::uint64_t now=1000;
    auto clock=[&]{return now;};
    require(engine.step(now,send,clock),"EP first copy");
    auto request=encodeNack(Nack{2,8,{0}});
    engine.feedback(request.bytes.data(),request.size,now);
    engine.setNextSample(1,2000);
    now=1999;
    require(!engine.step(now,send,clock),"reserve upcoming NDI sample boundary");
    now=2001;
    require(engine.step(now,send,clock),"blocked NDI acquisition must not reserve a full deadline window");
    require(decodeData(output.back().data(),output.back().size()).retransmission,"EP retry served");
}

void testInitialDeadlineEvidence(){
    Engine engine(Config{});
    std::uint64_t now=2000;
    auto clock=[&]{return now;};
    auto send=[](auto*,auto){return true;};
    engine.submit(make(2,77));
    require(engine.step(now,send,clock),"first fragment");
    now=20001;
    require(engine.step(now,send,clock),"late last fragment");
    auto request=encodeNack(Nack{2,77,{0}});
    for(unsigned i=0;i<40;++i){
        engine.feedback(request.bytes.data(),request.size,now);
        now+=1000;
        require(engine.step(now,send,clock),"retry burst");
    }
    auto snapshot=engine.snapshot();
    require(snapshot.initialDeadlineEvents.size()==1,"independent deadline evidence");
    const auto& event=snapshot.initialDeadlineEvents.front();
    require(event.moduleId==2 && event.frameId==77 && event.complete,"late frame identity");
    require(event.plannedSampleUs==1000 && event.sampleUs==1000 && event.readyUs==1000 &&
        event.firstAttemptUs==2000 && event.endUs==20001 && event.deadlineUs==19000 &&
        event.overrunUs()==1001,"deadline event measures preparation and both sends");
}
}
int main(){try{testPriority();testBudget();testCacheStopAndLate();testPreflight();testLateStatusAndFaults();testRetryDuringDelayedAcquisition();testInitialDeadlineEvidence();std::cout<<"V2 engine tests passed\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
