#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace rt {
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint32_t Magic = 0xAA55A55Au;
constexpr std::size_t MaxDatagram = 1472;
constexpr std::size_t HeaderSize = 32;
constexpr std::size_t Overhead = 36;
constexpr std::size_t PayloadStep = 1436;
constexpr std::size_t MaxFrameBytes = 2097152;
constexpr std::size_t ModuleHeaderSize = 5;
constexpr std::size_t NackFixedSize = 20;
constexpr std::size_t MaxRanges = 363;
constexpr std::uint64_t SamplingPeriodUs = 20000;
constexpr std::uint64_t InitialWindowUs = 15000;
constexpr std::uint8_t DataType = 1;
constexpr std::uint8_t NackType = 2;

struct Frame {
    std::uint32_t logicId = 0;
    std::uint64_t timestampUs = 0;
    std::uint32_t moduleMask = 0;
    Bytes bytes;
    std::uint16_t fragmentCount() const;
};

struct DataPacket {
    std::uint32_t logicId = 0;
    std::uint32_t frameLength = 0;
    std::uint32_t moduleMask = 0;
    std::uint64_t timestampUs = 0;
    std::uint16_t fragmentIndex = 0;
    std::uint16_t fragmentCount = 0;
    bool retransmission = false;
    Bytes payload;
};

struct Range {
    std::uint16_t start = 0;
    std::uint16_t count = 0;
};

struct Nack {
    std::uint32_t logicId = 0;
    std::vector<Range> ranges;
};

struct Datagram {
    std::array<std::uint8_t, MaxDatagram> bytes{};
    std::size_t size = 0;
};

class ProtocolError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

inline std::uint16_t fragmentCount(std::size_t n) {
    if (!n || n > MaxFrameBytes)
        throw ProtocolError("frame_len must be 1..2097152");
    return static_cast<std::uint16_t>((n + PayloadStep - 1) / PayloadStep);
}

inline std::uint16_t Frame::fragmentCount() const {
    return rt::fragmentCount(bytes.size());
}

// RFC-style modular ordering valid only for distances < 2^31; NOT ordinary integer ordering.
inline bool newerId(std::uint32_t a, std::uint32_t b) {
    const auto delta = static_cast<std::uint32_t>(a - b);
    return delta != 0 && delta < 0x80000000u;
}
} // namespace rt
