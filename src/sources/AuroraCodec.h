#pragma once
#include "protocol/ProtocolTypes.h"
#include <array>
#include <deque>
namespace rt::aurora {
constexpr std::size_t MaxBxReplyBytes=16384;
enum class HandleStatus:std::uint8_t {Valid=1,Missing=2,Disabled=4};
struct Tool {
    std::uint8_t handle=0;
    HandleStatus status=HandleStatus::Disabled;
    std::array<float,4> quaternion{}; // q0, qx, qy, qz; only meaningful for Valid
    std::array<float,3> positionMm{};
    float quality=0; // vendor metric, not probability/accuracy; 5DOF reports zero
    std::uint32_t portStatus=0,frameNumber=0;
    bool transformUsable() const noexcept;
};
struct Reply {std::vector<Tool> tools;std::uint16_t systemStatus=0;};
std::uint16_t crc16(const std::uint8_t*,std::size_t) noexcept;
Reply decodeBx(const std::uint8_t*,std::size_t);
inline Reply decodeBx(const Bytes&b){return decodeBx(b.data(),b.size());}
Bytes encodeBx(const Reply&);
Bytes generateSample(std::size_t toolCount,std::uint32_t sourceFrame);
std::string asciiCommand(const std::string& command,const std::string& parameters={});
std::string decodeAsciiReply(const Bytes&); // checks CRC16 and CR; returns body
enum class EventKind {Bx,Error,Reset,Ascii};
struct StreamEvent {EventKind kind=EventKind::Bx;Reply reply;Bytes bytes;std::string text;};
// Incremental decoder: length-framed binary can contain CR, with bounded resynchronization.
class StreamDecoder {
public:
    void feed(const std::uint8_t*,std::size_t);
    bool pop(StreamEvent&);
    void reset();
    std::uint64_t rejected()const noexcept{return rejected_;}
    std::uint64_t discardedEvents()const noexcept{return discardedEvents_;}
    std::size_t buffered()const noexcept{return buffer_.size();}
private:
    Bytes buffer_;std::deque<StreamEvent> events_;std::uint64_t rejected_=0,discardedEvents_=0;
    void parse();void pushEvent(StreamEvent);
};
}
