#pragma once
#include "application/CommandDispatcher.h"
#include "network/IUdpTransport.h"
namespace rt {
    using SnapshotCallback=std::function<void(const Snapshot&)>;
    using LogCallback=std::function<void(const std::string&)>;
    // Runs entirely on one worker. Caller owns thread; exceptions propagate only to this explicit boundary.
    Snapshot runStream(const StreamConfig&,IUdpTransport&,CommandDispatcher&,const SnapshotCallback&,const LogCallback&);
    std::string snapshotJson(const Snapshot&);
}
