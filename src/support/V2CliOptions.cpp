#include "support/V2CliOptions.h"
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

namespace rt::v2 {
namespace {
std::uint64_t number(const std::string &s, std::uint64_t maximum) {
    if (s.empty() || s.front() == '-' || s.front() == '+')
        throw std::invalid_argument("invalid unsigned integer: " + s);
    std::size_t used = 0;
    const auto n = std::stoull(s, &used, 0);
    if (used != s.size() || n > maximum)
        throw std::invalid_argument("integer out of range: " + s);
    return n;
}

std::uint64_t seconds(const std::string &s) {
    std::size_t used = 0;
    const double d = std::stod(s, &used);
    if (used != s.size() || !std::isfinite(d) || d < 0 || d > 86400.0 * 365.0)
        throw std::invalid_argument("duration must be finite seconds from 0 to 31536000");
    return static_cast<std::uint64_t>(std::llround(d * 1000000.0));
}

bool traceMode(const std::string &value, const std::string &option) {
    if (value == "on")
        return true;
    if (value == "off")
        return false;
    throw std::invalid_argument(option + " expects on or off");
}
} // namespace

CliOptions parseV2Options(int argc, char **argv) {
    CliOptions o;
    std::optional<unsigned> profile;
    std::optional<unsigned> legacy;
    bool nackExplicit = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() {
            if (i + 1 >= argc)
                throw std::invalid_argument("missing value for " + a);
            return std::string(argv[++i]);
        };
        if (a == "--help" || a == "-h")
            o.help = true;
        else if (a == "--protocol") {
            if (value() != "v2")
                throw std::invalid_argument("V2 options require --protocol v2");
        } else if (a == "--transport") {
            auto v = value();
            if (v != "native" && v != "asio-sync" && v != "asio-async")
                throw std::invalid_argument("--transport expects native, asio-sync or asio-async");
            o.transport = v;
        } else if (a == "--mailbox-fast-path")
            o.config.mailboxFastPath = traceMode(value(), a);
        else if (a == "--sample-lead-cap-us")
            o.config.sampleLeadCapUs = static_cast<unsigned>(number(value(), 4000));
        else if (a == "--ndi-handoff-guard-us")
            o.config.ndiHandoffGuardUs = static_cast<unsigned>(number(value(), 400));
        else if (a == "--network-profile") {
            const auto n = parseNetworkProfile(value());
            if (profile && *profile != n)
                throw std::invalid_argument("conflicting network profiles");
            profile = n;
        } else if (a == "--link-mbps") {
            const auto n = static_cast<unsigned>(number(value(), 2500));
            if (n != 100 && n != 1000 && n != 2500)
                throw std::invalid_argument("V2 supports only 100, 1000 or 2500 Mbps");
            if (legacy && *legacy != n)
                throw std::invalid_argument("conflicting link speeds");
            legacy = n;
        } else if (a == "--target")
            o.config.targetIp = value();
        else if (a == "--bind")
            o.config.bindIp = value();
        else if (a == "--port")
            o.config.targetPort = static_cast<std::uint16_t>(number(value(), 65535));
        else if (a == "--command-port" || a == "--cmd-port")
            o.config.commandPort = static_cast<std::uint16_t>(number(value(), 65535));
        else if (a == "--allowed-nack-ip" || a == "--nack-ip") {
            o.config.allowedNackIp = value();
            nackExplicit = true;
        } else if (a == "--allowed-nack-port" || a == "--nack-port")
            o.config.allowedNackPort = static_cast<std::uint16_t>(number(value(), 65535));
        else if (a == "--observed-link-mbps")
            o.config.observedLinkMbps = static_cast<unsigned>(number(value(), 1000000));
        else if (a == "--path-limit-mbps")
            o.config.pathLimitMbps = static_cast<unsigned>(number(value(), 1000000));
        else if (a == "--ep-bytes")
            o.config.epBytes = static_cast<std::uint32_t>(number(value(), MaxFrameBytes));
        else if (a == "--ndi-tools")
            o.config.ndiTools = static_cast<std::uint32_t>(number(value(), 255));
        else if (a == "--ndi-hz")
            o.config.ndiHz = static_cast<unsigned>(number(value(), 1000));
        else if (a == "--ep-hz")
            o.config.epHz = static_cast<unsigned>(number(value(), 1000));
        else if (a == "--ndi-replay")
            o.config.ndiReplayFile = value();
        else if (a == "--ep-replay")
            o.config.epReplayFile = value();
        else if (a == "--no-ndi")
            o.config.ndiEnabled = false;
        else if (a == "--no-ep")
            o.config.epEnabled = false;
        else if (a == "--duration")
            o.config.durationUs = seconds(value());
        else if (a == "--frames")
            o.config.frameLimit = number(value(), 1000000000ull);
        else if (a == "--ndi-start-id")
            o.config.initialNdiId = static_cast<std::uint32_t>(number(value(), 0xFFFFFFFFull));
        else if (a == "--ep-start-id")
            o.config.initialEpId = static_cast<std::uint32_t>(number(value(), 0xFFFFFFFFull));
        else if (a == "--status-start-seq")
            o.config.initialStatusSequence =
                static_cast<std::uint32_t>(number(value(), 0xFFFFFFFFull));
        else if (a == "--ndi-phase-us")
            o.config.ndiPhaseUs = number(value(), 1000000);
        else if (a == "--ep-phase-us")
            o.config.epPhaseUs = number(value(), 1000000);
        else if (a == "--status-interval-us")
            o.config.statusIntervalUs = number(value(), 3600000000ull);
        else if (a == "--run-id")
            o.config.runId = value();
        else if (a == "--report")
            o.report = value();
        else if (a == "--handoff-trigger-report")
            o.handoffTriggerReport = value();
        else if (a == "--fragment-trigger-report")
            o.fragmentTriggerReport = value();
        else if (a == "--handoff-trace")
            o.config.handoffTrace = traceMode(value(), a);
        else if (a == "--fragment-trace")
            o.config.fragmentTrace = traceMode(value(), a);
        else if (a == "--asio-pooled-handler")
            o.config.asioPooledHandler = traceMode(value(), a);
        else if (a == "--allow-oversubscribed")
            o.config.allowOversubscribed = true;
        else if (a == "--drop-first")
            o.config.faults.dropFirst = parseFragmentIds(value());
        else if (a == "--drop-always")
            o.config.faults.dropAlways = parseFragmentIds(value());
        else if (a == "--corrupt-first")
            o.config.faults.corruptFirst = parseFragmentIds(value());
        else if (a == "--duplicate-first")
            o.config.faults.duplicateFirst = parseFragmentIds(value());
        else if (a == "--reverse")
            o.config.faults.reverseInitial = true;
        else if (a == "--fault-module")
            o.config.faultModule = static_cast<std::uint8_t>(number(value(), 2));
        else
            throw std::invalid_argument("unknown V2 option: " + a);
    }
    if (profile && legacy && *profile != *legacy)
        throw std::invalid_argument("--network-profile conflicts with --link-mbps");
    o.config.linkMbps = profile.value_or(legacy.value_or(1000));
    if (!nackExplicit)
        o.config.allowedNackIp = o.config.targetIp;
    if (!o.handoffTriggerReport.empty() && !o.config.handoffTrace)
        throw std::invalid_argument("--handoff-trigger-report requires --handoff-trace on");
    if (!o.fragmentTriggerReport.empty() && !o.config.fragmentTrace)
        throw std::invalid_argument("--fragment-trigger-report requires --fragment-trace on");
    if (!o.help)
        validateConfig(o.config);
    return o;
}

