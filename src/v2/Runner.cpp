#include "v2/Runner.h"
#include "v2/Engine.h"
#include "sources/AuroraCodec.h"
#include "sources/AuroraSource.h"
#include "core/SamplingClock.h"
#include "protocol/PacketCodec.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

namespace rt::v2 {
namespace {
struct SourceStats {
    std::uint64_t sampled=0,built=0,skipped=0,queueDrops=0,errors=0,queueHighWater=0;
    std::uint64_t firstSampleUs=0,lastSampleUs=0,duplicateSource=0,prepareMaxUs=0;
    std::uint64_t newSourceFrames=0,sourceCounterGaps=0,sourceCounterResets=0,mixedCounterFrames=0;
    std::uint64_t semanticChanges=0;
    std::uint64_t unmappedSourceFrames=0,clockErrorBoundUs=0,cancelledAttempts=0;
    double actualDeviceHz=0,counterHz=0;
    bool commonClockMapped=false;
    std::uint32_t nextId=1;
    std::size_t payloadBytes=0;
    Ledger ledger;
    Distribution intervals,deviation,lateness;
    std::string error,kind="simulation";
};
struct SourceMailbox {
    std::mutex mutex;std::deque<PreparedPtr> queue;SourceStats stats;
    std::atomic<std::uint64_t> nextSample{0};std::atomic<bool> done{false};
};
struct State {
    Config config;Preflight capacity;LinkObservation link;
    std::array<SourceStats,2> sources;EngineSnapshot engine;
    std::uint64_t startedUs=0,endedUs=0,elapsedUs=0;
    std::uint16_t localPort=0;bool df=false,finished=false;
    std::string transport,error,endedReason;
    std::uint64_t statusIngressAccepted=0,statusIngressRejected=0,statusIngressCancelled=0,statusIngressPending=0;
};
std::string quote(const std::string& s){std::ostringstream o;o<<'"';for(unsigned char c:s){switch(c){case '"':o<<"\\\"";break;case '\\':o<<"\\\\";break;case '\n':o<<"\\n";break;case '\r':o<<"\\r";break;case '\t':o<<"\\t";break;default:if(c<32)o<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<unsigned(c)<<std::dec;else o<<c;}}o<<'"';return o.str();}
std::string distribution(const Distribution& d){std::ostringstream o;o<<"{\"count\":"<<d.count<<",\"max_us\":"<<d.maxUs<<",\"p50_us\":"<<d.p50Us<<",\"p95_us\":"<<d.p95Us<<",\"p99_us\":"<<d.p99Us<<",\"p999_us\":"<<d.p999Us<<",\"violations\":"<<d.overLimit<<'}';return o.str();}
std::string ledgerJson(const Ledger& l){std::ostringstream o;o<<"{\"count\":"<<l.count<<",\"id_sum\":\""<<l.idSum<<"\",\"crc_sum\":\""<<l.crcSum<<"\",\"stamp_sum\":\""<<l.stampSum<<"\"}";return o.str();}
std::string report(const State& s){
    std::ostringstream o;o<<std::setprecision(12);
    o<<"{\"kind\":\"v2_sender\",\"protocol_version\":2,\"report_schema_version\":1,\"architecture\":\"B: independent preparation workers and one UDP sender\",\"run_id\":"<<quote(s.config.runId)
     <<",\"finished\":"<<(s.finished?"true":"false")<<",\"error\":"<<quote(s.error)<<",\"ended_reason\":"<<quote(s.endedReason)
     <<",\"elapsed_us\":"<<s.elapsedUs<<",\"started_host_us\":\""<<s.startedUs<<"\",\"ended_host_us\":\""<<s.endedUs<<"\",\"transport\":"<<quote(s.transport)<<",\"local_port\":"<<s.localPort<<",\"df_enabled\":"<<(s.df?"true":"false")
     <<",\"config\":{\"network_profile\":"<<quote(networkProfileName(s.config.linkMbps))<<",\"selected_link_mbps\":"<<s.config.linkMbps<<",\"effective_link_mbps\":"<<s.engine.effectiveMbps
     <<",\"target_ip\":"<<quote(s.config.targetIp)<<",\"bind_ip\":"<<quote(s.config.bindIp)<<",\"target_port\":"<<s.config.targetPort<<",\"command_port\":"<<s.config.commandPort
     <<",\"configured_observed_limit_mbps\":"<<s.config.observedLinkMbps<<",\"path_limit_mbps\":"<<s.config.pathLimitMbps<<",\"ndi_tools\":"<<s.config.ndiTools<<",\"ep_bytes\":"<<s.config.epBytes
     <<",\"ndi_hz\":"<<s.config.ndiHz<<",\"ep_hz\":"<<s.config.epHz<<",\"ndi_enabled\":"<<(s.config.ndiEnabled?"true":"false")<<",\"ep_enabled\":"<<(s.config.epEnabled?"true":"false")
     <<",\"duration_us\":"<<s.config.durationUs<<",\"frame_limit\":"<<s.config.frameLimit<<",\"status_interval_us\":"<<s.config.statusIntervalUs<<",\"fault_injection\":"<<(s.config.faults.any()?"true":"false")<<",\"allow_oversubscribed\":"<<(s.config.allowOversubscribed?"true":"false")<<'}'
     <<",\"link_observation\":{\"local_link_mbps\":"<<s.link.mbps<<",\"loopback\":"<<(s.link.loopback?"true":"false")<<",\"physical_ethernet\":"<<(s.link.physical?"true":"false")<<",\"interface\":"<<quote(s.link.interfaceName)<<",\"reason\":"<<quote(s.link.reason)<<",\"observed_host_us\":\""<<s.link.observedAtUs<<"\"}"
     <<",\"preflight\":{\"feasible\":"<<(s.capacity.feasible?"true":"false")<<",\"formula\":\"IPv4 Ethernet: UDP bytes + 66, DATA frame bytes + 102 * fragments; no VLAN/tunnel\",\"utilization\":"<<s.capacity.utilization<<",\"ndi_wire_us\":"<<s.capacity.ndiWireUs<<",\"ep_wire_us\":"<<s.capacity.epWireUs<<",\"simultaneous_wire_us\":"<<s.capacity.synchronousWireUs<<",\"reason\":"<<quote(s.capacity.reason)<<'}'
     <<",\"clock\":{\"domain\":\"host_monotonic_simulation_or_playback\",\"hardware_verified\":false,\"timestamp_note\":\"Wire timestamp is explicitly simulated/playback time. Aurora device frame counter is retained inside BX; not an independently synchronized hardware clock.\"},\"modules\":{";
    for(std::size_t i=0;i<2;++i){if(i)o<<',';const auto& a=s.sources[i];const auto& m=s.engine.modules[i];
        o<<'"'<<(i+1)<<"\":{\"source_kind\":"<<quote(a.kind)<<",\"sampled\":"<<a.sampled<<",\"built\":"<<a.built<<",\"skipped_slots\":"<<a.skipped<<",\"queue_drops\":"<<a.queueDrops<<",\"source_errors\":"<<a.errors<<",\"source_error\":"<<quote(a.error)
         <<",\"queue_high_water\":"<<a.queueHighWater<<",\"payload_bytes\":"<<a.payloadBytes<<",\"next_frame_id\":"<<a.nextId<<",\"duplicate_source_frames\":"<<a.duplicateSource<<",\"prepare_max_us\":"<<a.prepareMaxUs
         <<",\"new_source_frames\":"<<a.newSourceFrames<<",\"source_counter_gaps\":"<<a.sourceCounterGaps<<",\"source_counter_resets\":"<<a.sourceCounterResets<<",\"mixed_counter_frames\":"<<a.mixedCounterFrames<<",\"semantic_changes\":"<<a.semanticChanges
         <<",\"unmapped_source_frames\":"<<a.unmappedSourceFrames<<",\"common_clock_mapped\":"<<(a.commonClockMapped?"true":"false")<<",\"clock_error_bound_us\":"<<a.clockErrorBoundUs<<",\"actual_device_hz\":"<<a.actualDeviceHz<<",\"counter_hz\":"<<a.counterHz
         <<",\"cancelled_attempts\":"<<a.cancelledAttempts<<",\"sampling_measurement\":"<<quote(a.kind=="device"?"host query start timing, not hardware sampling":"simulated/playback frame production timing")
         <<",\"sampling_span_us\":"<<(a.lastSampleUs>=a.firstSampleUs?a.lastSampleUs-a.firstSampleUs:0)<<",\"sampling_intervals\":"<<distribution(a.intervals)<<",\"sampling_deviation\":"<<distribution(a.deviation)<<",\"sampling_lateness\":"<<distribution(a.lateness)
         <<",\"accepted\":"<<m.accepted<<",\"initial_complete\":"<<m.initialComplete<<",\"initial_incomplete\":"<<m.initialIncomplete<<",\"deadline_misses\":"<<m.deadlineMisses<<",\"send_errors\":"<<m.sendErrors
         <<",\"datagrams\":"<<m.datagrams<<",\"bytes\":"<<m.bytes<<",\"retransmissions\":"<<m.retransmissions<<",\"unsent_at_stop\":"<<m.unsentAtStop<<",\"unsent_evicted\":"<<m.unsentEvicted<<",\"retry_cancelled\":"<<m.retryCancelled
         <<",\"nacks\":"<<m.nacks<<",\"invalid_nacks\":"<<m.invalidNacks<<",\"cache_misses\":"<<m.cacheMisses<<",\"requested_fragments\":"<<m.requestedFragments<<",\"duplicate_pending\":"<<m.duplicatePending<<",\"retry_queue_drops\":"<<m.retryQueueDrops<<",\"cache_high_water\":"<<m.cacheHighWater<<",\"retry_high_water\":"<<m.retryHighWater<<",\"injected_drops\":"<<m.injectedDrops
         <<",\"initial_duration\":"<<distribution(m.initialDuration)<<",\"packet_gaps\":"<<distribution(m.packetGaps)<<",\"packet_gap_deviation\":"<<distribution(m.packetGapDeviation)<<",\"original_plan_lateness\":"<<distribution(m.originalPlanLateness)<<",\"retry_wait\":"<<distribution(m.retryWait)
         <<",\"ledger\":"<<ledgerJson(a.ledger)<<",\"initial_submission_ledger\":"<<ledgerJson(m.submitted)<<'}';
    }
    o<<"},\"status\":{\"accepted\":"<<s.engine.statusAccepted<<",\"sent\":"<<s.engine.statusSent<<",\"rejected\":"<<s.engine.statusDropped<<",\"send_errors\":"<<s.engine.statusSendErrors<<",\"cancelled\":"<<s.engine.statusCancelled<<",\"queue_high_water\":"<<s.engine.statusQueueHighWater<<",\"maximum_submit_latency_us\":"<<s.engine.statusMaximumLatencyUs<<",\"next_sequence\":"<<s.engine.nextStatusSequence<<'}'
     <<",\"status_ingress\":{\"accepted\":"<<s.statusIngressAccepted<<",\"rejected\":"<<s.statusIngressRejected<<",\"cancelled\":"<<s.statusIngressCancelled<<",\"pending\":"<<s.statusIngressPending<<'}'
     <<",\"invalid_feedback\":"<<s.engine.invalidFeedback<<",\"feedback_source_rejected\":"<<s.engine.sourceRejected<<",\"rate_changes\":"<<s.engine.rateChanges<<",\"global_packet_gaps\":"<<distribution(s.engine.globalPacketGaps)<<",\"global_packet_gap_deviation\":"<<distribution(s.engine.globalPacketGapDeviation)
     <<",\"worst_packet_events\":[";
    for(std::size_t i=0;i<s.engine.worstEvents.size();++i){if(i)o<<',';const auto&e=s.engine.worstEvents[i];o<<"{\"module_id\":"<<unsigned(e.moduleId)<<",\"frame_id\":"<<e.frameId<<",\"fragment_index\":"<<e.index<<",\"planned_sample_us\":\""<<e.plannedSampleUs<<"\",\"actual_sample_us\":\""<<e.sampleUs<<"\",\"ready_us\":\""<<e.readyUs<<"\",\"original_due_us\":\""<<e.originalDueUs<<"\",\"adjusted_due_us\":\""<<e.adjustedDueUs<<"\",\"send_start_us\":\""<<e.sendStartUs<<"\",\"send_end_us\":\""<<e.sendEndUs<<"\",\"deadline_us\":\""<<e.deadlineUs<<"\",\"retry\":"<<(e.retry?"true":"false")<<",\"success\":"<<(e.success?"true":"false")<<'}';}
    o<<"],\"acceptance\":[";bool first=true;
    auto row=[&](const std::string& id,unsigned module,const std::string& req,const std::string& measurement,const std::string& threshold,const std::string& measured,const std::string& status,const std::string& reason){if(!first)o<<',';first=false;o<<"{\"id\":"<<quote(id)<<",\"module_id\":"<<module<<",\"requirement\":"<<quote(req)<<",\"measurement\":"<<quote(measurement)<<",\"threshold\":"<<quote(threshold)<<",\"measured\":"<<quote(measured)<<",\"status\":"<<quote(status)<<",\"reason\":"<<quote(reason)<<'}';};
    for(unsigned i=0;i<2;++i){const auto&a=s.sources[i];const auto&m=s.engine.modules[i];const bool enabled=i==0?s.config.ndiEnabled:s.config.epEnabled;const auto noData=!enabled||!a.sampled;const std::string name=i==0?"NDI":"EP";
        row("sampling_integrity",i+1,name+" source continuity","Original independent sample/query slots; no interpolation","0 skipped slots / queue loss / source error / unmapped device frame",std::to_string(a.skipped)+" skipped, "+std::to_string(a.queueDrops)+" queue drops, "+std::to_string(a.errors)+" errors, "+std::to_string(a.unmappedSourceFrames)+" unmapped",a.skipped||a.queueDrops||a.errors||a.unmappedSourceFrames?"fail":(noData?"not_tested":"pass"),"A source failure remains failed even before its first sample. Hardware sampling is separate.");
        row("sampling_jitter",i+1,name+" sampling deviation (retained engineering target)","Absolute actual production/query interval minus configured nominal period; all intervals","strictly < 1000 us",std::to_string(a.deviation.maxUs)+" us; "+std::to_string(a.deviation.overLimit)+" / "+std::to_string(a.deviation.count)+" violations",!a.deviation.count||a.kind=="device"?"not_tested":(a.deviation.maxUs>=1000?"fail":"pass"),"Device host query intervals cannot establish hardware sample jitter; the <1 ms engineering threshold is unchanged");
        row("initial_submission",i+1,name+" complete initial transmission","Simulation/playback sample to last local send return; device input retains host-request proxy separately",i==0?"<= 15000 us":"<= 18000 us",std::to_string(m.initialDuration.maxUs)+" us; "+std::to_string(m.deadlineMisses)+" late; "+std::to_string(m.initialIncomplete)+" incomplete",!a.built||a.kind=="device"?"not_tested":(m.deadlineMisses||m.initialIncomplete||m.initialComplete!=a.built?"fail":"pass"),"Local submission only; device common timestamp does not independently establish source-to-host clock offset or physical egress");
        row("packet_gap_jitter",i+1,name+" packet interval deviation (engineering target)","Actual first-copy interval minus original global scheduling plan interval","strictly < 1000 us",std::to_string(m.packetGapDeviation.maxUs)+" us; "+std::to_string(m.packetGapDeviation.overLimit)+" / "+std::to_string(m.packetGapDeviation.count)+" violations",!m.packetGapDeviation.count?"not_tested":(m.packetGapDeviation.maxUs>=1000?"fail":"pass"),"Full-population counters; original due time is never replaced by catch-up due time");
        row("hardware_sampling",i+1,name+" new hardware sampling and common timestamp","Device frame counter, calibrated clock mapping and measured source rate",i==0?"V2 ~60 Hz and verified common microsecond clock":"V2 50 Hz and verified common microsecond clock","No physical device timing evidence","not_tested","Simulation/playback cannot verify real Aurora 40/66 Hz mode or EP sampling clock");
        row("receiver_delivery",i+1,name+" complete receive and consume","Compare paired receiver report and content/frame ledgers","All built frames accounted for","Requires paired receiver report","not_tested","UDP send success alone is not delivery evidence");
        row("nack_timing",i+1,name+" receiver retry sequence","Receiver gap/tail detection and per-module request timestamps","2 ms tail; immediate first; 10 ms spacing; 3 rounds",std::to_string(m.nacks)+" NACK packets received","not_tested","Evaluate exact receiver timing and deterministic tests separately; sender cannot infer receiver's detection clock");
    }
    row("capacity",0,"Declared load capacity","Ethernet serialization feasibility screening; not performance measurement","Demand within effective rate and 15/18 ms windows",std::to_string(s.capacity.utilization*100)+" percent",s.capacity.feasible?"pass":"fail",s.capacity.reason);
    row("status_immediate",3,"Immediate complete status snapshot","Status confirmation to local socket submission","Numerical immediate bound unspecified",std::to_string(s.engine.statusMaximumLatencyUs)+" us; "+std::to_string(s.engine.statusSent)+" sent","not_tested","No invented timing threshold; status uses one packet and no NACK");
    row("physical_egress",0,"Actual network egress deadlines","Hardware NIC or external capture timestamps","NDI <=15 ms; EP <=18 ms","Unavailable","not_tested",s.link.reason);
    row("display",0,"Independent display and timestamp association","Receive-consume-render trace with common hardware time","No wait beyond a module period; real waveform contract required","Sender report has no render timestamps","not_tested","Use receiver GUI consumption evidence; no real EP layout or verified clock was supplied");
    o<<"]}";return o.str();
}
Bytes readFile(const std::string& path,std::size_t max){
    std::ifstream f(std::filesystem::u8path(path),std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("Cannot open replay file: "+path);
    const auto n=f.tellg();if(n<=0||static_cast<std::uint64_t>(n)>max)throw std::runtime_error("Replay payload size outside allowed range");
    Bytes b(static_cast<std::size_t>(n));f.seekg(0);if(!f.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(b.size())))throw std::runtime_error("Cannot read complete replay payload");return b;
}
}
struct Session::Impl {
    TransportFactory factory;rt::aurora::NdiSourceFactory ndiFactory;std::thread worker;
    std::atomic<bool> stopRequested{false},active{false};
    mutable std::mutex mutex;State state;
    std::mutex statusMutex;std::deque<std::pair<Bytes,std::uint64_t>> statuses;
    std::uint64_t acceptedStatuses=0,rejectedStatuses=0,cancelledStatuses=0;
    explicit Impl(TransportFactory f,rt::aurora::NdiSourceFactory n):factory(std::move(f)),ndiFactory(std::move(n)){}
    void execute(Config c);
    void source(Config c,unsigned id,std::uint64_t origin,SourceMailbox& mail,std::size_t ndiBytes,std::size_t epBytes);
};
void Session::Impl::source(Config c,unsigned id,std::uint64_t origin,SourceMailbox& mail,std::size_t ndiBytes,std::size_t epBytes){
    SourceStats stats;stats.nextId=id==1?c.initialNdiId:c.initialEpId;TimingHistogram interval,deviation,late;
    std::unique_ptr<rt::aurora::INdiSource> ndi;
    auto publish=[&]{stats.intervals=interval.summary();stats.deviation=deviation.summary();stats.lateness=late.summary();std::lock_guard<std::mutex> lock(mail.mutex);mail.stats=stats;};
    try{
        const bool enabled=id==1?c.ndiEnabled:c.epEnabled;if(!enabled){publish();mail.done=true;return;}
        const auto file=id==1?c.ndiReplayFile:c.epReplayFile;Bytes replay,ep;
        if(!file.empty()){stats.kind="replay";replay=readFile(file,id==1?16384:rt::MaxFrameBytes);if(id==1)rt::aurora::decodeBx(replay);}
        if(id==1){
            auto sourceFactory=ndiFactory?ndiFactory:(!replay.empty()?rt::aurora::replaySourceFactory(replay,c.ndiHz):rt::aurora::simulationSourceFactory(c.ndiTools,c.ndiHz));
            ndi=sourceFactory();if(!ndi)throw std::runtime_error("NDI factory returned no data source");
            stats.kind=ndi->kind();ndi->start(stopRequested);stats.actualDeviceHz=stats.kind=="device"?ndi->actualHz():0;stats.counterHz=ndi->counterHz();
        }
        if(id==2&&replay.empty()){ep.resize(c.epBytes);for(std::size_t j=0;j<ep.size();++j)ep[j]=static_cast<std::uint8_t>(j*17+3);}
        const std::size_t payload=file.empty()?(id==1?11+42*c.ndiTools:ep.size()):replay.size();stats.payloadBytes=payload;
        const auto check=preflight(c,id==1?payload:ndiBytes,id==2?payload:epBytes);if(!check.feasible&&!c.allowOversubscribed)throw std::runtime_error(check.reason);
        const auto hz=id==1?c.ndiHz:c.epHz;const auto phase=id==1?c.ndiPhaseUs:c.epPhaseUs;
        std::uint64_t slot=0,previous=0;
        auto advance=[&](std::uint64_t now,PreparedPtr p={}){
            auto next=slot+1;const auto latest=(now-origin-phase)*hz/1000000ULL+1;
            if(latest>next){stats.skipped+=latest-next;next=latest;}slot=next;
            stats.intervals=interval.summary();stats.deviation=deviation.summary();stats.lateness=late.summary();
            std::lock_guard<std::mutex> lock(mail.mutex);
            if(p){if(mail.queue.size()==4)++stats.queueDrops;else mail.queue.push_back(std::move(p));}
            stats.queueHighWater=std::max<std::uint64_t>(stats.queueHighWater,mail.queue.size());mail.stats=stats;mail.nextSample=origin+phase+(slot*1000000ULL)/hz;
        };
        while(!stopRequested.load(std::memory_order_acquire)){
            const auto planned=origin+phase+(slot*1000000ULL)/hz;
            mail.nextSample=planned;
            if(c.durationUs&&planned>=origin+c.durationUs)break;
            if(c.frameLimit&&stats.sampled>=c.frameLimit)break;
            auto now=rt::monotonicUs();
            if(now<planned){rt::waitBrieflyUntil(std::min(planned,now+1000));continue;}
            if(c.durationUs&&now>=origin+c.durationUs){const auto total=(c.durationUs>phase?((c.durationUs-phase)*hz+999999)/1000000:0);if(total>slot)stats.skipped+=total-slot;break;}
            const auto sampled=now;++stats.sampled;if(!stats.firstSampleUs)stats.firstSampleUs=sampled;stats.lastSampleUs=sampled;
            late.add(sampled>planned?sampled-planned:0,999);
            if(previous){const auto gap=sampled-previous;interval.add(gap);const auto nominal=static_cast<std::uint64_t>(std::llround(1000000.0/hz));deviation.add(gap>nominal?gap-nominal:nominal-gap,999);}previous=sampled;
            Frame frame;frame.moduleId=static_cast<std::uint8_t>(id);frame.timestampUs=sampled;
            std::uint64_t observedCounter=0;
            if(id==1){auto observation=ndi->read(rt::monotonicUs,stopRequested);frame.bytes=std::move(observation.bytes);
                if(observation.newSourceFrame)++stats.newSourceFrames;else ++stats.duplicateSource;
                if(observation.semanticChange)++stats.semanticChanges;
                bool firstCounter=true,mixed=false;
                for(const auto&tool:observation.toolTimes){stats.sourceCounterGaps+=tool.counter.missingSamples;stats.sourceCounterResets+=tool.counter.reset?1:0;if(firstCounter){observedCounter=tool.counter.unwrapped;firstCounter=false;}else mixed=mixed||observedCounter!=tool.counter.unwrapped;}
                if(mixed)++stats.mixedCounterFrames;
                stats.commonClockMapped=observation.sourceTime.has_value();
                if(observation.sourceTime)stats.clockErrorBoundUs=std::max(stats.clockErrorBoundUs,observation.sourceTime->errorBoundUs);
                if(stats.kind=="device"){
                    if(!observation.sourceTime){++stats.unmappedSourceFrames;advance(rt::monotonicUs());continue;}
                    frame.timestampUs=observation.sourceTime->timestampUs;
                    if(!observation.newSourceFrame&&!observation.semanticChange){advance(rt::monotonicUs());continue;}
                }
                stats.payloadBytes=frame.bytes.size();const auto currentBudget=preflight(c,frame.bytes.size(),epBytes);
                if(!currentBudget.feasible&&!c.allowOversubscribed)throw std::runtime_error("NDI observed length fails capacity screening: "+currentBudget.reason);
            }
            else frame.bytes=!replay.empty()?replay:ep;
            frame.frameId=stats.nextId++;
            auto p=prepareFrame(std::move(frame),planned,sampled,0,observedCounter);
            now=rt::monotonicUs();stats.prepareMaxUs=std::max(stats.prepareMaxUs,now-sampled);++stats.built;stats.ledger.add(*p);
            advance(now,std::move(p));
        }
    }catch(const std::exception&e){if(stopRequested)++stats.cancelledAttempts;else{++stats.errors;stats.error=e.what();}}
    if(ndi){try{ndi->shutdown();}catch(const std::exception&e){++stats.errors;if(stats.error.empty())stats.error=e.what();}}
    publish();mail.nextSample=0;mail.done.store(true,std::memory_order_release);
}
void Session::Impl::execute(Config c){
    State local;local.config=c;std::array<SourceMailbox,2> sources;std::array<std::thread,2> producers;
    sources[0].stats.nextId=c.initialNdiId;sources[1].stats.nextId=c.initialEpId;local.engine.nextStatusSequence=c.initialStatusSequence;
    std::unique_ptr<Engine> engineOwner;
    std::thread observer;std::atomic<bool> observerStop{false};std::atomic<unsigned> observedRate{0};std::mutex observationMutex;LinkObservation latestObservation;
    auto publish=[&](const Engine* engine){
        if(engine)local.engine=engine->snapshot();
        for(unsigned i=0;i<2;++i){std::lock_guard<std::mutex> lock(sources[i].mutex);local.sources[i]=sources[i].stats;}
        {std::lock_guard<std::mutex> lock(observationMutex);if(latestObservation.observedAtUs)local.link=latestObservation;}
        {std::lock_guard<std::mutex> lock(statusMutex);local.statusIngressAccepted=acceptedStatuses;local.statusIngressRejected=rejectedStatuses;local.statusIngressCancelled=cancelledStatuses;local.statusIngressPending=statuses.size();}
        local.elapsedUs=local.startedUs&&rt::monotonicUs()>local.startedUs?rt::monotonicUs()-local.startedUs:0;
        std::lock_guard<std::mutex> lock(mutex);state=local;
    };
    try{
        local.link=observeLink(c);latestObservation=local.link;observedRate=local.link.mbps;
        if(local.link.mbps)c.observedLinkMbps=c.observedLinkMbps?std::min(c.observedLinkMbps,local.link.mbps):local.link.mbps;
        local.engine.effectiveMbps=effectiveMbps(c);
        auto fileBytes=[](const std::string& path){std::error_code error;if(path.empty())return std::size_t(0);const auto size=std::filesystem::file_size(std::filesystem::u8path(path),error);return error?std::size_t(0):static_cast<std::size_t>(size);};
        std::size_t knownNdiBytes=fileBytes(c.ndiReplayFile),knownEpBytes=fileBytes(c.epReplayFile);
        local.capacity=preflight(c,knownNdiBytes,knownEpBytes);if(!local.capacity.feasible&&!c.allowOversubscribed)throw std::runtime_error(local.capacity.reason);
        auto udp=factory();if(!udp)throw std::runtime_error("Transport factory returned no socket");
        StreamConfig transportConfig;transportConfig.targetIp=c.targetIp;transportConfig.bindIp=c.bindIp;transportConfig.targetPort=c.targetPort;transportConfig.commandPort=c.commandPort;
        udp->open(transportConfig);local.localPort=udp->localPort();local.df=udp->dfEnabled();local.transport=udp->name();
        engineOwner=std::make_unique<Engine>(c);auto& engine=*engineOwner;local.startedUs=rt::monotonicUs()+50000;
        for(unsigned i=0;i<2;++i)producers[i]=std::thread([&,i,initialNdi=knownNdiBytes,initialEp=knownEpBytes]{source(c,i+1,local.startedUs,sources[i],initialNdi,initialEp);});
        observer=std::thread([&]{while(!observerStop){for(unsigned n=0;n<10&&!observerStop;++n)std::this_thread::sleep_for(std::chrono::milliseconds(100));if(observerStop)break;
            try{auto observation=observeLink(c);if(observation.mbps)observedRate=observation.mbps;std::lock_guard<std::mutex> lock(observationMutex);latestObservation=std::move(observation);}
            catch(const std::exception&e){std::lock_guard<std::mutex> lock(observationMutex);latestObservation.reason=std::string("Link observer failed; retaining last known rate: ")+e.what();}
            catch(...){std::lock_guard<std::mutex> lock(observationMutex);latestObservation.reason="Link observer failed; retaining last known rate";}
        }});
        std::uint64_t nextPublish=0,nextStatus=c.statusIntervalUs?local.startedUs:UINT64_MAX,drainStart=0;
        unsigned consecutiveSends=0;
        while(!stopRequested.load(std::memory_order_acquire)){
            auto now=rt::monotonicUs();
            for(unsigned i=0;i<2;++i){
                PreparedPtr p;std::uint64_t nextSample=0;{std::lock_guard<std::mutex> lock(sources[i].mutex);if(!sources[i].queue.empty()){p=std::move(sources[i].queue.front());sources[i].queue.pop_front();}nextSample=sources[i].nextSample;}
                if(p){
                    auto& known=i==0?knownNdiBytes:knownEpBytes;
                    if(known!=p->frame.bytes.size()){known=p->frame.bytes.size();local.capacity=preflight(c,knownNdiBytes,knownEpBytes);if(!local.capacity.feasible&&!c.allowOversubscribed)throw std::runtime_error("Actual combined payload sizes exceed the link budget");}
                    engine.submit(std::move(p));
                }engine.setNextSample(static_cast<std::uint8_t>(i+1),nextSample);
            }
            {std::lock_guard<std::mutex> lock(statusMutex);if(!statuses.empty()){auto item=std::move(statuses.front());statuses.pop_front();engine.submitStatus(std::move(item.first),item.second);}}
            if(c.durationUs&&now>=local.startedUs+c.durationUs)nextStatus=UINT64_MAX;
            if(now>=nextStatus){Bytes b(16);rt::writeLe32(b.data(),1);rt::writeLe32(b.data()+4,c.ndiEnabled?1:0);rt::writeLe32(b.data()+8,c.epEnabled?1:0);rt::writeLe32(b.data()+12,c.linkMbps);engine.submitStatus(std::move(b),now);nextStatus+=c.statusIntervalUs;if(nextStatus<=now)nextStatus=now+c.statusIntervalUs;}
            auto budget=effectiveMbps(c);const auto currentObserved=observedRate.load();if(currentObserved)budget=std::min(budget,currentObserved);
            if(budget!=local.capacity.effectiveMbps){auto effectiveConfig=c;effectiveConfig.observedLinkMbps=budget;local.capacity=preflight(effectiveConfig,knownNdiBytes,knownEpBytes);if(!local.capacity.feasible&&!c.allowOversubscribed)throw std::runtime_error("Observed link change makes declared load infeasible");}
            engine.setEffectiveMbps(budget,now);
            // Exactly one send per turn. Stop and producer queues are revisited at every packet boundary.
            const bool sent=engine.step(now,[&](const auto*p,std::size_t n){return udp->send(p,n);},rt::monotonicUs);
            if(sent&&++consecutiveSends<8&&now<nextPublish&&(!c.durationUs||now<local.startedUs+c.durationUs+100000))continue;
            consecutiveSends=0;
            const auto feedbackStart=rt::monotonicUs();IncomingDatagram incoming;
            for(unsigned n=0;n<4&&!stopRequested&&rt::monotonicUs()-feedbackStart<40;++n){
                if(!udp->receive(incoming))break;
                if(incoming.sourceIp!=c.allowedNackIp||(c.allowedNackPort&&incoming.sourcePort!=c.allowedNackPort))engine.rejectFeedbackSource();
                else engine.feedback(incoming.bytes.data(),incoming.bytes.size(),rt::monotonicUs());
                if(engine.nextDueUs()<=rt::monotonicUs())break;
            }
            now=rt::monotonicUs();if(now>=nextPublish){publish(&engine);nextPublish=now+100000;}
            bool done=sources[0].done&&sources[1].done&&(c.ndiEnabled||c.epEnabled||(c.durationUs&&now>=local.startedUs+c.durationUs));bool empty=true;
            for(auto& sourceBox:sources){std::lock_guard<std::mutex> lock(sourceBox.mutex);empty=empty&&sourceBox.queue.empty();}
            if(done&&empty&&!engine.pendingInitial()){
                if(!drainStart)drainStart=now;
                if(now-drainStart>=100000){local.endedReason="sources_finished_and_retry_grace_elapsed";break;}
            }
            if(c.durationUs&&now>=local.startedUs+c.durationUs+100000){local.endedReason="duration_expired";break;}
            auto due=engine.nextDueUs();if(due<=now&&sent)continue;if(due<=now)due=now+20;
            for(auto& sourceBox:sources){const auto sample=sourceBox.nextSample.load();if(sample>now)due=std::min(due,sample);}
            rt::waitBrieflyUntil(std::min(due,now+100));
        }
        if(local.endedReason.empty())local.endedReason="user_stop";
    }catch(const std::exception&e){local.error=e.what();local.endedReason="local_failure";}
    catch(...){local.error="Unknown worker error";local.endedReason="local_failure";}
    stopRequested=true;for(auto& t:producers)if(t.joinable())t.join();observerStop=true;if(observer.joinable())observer.join();
    for(auto& sourceBox:sources){std::lock_guard<std::mutex> lock(sourceBox.mutex);sourceBox.stats.queueDrops+=sourceBox.queue.size();sourceBox.queue.clear();}
    {std::lock_guard<std::mutex> lock(statusMutex);cancelledStatuses+=statuses.size();statuses.clear();}
    if(engineOwner)engineOwner->stop();
    local.finished=true;local.endedUs=rt::monotonicUs();publish(engineOwner.get());active.store(false,std::memory_order_release);
}
Session::Session(TransportFactory f,rt::aurora::NdiSourceFactory n):p_(std::make_unique<Impl>(std::move(f),std::move(n))){}
Session::~Session(){stop();if(p_->worker.joinable())p_->worker.join();}
void Session::start(const Config& config){
    if(busy())throw std::runtime_error("V2 session is already active or stopping");
    if(p_->worker.joinable())p_->worker.join();validateConfig(config);
    auto c=config;if(c.runId.empty())c.runId="v2-"+std::to_string(rt::monotonicUs());
    {std::lock_guard<std::mutex> lock(p_->mutex);p_->state={};p_->state.config=c;p_->state.capacity=preflight(c);p_->state.sources[0].nextId=c.initialNdiId;p_->state.sources[1].nextId=c.initialEpId;p_->state.engine.nextStatusSequence=c.initialStatusSequence;p_->state.engine.effectiveMbps=effectiveMbps(c);}
    {std::lock_guard<std::mutex> lock(p_->statusMutex);p_->statuses.clear();p_->acceptedStatuses=0;p_->rejectedStatuses=0;p_->cancelledStatuses=0;}
    p_->stopRequested=false;p_->active=true;
    try{p_->worker=std::thread([this,c]{p_->execute(c);});}catch(...){p_->active=false;throw;}
}
void Session::stop()noexcept{p_->stopRequested.store(true,std::memory_order_release);}
bool Session::busy()const noexcept{return p_->active.load(std::memory_order_acquire);}
RunResult Session::snapshot()const{
    State s;{std::lock_guard<std::mutex> lock(p_->mutex);s=p_->state;}
    RunResult r;r.reportJson=report(s);r.error=s.error;r.nextNdiId=s.sources[0].nextId;r.nextEpId=s.sources[1].nextId;r.nextStatusSequence=s.engine.nextStatusSequence;
    r.success=s.finished&&s.error.empty()&&s.engine.statusSendErrors==0;
    for(unsigned i=0;i<2;++i)r.success=r.success&&!s.sources[i].errors&&!s.engine.modules[i].sendErrors;
    return r;
}
RunResult Session::wait(){if(p_->worker.joinable())p_->worker.join();return snapshot();}
bool Session::postStatus(const Bytes& b,std::uint64_t when){
    if(!busy()||p_->stopRequested||b.size()>1444)return false;if(!when)when=rt::monotonicUs();
    std::lock_guard<std::mutex> lock(p_->statusMutex);if(p_->stopRequested||p_->statuses.size()>=64){++p_->rejectedStatuses;return false;}p_->statuses.emplace_back(b,when);++p_->acceptedStatuses;return true;
}
RunResult run(const Config& c,const Session::TransportFactory& factory,const std::function<bool()>& cancelled){
    Session session(factory);session.start(c);while(session.busy()){if(cancelled&&cancelled())session.stop();std::this_thread::sleep_for(std::chrono::milliseconds(10));}return session.wait();
}
}
