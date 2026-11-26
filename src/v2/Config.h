#pragma once
#include "core/Types.h"
#include <string>

namespace rt::v2 {
struct Config {
    std::string runId;
    std::string targetIp = "127.0.0.1";
    std::string bindIp = "127.0.0.1";
    std::string allowedNackIp = "127.0.0.1";
    std::uint16_t targetPort = 5650;
    std::uint16_t commandPort = 5500;
    std::uint16_t allowedNackPort = 0;
    unsigned linkMbps = 1000;
    unsigned observedLinkMbps = 0;
    unsigned pathLimitMbps = 0;
    unsigned sampleLeadCapUs = 2000;
    unsigned ndiHandoffGuardUs = 0;
    bool allowOversubscribed = false;
    bool mailboxFastPath = true;
    bool handoffTrace = false;
    bool fragmentTrace = false;
    bool asioPooledHandler = false;
    std::uint64_t durationUs = 0;
    std::uint64_t frameLimit = 0;
    std::uint32_t initialNdiId = 1;
    std::uint32_t initialEpId = 1;
    std::uint32_t initialStatusSequence = 1;
    std::uint32_t epBytes = 204800;
    std::uint32_t ndiTools = 4;
    bool ndiEnabled = true;
    bool epEnabled = true;
    unsigned ndiHz = 60;
    unsigned epHz = 50;
    std::string ndiReplayFile;
    std::string epReplayFile;
    std::uint64_t statusIntervalUs = 0;
    std::uint64_t ndiPhaseUs = 0;
    std::uint64_t epPhaseUs = 0;
    // Deliberate, reported fault injection; never enabled by default.
    FaultConfig faults;
    std::uint8_t faultModule = 2;
};

struct LinkObservation {
    unsigned mbps = 0;
    bool loopback = false;
    bool physical = false;
    std::string interfaceName;
    std::string reason;
    std::uint64_t observedAtUs = 0;
};

struct Preflight {
    unsigned selectedMbps = 1000;
    unsigned effectiveMbps = 1000;
    std::size_t ndiBytes = 0;
    std::size_t epBytes = 0;
    double bitsPerSecond = 0;
    double utilization = 0;
    double ndiWireUs = 0;
    double epWireUs = 0;
    double synchronousWireUs = 0;
    bool feasible = true;
    std::string reason;
};

unsigned parseNetworkProfile(const std::string &text);
std::string networkProfileName(unsigned mbps);
std::uint64_t wireBytes(std::size_t payloadBytes);
unsigned effectiveMbps(const Config &, unsigned liveLinkMbps = 0);
Preflight preflight(const Config &, std::size_t actualNdiBytes = 0, std::size_t actualEpBytes = 0,
                    unsigned liveLinkMbps = 0);
void validateConfig(const Config &);
LinkObservation observeLink(const Config &);
} // namespace rt::v2
