#pragma once
#include "protocol/ProtocolTypes.h"
namespace rt {
    Datagram encodeData(const Frame& frame, std::uint16_t index, bool retry=false);
    Datagram encodeNack(const Nack& nack);
    DataPacket decodeData(const std::uint8_t* p, std::size_t n);
    Nack decodeNack(const std::uint8_t* p, std::size_t n);
    void validateRanges(const Nack& nack, std::uint16_t fragmentCount);
    std::vector<Range> mergeIds(std::vector<std::uint16_t> ids);
    // Used by independent tests as well; explicit LE serialization, never native struct memory.
    void writeLe16(std::uint8_t* p, std::uint16_t v) noexcept;
    void writeLe32(std::uint8_t* p, std::uint32_t v) noexcept;
    std::uint16_t readLe16(const std::uint8_t* p) noexcept;
    std::uint32_t readLe32(const std::uint8_t* p) noexcept;
}
