#pragma once
#include "protocol/ProtocolTypes.h"
namespace rt {
    std::uint32_t crc32(const std::uint8_t* p, std::size_t n) noexcept;
    inline std::uint32_t crc32(const Bytes& b) noexcept {
        return crc32(b.data(), b.size());
    }
}
