#pragma once
#include "application/CommandDispatcher.h"
#include "network/IUdpTransport.h"
namespace rt {
    using SnapshotCallback=std::function<void(const Snapshot&)>;
    using LogCallback=std::function<void(const std::string&)>;
    struct RunnerHooks {
        std::function<std::uint64_t()> clock;
        std::function<void(std::uint64_t)> waitUntil;
        // Optional bounded local control poll; independent of deferred UI publication.
        std::function<void()> pollControl;
        // Priority cancellation check, also under continuous sampling overload.
        // Must only read a flag: no I/O, allocation, waiting or command dispatch.
        std::function<bool()> cancelRequested;
    };
    // Runs entirely on one worker. Caller owns thread; exceptions propagate only to this explicit boundary.
    Snapshot runStream(const StreamConfig&,IUdpTransport&,CommandDispatcher&,const SnapshotCallback&,const LogCallback&,const RunnerHooks& hooks={});
    std::string snapshotJson(const Snapshot&);
}
