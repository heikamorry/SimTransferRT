#include "v2/Runner.h"
#include "protocol/V2PacketCodec.h"
#include "sources/AuroraCodec.h"
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
void require(bool x,const char* text){if(!x)throw std::runtime_error(text);}
struct Capture{std::mutex mutex;std::vector<rt::v2::DataPacket> packets;std::atomic<unsigned> statuses{0},receives{0};bool failOpen=false,failSend=false;};
class Transport:public rt::IUdpTransport{
    std::shared_ptr<Capture> capture_;
public:
    explicit Transport(std::shared_ptr<Capture> c):capture_(std::move(c)){}
    void open(const rt::StreamConfig&)override{if(capture_->failOpen)throw std::runtime_error("intentional open failure");}
    bool send(const std::uint8_t* p,std::size_t n)override{
        if(p[6]==3){++capture_->statuses;return true;}
        auto packet=rt::v2::decodeData(p,n);std::lock_guard<std::mutex> lock(capture_->mutex);
        if(capture_->failSend&&capture_->packets.size()==4)throw std::runtime_error("intentional send exception");
        capture_->packets.push_back(std::move(packet));return true;
    }
    bool receive(rt::IncomingDatagram&)override{++capture_->receives;return false;}
    std::uint16_t localPort()const override{return 54321;}
    bool dfEnabled()const override{return true;}
    std::string lastError()const override{return {};}
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
void preserveIdsOnOpenFailure(){auto capture=std::make_shared<Capture>();capture->failOpen=true;rt::v2::Config c;c.initialNdiId=234;c.initialEpId=345;c.initialStatusSequence=456;
    auto r=rt::v2::run(c,factory(capture));require(!r.success,"open failure reported");require(r.nextNdiId==234&&r.nextEpId==345&&r.nextStatusSequence==456,"failed open cannot reset continuation IDs");}
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
}
int main(){try{statusOnly();preserveIdsOnOpenFailure();failedReports();isolatedDeviceAndUnmapped();fastStopAndException();std::cout<<"V2 runner lifecycle tests passed\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
