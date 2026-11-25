#pragma once
#include "sources/AuroraSource.h"
#include <memory>

namespace rt::aurora {
using HostClock = std::function<std::uint64_t()>;

// One instance belongs to exactly one acquisition worker. No socket sender calls this interface.
class INdiSource {
  public:
    virtual ~INdiSource() = default;
    virtual void start(const std::atomic<bool> &cancel) = 0;
    virtual SourceSample read(const HostClock &, const std::atomic<bool> &cancel) = 0;
    virtual void shutdown() = 0;
    virtual std::string kind() const = 0;
    // Source rate, never query/UDP rate. Replay's original rates are unknown and return zero.
    virtual double actualHz() const = 0;
    virtual double counterHz() const = 0;
};

using NdiSourceFactory = std::function<std::unique_ptr<INdiSource>()>;
using DeviceSetup = std::function<void(DeviceSource &)>;
NdiSourceFactory simulationSourceFactory(std::size_t tools, unsigned hz);
NdiSourceFactory replaySourceFactory(Bytes completeBxReply, unsigned queryHz = 60);
// setup runs after initialization/rate queries, on the source thread. It may install an
// externally verified common-clock anchor; common-clock timestamps are NOT assumed to be QPC.
// Without an anchor, read returns sourceTime=nullopt. The runner must reject unverified
// device frames for protocol transmission; it may still expose source diagnostics.
NdiSourceFactory deviceSourceFactory(Exchange, DeviceSetup setup = {});
} // namespace rt::aurora
