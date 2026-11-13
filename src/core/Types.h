#pragma once
#include "protocol/ProtocolTypes.h"
#include <functional>
#include <optional>
#include <set>
namespace rt {
    enum class CachePolicy {
        SpecModulo, FifoErratum
    };
    struct ModuleConfig {
        std::uint8_t id=0;
        bool online=true;
        std::uint32_t dataBytes=65531;
        std::uint8_t seed=0;
        std::string file;
        // Empty => deterministic synthetic opaque bytes.
    };
    struct FaultConfig {
        std::set<std::uint16_t> dropFirst, dropAlways, corruptFirst, duplicateFirst;
        bool reverseInitial=false;
        bool any()const {
            return reverseInitial||!dropFirst.empty()||!dropAlways.empty()||!corruptFirst.empty()||!duplicateFirst.empty();
        }
    };
    struct StreamConfig {
        std::string targetIp="127.0.0.1", bindIp="127.0.0.1", allowedNackIp="127.0.0.1";
        std::uint16_t targetPort=5650, commandPort=5500, allowedNackPort=0;
        std::uint32_t initialLogicId=1;
        std::uint64_t frameLimit=0;
        // 0 = continuous; local test control, never a wire field.
        unsigned linkMbps=1000;
        bool allowOversubscribed=false;
        CachePolicy cachePolicy=CachePolicy::SpecModulo;
        std::vector<ModuleConfig> modules {
            ModuleConfig {
            }
        };
        FaultConfig faults;
    };
    struct FragmentStatus {
        std::uint16_t id=0;
        std::uint32_t bytes=0, retries=0;
        std::uint64_t lastSendUs=0;
        std::string state="Queued";
    };
    struct FrameStatus {
        std::uint32_t id=0, bytes=0, mask=0;
        std::uint64_t timestampUs=0;
        std::uint16_t count=0;
        bool initialFinished=false;
        std::vector<FragmentStatus> fragments;
    };
    struct Statistics {
        std::uint64_t sampled=0, built=0, skippedNoModules=0, skippedOverrun=0, buildRejected=0;
        std::uint64_t firstAttempts=0, datagrams=0, bytesSent=0, retransmissions=0, injectedDrops=0, injectedCorrupt=0;
        std::uint64_t nacks=0, invalidNacks=0, cacheMisses=0, duplicatePending=0, queueDrops=0, sourceRejected=0;
        std::uint64_t unsentExpired=0, sendErrors=0, deadlineMisses=0, maxSampleLatenessUs=0, maxSendLatenessUs=0;
        std::uint64_t lastInitialDurationUs=0, elapsedUs=0;
    };
    struct Snapshot {
        bool running=false, draining=false;
        std::string detail="Idle; receiver delivery unknown";
        Statistics stats;
        std::vector<FrameStatus> frames;
        std::uint32_t nextLogicId=1;
        std::uint16_t localPort=0;
        bool dfEnabled=false;
    };
    using SendFunction=std::function<bool(const std::uint8_t*,std::size_t)>;
    std::set<std::uint16_t> parseFragmentIds(const std::string& text);
    std::uint64_t conservativeWireBytes(std::size_t frameBytes);
}
