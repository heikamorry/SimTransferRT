#include "sources/NdiSource.h"
#include <exception>

namespace rt::aurora {
namespace {
void ready(bool started, const HostClock &clock, const std::atomic<bool> &cancel) {
    if (!started)
        throw ProtocolError("NDI source has not started");
    if (cancel.load())
        throw ProtocolError("NDI source cancelled");
    if (!clock)
        throw ProtocolError("NDI host clock is required");
}

class Simulation final : public INdiSource {
  public:
    Simulation(std::size_t tools, unsigned hz) : tools_(tools), hz_(hz) {}

    void start(const std::atomic<bool> &cancel) override {
        if (cancel.load())
            throw ProtocolError("NDI source cancelled");
        tracker_.reset();
        counter_ = 0;
        started_ = true;
    }

    SourceSample read(const HostClock &clock, const std::atomic<bool> &cancel) override {
        ready(started_, clock, cancel);
        auto sample = tracker_.observe(generateSample(tools_, counter_), 0);
        counter_ += 8;
        sample.hostReadyUs = clock();
        return sample;
    }

    void shutdown() override {
        started_ = false;
        tracker_.reset();
    }

    std::string kind() const override {
        return "simulation";
    }

    double actualHz() const override {
        return hz_;
    }

    double counterHz() const override {
        return double(hz_) * 8;
    }

  private:
    std::size_t tools_;
    unsigned hz_;
    std::uint32_t counter_ = 0;
    bool started_ = false;
    SampleTracker tracker_;
};

class RepeatedReply final : public INdiSource {
  public:
    explicit RepeatedReply(Bytes bytes) : bytes_(std::move(bytes)) {}

    void start(const std::atomic<bool> &cancel) override {
        if (cancel.load())
            throw ProtocolError("NDI source cancelled");
        decodeBx(bytes_);
        tracker_.reset();
        started_ = true;
    }

    SourceSample read(const HostClock &clock, const std::atomic<bool> &cancel) override {
        ready(started_, clock, cancel);
        auto sample = tracker_.observe(bytes_, 0);
        sample.hostReadyUs = clock();
        return sample;
    }

    void shutdown() override {
        started_ = false;
        tracker_.reset();
    }

    std::string kind() const override {
        return "replay";
    }

    double actualHz() const override {
        return 0;
    }

    double counterHz() const override {
        return 0;
    }

  private:
    Bytes bytes_;
    SampleTracker tracker_;
    bool started_ = false;
};

class Device final : public INdiSource {
  public:
    Device(Exchange exchange, DeviceSetup setup)
        : source_(std::move(exchange)), setup_(std::move(setup)) {}

    ~Device() override {
        try {
            shutdown();
        } catch (...) {
        }
    }

    void start(const std::atomic<bool> &cancel) override {
        if (started_)
            throw ProtocolError("NDI device is already started");
        if (cancel.load())
            throw ProtocolError("NDI source cancelled");
        try {
            source_.connect(cancel);
            if (setup_)
                setup_(source_);
            started_ = true;
        } catch (...) {
            auto failure = std::current_exception();
            try {
                shutdown();
            } catch (...) {
            }
            std::rethrow_exception(failure);
        }
    }

    SourceSample read(const HostClock &clock, const std::atomic<bool> &cancel) override {
        ready(started_, clock, cancel);
        return source_.acquire(clock, cancel);
    }

    void shutdown() override {
        started_ = false;
        if (source_.state() == SourceState::Disconnected)
            return;
        // Cancellation stops ordinary reads immediately. Cleanup gets a distinct token so
        // TSTOP is actually sent, while Exchange still enforces its 1000 ms timeout.
        std::atomic<bool> cleanupCancel{false};
        try {
            source_.stop(cleanupCancel);
            source_.disconnect();
        } catch (...) {
            source_.disconnect();
            throw;
        }
    }

    std::string kind() const override {
        return "device";
    }

    double actualHz() const override {
        return source_.trackingFrequencyHz();
    }

    double counterHz() const override {
        return source_.counterFrequencyHz();
    }

  private:
    DeviceSource source_;
    DeviceSetup setup_;
    bool started_ = false;
};
} // namespace

NdiSourceFactory simulationSourceFactory(std::size_t tools, unsigned hz) {
    if (tools > 255 || !hz || hz > 1000)
        throw ProtocolError("Invalid simulated NDI source configuration");
    return [tools, hz] {
        return std::make_unique<Simulation>(tools, hz);
    };
}

NdiSourceFactory replaySourceFactory(Bytes bytes, unsigned queryHz) {
    if (bytes.empty() || bytes.size() > MaxBxReplyBytes || !queryHz || queryHz > 1000)
        throw ProtocolError("Invalid NDI replay configuration");
    return [bytes = std::move(bytes)] {
        return std::make_unique<RepeatedReply>(bytes);
    };
}

NdiSourceFactory deviceSourceFactory(Exchange exchange, DeviceSetup setup) {
    if (!exchange)
        throw ProtocolError("NDI device exchange is required");
    return [exchange = std::move(exchange), setup = std::move(setup)] {
        return std::make_unique<Device>(exchange, setup);
    };
}
} // namespace rt::aurora
