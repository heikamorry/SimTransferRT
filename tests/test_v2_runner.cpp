#include "v2/Runner.h"
#include "v2/ProducerWait.h"
#include "core/SamplingClock.h"
#include "protocol/V2PacketCodec.h"
#include "sources/AuroraCodec.h"
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
void require(bool x,const char* text){if(!x)throw std::runtime_error(text);}
struct Capture{std::mutex mutex;std::vector<rt::v2::DataPacket> packets;std::atomic<unsigned> statuses{0},receives{0};bool failOpen=false,failSend=false,failFirstRead=false;
    bool requestFirstFragment=false;std::size_t nackAtPacket=0;
    bool delayFirstEpFragment=false;
};
class Transport:public rt::IUdpTransport{
    std::shared_ptr<Capture> capture_;
public:
    explicit Transport(std::shared_ptr<Capture> c):capture_(std::move(c)){}
    void open(const rt::StreamConfig&)override{if(capture_->failOpen)throw std::runtime_error("intentional open failure");}
    bool send(const std::uint8_t* p,std::size_t n)override{
        if(p[6]==3){++capture_->statuses;return true;}
        auto packet=rt::v2::decodeData(p,n);std::lock_guard<std::mutex> lock(capture_->mutex);
        if(capture_->failSend&&capture_->packets.size()==4)throw std::runtime_error("intentional send exception");
        if(capture_->delayFirstEpFragment && packet.moduleId==rt::v2::EpModule &&
           packet.fragmentIndex==0 && !packet.retransmission)
            std::this_thread::sleep_for(std::chrono::milliseconds(21));
        capture_->packets.push_back(std::move(packet));return true;
    }
    bool receive(rt::IncomingDatagram& incoming)override{
        ++capture_->receives;
        if(!capture_->requestFirstFragment || capture_->nackAtPacket || capture_->packets.empty())return false;
        const auto& first=capture_->packets.front();
        const auto nack=rt::v2::encodeNack({first.moduleId,first.frameId,{0}});
        capture_->nackAtPacket=capture_->packets.size();
        incoming.sourceIp="127.0.0.1";
        incoming.bytes.assign(nack.bytes.begin(),nack.bytes.begin()+nack.size);
        return true;
    }
    rt::ReceiveOutcome receiveFeedback(rt::IncomingDatagram& packet)override{
        if(capture_->failFirstRead && !capture_->receives){
            ++capture_->receives;
            return rt::ReceiveOutcome::Error;
        }
        return rt::IUdpTransport::receiveFeedback(packet);
    }
    std::uint16_t localPort()const override{return 54321;}
    bool dfEnabled()const override{return true;}
    std::string lastError()const override{return capture_->failFirstRead?"feedback error example":"";}
};
rt::v2::Session::TransportFactory factory(std::shared_ptr<Capture> c){return [c]{return std::make_unique<Transport>(c);};}
class DeviceModel:public rt::aurora::INdiSource{
    unsigned number_=0;bool delay_;
public:
    explicit DeviceModel(bool delay):delay_(delay){}
    void start(const std::atomic<bool>&)override{}
    rt::aurora::SourceSample read(const rt::aurora::HostClock& clock,const std::atomic<bool>& stop)override{
        if(delay_)for(unsigned i=0;i<100&&!stop;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if(stop)throw std::runtime_error("cancelled source");
        rt::aurora::SourceSample s;s.bytes=rt::aurora::generateSample(1,number_*8);s.reply=rt::aurora::decodeBx(s.bytes);s.newSourceFrame=true;s.semanticChange=true;s.hostReadyUs=clock();
        if(number_++%2)s.sourceTime=rt::aurora::MappedTime{1000000+number_*25000,4};
        return s;
    }
    void shutdown()override{}
    std::string kind()const override{return "device";}
    double actualHz()const override{return 40;}
    double counterHz()const override{return 320;}
};
void statusOnly(){auto capture=std::make_shared<Capture>();rt::v2::Config c;c.ndiEnabled=false;c.epEnabled=false;c.statusIntervalUs=10000;c.durationUs=120000;
    auto r=rt::v2::run(c,factory(capture));require(r.success,"status-only succeeds");require(capture->statuses>=8,"periodic status must stay active through its duration");}
void producerWaitCancellation(){
    using rt::v2::ProducerWait;
    rt::v2::ProducerStop stop;
    std::array<rt::v2::ProducerWaitStats,2> stats{};
    std::array<std::promise<void>,2> entered;
    std::array<std::future<bool>,2> results;
    for(unsigned i=0;i<2;++i){
        results[i]=std::async(std::launch::async,[&,i]{
            const auto backend=i?ProducerWait::Timer::ConditionVariable:ProducerWait::Timer::Automatic;
            ProducerWait wait(stop,stats[i],50,backend);
            std::uint64_t sample=0;
            entered[i].set_value();
            return wait.until(rt::monotonicUs()+3000000,sample);
        });
    }
    for(auto& ready:entered)ready.get_future().wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stop.request();
    for(auto& result:results){
        require(result.wait_for(std::chrono::seconds(1))==std::future_status::ready&&!result.get(),
                "stop broadcasts to both the native and fallback timed waits");
    }
    require(stats[0].cancelled==1&&stats[1].cancelled==1,"both cancellations counted");

    rt::v2::ProducerWaitStats restarted;
    ProducerWait wait(stop,restarted,1000);
    std::uint64_t sample=0;
    require(!wait.until(rt::monotonicUs()+3000000,sample)&&restarted.timedWaitSlots==0,
            "a stop already issued is observed before arming");
    stop.reset();
    const auto slot=rt::monotonicUs()+5000;
    require(wait.until(slot,sample)&&sample>=slot,"after reset keep the absolute slot and never sample early");
    require(restarted.leadUs>=80&&restarted.leadUs<=restarted.leadCapUs&&restarted.leadCapUs==125,
            "adaptive lead remains limited at the maximum source rate");
    require(restarted.worstReturnUs>=restarted.worstTargetUs&&
            restarted.overshootMaxUs==restarted.worstReturnUs-restarted.worstTargetUs,"wake overshoot timestamps agree");
}
void producerSessionRestart(){
    auto capture=std::make_shared<Capture>();
    rt::v2::Session session(factory(capture));
    rt::v2::Config c;c.frameLimit=1;c.epBytes=1;
    c.ndiPhaseUs=c.epPhaseUs=1000000;
    session.start(c);
    const auto cutoff=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!capture->receives&&session.busy()&&std::chrono::steady_clock::now()<cutoff)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    session.stop();
    const auto stopped=session.wait();
    require(stopped.error.empty()&&capture->packets.empty(),"stop before phased slots produces no frame");
    c.ndiPhaseUs=c.epPhaseUs=0;
    session.start(c);
    const auto finished=session.wait();
    require(finished.success&&capture->packets.size()==2&&capture->packets[0].moduleId!=capture->packets[1].moduleId,
            "restarted session wakes and delivers one original frame from each producer");
    const auto first=finished.reportJson.find("\"producer_wait\":{");
    require(first!=std::string::npos&&finished.reportJson.find("\"producer_wait\":{",first+1)!=std::string::npos
            &&finished.reportJson.find("\"max_wake_overshoot_us\":")!=std::string::npos,
            "both source reports include wait telemetry");
}
void frameHandoffReport(){
    auto capture=std::make_shared<Capture>();
    rt::v2::Session session(factory(capture));
    require(!session.epHandoffExceeded(0),"no handoff maximum before a run");
    rt::v2::Config c;c.ndiEnabled=false;c.epBytes=1;c.frameLimit=1;c.handoffTrace=true;
    session.start(c);
    const auto result=session.wait();
    require(result.success,"traced one-frame transfer");
    const auto first=result.reportJson.find("\"worst_first_dispatch\":{\"1\":[],\"2\":[{");
    require(first!=std::string::npos,"bounded candidates attributed to EP");
    auto stamp=[&](const std::string& key){
        const auto prefix="\""+key+"\":\"";
        const auto at=result.reportJson.find(prefix,first);
        require(at!=std::string::npos,"traced event has an observed monotonic timestamp");
        return std::stoull(result.reportJson.substr(at+prefix.size()));
    };
    const auto ready=stamp("ready_us"),queued=stamp("enqueued_us"),notify=stamp("notify_issue_us");
    const auto observed=stamp("sender_observed_us"),dequeued=stamp("dequeued_us"),submitted=stamp("submitted_us");
    const auto selected=stamp("selected_us"),attempt=stamp("first_attempt_us");
    require(ready<=queued && queued<=notify,"producer stamps are ordered");
    require(queued<=observed && observed<=dequeued && dequeued<=submitted &&
            submitted<=selected && selected<=attempt,"sender observations follow the frame through dispatch");
    const auto max=attempt-ready;
    require(!session.epHandoffExceeded(max) && (!max || session.epHandoffExceeded(max-1)),"exact EP threshold is strict");
    c.handoffTrace=false;
    session.start(c);
    const auto untraced=session.wait();
    require(untraced.success && untraced.reportJson.find("\"handoff_trace\":{\"enabled\":false}")!=std::string::npos &&
            untraced.reportJson.find("\"worst_first_dispatch\"")==std::string::npos &&
            !session.epHandoffExceeded(0),"disabled restart clears the traced maximum");
}
void fragmentTriggerReport(){
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path=std::filesystem::temp_directory_path()/("simrt-fragment-trigger-"+std::to_string(stamp)+".json");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup(){std::error_code ec;std::filesystem::remove(path,ec);}
    } cleanup{path};
    auto capture=std::make_shared<Capture>();
    rt::v2::Config c;
    c.frameLimit=1;c.ndiEnabled=false;c.epBytes=3*1436;
    bool rejected=false;
    try{rt::v2::run(c,factory(capture),{},{},path.u8string());}
    catch(const std::invalid_argument&){rejected=true;}
    require(rejected && !std::filesystem::exists(path),"disabled fragment trigger rejected before launch");
    capture->delayFirstEpFragment=true;
    c.fragmentTrace=true;
    const auto r=rt::v2::run(c,factory(capture),{},{},path.u8string());
    require(r.success && std::filesystem::exists(path),"standalone EP fragment-span trigger fires");
    std::ifstream trigger(path,std::ios::binary);
    const std::string marker{std::istreambuf_iterator<char>(trigger),std::istreambuf_iterator<char>()};
    for(const auto* json:{&marker,&r.reportJson}){
        require(json->find("\"handoff_trace\":{\"enabled\":false}")!=std::string::npos,"handoff stays independently disabled");
        const auto start=json->find("\"fragment_trace\":{\"enabled\":true,");
        require(start!=std::string::npos,"fragment trace enabled in marker and final report");
        require(json->find("\"worst_gap_pairs\":{\"global\":{\"count\":2,",start)!=std::string::npos &&
                json->find("\"1\":{\"count\":0,\"max_us\":0,\"pair\":null}",start)!=std::string::npos &&
                json->find("\"kind\":\"data\",\"fragment_index\":",start)!=std::string::npos,
                "maximum-gap packet identities and empty module stream are published");
#ifdef _WIN32
        require(json->find("\"qpc_frequency_hz\":\"",start)!=std::string::npos,"fragment-only capture has a calibrated kernel clock");
#endif
        const auto span=json->find("\"span_us\":",start);
        require(span!=std::string::npos && std::stoull(json->substr(span+10))>=18000,"trigger measures the finalized initial span");
        const auto expectedSpan=std::to_string(std::stoull(json->substr(span+10)));
        const std::string expectedDistribution="\"span_distribution_by_module\":{\"1\":{\"count\":0,\"p999_us\":0,\"max_us\":0,\"ge_18000_us\":0},"
            "\"2\":{\"count\":1,\"p999_us\":"+expectedSpan+",\"max_us\":"+expectedSpan+",\"ge_18000_us\":1}}";
        require(json->find(expectedDistribution,start)!=std::string::npos,"trigger and final report agree on the per-module span distribution");
        require(json->find("\"call_count\":3,",start)!=std::string::npos &&
                json->find("\"fragment_count\":3,\"fragments\":[",start)!=std::string::npos &&
                json->find("\"fragment_index\":2,",start)!=std::string::npos,"all three EP call rows retained");
        require(json->find("\"worst_ndi_ready_to_end\":null",start)!=std::string::npos,"disabled NDI has no candidate");
    }
}
void preserveIdsOnOpenFailure(){
    auto capture=std::make_shared<Capture>();capture->failOpen=true;
    rt::v2::Session session(factory(capture));
    const auto idle=session.snapshot();
    require(!idle.success&&idle.nextNdiId==1&&idle.nextEpId==1,"idle session has an initial snapshot");
    rt::v2::Config c;c.initialNdiId=234;c.initialEpId=345;c.initialStatusSequence=456;
    session.start(c);
    auto r=session.wait();
    require(!r.success,"open failure reported");
    require(r.nextNdiId==234&&r.nextEpId==345&&r.nextStatusSequence==456,"failed open cannot reset continuation IDs");
    c.runId="snapshot-restart";c.initialEpId=789;
    session.start(c);
    const auto restarted=session.snapshot();
    const auto finished=session.wait();
    require(restarted.nextEpId==789&&finished.nextEpId==789,"restart replaces the previous final snapshot");
    require(restarted.reportJson.find(c.runId)!=std::string::npos,"restart exposes its own report");
    require(session.snapshot().reportJson==finished.reportJson,"final snapshot remains readable after wait");
}
void failedReports(){auto capture=std::make_shared<Capture>();rt::v2::Config c;c.linkMbps=100;c.epBytes=2097152;
    auto rejected=rt::v2::run(c,factory(capture));require(!rejected.success,"impossible capacity rejected");require(rejected.reportJson.find("\"effective_link_mbps\":100,")!=std::string::npos,"rejected preflight must retain actual budget");
    class BadSource:public DeviceModel{public:BadSource():DeviceModel(false){}void start(const std::atomic<bool>&)override{throw std::runtime_error("intentional source setup error");}};
    c.linkMbps=1000;c.epBytes=1;c.durationUs=80000;
    rt::v2::Session session(factory(capture),[]{return std::make_unique<BadSource>();});session.start(c);auto result=session.wait();
    const auto begin=result.reportJson.find("\"id\":\"sampling_integrity\"");const auto end=result.reportJson.find('}',begin);const auto row=result.reportJson.substr(begin,end-begin);
    require(row.find("\"status\":\"fail\"")!=std::string::npos,"source setup failure must fail acceptance even with zero samples");
    require(!capture->packets.empty()&&capture->packets[0].moduleId==2,"source setup failure isolated from EP");
}
void isolatedDeviceAndUnmapped(){auto capture=std::make_shared<Capture>();rt::v2::Config c;c.epBytes=1;c.durationUs=180000;
    rt::v2::Session s(factory(capture),[]{return std::make_unique<DeviceModel>(false);});s.start(c);auto r=s.wait();
    std::array<unsigned,2> count{};for(const auto&p:capture->packets){++count[p.moduleId-1];if(p.moduleId==1)require(p.timestampUs<2000000,"device common timestamp must not be replaced with QPC");}
    require(count[0]>=3&&count[1]>=7,"temporary unmapped NDI must recover while EP continues");
    require(r.reportJson.find("\"unmapped_source_frames\":0")==std::string::npos||count[0]<count[1],"unmapped frames accounted");
    require(r.reportJson.find("\"actual_device_hz\":40")!=std::string::npos,"actual device frequency retained");
}
void fastStopAndException(){auto capture=std::make_shared<Capture>();rt::v2::Config c;c.epBytes=1;
    rt::v2::Session s(factory(capture),[]{return std::make_unique<DeviceModel>(true);});s.start(c);std::this_thread::sleep_for(std::chrono::milliseconds(90));
    for(unsigned i=0;i<200;++i)s.postStatus({1,2,3});auto begin=std::chrono::steady_clock::now();s.stop();auto r=s.wait();
    require(std::chrono::steady_clock::now()-begin<std::chrono::milliseconds(500),"Stop cancels independent source and sender promptly");
    require(!s.busy()&&r.error.empty(),"normal cancellation is not a source failure");
    require(r.reportJson.find("\"status_ingress\"")!=std::string::npos,"accepted/rejected/cancelled ingress retained");
    auto failed=std::make_shared<Capture>();failed->failSend=true;c.ndiEnabled=false;c.epBytes=204800;c.frameLimit=1;
    auto error=rt::v2::run(c,factory(failed));require(!error.success&&error.error.find("intentional")!=std::string::npos,"send exception reported");
    require(error.reportJson.find("\"datagrams\":4")!=std::string::npos,"send exception retains current counts");
    require(error.reportJson.find("\"initial_incomplete\":1")!=std::string::npos,"send exception accounts unfinished frame");
}
void feedbackReadFailure(){
    auto capture=std::make_shared<Capture>();
    capture->failFirstRead=true;
    rt::v2::Config c;
    c.frameLimit=1;
    c.ndiEnabled=false;
    c.epBytes=1;
    auto result=rt::v2::run(c,factory(capture));
    require(!result.success && result.error.find("feedback error example")!=std::string::npos,
        "feedback channel error fails a finished run");
    require(result.reportJson.find("\"feedback_read_errors\":1,")!=std::string::npos,"ordinary empty reads excluded");
    require(result.reportJson.find("\"feedback_empty_polls\":"+std::to_string(capture->receives.load()-1)+",")!=std::string::npos,
            "failed feedback read excluded from empty poll count");
    require(capture->packets.size()==1,"temporary feedback error does not discard independent initial data");
}

