#include "sources/AuroraSource.h"
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
namespace rt::aurora {
CounterTracker::CounterTracker(std::uint32_t increment):increment_(increment){if(!increment_)throw ProtocolError("Aurora counter increment must be positive");}
CounterObservation CounterTracker::observe(std::uint32_t value){
    CounterObservation o;if(!initialized_){initialized_=true;last_=value;unwrapped_=value;o.unwrapped=unwrapped_;return o;}
    auto delta=std::uint32_t(value-last_);o.duplicate=delta==0;
    if(delta>=0x80000000u||delta%increment_!=0){o.reset=true;unwrapped_=value;}
    else{unwrapped_+=delta;o.missingSamples=delta?delta/increment_-1:0;}
    last_=value;o.unwrapped=unwrapped_;return o;
}
void ClockMapping::setVerifiedAnchor(std::uint64_t counter,std::uint64_t commonUs,double rate,std::uint64_t error,std::string evidence){if(!std::isfinite(rate)||rate<=0||evidence.empty())throw ProtocolError("Aurora clock needs verified synchronization evidence/rate");counter_=counter;time_=commonUs;rate_=rate;error_=error;evidence_=std::move(evidence);verified_=true;}
std::optional<MappedTime> ClockMapping::map(std::uint64_t counter)const{
    if(!verified_)return std::nullopt;
    const bool positive=counter>=counter_;auto magnitude=positive?counter-counter_:counter_-counter;
    auto offset=std::floor(static_cast<long double>(magnitude)*1000000.0L/rate_+0.5L);
    // MSVC long double is binary64: never round a uint64 anchor through floating point.
    if(!std::isfinite(offset)||offset<0||offset>=std::ldexp(1.0L,64))return std::nullopt;
    auto delta=std::uint64_t(offset);auto maximum=(std::numeric_limits<std::uint64_t>::max)();
    if((positive&&delta>maximum-time_)||(!positive&&delta>time_))return std::nullopt;
    return MappedTime{positive?time_+delta:time_-delta,error_};
}
SourceSample SampleTracker::observe(Bytes b,std::uint64_t hostReadyUs){
    SourceSample s;s.reply=decodeBx(b);s.hostReadyUs=hostReadyUs;s.semanticChange=b!=previous_;s.bytes=std::move(b);previous_=s.bytes;
    std::set<std::uint8_t> current;std::optional<std::uint64_t> commonCounter;bool uniform=true,reset=false;
    for(const auto&t:s.reply.tools){current.insert(t.handle);if(t.status==HandleStatus::Disabled)continue;auto o=counters_[t.handle].observe(t.frameNumber);s.newSourceFrame=s.newSourceFrame||!o.duplicate;reset=reset||o.reset;if(!commonCounter)commonCounter=o.unwrapped;else uniform=uniform&&*commonCounter==o.unwrapped;s.toolTimes.push_back({t.handle,o,std::nullopt});}
    for(auto it=counters_.begin();it!=counters_.end();)if(!current.count(it->first))it=counters_.erase(it);else ++it;
    if(reset)mapping_.invalidate();for(auto&o:s.toolTimes)o.sourceTime=mapping_.map(o.counter.unwrapped);
    if(uniform&&commonCounter)s.sourceTime=mapping_.map(*commonCounter);return s;
}
void SampleTracker::reset(){counters_.clear();previous_.clear();mapping_.invalidate();}
namespace {
std::string hex2(unsigned v){std::ostringstream s;s<<std::uppercase<<std::hex<<std::setw(2)<<std::setfill('0')<<v;return s.str();}
unsigned number(const std::string&s){if(s.empty()||s.find_first_not_of("0123456789ABCDEFabcdef")!=std::string::npos)throw ProtocolError("Aurora malformed handle enumeration");return unsigned(std::stoul(s,nullptr,16));}
std::vector<unsigned> handles(const std::string&s){if(s.size()<2)throw ProtocolError("Aurora truncated handle list");auto n=number(s.substr(0,2));if(s.size()!=2+5*n)throw ProtocolError("Aurora handle list length mismatch");std::vector<unsigned> v;std::set<unsigned> seen;for(unsigned i=0;i<n;++i){auto h=number(s.substr(2+5*i,2));number(s.substr(4+5*i,3));if(!seen.insert(h).second)throw ProtocolError("Aurora duplicate enumerated handle");v.push_back(h);}return v;}
double frequency(const std::string&s){auto at=s.rfind('=');auto value=at==std::string::npos?s:s.substr(at+1);try{std::size_t used=0;auto f=std::stod(value,&used);if(used!=value.size()||!std::isfinite(f)||f<=0)throw ProtocolError("Aurora invalid frequency reply");return f;}catch(const std::exception&){throw ProtocolError("Aurora invalid frequency reply");}}
void apiCompatible(const std::string&s){if(s.size()!=9||s.substr(0,6)!="D.001."||s.substr(6).find_first_not_of("0123456789")!=std::string::npos)throw ProtocolError("Aurora unsupported API family/major revision");}
}
DeviceSource::DeviceSource(Exchange exchange):exchange_(std::move(exchange)){if(!exchange_)throw ProtocolError("Aurora exchange adapter required");}
std::string DeviceSource::command(const std::string&cmd,const std::string&params,std::uint32_t timeout,const std::atomic<bool>&cancel){if(cancel.load())throw ProtocolError("Aurora acquisition cancelled");auto body=decodeAsciiReply(exchange_(asciiCommand(cmd,params),timeout,cancel));if(body.rfind("ERROR",0)==0||body=="RESET")throw ProtocolError("Aurora device: "+body);return body;}
void DeviceSource::enumerate(const std::atomic<bool>&cancel){
    for(auto h:handles(command("PHSR","01",1000,cancel)))if(command("PHF",hex2(h),1000,cancel)!="OKAY")throw ProtocolError("Aurora PHF failed");
    // Re-enumerate after PINIT: dual 5DOF tools may allocate their second handle then.
    for(unsigned pass=0;pass<256;++pass){auto pending=handles(command("PHSR","02",1000,cancel));if(pending.empty())break;if(pass==255)throw ProtocolError("Aurora unstable tool topology");for(auto h:pending)if(command("PINIT",hex2(h),10000,cancel)!="OKAY")throw ProtocolError("Aurora PINIT failed");}
    for(auto h:handles(command("PHSR","03",1000,cancel)))if(command("PENA",hex2(h)+"D",1000,cancel)!="OKAY")throw ProtocolError("Aurora PENA failed");
    handles(command("PHSR","04",1000,cancel));
}
void DeviceSource::connect(const std::atomic<bool>&cancel){
    if(state_!=SourceState::Disconnected&&state_!=SourceState::Fault)throw ProtocolError("Aurora source already connected");
    try{tracker_.reset();api_=command("APIREV","",1000,cancel);apiCompatible(api_);firmware_=command("VER","5",1000,cancel);devices_=command("GET","Device.*",1000,cancel);if(command("INIT","",10000,cancel)!="OKAY")throw ProtocolError("Aurora INIT failed");state_=SourceState::Setup;enumerate(cancel);if(command("TSTART","00",1000,cancel)!="OKAY")throw ProtocolError("Aurora TSTART failed");state_=SourceState::Tracking;readFrequencies(cancel);}catch(...){state_=SourceState::Fault;throw;}
}
void DeviceSource::readFrequencies(const std::atomic<bool>&cancel){trackingHz_=frequency(command("GET","Param.Tracking.Track Frequency",1000,cancel));counterHz_=frequency(command("GET","Param.Tracking.Frame Frequency",1000,cancel));}
SourceSample DeviceSource::acquire(const std::function<std::uint64_t()>& hostClock,const std::atomic<bool>&cancel){
    if(state_!=SourceState::Tracking)throw ProtocolError("Aurora not tracking");if(cancel.load())throw ProtocolError("Aurora acquisition cancelled");
    try{if(!hostClock)throw ProtocolError("Aurora host clock required");auto bytes=exchange_(asciiCommand("BX","0001"),1000,cancel);if(!bytes.empty()&&(bytes[0]=='E'||bytes[0]=='R'))throw ProtocolError("Aurora device: "+decodeAsciiReply(bytes));auto sample=tracker_.observe(std::move(bytes),0);sample.hostReadyUs=hostClock();return sample;}catch(...){state_=SourceState::Fault;tracker_.clockMapping().invalidate();throw;}
}
void DeviceSource::stop(const std::atomic<bool>&cancel){if(state_==SourceState::Tracking||state_==SourceState::Fault){try{if(command("TSTOP","",1000,cancel)!="OKAY")throw ProtocolError("Aurora TSTOP failed");state_=SourceState::Setup;}catch(...){state_=SourceState::Fault;throw;}}}
void DeviceSource::refreshTools(const std::atomic<bool>&cancel){try{stop(cancel);tracker_.clockMapping().invalidate();if(state_!=SourceState::Setup)throw ProtocolError("Aurora refresh requires setup");enumerate(cancel);if(command("TSTART","00",1000,cancel)!="OKAY")throw ProtocolError("Aurora TSTART failed");state_=SourceState::Tracking;readFrequencies(cancel);}catch(...){state_=SourceState::Fault;throw;}}
void DeviceSource::disconnect()noexcept{state_=SourceState::Disconnected;tracker_.reset();trackingHz_=0;counterHz_=0;}
ReplaySource::ReplaySource(std::vector<Bytes>replies):replies_(std::move(replies)){for(const auto&b:replies_)decodeBx(b);}
std::optional<SourceSample> ReplaySource::next(std::uint64_t now){if(index_>=replies_.size())return std::nullopt;return tracker_.observe(replies_[index_++],now);}
void ReplaySource::rewind(){index_=0;tracker_.reset();}
}
