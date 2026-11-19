#include "v2/Engine.h"
#include "protocol/Checksum.h"
#include "protocol/PacketCodec.h"
#include "core/SamplingClock.h"
#include <algorithm>
#include <limits>

namespace rt::v2 {
namespace {std::uint64_t absDiff(std::uint64_t a,std::uint64_t b){return a>b?a-b:b-a;}}
EncodedPayload::EncodedPayload(const Frame& f):byteSize(f.bytes.size()),crc(rt::crc32(f.bytes)){
    const auto count=f.fragmentCount();
    packets.reserve(count);
    for(std::uint16_t i=0;i<count;++i)packets.push_back(encodeData(f,i));
}
Datagram PreparedFrame::packet(std::uint16_t index,bool retry)const{
    auto d=payload->packets.at(index);
    if(stamp)stamp->apply(d);
    if(retry){
        d.bytes[7]|=1;
        rt::writeLe32(d.bytes.data()+d.size-4,rt::crc32(d.bytes.data(),d.size-4));
    }
    return d;
}
PreparedPtr prepareFrame(Frame frame,std::uint64_t planned,std::uint64_t sampled,std::uint64_t ready,std::uint64_t sourceCounter) {
    auto p=std::make_shared<PreparedFrame>();p->moduleId=frame.moduleId;p->frameId=frame.frameId;p->timestampUs=frame.timestampUs;
    p->plannedSampleUs=planned;p->sampleUs=sampled;p->readyUs=ready;p->sourceCounter=sourceCounter;
    p->payload=std::make_shared<const EncodedPayload>(frame);
    if(!p->readyUs)p->readyUs=rt::monotonicUs();
    return p;
}
StaticEpPayload::StaticEpPayload(Bytes data){
    Frame base{EpModule,0,0,std::move(data)};
    payload_=std::make_shared<const EncodedPayload>(base);
}
PreparedPtr StaticEpPayload::sample(std::uint32_t id,std::uint64_t planned,std::uint64_t sampled)const{
    auto p=std::make_shared<PreparedFrame>();
    p->moduleId=EpModule;p->frameId=id;p->timestampUs=sampled;
    p->plannedSampleUs=planned;p->sampleUs=sampled;p->payload=payload_;
    p->stamp=makeDataStamp(payload_->packets,id,sampled);
    p->readyUs=rt::monotonicUs();
    return p;
}
void Ledger::add(const PreparedFrame& p){++count;idSum+=p.frameId;crcSum+=p.payload->crc;stampSum+=p.timestampUs;}
Engine::Engine(Config c,unsigned liveLinkMbps):config_(std::move(c)),rate_(effectiveMbps(config_,liveLinkMbps)),nextStatus_(config_.initialStatusSequence){
    validateConfig(config_);
    worst_.reserve(MaxPacketEvents);
    initialDeadlineEvents_.reserve(32);
    if(config_.handoffTrace)for(auto& events:worstFirstDispatch_)events.reserve(32);
    if(config_.fragmentTrace)fragmentSpanHistograms_.resize(caches_.size());
}
void Engine::cancel(Slot& s,bool stopping) {
    s.duplicatePending=false;
    auto& m=stats_[s.p->moduleId-1];
    if(!s.finalized){++m.initialIncomplete;const auto left=s.p->payload->packets.size()-s.next;if(stopping)m.unsentAtStop+=left;else m.unsentEvicted+=left;s.finalized=true;}
    m.retryCancelled+=s.pendingRetries.size();s.pendingRetries.clear();
}
void Engine::submit(PreparedPtr p,FrameHandoff handoff) {
    if(!p||!p->payload||(p->moduleId!=1&&p->moduleId!=2)||p->payload->packets.size()!=rt::fragmentCount(p->payload->byteSize))throw std::invalid_argument("invalid prepared V2 frame");
    const auto id=p->moduleId-1;auto& cache=caches_[id];
    if(cache.size()==4){auto old=cache.front();cancel(*old,false);retries_.erase(std::remove_if(retries_.begin(),retries_.end(),[&](const Retry&r){return r.slot==old;}),retries_.end());cache.pop_front();}
    auto slot=std::make_shared<Slot>();slot->p=std::move(p);
    if(config_.fragmentTrace){
        slot->fragmentTrace=std::make_shared<FragmentSpan>();
        auto& span=*slot->fragmentTrace;
        span.moduleId=slot->p->moduleId;
        span.frameId=slot->p->frameId;
        span.sampleUs=slot->p->sampleUs;
        span.readyUs=slot->p->readyUs;
        span.fragments.reserve(slot->p->payload->packets.size());
    }
    if(config_.handoffTrace){
        slot->firstDispatch=std::make_unique<FirstDispatchEvent>();
        auto& event=*slot->firstDispatch;
        event.moduleId=slot->p->moduleId;
        event.frameId=slot->p->frameId;
        event.sampleUs=slot->p->sampleUs;
        event.readyUs=slot->p->readyUs;
        event.handoff=std::move(handoff);
        event.admissionPacingDueUs=(allowedTick_+rate_-1)/rate_;
        event.previousAtSubmit=previousDispatch_;
        slot->countsAtSubmit=dispatchCounts_;
        // The selector only considers the oldest unfinished slot in each module.
        // Capture those two competitors before inserting this frame.
        for(std::size_t i=0;i<caches_.size();++i){
            for(const auto& candidate:caches_[i]){
                if(candidate->finalized&&!candidate->duplicatePending)continue;
                const auto& frame=*candidate->p;
                event.headsAtSubmit[i]={frame.moduleId,frame.frameId,
                    frame.sampleUs+(frame.moduleId==NdiModule?15000u:18000u),candidate->next,
                    frame.payload->packets.size()-candidate->next,candidate->duplicatePending};
                break;
            }
        }
    }
    cache.push_back(slot);
    ++stats_[id].accepted;stats_[id].cacheHighWater=std::max<std::uint64_t>(stats_[id].cacheHighWater,cache.size());
}
bool Engine::submitStatus(Bytes payload,std::uint64_t when) {
    if(payload.size()>1444||statuses_.size()>=64){++statusDropped_;return false;}
    Status s;s.sequence=nextStatus_++;s.timestampUs=when;s.payload=std::move(payload);
    statuses_.push_back({encodeStatus(s),when,s.sequence});++statusAccepted_;statusHigh_=std::max<std::uint64_t>(statusHigh_,statuses_.size());return true;
}
void Engine::feedback(const std::uint8_t* p,std::size_t n,std::uint64_t now) {
    try {
        const auto nack=decodeNack(p,n);auto& m=stats_[nack.moduleId-1];++m.nacks;
        auto& cache=caches_[nack.moduleId-1];auto it=std::find_if(cache.begin(),cache.end(),[&](const auto&s){return s->p->frameId==nack.frameId;});
        if(it==cache.end()){++m.cacheMisses;return;}
        try{validateIndices(nack,rt::fragmentCount((*it)->p->payload->byteSize));}catch(const ProtocolError&){++m.invalidNacks;return;}
        m.requestedFragments+=nack.indices.size();
        for(auto index:nack.indices){
            auto& slot=**it;
            if(slot.pendingRetries.count(index)){++m.duplicatePending;continue;}
            if(retries_.size()>=4096){++m.retryQueueDrops;continue;}
            slot.pendingRetries.insert(index);retries_.push_back({*it,index,now});
        }
        const auto modulePending=std::count_if(retries_.begin(),retries_.end(),[&](const Retry&r){return r.slot->p->moduleId==nack.moduleId;});
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
    if(s.finalized)return;s.finalized=true;auto& m=stats_[s.p->moduleId-1];
    const auto elapsed=end>=s.p->sampleUs?end-s.p->sampleUs:0;
    const auto limit=s.p->moduleId==1?15000ULL:18000ULL;
    if(s.successful==s.p->payload->packets.size()){++m.initialComplete;m.submitted.add(*s.p);duration_[s.p->moduleId-1].add(elapsed,limit);}
    else ++m.initialIncomplete;
    if(s.fragmentTrace)finishFragmentSpan(s);
    if(elapsed>limit){
        ++m.deadlineMisses;
        const auto& p=*s.p;
        InitialDeadlineEvent event{p.moduleId,p.frameId,p.plannedSampleUs,p.sampleUs,
            p.readyUs,s.firstAttemptUs,end,p.sampleUs+limit,s.successful==p.payload->packets.size(),{}};
        if(s.firstDispatch)event.firstDispatch=*s.firstDispatch;
        if(initialDeadlineEvents_.size()<32){
            initialDeadlineEvents_.push_back(event);
        }else{
            auto smallest=std::min_element(initialDeadlineEvents_.begin(),initialDeadlineEvents_.end(),
                [](const auto& left,const auto& right){return left.overrunUs()<right.overrunUs();});
            if(event.overrunUs()>smallest->overrunUs())*smallest=event;
        }
    }
}
void Engine::recordFragment(Slot& slot,FragmentInterval row){
    auto& span=*slot.fragmentTrace;
    if(span.fragments.empty()){
        span.firstAttemptUs=row.attemptUs;
        slot.fragmentCountsAtFirst=fragmentCounts_;
    }else{
        const auto previousEnd=span.fragments.back().endUs;
        row.pacingGapUs=std::clamp(row.pacingDueUs,previousEnd,row.attemptUs)-previousEnd;
        span.pacingGapUs+=row.pacingGapUs;
    }
    if(row.called){
        if(span.callCount==0)span.beforeFirstCallUs=row.callStartUs-span.firstAttemptUs;
        else span.betweenCallsUs+=row.callStartUs-slot.lastCallEndUs;
        span.ownCallUs+=row.endUs-row.callStartUs;
        slot.lastCallEndUs=row.endUs;
        ++span.callCount;
    }
    span.fragments.push_back(row);
}
void Engine::finishFragmentSpan(Slot& slot){
    auto& span=*slot.fragmentTrace;
    span.endUs=span.fragments.back().endUs;
    span.complete=slot.successful==slot.p->payload->packets.size();
    // TimingHistogram counts values strictly above its limit. Include exactly 18 ms.
    fragmentSpanHistograms_[span.moduleId-1].add(span.spanUs(),17999);
    if(span.callCount)span.afterLastCallUs=span.endUs-slot.lastCallEndUs;
    else span.beforeFirstCallUs=span.spanUs();
    const auto& atFirst=slot.fragmentCountsAtFirst;
    span.intervening={fragmentCounts_.ndi-atFirst.ndi,fragmentCounts_.ep-atFirst.ep,
        fragmentCounts_.status-atFirst.status,fragmentCounts_.retry-atFirst.retry,
        fragmentCounts_.duplicate-atFirst.duplicate};
    // Counts are updated after each dispatch. Remove this frame's originals
    // up to, but excluding, the current (last) dispatch.
    auto& sameModule=span.moduleId==NdiModule?span.intervening.ndi:span.intervening.ep;
    sameModule-=span.fragments.size()-1;

    slot.finishedSpan=std::move(slot.fragmentTrace);
    auto& worst=worstFragmentSpan_[span.moduleId-1];
    if(!worst||span.spanUs()>worst->spanUs())worst=slot.finishedSpan;
    if(span.moduleId==EpModule){
        if(pendingNdiWaitEp_.lock().get()==&slot){
            worstNdiWaitEp_=slot.finishedSpan;
            pendingNdiWaitEp_.reset();
        }
        return;
    }
    if(!worstNdiReadyToEnd_||span.readyToEndUs()>worstNdiReadyToEnd_->readyToEndUs())
        worstNdiReadyToEnd_=slot.finishedSpan;
    if(worstNdiWait_&&span.readyToFirstUs()<=worstNdiWait_->readyToFirstUs())return;
    worstNdiWait_=slot.finishedSpan;
    worstNdiWaitEp_.reset();
    pendingNdiWaitEp_.reset();
    // A cached EP frame may finish on either side of this NDI dispatch. Only
    // intervals already observed can establish an overlap with NDI's wait.
    std::uint64_t longestOverlap=0;
    for(const auto& ep:caches_[EpModule-1]){
        const auto* observed=ep->finishedSpan?ep->finishedSpan.get():ep->fragmentTrace.get();
        if(!observed||observed->fragments.empty())continue;
        const auto begin=std::max(span.readyUs,observed->firstAttemptUs);
        const auto end=std::min(span.firstAttemptUs,observed->fragments.back().endUs);
        if(end<=begin||end-begin<=longestOverlap)continue;
        longestOverlap=end-begin;
        worstNdiWaitEp_=ep->finishedSpan;
        pendingNdiWaitEp_=ep;
    }
}
void Engine::recordFirstDispatch(Slot& s,std::uint64_t pacingDueUs){
    auto& event=*s.firstDispatch;
    event.firstAttemptUs=s.firstAttemptUs;
    event.pacingDueUs=pacingDueUs;
    event.previousAtFirst=previousDispatch_;
    const auto& atSubmit=s.countsAtSubmit;
    event.intervening={dispatchCounts_.ndi-atSubmit.ndi,dispatchCounts_.ep-atSubmit.ep,
        dispatchCounts_.status-atSubmit.status,dispatchCounts_.retry-atSubmit.retry};
    const auto i=event.moduleId-1;
    const auto delay=event.readyToFirstUs();
    readyToFirst_[i].add(delay);
    auto& events=worstFirstDispatch_[i];
    if(events.size()<32){
        events.push_back(event);
        return;
    }
    auto smallest=std::min_element(events.begin(),events.end(),
        [](const auto& left,const auto& right){return left.readyToFirstUs()<right.readyToFirstUs();});
    if(delay>smallest->readyToFirstUs())*smallest=event;
}
void Engine::recordEvent(const PacketEvent& event){
    if(worst_.size()<MaxPacketEvents){
        if(!worst_.empty()&&event.scoreUs<worst_[lowestPacketEvent_].scoreUs)
            lowestPacketEvent_=worst_.size();
        worst_.push_back(event);
        return;
    }
    if(event.scoreUs<=worst_[lowestPacketEvent_].scoreUs)return;
    worst_[lowestPacketEvent_]=event;
    // Replace in place and keep the first equal minimum for the next replacement.
    const auto lowest=std::min_element(worst_.begin(),worst_.end(),
        [](const auto& a,const auto& b){return a.scoreUs<b.scoreUs;});
    lowestPacketEvent_=static_cast<std::size_t>(lowest-worst_.begin());
}
bool Engine::canServeNdiAtEpBoundary(const Slot& ndi,const Slot& ep,std::uint64_t now)const{
    if(ep.yieldedToNdi||ep.next<2||ep.successful!=ep.next||ndi.next||
       ndi.p->payload->packets.size()!=1||config_.faults.any())return false;
    const auto ndiDeadline=ndi.p->sampleUs+15000;
    if(now>=ndiDeadline||now<=ep.firstAttemptUs)return false;
    // A newer planned sample must not advance the unfinished EP's original
    // global timeline. Wait for an eligible packet boundary on that timeline.
    if(originalTick_/rate_<ndi.p->plannedSampleUs)return false;

    // One estimate covers the whole observed prefix; the other accounts for
    // the most recent service interval, including the previous transport call.
    const auto averageUs=(now-ep.firstAttemptUs+ep.next-1)/ep.next;
    const auto recentUs=now-lastStart_[EpModule-1];
    const auto serviceUs=std::max<std::uint64_t>(averageUs,recentUs);
    const auto wireUs=[this](std::uint64_t bytes){return (bytes*8+rate_-1)/rate_;};
    const auto ndiCost=std::max(serviceUs,wireUs(ndi.p->payload->packets.front().size+66));
    if(ndiCost>ndiDeadline-now)return false;
    const auto epDeadline=ep.p->sampleUs+18000;
    if(now>=epDeadline)return true; // Only NDI can still meet its deadline.

    const auto& payload=*ep.p->payload;
    const auto remaining=payload.packets.size()-ep.next;
    const auto remainingBytes=payload.byteSize-ep.next*rt::PayloadStep;
    const auto epWireUs=wireUs(remainingBytes+remaining*(rt::Overhead+66));
    // Keep one millisecond of EP margin in addition to the single NDI service
    // budget. Use the slower of observed service and the active wire budget.
    constexpr std::uint64_t marginUs=1000;
    const auto available=epDeadline-now;
    if(available<=marginUs||ndiCost>available-marginUs)return false;
    const auto leftover=available-marginUs-ndiCost;
    return epWireUs<=leftover&&serviceUs<=leftover/remaining;
}
bool Engine::step(std::uint64_t now,const SendFunction& send,const std::function<std::uint64_t()>& clock) {
    // Selection below already tests whether any packet is pending.
    const auto adjusted=(allowedTick_+rate_-1)/rate_;
    if(now<adjusted)return false;
    std::shared_ptr<Slot> selected;std::uint16_t index=0;bool retry=false,status=false,duplicate=false;
    std::uint64_t requested=0,anchor=now;
    if(!statuses_.empty()){status=true;anchor=statuses_.front().confirmedUs;}
    else {
        std::uint64_t earliest=UINT64_MAX;
        // The cache owns this head throughout selection on the sender thread.
        const std::shared_ptr<Slot>* ndiHead=nullptr;
        for(auto& cache:caches_)for(auto& s:cache)if(!s->finalized||s->duplicatePending){
            if(s->p->moduleId==NdiModule)ndiHead=&s;
            const auto deadline=s->p->sampleUs+(s->p->moduleId==1?15000:18000);
            if(deadline<earliest){earliest=deadline;selected=s;}
            break;
        }
        if(selected){
            if(ndiHead&&selected->p->moduleId==EpModule&&canServeNdiAtEpBoundary(**ndiHead,*selected,now)){
                selected->yieldedToNdi=true;
                selected=*ndiHead;
            }
            duplicate=selected->duplicatePending;
            if(duplicate)index=selected->duplicateIndex;
            else index=static_cast<std::uint16_t>(config_.faults.reverseInitial&&selected->p->moduleId==config_.faultModule?selected->p->payload->packets.size()-1-selected->next:selected->next);
            anchor=selected->p->plannedSampleUs;
        }
        else if(!retries_.empty()){
            const auto& r=retries_.front();selected=r.slot;index=r.index;retry=true;requested=r.requestedUs;anchor=requested;
            const auto cost=(selected->p->payload->packets[index].size+66)*8ULL;
            // Retries may not occupy the next known source's initial opportunity.
            for(const auto due:nextSample_){
                // Reserve the upcoming sample boundary. Once due, the source may
                // be blocked in a device read. Ready frames win on the next step.
                if(due>now && now+(cost+rate_-1)/rate_>=due)return false;
            }
        }else return false;
    }
    if(selected&&selected->firstDispatch&&!selected->next&&!retry&&!duplicate)
        selected->firstDispatch->selectedUs=clock();
    Datagram packet=status?statuses_.front().packet:selected->p->packet(index,retry);
    // Status and retries may delay DATA, but never move the retained initial plan forward.
    const auto original=(status||retry)?anchor*rate_:std::max(originalTick_,anchor*rate_);
    const auto plan=(original+rate_-1)/rate_;
    const auto start=clock();bool dropped=false,success=false;
    const auto module=status?3:selected->p->moduleId;
    const bool inject=!status&&module==config_.faultModule;
    const bool traceOriginal=config_.fragmentTrace&&!status&&!retry&&!duplicate;
    if(inject&&(config_.faults.dropAlways.count(index)||(!retry&&config_.faults.dropFirst.count(index))))dropped=true;
    if(inject&&!retry&&config_.faults.corruptFirst.count(index))packet.bytes[packet.size-1]^=0x80;
    std::uint64_t callStart=0,end;
    if(dropped){
        end=clock();
    }else{
        if(traceOriginal)callStart=clock();
        success=send(packet.bytes.data(),packet.size);
        end=clock();
    }
    const auto cost=(packet.size+66)*8ULL;
    if(traceOriginal)recordFragment(*selected,{index,plan,adjusted,start,callStart,end,0,!dropped,success});
    if(!status&&!retry)originalTick_=original+cost;
    // At most one datagram of catch-up credit. Long stalls cannot release an entire frame burst.
    const auto actual=start*rate_;const auto floor=actual>cost?actual-cost:0;
    allowedTick_=std::max(allowedTick_,floor)+cost;
    GapPacket current;
    if(config_.fragmentTrace && !dropped){
        current={static_cast<std::uint8_t>(module),status?statuses_.front().sequence:selected->p->frameId,index,
                 status?"status":retry?"retry":duplicate?"duplicate":"data",plan,start,end,success};
    }
    // Consume the exact interval already passed to each histogram. On ties
    // retain the first pair, including a valid first observation of zero.
    auto retainGap=[&](unsigned scope,std::uint64_t gap,std::uint64_t expected,std::uint64_t deviation){
        auto& worst=worstPacketGaps_[scope];
        if(!worst.current.kind || deviation>worst.deviationUs)
            worst={previousGapPacket_[scope],current,gap,expected,deviation};
    };
    if(lastGlobalStart_&&!dropped){
        const auto gap=start-lastGlobalStart_;
        const auto expected=plan>=lastGlobalPlan_?plan-lastGlobalPlan_:0;
        const auto dev=absDiff(gap,expected);
        globalGaps_.add(gap);globalDeviation_.add(dev,999);if(dev>=1000)++globalViolations_;
        if(config_.fragmentTrace)retainGap(0,gap,expected,dev);
    }
    if(!dropped){
        lastGlobalStart_=start;lastGlobalPlan_=plan;
        if(config_.fragmentTrace)previousGapPacket_[0]=current;
    }
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
            if(!selected->next){
                selected->firstAttemptUs=start;
                if(selected->firstDispatch)recordFirstDispatch(*selected,adjusted);
            }
            if(lastStart_[module-1]&&!dropped){
                const auto gap=start-lastStart_[module-1];
                const auto expected=plan>=lastPlan_[module-1]?plan-lastPlan_[module-1]:0;
                const auto dev=absDiff(gap,expected);
                gaps_[module-1].add(gap);gapDeviation_[module-1].add(dev,999);if(dev>=1000)++m.gapViolations;
                if(config_.fragmentTrace)retainGap(module,gap,expected,dev);
            }
            if(!dropped){
                lastStart_[module-1]=start;lastPlan_[module-1]=plan;
                if(config_.fragmentTrace)previousGapPacket_[module]=current;
            }
            planLateness_[module-1].add(late,999);if(late>=1000)++m.originalPlanViolations;
            ++selected->next;if(success)++selected->successful;
            if(inject&&config_.faults.duplicateFirst.count(index)){selected->duplicatePending=true;selected->duplicateIndex=index;}
            if(selected->next==selected->p->payload->packets.size())finalize(*selected,end);
        }
        event.frameId=selected->p->frameId;event.index=index;event.plannedSampleUs=selected->p->plannedSampleUs;event.sampleUs=selected->p->sampleUs;event.readyUs=selected->p->readyUs;
        event.deadlineUs=selected->p->sampleUs+(module==1?15000:18000);event.scoreUs=std::max(late,end>event.deadlineUs?end-event.deadlineUs:0);
    }
    if(config_.fragmentTrace){
        if(status)++fragmentCounts_.status;
        else if(retry)++fragmentCounts_.retry;
        else if(duplicate)++fragmentCounts_.duplicate;
        else if(module==NdiModule)++fragmentCounts_.ndi;
        else ++fragmentCounts_.ep;
    }
    if(config_.handoffTrace){
        // Count attempted dispatches, including fault-injected drops. An injected
        // duplicate counts as DATA for its module, and retries have their own counter.
        if(status)++dispatchCounts_.status;
        else if(retry)++dispatchCounts_.retry;
        else if(module==NdiModule)++dispatchCounts_.ndi;
        else ++dispatchCounts_.ep;
        previousDispatch_={event.moduleId,event.frameId,event.index,start,end,retry};
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
    if(config_.handoffTrace){
        for(std::size_t i=0;i<readyToFirst_.size();++i)s.modules[i].readyToFirst=readyToFirst_[i].summary();
        s.worstFirstDispatch=worstFirstDispatch_;
    }
    s.worstFragmentSpan=worstFragmentSpan_;
    if(config_.fragmentTrace)s.worstPacketGaps=worstPacketGaps_;
    for(std::size_t i=0;i<fragmentSpanHistograms_.size();++i)
        s.fragmentSpanDistribution[i]=fragmentSpanHistograms_[i].summary();
    s.worstNdiWait=worstNdiWait_;
    s.worstNdiWaitEp=worstNdiWaitEp_;
    s.worstNdiReadyToEnd=worstNdiReadyToEnd_;
    s.initialDeadlineEvents=initialDeadlineEvents_;return s;
}
}
