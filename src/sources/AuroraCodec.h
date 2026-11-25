#pragma once
#include "protocol/ProtocolTypes.h"
#include <array>
#include <deque>
#include <functional>

namespace rt::aurora {
constexpr std::size_t MaxBxReplyBytes = 16384;
enum class HandleStatus : std::uint8_t {
    Valid = 1,
    Missing = 2,
    Disabled = 4
};

struct Tool {
    std::uint8_t handle = 0;
    HandleStatus status = HandleStatus::Disabled;
    std::array<float, 4> quaternion{}; // q0, qx, qy, qz; only meaningful for Valid
    std::array<float, 3> positionMm{};
    float quality = 0; // vendor metric, not probability/accuracy; 5DOF reports zero
    std::uint32_t portStatus = 0;
    std::uint32_t frameNumber = 0;
    bool transformUsable() const noexcept;
};

struct Reply {
    std::vector<Tool> tools;
    std::uint16_t systemStatus = 0;
};

// One valid position record from a BX reply. Missing/Disabled records never
// produce a position.
struct BxPosition {
    std::uint8_t handle = 0;
    std::array<float, 3> positionMm{};
    std::uint32_t portStatus = 0;
    std::uint32_t frameNumber = 0;
};

struct BxScanStats {
    std::uint64_t frames = 0;
    std::uint64_t valid = 0;
    std::uint64_t missing = 0;
    std::uint64_t disabled = 0;
};

// Strictly parses concatenated standard BX replies (no private header or
// separators). Throws ProtocolError with the byte offset on truncation, bad
// CRC, duplicate handle, unsupported status or trailing data. onValid is called
// once per Valid tool record in frame/tool order; lastReply receives the last
// frame decoded through decodeBx. checkpoint runs before parsing and at least
// every 1024 frames, including frames without a Valid position; it may throw to cancel.
void scanBxSequence(const std::uint8_t *, std::size_t,
                    const std::function<void(const BxPosition &)> &onValid, BxScanStats &,
                    Reply *lastReply = nullptr, const std::function<void()> &checkpoint = {});
constexpr std::size_t SingleToolBxSize = 53;
// Explicit layout for the model-to-BX simulation mapping: one valid tool per
// reply. Kept separate from encodeBx so millions of frames avoid per-frame
// vectors; the fixed 53-byte golden sample asserts both produce equal bytes.
std::array<std::uint8_t, SingleToolBxSize>
encodeSingleValidTool(std::uint8_t handle, const std::array<float, 3> &positionMm,
                      std::uint32_t frameNumber, std::uint32_t portStatus = 0x31,
                      const std::array<float, 4> &quaternion = {1, 0, 0, 0}, float indicator = 0,
                      std::uint16_t systemStatus = 0);
std::uint16_t crc16(const std::uint8_t *, std::size_t) noexcept;
Reply decodeBx(const std::uint8_t *, std::size_t);

inline Reply decodeBx(const Bytes &b) {
    return decodeBx(b.data(), b.size());
}

Bytes encodeBx(const Reply &);
Bytes generateSample(std::size_t toolCount, std::uint32_t sourceFrame);
std::string asciiCommand(const std::string &command, const std::string &parameters = {});
std::string decodeAsciiReply(const Bytes &); // checks CRC16 and CR; returns body
enum class EventKind {
    Bx,
    Error,
    Reset,
    Ascii
};

struct StreamEvent {
    EventKind kind = EventKind::Bx;
    Reply reply;
    Bytes bytes;
    std::string text;
};

// Incremental decoder: length-framed binary can contain CR, with bounded resynchronization.
class StreamDecoder {
  public:
    void feed(const std::uint8_t *, std::size_t);
    bool pop(StreamEvent &);
    void reset();

    std::uint64_t rejected() const noexcept {
        return rejected_;
    }

    std::uint64_t discardedEvents() const noexcept {
        return discardedEvents_;
    }

    std::size_t buffered() const noexcept {
        return buffer_.size();
    }

  private:
    Bytes buffer_;
    std::deque<StreamEvent> events_;
    std::uint64_t rejected_ = 0;
    std::uint64_t discardedEvents_ = 0;
    void parse();
    void pushEvent(StreamEvent);
};
} // namespace rt::aurora