void feedbackDuringPacedFrame(){
    auto capture=std::make_shared<Capture>();
    capture->requestFirstFragment=true;
    rt::v2::Config c;
    c.frameLimit=1;c.ndiEnabled=false;c.epBytes=24*1436;
    const auto result=rt::v2::run(c,factory(capture));
    require(result.success && capture->nackAtPacket>0 && capture->nackAtPacket<=8,
            "check pending feedback within eight DATA dispatches");
    require(capture->packets.size()==25 && capture->packets.back().retransmission &&
            capture->packets.back().fragmentIndex==0,"service queued retry after the initial burst");
    require(result.reportJson.find("\"feedback_poll_calls\":"+std::to_string(capture->receives.load())+",")!=std::string::npos,
            "report counts actual nonblocking receive calls");
}

void statusOnlyFrameBound(){
    rt::v2::Config c;
    c.ndiEnabled=false;c.epEnabled=false;c.statusIntervalUs=10000;c.frameLimit=3;
    bool rejected=false;
    try{rt::v2::validateConfig(c);}catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"frames alone cannot terminate a status-only run");
    c.durationUs=50000;
    rt::v2::validateConfig(c);
    c.durationUs=0;c.frameLimit=0;
    rt::v2::validateConfig(c);
}

void linkRecovery(){
    auto capture=std::make_shared<Capture>();
    rt::v2::Config c;
    c.ndiEnabled=false;c.epEnabled=false;c.statusIntervalUs=20000;c.durationUs=1400000;
    std::atomic<unsigned> observations{0};
    rt::v2::Session session(factory(capture),{},[&](const rt::v2::Config&){
        rt::v2::LinkObservation link;
        link.observedAtUs=observations.fetch_add(1)+1;
        link.mbps=link.observedAtUs==1?100:1000;
        return link;
    });
    session.start(c);
    auto r=session.wait();
    require(r.success && observations>=2,"live link observed twice");
    require(r.reportJson.find("\"effective_link_mbps\":1000,")!=std::string::npos &&
        r.reportJson.find("\"rate_changes\":1,")!=std::string::npos,"initial observation must not pin a session at 100 Mbps");
    c.pathLimitMbps=500;c.observedLinkMbps=250;
    require(rt::v2::effectiveMbps(c,1000)==250 && rt::v2::effectiveMbps(c,100)==100,"keep manual caps across renegotiation");
}

