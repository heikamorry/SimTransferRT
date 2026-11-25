#pragma once
#include "protocol/ProtocolTypes.h"
#include <cstdint>
#include <functional>
#include <set>
#include <string>

namespace rt {
// Deliberate fault injection shared by the V2 configuration and tests.
struct FaultConfig {
    std::set<std::uint16_t> dropFirst;
    std::set<std::uint16_t> dropAlways;
    std::set<std::uint16_t> corruptFirst;
    std::set<std::uint16_t> duplicateFirst;
    bool reverseInitial = false;

    bool any() const {
        return reverseInitial || !dropFirst.empty() || !dropAlways.empty() ||
               !corruptFirst.empty() || !duplicateFirst.empty();
    }
};

// Socket endpoint contract used by every transport adapter.
struct StreamConfig {
    std::string runId;
    std::string targetIp = "127.0.0.1";
    std::string bindIp = "127.0.0.1";
    std::string allowedNackIp = "127.0.0.1";
    std::uint16_t targetPort = 5650;
    std::uint16_t commandPort = 5500;
    std::uint16_t allowedNackPort = 0;
};

using SendFunction = std::function<bool(const std::uint8_t *, std::size_t)>;
std::set<std::uint16_t> parseFragmentIds(const std::string &text);
} // namespace rt