std::string v2CliHelp() {
    return "SimTransfer V2 / Plan B (Asio-only branch)\n"
           "  --protocol v2 (required) --network-profile 100m|1g|2g5 (default 1g; 2.5g alias)\n"
           "  --transport native|asio-sync|asio-async (both CLIs default to native; the GUI is "
           "Asio-only)\n"
           "  --mailbox-fast-path on|off (four-slot SPSC frame handoff; default off)\n"
           "  --sample-lead-cap-us 80..4000 (default 2000, capped at 1/8 of one period)\n"
           "  --ndi-handoff-guard-us 0..400 (0 disables the idle sender's NDI pre-wake)\n"
           "  --link-mbps 100|1000|2500 (compatibility alias; conflicting values fail)\n"
           "  --target 127.0.0.1 --port 5650 --bind 127.0.0.1 --command-port 5500\n"
           "  --allowed-nack-ip 127.0.0.1 --allowed-nack-port 0\n"
           "  --ep-bytes 204800 --ndi-tools 4 --ndi-hz 60 --ep-hz 50\n"
           "  --ndi-replay reply.bx --ep-replay frame.bin (default: simulated inputs)\n"
           "  --no-ndi --no-ep --duration 600 --frames 0 (0=continuous)\n"
           "  --observed-link-mbps 0 --path-limit-mbps 0 (0=unknown, not measured)\n"
           "  --ndi-start-id 1 --ep-start-id 1 --status-start-seq 1\n"
           "  --ndi-phase-us 0 --ep-phase-us 0 --status-interval-us 0\n"
           "  --drop-first 2 --drop-always 2 --corrupt-first 2 --duplicate-first 2 --reverse\n"
           "  --fault-module 1|2 --allow-oversubscribed (explicit overload test)\n"
           "  --run-id unique-run --report report.json\n"
           "  --handoff-trace on|off (bounded frame handoff timings, default off)\n"
           "  --handoff-trigger-report new-file.json (snapshot at first observed EP ready-to-first "
           ">=7 ms)\n"
           "  --fragment-trace on|off (bounded initial-fragment send intervals, default off)\n"
            "  --asio-pooled-handler on|off (reuse Asio async operation storage, default off)\n"
           "  --fragment-trigger-report new-file.json (snapshot on EP first-to-last span >=18 ms)\n"
           "Simulated 60 Hz is not proof of Aurora hardware sampling at 60 Hz.\n";
}
} // namespace rt::v2
