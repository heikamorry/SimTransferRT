#pragma once
#include "protocol/ProtocolTypes.h"

namespace rt::v2 {
constexpr std::uint8_t NdiModule = 1;
constexpr std::uint8_t EpModule = 2;
constexpr std::uint8_t StatusModule = 3;
constexpr std::uint8_t StatusType = 3;
constexpr std::size_t StatusHeaderSize = 24;
constexpr std::size_t StatusOverhead = 28;
constexpr std::size_t MaxStatusPayload = 1444;
constexpr std::size_t NackFixedSize = 24;
constexpr std::size_t MaxNackIndices = 724;

struct Frame {
    std::uint8_t moduleId = NdiModule;
    std::uint32_t frameId = 0;
    std::uint64_t timestampUs = 0;
    Bytes bytes;

    std::uint16_t fragmentCount() const {
        return rt::fragmentCount(bytes.size());
    }
};

struct DataPacket {
    std::uint8_t moduleId = NdiModule;
    std::uint32_t frameId = 0;
    std::uint32_t frameLength = 0;
    std::uint64_t timestampUs = 0;
    std::uint16_t fragmentIndex = 0;
    std::uint16_t fragmentCount = 0;
    bool retransmission = false;
    Bytes payload;
};

struct Nack {
    std::uint8_t moduleId = NdiModule;
    std::uint32_t frameId = 0;
    std::vector<std::uint16_t> indices;
};

struct Status {
    std::uint32_t sequence = 0;
    std::uint64_t timestampUs = 0;
    Bytes payload;
};

Datagram encodeData(const Frame &, std::uint16_t index, bool retry = false);

// All template datagrams belong to one frame with the same original ID and timestamp.
struct DataStamp {
    std::uint32_t frameId;
    std::uint64_t timestampUs;
    std::uint32_t fullCrcXor;
    std::uint32_t tailCrcXor;
    void apply(Datagram &) const;
};

DataStamp makeDataStamp(const std::vector<Datagram> &, std::uint32_t frameId,
                        std::uint64_t timestampUs);
Datagram encodeNack(const Nack &);
Datagram encodeStatus(const Status &);
DataPacket decodeData(const std::uint8_t *, std::size_t);
Nack decodeNack(const std::uint8_t *, std::size_t);
Status decodeStatus(const std::uint8_t *, std::size_t);
void validateIndices(const Nack &, std::uint16_t fragmentCount);
} // namespace rt::v2
