#include "v2/Engine.h"
#include "protocol/Checksum.h"
#include "protocol/PacketCodec.h"
#include "core/SamplingClock.h"
#include <algorithm>
#include <limits>

namespace rt::v2 {
namespace {std::uint64_t absDiff(std::uint64_t a,std::uint64_t b){return a>b?a-b:b-a;}}
PreparedPtr prepareFrame(Frame frame,std::uint64_t planned,std::uint64_t sampled,std::uint64_t ready,std::uint64_t sourceCounter) {
    auto p=std::make_shared<PreparedFrame>();p->frame=std::move(frame);
    p->plannedSampleUs=planned;p->sampleUs=sampled;p->readyUs=ready;p->sourceCounter=sourceCounter;
    p->payloadCrc=rt::crc32(p->frame.bytes.data(),p->frame.bytes.size());
    p->packets.reserve(p->frame.fragmentCount());
    for(std::uint16_t i=0;i<p->frame.fragmentCount();++i)p->packets.push_back(encodeData(p->frame,i));
    if(!p->readyUs)p->readyUs=rt::monotonicUs();
    return p;
}
void Ledger::add(const PreparedFrame& p){++count;idSum+=p.frame.frameId;crcSum+=p.payloadCrc;stampSum+=p.frame.timestampUs;}
Engine::Engine(Config c,unsigned liveLinkMbps):config_(std::move(c)),rate_(effectiveMbps(config_,liveLinkMbps)),nextStatus_(config_.initialStatusSequence){
    validateConfig(config_);
    worst_.reserve(32);
    initialDeadlineEvents_.reserve(32);
}
void Engine::cancel(Slot& s,bool stopping) {
    s.duplicatePending=false;
    auto& m=stats_[s.p->frame.moduleId-1];
    if(!s.finalized){++m.initialIncomplete;const auto left=s.p->packets.size()-s.next;if(stopping)m.unsentAtStop+=left;else m.unsentEvicted+=left;s.finalized=true;}
    m.retryCancelled+=s.pendingRetries.size();s.pendingRetries.clear();
}
void Engine::submit(PreparedPtr p) {
    if(!p||(p->frame.moduleId!=1&&p->frame.moduleId!=2)||p->packets.size()!=p->frame.fragmentCount())throw std::invalid_argument("invalid prepared V2 frame");
    const auto id=p->frame.moduleId-1;auto& cache=caches_[id];
    if(cache.size()==4){auto old=cache.front();cancel(*old,false);retries_.erase(std::remove_if(retries_.begin(),retries_.end(),[&](const Retry&r){return r.slot==old;}),retries_.end());cache.pop_front();}
    auto slot=std::make_shared<Slot>();slot->p=std::move(p);cache.push_back(slot);
    ++stats_[id].accepted;stats_[id].cacheHighWater=std::max<std::uint64_t>(stats_[id].cacheHighWater,cache.size());
}
bool Engine::submitStatus(Bytes payload,std::uint64_t when) {
    if(payload.size()>1444||statuses_.size()>=64){++statusDropped_;return false;}
    Status s;s.sequence=nextStatus_++;s.timestampUs=when;s.payload=std::move(payload);
    statuses_.push_back({encodeStatus(s),when});++statusAccepted_;statusHigh_=std::max<std::uint64_t>(statusHigh_,statuses_.size());return true;
}
void Engine::feedback(const std::uint8_t* p,std::size_t n,std::uint64_t now) {
    try {
        const auto nack=decodeNack(p,n);auto& m=stats_[nack.moduleId-1];++m.nacks;
        auto& cache=caches_[nack.moduleId-1];auto it=std::find_if(cache.begin(),cache.end(),[&](const auto&s){return s->p->frame.frameId==nack.frameId;});
        if(it==cache.end()){++m.cacheMisses;return;}
        try{validateIndices(nack,(*it)->p->frame.fragmentCount());}catch(const ProtocolError&){++m.invalidNacks;return;}
        m.requestedFragments+=nack.indices.size();
        for(auto index:nack.indices){
            auto& slot=**it;
            if(slot.pendingRetries.count(index)){++m.duplicatePending;continue;}
            if(retries_.size()>=4096){++m.retryQueueDrops;continue;}
            slot.pendingRetries.insert(index);retries_.push_back({*it,index,now});
        }
        const auto modulePending=std::count_if(retries_.begin(),retries_.end(),[&](const Retry&r){return r.slot->p->frame.moduleId==nack.moduleId;});
        m.retryHighWater=std::max<std::uint64_t>(m.retryHighWater,modulePending);
    }catch(const ProtocolError&){++invalidFeedback_;}
}
void Engine::setEffectiveMbps(unsigned rate,std::uint64_t now){
    if(!rate||rate==rate_)return;
    // Preserve absolute original plan across budget changes; don't silently reset a missed deadline.
    allowedTick_=((allowedTick_+rate_-1)/rate_)*rate;originalTick_=((originalTick_+rate_-1)/rate_)*rate;
    rate_=rate;allowedTick_=std::max(allowedTick_,now*rate_);++rateChanges_;
}
void Engine::setNextSample(std::uint8_t id,std::uint64_t time){if(id==1||id==2)nextSample_[id-1]=time;}
bool Engine::pendingInitial()const {for(const auto& c:caches_)for(const auto&s:c)if(!s->finalized||s->duplicatePending)return true;return false;}
std::uint64_t Engine::nextDueUs()const {
    if(statuses_.empty()&&!pendingInitial()&&retries_.empty())return UINT64_MAX;
    return (allowedTick_+rate_-1)/rate_;
}
void Engine::finalize(Slot& s,std::uint64_t end) {
    if(s.finalized)return;s.finalized=true;auto& m=stats_[s.p->frame.moduleId-1];
    const auto elapsed=end>=s.p->sampleUs?end-s.p->sampleUs:0;
    const auto limit=s.p->frame.moduleId==1?15000ULL:18000ULL;
    if(s.successful==s.p->packets.size()){++m.initialComplete;m.submitted.add(*s.p);duration_[s.p->frame.moduleId-1].add(elapsed,limit);}
    else ++m.initialIncomplete;
    if(elapsed>limit){
        ++m.deadlineMisses;
        const auto& p=*s.p;
        InitialDeadlineEvent event{p.frame.moduleId,p.frame.frameId,p.plannedSampleUs,p.sampleUs,
            p.readyUs,s.firstAttemptUs,end,p.sampleUs+limit,s.successful==p.packets.size()};
        if(initialDeadlineEvents_.size()<32){
            initialDeadlineEvents_.push_back(event);
        }else{
            auto smallest=std::min_element(initialDeadlineEvents_.begin(),initialDeadlineEvents_.end(),
                [](const auto& left,const auto& right){return left.overrunUs()<right.overrunUs();});
            if(event.overrunUs()>smallest->overrunUs())*smallest=event;
        }
    }
}
void Engine::recordEvent(PacketEvent event){
    if(worst_.size()<32){worst_.push_back(event);return;}
    auto it=std::min_element(worst_.begin(),worst_.end(),[](const auto&a,const auto&b){return a.scoreUs<b.scoreUs;});
    if(event.scoreUs>it->scoreUs)*it=event;
}
bool Engine::step(std::uint64_t now,const SendFunction& send,const std::function<std::uint64_t()>& clock) {
    if(now<nextDueUs())return false;
    std::shared_ptr<Slot> selected;std::uint16_t index=0;bool retry=false,status=false,duplicate=false;
    std::uint64_t requested=0,anchor=now;
    if(!statuses_.empty()){status=true;anchor=statuses_.front().confirmedUs;}
    else {
        std::uint64_t earliest=UINT64_MAX;
        for(auto& cache:caches_)for(auto& s:cache)if(!s->finalized||s->duplicatePending){
            const auto deadline=s->p->sampleUs+(s->p->frame.moduleId==1?15000:18000);
            if(deadline<earliest){earliest=deadline;selected=s;}
            break;
        }
        if(selected){
            duplicate=selected->duplicatePending;
            if(duplicate)index=selected->duplicateIndex;
            else index=static_cast<std::uint16_t>(config_.faults.reverseInitial&&selected->p->frame.moduleId==config_.faultModule?selected->p->packets.size()-1-selected->next:selected->next);
            anchor=selected->p->plannedSampleUs;
        }
        else if(!retries_.empty()){
            const auto& r=retries_.front();selected=r.slot;index=r.index;retry=true;requested=r.requestedUs;anchor=requested;
            const auto cost=(selected->p->packets[index].size+66)*8ULL;
            // Retries may not occupy the next known source's initial opportunity.
            for(const auto due:nextSample_){
                // Reserve the upcoming sample boundary. Once due, the source may
                // be blocked in a device read. Ready frames win on the next step.
                if(due>now && now+(cost+rate_-1)/rate_>=due)return false;
            }
        }else return false;
    }
    Datagram packet=status?statuses_.front().packet:selected->p->packets[index];
    if(retry){packet.bytes[7]|=1;rt::writeLe32(packet.bytes.data()+packet.size-4,rt::crc32(packet.bytes.data(),packet.size-4));}
    // Status and retries may delay DATA, but never move the retained initial plan forward.
    const auto original=(status||retry)?anchor*rate_:std::max(originalTick_,anchor*rate_);
    const auto plan=(original+rate_-1)/rate_,adjusted=(allowedTick_+rate_-1)/rate_;
    const auto start=clock();bool dropped=false,success=false;
    const auto module=status?3:selected->p->frame.moduleId;
    const bool inject=!status&&module==config_.faultModule;
    if(inject&&(config_.faults.dropAlways.count(index)||(!retry&&config_.faults.dropFirst.count(index))))dropped=true;
    if(inject&&!retry&&config_.faults.corruptFirst.count(index))packet.bytes[packet.size-1]^=0x80;
    if(!dropped)success=send(packet.bytes.data(),packet.size);
    const auto end=clock();const auto cost=(packet.size+66)*8ULL;
    if(!status&&!retry)originalTick_=original+cost;
    // At most one datagram of catch-up credit. Long stalls cannot release an entire frame burst.
    const auto actual=start*rate_;const auto floor=actual>cost?actual-cost:0;
    allowedTick_=std::max(allowedTick_,floor)+cost;
    if(lastGlobalStart_&&!dropped){const auto gap=start-lastGlobalStart_;const auto expected=plan>=lastGlobalPlan_?plan-lastGlobalPlan_:0;const auto dev=absDiff(gap,expected);globalGaps_.add(gap);globalDeviation_.add(dev,999);if(dev>=1000)++globalViolations_;}
    if(!dropped){lastGlobalStart_=start;lastGlobalPlan_=plan;}
    PacketEvent event;event.moduleId=static_cast<std::uint8_t>(module);event.originalDueUs=plan;event.adjustedDueUs=adjusted;event.sendStartUs=start;event.sendEndUs=end;event.retry=retry;event.success=success;
    if(status){
        if(success)++statusSent_;else ++statusSendErrors_;
        statusLatency_=std::max(statusLatency_,end>=anchor?end-anchor:0);statuses_.pop_front();event.scoreUs=end>=anchor?end-anchor:0;
    }else{
        auto& m=stats_[module-1];const auto late=start>plan?start-plan:0;
        if(dropped)++m.injectedDrops;else if(success){++m.datagrams;m.bytes+=packet.size;if(retry)++m.retransmissions;}else ++m.sendErrors;
        if(retry){retryWait_[module-1].add(start>=requested?start-requested:0);selected->pendingRetries.erase(index);retries_.pop_front();}
        else if(duplicate){selected->duplicatePending=false;}
        else{
            if(!selected->next)selected->firstAttemptUs=start;
            if(lastStart_[module-1]&&!dropped){auto gap=start-lastStart_[module-1];auto expected=plan>=lastPlan_[module-1]?plan-lastPlan_[module-1]:0;auto dev=absDiff(gap,expected);gaps_[module-1].add(gap);gapDeviation_[module-1].add(dev,999);if(dev>=1000)++m.gapViolations;}
            if(!dropped){lastStart_[module-1]=start;lastPlan_[module-1]=plan;}
            planLateness_[module-1].add(late,999);if(late>=1000)++m.originalPlanViolations;
            ++selected->next;if(success)++selected->successful;
            if(inject&&config_.faults.duplicateFirst.count(index)){selected->duplicatePending=true;selected->duplicateIndex=index;}
            if(selected->next==selected->p->packets.size())finalize(*selected,end);
        }
        event.frameId=selected->p->frame.frameId;event.index=index;event.plannedSampleUs=selected->p->plannedSampleUs;event.sampleUs=selected->p->sampleUs;event.readyUs=selected->p->readyUs;
        event.deadlineUs=selected->p->sampleUs+(module==1?15000:18000);event.scoreUs=std::max(late,end>event.deadlineUs?end-event.deadlineUs:0);
    }
    recordEvent(event);return true;
}
void Engine::stop(){for(auto& c:caches_)for(auto& s:c)cancel(*s,true);retries_.clear();statusCancelled_+=statuses_.size();statuses_.clear();}
EngineSnapshot Engine::snapshot()const{
    EngineSnapshot s;s.modules=stats_;s.effectiveMbps=rate_;s.nextStatusSequence=nextStatus_;
    for(std::size_t i=0;i<2;++i){s.modules[i].initialDuration=duration_[i].summary();s.modules[i].packetGaps=gaps_[i].summary();s.modules[i].packetGapDeviation=gapDeviation_[i].summary();s.modules[i].originalPlanLateness=planLateness_[i].summary();s.modules[i].retryWait=retryWait_[i].summary();}
    s.statusAccepted=statusAccepted_;s.statusSent=statusSent_;s.statusDropped=statusDropped_;s.statusSendErrors=statusSendErrors_;s.statusCancelled=statusCancelled_;s.statusQueueHighWater=statusHigh_;s.statusMaximumLatencyUs=statusLatency_;
    s.invalidFeedback=invalidFeedback_;s.sourceRejected=sourceRejected_;s.globalGapViolations=globalViolations_;s.rateChanges=rateChanges_;
    s.globalPacketGaps=globalGaps_.summary();s.globalPacketGapDeviation=globalDeviation_.summary();s.worstEvents=worst_;
    s.initialDeadlineEvents=initialDeadlineEvents_;return s;
}
}
