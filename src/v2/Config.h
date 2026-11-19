#pragma once
#include "core/Types.h"
#include <string>

namespace rt::v2 {
struct Config {
    std::string runId, targetIp="127.0.0.1", bindIp="127.0.0.1", allowedNackIp="127.0.0.1";
    std::uint16_t targetPort=5650, commandPort=5500, allowedNackPort=0;
    unsigned linkMbps=1000, observedLinkMbps=0, pathLimitMbps=0;
    bool allowOversubscribed=false;
    bool handoffTrace=false;
    bool fragmentTrace=false;
    std::uint64_t durationUs=0, frameLimit=0;
    std::uint32_t initialNdiId=1, initialEpId=1, initialStatusSequence=1;
    std::uint32_t epBytes=204800, ndiTools=4;
    bool ndiEnabled=true, epEnabled=true;
    unsigned ndiHz=60, epHz=50;
    std::string ndiReplayFile, epReplayFile;
    std::uint64_t statusIntervalUs=0;
    std::uint64_t ndiPhaseUs=0, epPhaseUs=0;
    // Deliberate, reported fault injection; never enabled by default.
    FaultConfig faults;
    std::uint8_t faultModule=2;
};
struct LinkObservation {
    unsigned mbps=0;
    bool loopback=false, physical=false;
    std::string interfaceName, reason;
    std::uint64_t observedAtUs=0;
};
struct Preflight {
    unsigned selectedMbps=1000, effectiveMbps=1000;
    std::size_t ndiBytes=0, epBytes=0;
    double bitsPerSecond=0, utilization=0, ndiWireUs=0, epWireUs=0, synchronousWireUs=0;
    bool feasible=true;
    std::string reason;
};
unsigned parseNetworkProfile(const std::string& text);
std::string networkProfileName(unsigned mbps);
std::uint64_t wireBytes(std::size_t payloadBytes);
unsigned effectiveMbps(const Config&, unsigned liveLinkMbps=0);
Preflight preflight(const Config&, std::size_t actualNdiBytes=0, std::size_t actualEpBytes=0,unsigned liveLinkMbps=0);
void validateConfig(const Config&);
LinkObservation observeLink(const Config&);
}
