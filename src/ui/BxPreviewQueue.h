#pragma once
#include <QByteArray>
#include <cstdint>
#include <optional>
#include <utility>

namespace rt {
struct BxPreview {
    QByteArray bytes;
    std::uint64_t completedUs;
    std::uint32_t frameId;
};

// Single latest frame shared by the receiver and the GUI under receiverMutex.
class BxPreviewQueue {
  public:
    void begin() {
        latest_.reset();
        nextUs_ = 0;
        state_ = State::Receiving;
    }

    void finish(bool fileTaskActive) {
        if (fileTaskActive)
            close();
        else
            state_ = State::Finishing;
    }

    void localActionStarted() {
        if (state_ == State::Finishing)
            close();
    }

    void close() {
        state_ = State::Closed;
        latest_.reset();
    }

    // The caller owns the replaced buffer and releases it after unlocking.
    void offer(std::optional<BxPreview> &frame) {
        if (state_ != State::Closed)
            latest_.swap(frame);
    }

    std::optional<BxPreview> take(std::uint64_t now, bool panelBusy, bool receiverRunning) {
        if (!latest_)
            return {};
        if (state_ == State::Finishing) {
            if (receiverRunning)
                return {};
        } else if (panelBusy || now < nextUs_)
            return {};
        nextUs_ = now + 100000;
        return std::exchange(latest_, std::nullopt);
    }

  private:
    enum class State {
        Closed,
        Receiving,
        Finishing
    };
    State state_ = State::Closed;
    std::optional<BxPreview> latest_;
    std::uint64_t nextUs_ = 0;
};
} // namespace rt
