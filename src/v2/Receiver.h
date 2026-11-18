#pragma once
#include "protocol/V2PacketCodec.h"
#include "sources/AuroraCodec.h"
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace rt::v2 {
struct ReceiveLedger {
    std::uint64_t count=0,idSum=0,crcSum=0,stampSum=0;
};
struct ReceiveModuleStats {
    std::uint64_t datagrams=0,bytes=0,completed=0,consumed=0,duplicates=0,late=0;
    std::uint64_t conflicts=0,expired=0,wholeMissing=0,businessErrors=0,retries=0;
    std::uint64_t nackRounds=0,nackPackets=0,requestedFragments=0,tailTriggers=0,gapTriggers=0;
    std::uint64_t maxCompletionUs=0,maxNackLatenessUs=0,maxRetryIntervalUs=0;
    std::uint64_t retryIntervalCount=0,minRetryIntervalUs=0;
    std::size_t activeFrames=0,highWaterFrames=0;
    ReceiveLedger ledger;
};
struct ReceiveView {
    std::shared_ptr<const Frame> frame;
    std::uint64_t hostCompletedUs=0;
    bool stale=true;
};
struct ReceiverSnapshot {
    std::array<ReceiveModuleStats,2> modules{};
    std::array<ReceiveView,2> latest{};
    rt::aurora::Reply ndi;
    Status status;
    bool hasStatus=false;
    std::uint64_t statusApplied=0,statusOld=0,invalidPackets=0,unexpectedNacks=0;
    std::uint64_t statusAppliedUs=0,nearestTimestampDeltaUs=0;
    std::uint32_t associatedNdiId=0,associatedEpId=0;
    bool associationAvailable=false;
    // A matching timestamp is not proof of a verified common hardware clock.
    bool hardwareClockVerified=false;
};
class Receiver {
public:
    using Clock=std::function<std::uint64_t()>;
    using FrameConsumer=std::function<void(const Frame&,std::uint64_t)>;
    using StatusConsumer=std::function<void(const Status&,std::uint64_t)>;
    explicit Receiver(bool decodeAurora=true,Clock clock={});
    void receive(const std::uint8_t*,std::size_t,std::uint64_t nowUs);
    // While draining queued datagrams, only a newly observed gap/new frame
    // should trigger feedback. Service elapsed timers with poll at batch end.
    std::vector<rt::Datagram> receiveAndPollObserved(const std::uint8_t*,std::size_t,std::uint64_t nowUs);
    std::vector<rt::Datagram> poll(std::uint64_t nowUs);
    ReceiverSnapshot snapshot(std::uint64_t nowUs) const;
    void setFrameConsumer(FrameConsumer f){frameConsumer_=std::move(f);}
    void setStatusConsumer(StatusConsumer f){statusConsumer_=std::move(f);}
    // Caller must isolate old endpoint traffic/drain its socket before reset.
    void reset();
private:
    struct Assembly {
        Frame frame; std::vector<bool> seen;std::size_t received=0;
        std::uint64_t firstUs=0,lastUs=0,nextRequestUs=0,lastRequestUs=0;
        unsigned rounds=0;bool discovered=false,tail=false;
        std::uint16_t highest=0,contiguous=0;
    };
    struct Module {
        std::deque<Assembly> active;
        std::deque<std::uint32_t> retired;
        std::optional<std::uint32_t> highest;
        std::deque<std::pair<std::uint32_t,std::uint64_t>> consumedTimes;
    };
    bool decodeAurora_;Clock clock_;
    std::array<Module,2> state_;
    ReceiverSnapshot report_;
    FrameConsumer frameConsumer_;StatusConsumer statusConsumer_;
    std::optional<std::uint8_t> ingest(const std::uint8_t*,std::size_t,std::uint64_t);
    std::vector<rt::Datagram> pollRequests(std::uint64_t,std::optional<std::uint8_t> observedModule);
    void retire(std::size_t,std::uint32_t);
    void complete(std::size_t,Assembly&,std::uint64_t);
};
std::string receiverJson(const ReceiverSnapshot&);
}
