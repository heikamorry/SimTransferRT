#pragma once
#include "protocol/ProtocolTypes.h"
namespace rt::v2 {
constexpr std::uint8_t NdiModule=1, EpModule=2, StatusModule=3, StatusType=3;
constexpr std::size_t StatusHeaderSize=24, StatusOverhead=28, MaxStatusPayload=1444;
constexpr std::size_t NackFixedSize=24, MaxNackIndices=724;
struct Frame {
    std::uint8_t moduleId=NdiModule;
    std::uint32_t frameId=0;
    std::uint64_t timestampUs=0;
    Bytes bytes;
    std::uint16_t fragmentCount() const { return rt::fragmentCount(bytes.size()); }
};
struct DataPacket {
    std::uint8_t moduleId=NdiModule;
    std::uint32_t frameId=0,frameLength=0;
    std::uint64_t timestampUs=0;
    std::uint16_t fragmentIndex=0,fragmentCount=0;
    bool retransmission=false;
    Bytes payload;
};
struct Nack {
    std::uint8_t moduleId=NdiModule;
    std::uint32_t frameId=0;
    std::vector<std::uint16_t> indices;
};
struct Status {
    std::uint32_t sequence=0;
    std::uint64_t timestampUs=0;
    Bytes payload;
};
Datagram encodeData(const Frame&,std::uint16_t index,bool retry=false);
// All template datagrams belong to one frame with the same original ID and timestamp.
struct DataStamp {
    std::uint32_t frameId;
    std::uint64_t timestampUs;
    std::uint32_t fullCrcXor,tailCrcXor;
    void apply(Datagram&) const;
};
DataStamp makeDataStamp(const std::vector<Datagram>&,std::uint32_t frameId,std::uint64_t timestampUs);
Datagram encodeNack(const Nack&);
Datagram encodeStatus(const Status&);
DataPacket decodeData(const std::uint8_t*,std::size_t);
Nack decodeNack(const std::uint8_t*,std::size_t);
Status decodeStatus(const std::uint8_t*,std::size_t);
void validateIndices(const Nack&,std::uint16_t fragmentCount);
}
