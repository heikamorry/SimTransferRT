#include "v2/Config.h"
#include "core/SamplingClock.h"
#include <algorithm>
#include <sstream>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#endif

namespace rt::v2 {
#ifdef __linux__
void observeLinuxLink(const Config &config, LinkObservation &out);
#endif
unsigned parseNetworkProfile(const std::string &text) {
    if (text == "100m" || text == "100" || text == "100Mbps")
        return 100;
    if (text == "1g" || text == "1000" || text == "1GbE")
        return 1000;
    if (text == "2g5" || text == "2.5g" || text == "2500" || text == "2.5GbE")
        return 2500;
    throw std::invalid_argument("V2 network profile must be 100m, 1g or 2g5");
}

std::string networkProfileName(unsigned mbps) {
    switch (mbps) {
    case 100:
        return "100Mbps";
    case 1000:
        return "1GbE";
    case 2500:
        return "2.5GbE";
    default:
        return "invalid";
    }
}

std::uint64_t wireBytes(std::size_t n) {
    return n + 102ULL * rt::fragmentCount(n);
}

unsigned effectiveMbps(const Config &c, unsigned liveLinkMbps) {
    auto r = c.linkMbps;
    if (c.observedLinkMbps)
        r = std::min(r, c.observedLinkMbps);
    if (c.pathLimitMbps)
        r = std::min(r, c.pathLimitMbps);
    if (liveLinkMbps)
        r = std::min(r, liveLinkMbps);
    return r;
}

void validateConfig(const Config &c) {
    if (c.sampleLeadCapUs < 80 || c.sampleLeadCapUs > 4000)
        throw std::invalid_argument("Sampling wake lead cap must be 80..4000 us");
    if (c.ndiHandoffGuardUs > 400)
        throw std::invalid_argument("NDI handoff guard must be 0..400 us");
    if (c.linkMbps != 100 && c.linkMbps != 1000 && c.linkMbps != 2500)
        throw std::invalid_argument("V2 requires one of three network profiles");
    if (!c.targetPort)
        throw std::invalid_argument("target port must be nonzero");
    if (!c.ndiEnabled && !c.epEnabled && !c.statusIntervalUs)
        throw std::invalid_argument("at least one data source or periodic status must be enabled");
    if (!c.ndiEnabled && !c.epEnabled && c.frameLimit && !c.durationUs)
        throw std::invalid_argument(
            "--frames counts data samples; use --duration to bound a status-only run");
    if (!c.ndiHz || c.ndiHz > 1000 || !c.epHz || c.epHz > 1000)
        throw std::invalid_argument("source rate must be 1..1000 Hz");
    if (c.ndiTools > 255)
        throw std::invalid_argument(
            "BX tool record count must be 0..255; actual hardware capacity is separate");
    if (!c.epBytes || c.epBytes > rt::MaxFrameBytes)
        throw std::invalid_argument("EP payload must be 1..2097152 bytes");
    if (c.faultModule != 1 && c.faultModule != 2)
        throw std::invalid_argument("fault module must be 1 or 2");
}

Preflight preflight(const Config &c, std::size_t ndi, std::size_t ep, unsigned liveLinkMbps) {
    validateConfig(c);
    Preflight p;
    p.selectedMbps = c.linkMbps;
    p.effectiveMbps = effectiveMbps(c, liveLinkMbps);
    p.ndiBytes = c.ndiEnabled ? (ndi ? ndi : 11 + 42 * c.ndiTools) : 0;
    p.epBytes = c.epEnabled ? (ep ? ep : c.epBytes) : 0;
    const auto nw = p.ndiBytes ? wireBytes(p.ndiBytes) : 0;
    const auto ew = p.epBytes ? wireBytes(p.epBytes) : 0;
    p.ndiWireUs = double(nw) * 8 / p.effectiveMbps;
    p.epWireUs = double(ew) * 8 / p.effectiveMbps;
    p.synchronousWireUs = p.ndiWireUs + p.epWireUs;
    // Periodic simulator status is a 16-byte complete snapshot. External event peaks remain
    // unknown.
    p.bitsPerSecond = double(nw) * 8 * c.ndiHz + double(ew) * 8 * c.epHz +
                      (c.statusIntervalUs ? 110.0 * 8 * 1000000 / c.statusIntervalUs : 0);
    p.utilization = p.bitsPerSecond / (p.effectiveMbps * 1000000.0);
    p.feasible = p.utilization <= 1.0 && p.ndiWireUs <= 15000 && p.epWireUs <= 18000 &&
                 p.synchronousWireUs <= 18000;
    p.reason =
        p.feasible
            ? "Capacity screening only; host stalls, state bursts, device timing and physical "
              "egress remain unverified"
            : "Declared load exceeds average capacity or 15/18 ms minimum serialization budget";
    if (p.feasible && p.synchronousWireUs > 17000)
        p.reason += "; less than 1 ms simultaneous-frame wire margin";
    return p;
}

LinkObservation observeLink(const Config &c) {
    LinkObservation out;
    out.observedAtUs = rt::monotonicUs();
#ifndef __linux__
    if (c.targetIp.rfind("127.", 0) == 0) {
        out.loopback = true;
        out.reason = "Host loopback; no physical link or wire timing evidence";
        return out;
    }
#endif
#ifdef _WIN32
    WSADATA w{};
    if (WSAStartup(MAKEWORD(2, 2), &w)) {
        out.reason = "Winsock initialization failed during link observation";
        return out;
    }

    struct Cleanup {
        ~Cleanup() {
            WSACleanup();
        }
    } cleanup;

    sockaddr_in target{};
    target.sin_family = AF_INET;
    if (inet_pton(AF_INET, c.targetIp.c_str(), &target.sin_addr) != 1) {
        out.reason = "Invalid target IPv4";
        return out;
    }
    NET_IFINDEX index = 0;
    if (GetBestInterfaceEx(reinterpret_cast<sockaddr *>(&target), &index) != NO_ERROR) {
        out.reason = "Target route could not be observed";
        return out;
    }
    if (c.bindIp != "0.0.0.0" && c.bindIp != "") {
        ULONG bytes = 0;
        GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, nullptr, &bytes);
        std::vector<std::uint8_t> buffer(bytes);
        auto *adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
        bool found = false;
        if (GetAdaptersAddresses(
                AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                nullptr, adapters, &bytes) == NO_ERROR)
            for (auto *a = adapters; a; a = a->Next)
                for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
                    char ip[INET_ADDRSTRLEN]{};
                    if (u->Address.lpSockaddr->sa_family == AF_INET &&
                        inet_ntop(AF_INET,
                                  &reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr)->sin_addr,
                                  ip, sizeof(ip)) &&
                        c.bindIp == ip) {
                        index = a->IfIndex;
                        found = true;
                    }
                }
        if (!found) {
            out.reason = "Bound IPv4 interface could not be matched";
            return out;
        }
    }
    MIB_IF_ROW2 row{};
    row.InterfaceIndex = index;
    if (GetIfEntry2(&row) != NO_ERROR) {
        out.reason = "Interface link rate unavailable";
        return out;
    }
    char name[768]{};
    WideCharToMultiByte(CP_UTF8, 0, row.Alias, -1, name, sizeof(name), nullptr, nullptr);
    out.interfaceName = name;
    if (row.OperStatus != IfOperStatusUp) {
        out.reason = "Selected interface is not up";
        return out;
    }
    out.physical =
        row.Type == IF_TYPE_ETHERNET_CSMACD && row.InterfaceAndOperStatusFlags.HardwareInterface;
    out.loopback = row.Type == IF_TYPE_SOFTWARE_LOOPBACK;
    if (!out.loopback && row.TransmitLinkSpeed)
        out.mbps =
            static_cast<unsigned>(std::min<ULONG64>(row.TransmitLinkSpeed / 1000000, UINT_MAX));
    out.reason = out.physical
                     ? "Observed local Ethernet hop only; remote path and egress time unverified"
                     : "Wireless/virtual interface; Ethernet serialization model is conditional";
#elif defined(__linux__)
    observeLinuxLink(c, out);
#else
    out.reason = "Route/link observation is unavailable on this platform";
#endif
    return out;
}
} // namespace rt::v2
