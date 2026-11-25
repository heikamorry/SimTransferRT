#pragma once
#include "v2/Config.h"
#include "network/IUdpTransport.h"
#include "sources/NdiSource.h"
#include <atomic>
#include <functional>
#include <memory>

namespace rt::v2 {
// JSON is built on the monitoring/caller thread, never in the per-packet loop.
struct RunResult {
    std::string reportJson;
    std::string error;
    std::uint32_t nextNdiId = 1;
    std::uint32_t nextEpId = 1;
    std::uint32_t nextStatusSequence = 1;
    bool success = false;
};

class Session {
  public:
    using TransportFactory = std::function<std::unique_ptr<IUdpTransport>()>;
    using LinkObserver = std::function<LinkObservation(const Config &)>;
    explicit Session(TransportFactory, rt::aurora::NdiSourceFactory ndiSource = {},
                     LinkObserver linkObserver = observeLink);
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    // Owner-thread operation; replacing the factory during a run is rejected.
    void setTransportFactory(TransportFactory);
    void start(const Config &);
    void stop() noexcept;
    bool busy() const noexcept;
    // Exact EP maximum from the latest published, trace-enabled snapshot.
    bool epHandoffExceeded(std::uint64_t thresholdUs) const;
    // True once any retained finalized EP initial span reaches the threshold.
    bool epFragmentSpanReached(std::uint64_t thresholdUs) const;
    // During stop, busy stays true until all source and socket workers finish.
    RunResult snapshot() const;
    RunResult wait();
    bool postStatus(const Bytes &fullSnapshot, std::uint64_t confirmedAtUs = 0);

  private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

RunResult run(const Config &, const Session::TransportFactory &,
              const std::function<bool()> &cancelled = {},
              const std::string &handoffTriggerReport = {},
              const std::string &fragmentTriggerReport = {});
} // namespace rt::v2