void oversizedDeviceReplyOnObservedLink(){
    class GrowingDevice final : public DeviceModel {
        unsigned reads_=0;
    public:
        GrowingDevice():DeviceModel(false){}
        rt::aurora::SourceSample read(const rt::aurora::HostClock& clock,const std::atomic<bool>&)override{
            ++reads_;
            rt::aurora::SourceSample sample;
            sample.bytes=rt::aurora::generateSample(reads_==1?40:255,reads_*8);
            sample.hostReadyUs=clock();
            sample.newSourceFrame=true;
            sample.sourceTime=rt::aurora::MappedTime{1000000ULL+reads_*25000ULL,4};
            return sample;
        }
    };
    rt::v2::Config c;
    c.linkMbps=1000;c.ndiTools=40;c.epBytes=204800;c.frameLimit=4;c.statusIntervalUs=20000;
    auto capture=std::make_shared<Capture>();
    auto link=[](const rt::v2::Config&){rt::v2::LinkObservation value;value.observedAtUs=1;value.mbps=100;return value;};
    rt::v2::Session session(factory(capture),[]{return std::make_unique<GrowingDevice>();},link);
    session.start(c);
    const auto result=session.wait();
    require(!result.success && result.error.empty() &&
        result.reportJson.find("NDI observed length fails capacity screening")!=std::string::npos,
        "oversized live BX must fail its source, keep the shared transport running");
    unsigned ndiPackets=0,finalEpPackets=0;
    for(const auto& p:capture->packets){
        if(p.moduleId==1){++ndiPackets;require(p.frameId==1,"only the admissible NDI frame is transmitted");}
        if(p.moduleId==2 && p.frameId==4)++finalEpPackets;
    }
    require(ndiPackets==2 && finalEpPackets==143 && capture->statuses>1,
        "good NDI reply, final complete EP frame and periodic statuses survive the isolated source error");
}
}
int main(){try{statusOnly();producerWaitCancellation();producerSessionRestart();frameHandoffReport();fragmentTriggerReport();preserveIdsOnOpenFailure();failedReports();isolatedDeviceAndUnmapped();fastStopAndException();feedbackReadFailure();feedbackDuringPacedFrame();statusOnlyFrameBound();linkRecovery();oversizedDeviceReplyOnObservedLink();std::cout<<"V2 runner lifecycle tests passed\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
