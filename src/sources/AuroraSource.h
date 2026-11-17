#pragma once
#include "sources/AuroraCodec.h"
#include <atomic>
#include <functional>
#include <map>
#include <optional>
namespace rt::aurora {
struct CounterObservation {std::uint64_t unwrapped=0,missingSamples=0;bool duplicate=false,reset=false;};
class CounterTracker {
public:
    explicit CounterTracker(std::uint32_t increment=8);
    CounterObservation observe(std::uint32_t value);
    void reset()noexcept{initialized_=false;unwrapped_=0;}
private:std::uint32_t increment_,last_=0;std::uint64_t unwrapped_=0;bool initialized_=false;
};
struct MappedTime {std::uint64_t timestampUs=0,errorBoundUs=0;};
// A verified external anchor is mandatory. Host reception time never establishes an anchor.
class ClockMapping {
public:
    void setVerifiedAnchor(std::uint64_t counter,std::uint64_t commonUs,double ticksPerSecond,std::uint64_t errorBoundUs,std::string evidence);
    std::optional<MappedTime> map(std::uint64_t counter)const;
    void invalidate()noexcept{verified_=false;}
    bool verified()const noexcept{return verified_;}
    const std::string& evidence()const noexcept{return evidence_;}
private:bool verified_=false;std::uint64_t counter_=0,time_=0,error_=0;double rate_=0;std::string evidence_;
};
struct ToolObservation {std::uint8_t handle=0;CounterObservation counter;std::optional<MappedTime> sourceTime;};
struct SourceSample {
    Bytes bytes;Reply reply;std::uint64_t hostReadyUs=0;
    std::optional<MappedTime> sourceTime; // absent for mixed counters/unverified clock
    std::vector<ToolObservation> toolTimes;
    bool newSourceFrame=false,semanticChange=false;
};
class SampleTracker {
public:
    SourceSample observe(Bytes bytes,std::uint64_t hostReadyUs);
    ClockMapping& clockMapping()noexcept{return mapping_;}
    void reset();
private:std::map<std::uint8_t,CounterTracker> counters_;ClockMapping mapping_;Bytes previous_;
};
enum class SourceState {Disconnected,Setup,Tracking,Fault};
// Implementations must enforce timeout and cancellation. Called exclusively by an acquisition thread.
using Exchange=std::function<Bytes(const std::string&,std::uint32_t timeoutMs,const std::atomic<bool>& stop)>;
class DeviceSource {
public:
    explicit DeviceSource(Exchange exchange);
    void connect(const std::atomic<bool>&stop);
    SourceSample acquire(const std::function<std::uint64_t()>& hostClock,const std::atomic<bool>&stop);
    void stop(const std::atomic<bool>&cancel);
    void refreshTools(const std::atomic<bool>&cancel);
    void disconnect()noexcept;
    SourceState state()const noexcept{return state_;}
    SampleTracker& tracker()noexcept{return tracker_;}
    const std::string& apiRevision()const noexcept{return api_;}
    const std::string& firmwareRevision()const noexcept{return firmware_;}
    const std::string& deviceList()const noexcept{return devices_;}
    double trackingFrequencyHz()const noexcept{return trackingHz_;}
    double counterFrequencyHz()const noexcept{return counterHz_;}
private:
    Exchange exchange_;SourceState state_=SourceState::Disconnected;SampleTracker tracker_;
    std::string api_,firmware_,devices_;
    double trackingHz_=0,counterHz_=0;
    std::string command(const std::string&,const std::string&,std::uint32_t,const std::atomic<bool>&);
    void enumerate(const std::atomic<bool>&);
    void readFrequencies(const std::atomic<bool>&);
};
class ReplaySource {
public:
    explicit ReplaySource(std::vector<Bytes> replies);
    std::optional<SourceSample> next(std::uint64_t hostReadyUs);
    void rewind();
    SampleTracker& tracker()noexcept{return tracker_;}
private:std::vector<Bytes> replies_;std::size_t index_=0;SampleTracker tracker_;
};
}
