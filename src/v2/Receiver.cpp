#include "v2/Receiver.h"
#include "protocol/Checksum.h"
#include <algorithm>
#include <chrono>
#include <sstream>

namespace rt::v2 {
Receiver::Receiver(bool decodeAurora,Clock clock):decodeAurora_(decodeAurora),clock_(std::move(clock)){
    if(!clock_)clock_=[] {return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());};
}
void Receiver::reset(){state_={};report_={};}
void Receiver::retire(std::size_t module,std::uint32_t id){
    auto& ids=state_[module].retired;ids.push_back(id);if(ids.size()>32)ids.pop_front();
}
void Receiver::complete(std::size_t module,Assembly& a,std::uint64_t now){
    auto& stats=report_.modules[module];++stats.completed;
    ++stats.ledger.count;stats.ledger.idSum+=a.frame.frameId;
    stats.ledger.crcSum+=rt::crc32(a.frame.bytes);stats.ledger.stampSum+=a.frame.timestampUs;
    rt::aurora::Reply ndi;
    bool valid=true;
    try{if(module==0&&decodeAurora_)ndi=rt::aurora::decodeBx(a.frame.bytes);}
    catch(const std::exception&){++stats.businessErrors;valid=false;}
    const auto completed=std::max(now,clock_());
    stats.maxCompletionUs=std::max(stats.maxCompletionUs,completed-a.firstUs);
    if(!valid)return;
    auto& last=report_.latest[module];
    if(last.frame&&(!rt::newerId(a.frame.frameId,last.frame->frameId)||a.frame.timestampUs<last.frame->timestampUs)){++stats.late;return;}
    last.frame=std::make_shared<const Frame>(std::move(a.frame));last.hostCompletedUs=completed;last.stale=false;
    if(module==0&&decodeAurora_)report_.ndi=std::move(ndi);
    ++stats.consumed;
    auto& times=state_[module].consumedTimes;
    times.emplace_back(last.frame->frameId,last.frame->timestampUs);
    if(times.size()>4)times.pop_front();
    if(frameConsumer_)frameConsumer_(*last.frame,completed);
}
void Receiver::receive(const std::uint8_t* data,std::size_t size,std::uint64_t now){
    ingest(data,size,now);
}
std::vector<rt::Datagram> Receiver::receiveAndPollObserved(const std::uint8_t* data,std::size_t size,std::uint64_t now){
    const auto observed=ingest(data,size,now);
    if(!observed)return {};
    return pollRequests(std::max(now,clock_()),observed);
}
std::optional<std::uint8_t> Receiver::ingest(const std::uint8_t* data,std::size_t size,std::uint64_t now){
    std::optional<std::uint8_t> observed;
    try{
        if(!data||size<8)throw rt::ProtocolError("null or short packet");
        if(data[6]==3){
            const auto status=decodeStatus(data,size);
            if(report_.hasStatus&&!rt::newerId(status.sequence,report_.status.sequence)){++report_.statusOld;return observed;}
            report_.status=status;report_.hasStatus=true;++report_.statusApplied;
            report_.statusAppliedUs=std::max(now,clock_());
            if(statusConsumer_)statusConsumer_(status,report_.statusAppliedUs);return observed;
        }
        if(data[6]==2){decodeNack(data,size);++report_.unexpectedNacks;return observed;}
        const auto p=decodeData(data,size);const std::size_t mi=p.moduleId-1;
        auto& module=state_[mi];auto& stats=report_.modules[mi];
        ++stats.datagrams;stats.bytes+=size;if(p.retransmission)++stats.retries;
        if(std::find(module.retired.begin(),module.retired.end(),p.frameId)!=module.retired.end()){++stats.late;return observed;}
        auto found=std::find_if(module.active.begin(),module.active.end(),[&](const auto& a){return a.frame.frameId==p.frameId;});
        if(found==module.active.end()){
            const bool advancesWindow=!module.highest||rt::newerId(p.frameId,*module.highest);
            if(!advancesWindow&&static_cast<std::uint32_t>(*module.highest-p.frameId)>=4){++stats.late;return observed;}
            if(module.active.size()==4){
                const auto newest=advancesWindow?p.frameId:*module.highest;
                const auto oldest=std::max_element(module.active.begin(),module.active.end(),[newest](const Assembly& left,const Assembly& right){
                    return static_cast<std::uint32_t>(newest-left.frame.frameId)<static_cast<std::uint32_t>(newest-right.frame.frameId);
                });
                retire(mi,oldest->frame.frameId);
                module.active.erase(oldest);
                ++stats.expired;
            }
            if(advancesWindow){
                if(module.highest)stats.wholeMissing+=static_cast<std::uint32_t>(p.frameId-*module.highest)-1;
                module.highest=p.frameId;
                for(auto& old:module.active)if(rt::newerId(p.frameId,old.frame.frameId)){old.tail=true;if(!old.discovered){old.discovered=true;old.nextRequestUs=now;++stats.tailTriggers;observed=p.moduleId;}}
            }
            Assembly a;a.frame.moduleId=p.moduleId;a.frame.frameId=p.frameId;a.frame.timestampUs=p.timestampUs;a.frame.bytes.resize(p.frameLength);
            a.seen.resize(p.fragmentCount,false);a.firstUs=a.lastUs=now;
            module.active.push_back(std::move(a));found=std::prev(module.active.end());
            stats.highWaterFrames=std::max(stats.highWaterFrames,module.active.size());
        }
        auto& a=*found;
        if(a.frame.bytes.size()!=p.frameLength||a.seen.size()!=p.fragmentCount||a.frame.timestampUs!=p.timestampUs){
            ++stats.conflicts;retire(mi,p.frameId);module.active.erase(found);return observed;
        }
        if(a.seen[p.fragmentIndex]){++stats.duplicates;return observed;}
        std::copy(p.payload.begin(),p.payload.end(),a.frame.bytes.begin()+p.fragmentIndex*rt::PayloadStep);
        a.seen[p.fragmentIndex]=true;++a.received;a.lastUs=now;a.highest=std::max(a.highest,p.fragmentIndex);
        while(a.contiguous<a.seen.size()&&a.seen[a.contiguous])++a.contiguous;
        if(a.contiguous<a.highest&&!a.discovered){a.discovered=true;a.nextRequestUs=now;++stats.gapTriggers;observed=p.moduleId;}
        if(a.received==a.seen.size()){complete(mi,a,now);retire(mi,p.frameId);module.active.erase(found);}
    }catch(const rt::ProtocolError&){++report_.invalidPackets;}
    return observed;
}
std::vector<rt::Datagram> Receiver::poll(std::uint64_t now){
    return pollRequests(now,std::nullopt);
}
std::vector<rt::Datagram> Receiver::pollRequests(std::uint64_t now,std::optional<std::uint8_t> observedModule){
    std::vector<rt::Datagram> packets;
    for(std::size_t mi=0;mi<2;++mi){
        if(observedModule&&mi+1!=*observedModule)continue;
        auto& module=state_[mi];auto& stats=report_.modules[mi];
        for(auto i=module.active.begin();i!=module.active.end();){
            auto& a=*i;
            if(observedModule){
                if(!a.discovered||a.rounds){++i;continue;}
            }else if(now>=a.lastUs+2000&&!a.tail){a.tail=true;++stats.tailTriggers;if(!a.discovered){a.discovered=true;a.nextRequestUs=a.lastUs+2000;}}
            if(!a.discovered||now<a.nextRequestUs){++i;continue;}
            if(a.rounds==3){++stats.expired;retire(mi,a.frame.frameId);i=module.active.erase(i);continue;}
            Nack request;request.moduleId=static_cast<std::uint8_t>(mi+1);request.frameId=a.frame.frameId;
            for(std::size_t index=0;index<a.seen.size();++index)if(!a.seen[index]&&(a.tail||index<=a.highest))request.indices.push_back(static_cast<std::uint16_t>(index));
            if(request.indices.empty()){++i;continue;}
            stats.maxNackLatenessUs=std::max(stats.maxNackLatenessUs,now-a.nextRequestUs);
            if(a.rounds){const auto interval=now-a.lastRequestUs;
                stats.maxRetryIntervalUs=std::max(stats.maxRetryIntervalUs,interval);
                if(!stats.retryIntervalCount||interval<stats.minRetryIntervalUs)stats.minRetryIntervalUs=interval;
                ++stats.retryIntervalCount;}
            for(std::size_t start=0;start<request.indices.size();start+=724){
                Nack part;part.moduleId=request.moduleId;part.frameId=request.frameId;
                part.indices.assign(request.indices.begin()+start,request.indices.begin()+std::min(start+724,request.indices.size()));
                packets.push_back(encodeNack(part));++stats.nackPackets;stats.requestedFragments+=part.indices.size();
            }
            ++a.rounds;++stats.nackRounds;a.lastRequestUs=now;a.nextRequestUs=now+10000;++i;
        }
    }
    return packets;
}
ReceiverSnapshot Receiver::snapshot(std::uint64_t now)const{
    auto result=report_;
    for(std::size_t mi=0;mi<2;++mi){result.modules[mi].activeFrames=state_[mi].active.size();auto& v=result.latest[mi];v.stale=!v.frame||now>v.hostCompletedUs+(mi?20000:16667);}
    const auto& a=result.latest[0];const auto& b=result.latest[1];
    if(a.frame&&b.frame){
        result.associatedNdiId=a.frame->frameId;result.nearestTimestampDeltaUs=~std::uint64_t{0};
        for(const auto& candidate:state_[1].consumedTimes){const auto x=a.frame->timestampUs,y=candidate.second;const auto delta=x>y?x-y:y-x;
            if(delta<=result.nearestTimestampDeltaUs){result.nearestTimestampDeltaUs=delta;result.associatedEpId=candidate.first;}}
        result.associationAvailable=!a.stale&&!b.stale&&result.nearestTimestampDeltaUs<=20000;
    }
    return result;
}
std::string receiverJson(const ReceiverSnapshot& s){
    std::ostringstream o;o<<"{\"kind\":\"v2_receiver\",\"protocol_version\":2,\"report_schema_version\":1,\"receiver_schema_version\":1,\"hardware_clock_verified\":false,\"invalid_packets\":"<<s.invalidPackets<<",\"unexpected_nacks\":"<<s.unexpectedNacks<<",\"status_applied\":"<<s.statusApplied<<",\"status_old\":"<<s.statusOld<<",\"modules\":{";
    for(std::size_t mi=0;mi<2;++mi){if(mi)o<<',';const auto&m=s.modules[mi];o<<'"'<<(mi+1)<<"\":{";
#define FIELD(name) o<<"\"" #name "\":"<<m.name<<','
        FIELD(datagrams);FIELD(bytes);FIELD(completed);FIELD(consumed);FIELD(duplicates);FIELD(late);FIELD(conflicts);FIELD(expired);FIELD(wholeMissing);FIELD(businessErrors);FIELD(retries);FIELD(nackRounds);FIELD(nackPackets);FIELD(requestedFragments);FIELD(tailTriggers);FIELD(gapTriggers);FIELD(maxCompletionUs);FIELD(maxNackLatenessUs);FIELD(maxRetryIntervalUs);FIELD(minRetryIntervalUs);FIELD(retryIntervalCount);FIELD(activeFrames);
#undef FIELD
        o<<"\"highWaterFrames\":"<<m.highWaterFrames<<",\"business_errors\":"<<m.businessErrors<<",\"nack_rounds\":"<<m.nackRounds
         <<",\"retry_intervals\":{\"count\":"<<m.retryIntervalCount<<",\"min_us\":"<<m.minRetryIntervalUs<<",\"max_us\":"<<m.maxRetryIntervalUs<<"},\"request_lateness\":{\"count\":"<<m.nackRounds<<",\"max_us\":"<<m.maxNackLatenessUs<<"},\"ledger\":{\"count\":\""<<m.ledger.count<<"\",\"id_sum\":\""<<m.ledger.idSum<<"\",\"crc_sum\":\""<<m.ledger.crcSum<<"\",\"stamp_sum\":\""<<m.ledger.stampSum<<"\"}}";
    }
    o<<"},\"real_waveform_validation\":\"not_tested\",\"common_hardware_clock\":\"not_tested\",\"whole_missing_definition\":\"Forward ID gaps observed before late arrivals, not final loss count\",\"ledger_evidence\":\"Bounded additive summaries; matching sums do not prove bytewise equality of each frame\",\"acceptance\":[";
    bool first=true;
    auto row=[&](const char*id,unsigned module,const char*req,const char*measurement,const char*threshold,std::optional<std::uint64_t> measured,const char*status,const char*reason){
        if(!first)o<<',';first=false;
        o<<"{\"id\":\""<<id<<"\",\"module_id\":"<<module<<",\"requirement\":\""<<req<<"\",\"measurement\":\""<<measurement<<"\",\"threshold\":\""<<threshold<<"\",\"measured\":";
        if(measured)o<<*measured;else o<<"null";
        o<<",\"status\":\""<<status<<"\",\"reason\":\""<<reason<<"\"}";
    };
    for(unsigned i=0;i<2;++i){const auto&m=s.modules[i];
        row("receiver_consume",i+1,"Independent frame consumption","Validated complete frames versus consumed frames; business decode errors","All completed frames consumed; zero business errors",m.consumed,
            !m.completed?"not_tested":(m.completed==m.consumed&&!m.businessErrors?"pass":"fail"),"NDI requires BX CRC16 and record parsing; EP is an opaque business callback, not validated real waveform");
        const auto intervalDeviation=[](std::uint64_t us){return us>10000?us-10000:10000-us;};
        const bool lateRequest=m.nackRounds&&m.maxNackLatenessUs!=0;
        const auto worstIntervalDeviation=std::max(intervalDeviation(m.minRetryIntervalUs),intervalDeviation(m.maxRetryIntervalUs));
        const bool wrongInterval=m.retryIntervalCount&&worstIntervalDeviation!=0;
        const bool bothObserved=m.nackRounds&&m.retryIntervalCount;
        row("receiver_retry_timing",i+1,"Observed NACK generation timing","Generated-request lateness and adjacent retry intervals","Zero original-plan lateness; subsequent intervals exactly 10000 us",m.maxNackLatenessUs,
            lateRequest||wrongInterval?"fail":bothObserved?"pass":"not_tested","See separate original-plan lateness and inter-round interval measurements.");
        row("receiver_request_lateness",i+1,"Observed NACK request lateness","Maximum time from scheduled request to generation in host microseconds","0 us",
            m.nackRounds?std::optional{m.maxNackLatenessUs}:std::nullopt,
            !m.nackRounds?"not_tested":lateRequest?"fail":"pass",m.nackRounds?"Includes the first request and all subsequent rounds.":"No NACK request was generated.");
        row("receiver_retry_interval",i+1,"Observed NACK retry interval","Maximum absolute deviation from the 10000 us inter-round interval","0 us deviation",
            m.retryIntervalCount?std::optional{worstIntervalDeviation}:std::nullopt,
            !m.retryIntervalCount?"not_tested":wrongInterval?"fail":"pass",m.retryIntervalCount?"Raw interval extrema and sample count are retained in retry_intervals.":"No subsequent request for the same frame was generated.");
        row("receiver_joint_delivery",i+1,"Paired source and receiver delivery","Same isolated run and endpoint; built/completed ledgers","All frame quantities and summaries match",m.completed,"not_tested","Requires a paired sender report and run-scoped baseline; cumulative receiver counts alone cannot establish delivery");
        row("receiver_hardware_time",i+1,"Verified common hardware timestamps","Source clock calibration and hardware acquisition","Verified shared microsecond domain",0,"not_tested","Host monotonic receive time and numeric timestamp association do not verify common device time");
    }
    row("receiver_status_immediate",3,"Immediate full status application","Accepted sequence and host application time","Numerical immediate threshold unspecified",s.statusApplied,"not_tested","State application counted; no numerical deadline was invented");
    row("receiver_display",0,"Actual independent display","Consumption callback versus paint completion and data age","Measured render trace and real waveform contract",0,"not_tested","Receiver completion is not screen rendering; GUI must provide separate measured evidence");
    o<<"]}";return o.str();
}
}
