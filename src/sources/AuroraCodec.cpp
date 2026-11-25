#include "sources/AuroraCodec.h"
#include "protocol/PacketCodec.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

namespace rt::aurora {
namespace {
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
              "Aurora requires IEEE binary32");

float getFloat(const std::uint8_t *p) {
    auto bits = readLe32(p);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

void putFloat(std::uint8_t *p, float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    writeLe32(p, bits);
}

std::size_t recordSize(HandleStatus s) {
    switch (s) {
    case HandleStatus::Valid:
        return 42;
    case HandleStatus::Missing:
        return 10;
    case HandleStatus::Disabled:
        return 2;
    }
    throw ProtocolError("Aurora unknown handle status");
}

unsigned hexDigit(char c) {
    if (c >= '0' && c <= '9')
        return unsigned(c - '0');
    if (c >= 'a' && c <= 'f')
        return unsigned(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return unsigned(c - 'A' + 10);
    throw ProtocolError("Aurora invalid ASCII CRC");
}

std::string hex4(std::uint16_t v) {
    std::ostringstream s;
    s << std::uppercase << std::hex << std::setfill('0') << std::setw(4) << v;
    return s.str();
}

void validNumbers(const Tool &t) {
    for (auto f : t.quaternion)
        if (!std::isfinite(f))
            throw ProtocolError("Aurora nonfinite quaternion");
    for (auto f : t.positionMm)
        if (!std::isfinite(f))
            throw ProtocolError("Aurora nonfinite position");
    if (!std::isfinite(t.quality) || t.quality < 0)
        throw ProtocolError("Aurora invalid quality indicator");
}
} // namespace

bool Tool::transformUsable() const noexcept {
    // Ignore reserved status bits; known out-of-volume/fault bits invalidate consumer use.
    constexpr std::uint32_t faults =
        (1u << 6) | (1u << 7) | (1u << 8) | (1u << 10) | (1u << 11) | (1u << 12);
    return status == HandleStatus::Valid && (portStatus & faults) == 0;
}

std::uint16_t crc16(const std::uint8_t *p, std::size_t n) noexcept {
    std::uint16_t c = 0;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (unsigned b = 0; b < 8; ++b)
            c = std::uint16_t((c >> 1) ^ ((c & 1) ? 0xa001 : 0));
    }
    return c;
}

Reply decodeBx(const std::uint8_t *p, std::size_t n) {
    if (!p || n < 11 || n > MaxBxReplyBytes)
        throw ProtocolError("Aurora invalid BX reply size");
    if (readLe16(p) != 0xa5c4 || readLe16(p + 2) + 8u != n)
        throw ProtocolError("Aurora BX start/length mismatch");
    if (crc16(p, 4) != readLe16(p + 4))
        throw ProtocolError("Aurora BX header CRC16 mismatch");
    if (crc16(p + 6, n - 8) != readLe16(p + n - 2))
        throw ProtocolError("Aurora BX body CRC16 mismatch");
    Reply r;
    auto count = p[6];
    r.tools.reserve(count);
    std::size_t off = 7;
    std::set<std::uint8_t> handles;
    for (unsigned i = 0; i < count; ++i) {
        if (off + 2 > n - 4)
            throw ProtocolError("Aurora truncated tool record");
        Tool t;
        t.handle = p[off];
        t.status = static_cast<HandleStatus>(p[off + 1]);
        auto size = recordSize(t.status);
        if (off + size > n - 4)
            throw ProtocolError("Aurora truncated tool body");
        if (!handles.insert(t.handle).second)
            throw ProtocolError("Aurora duplicate handle");
        if (t.status == HandleStatus::Valid) {
            for (unsigned k = 0; k < 4; ++k)
                t.quaternion[k] = getFloat(p + off + 2 + 4 * k);
            for (unsigned k = 0; k < 3; ++k)
                t.positionMm[k] = getFloat(p + off + 18 + 4 * k);
            t.quality = getFloat(p + off + 30);
            t.portStatus = readLe32(p + off + 34);
            t.frameNumber = readLe32(p + off + 38);
            validNumbers(t);
        } else if (t.status == HandleStatus::Missing) {
            t.portStatus = readLe32(p + off + 2);
            t.frameNumber = readLe32(p + off + 6);
        }
        r.tools.push_back(t);
        off += size;
    }
    if (off != n - 4)
        throw ProtocolError("Aurora BX unexpected trailing data");
    r.systemStatus = readLe16(p + off);
    return r;
}

void scanBxSequence(const std::uint8_t *p, std::size_t n,
                    const std::function<void(const BxPosition &)> &onValid, BxScanStats &stats,
                    Reply *lastReply, const std::function<void()> &checkpoint) {
    if (!p && n)
        throw ProtocolError("Aurora null BX buffer");
    std::size_t off = 0;
    std::size_t lastOffset = 0;
    std::size_t lastLength = 0;
    std::size_t scanned = 0;
    while (off < n) {
        if (checkpoint && (scanned++ & 1023) == 0)
            checkpoint();
        if (n - off < 6)
            throw ProtocolError("Aurora truncated BX header at offset " + std::to_string(off));
        if (readLe16(p + off) != 0xa5c4)
            throw ProtocolError("Aurora BX start mismatch at offset " + std::to_string(off));
        const auto bodyLength = std::size_t(readLe16(p + off + 2));
        const auto length = bodyLength + 8;
        if (length < 11 || length > MaxBxReplyBytes || off + length > n)
            throw ProtocolError("Aurora BX length mismatch at offset " + std::to_string(off));
        if (crc16(p + off, 4) != readLe16(p + off + 4))
            throw ProtocolError("Aurora BX header CRC16 mismatch at offset " + std::to_string(off));
        if (crc16(p + off + 6, bodyLength) != readLe16(p + off + length - 2))
            throw ProtocolError("Aurora BX body CRC16 mismatch at offset " + std::to_string(off));
        const auto *body = p + off;
        const auto count = body[6];
        std::size_t at = 7;
        std::array<bool, 256> handles{};
        const auto recordEnd =
            std::size_t(6) + bodyLength - 2; // body minus the 2-byte SystemStatus
        for (unsigned i = 0; i < count; ++i) {
            if (at + 2 > recordEnd)
                throw ProtocolError("Aurora truncated tool record at offset " +
                                    std::to_string(off + at));
            Tool t;
            t.handle = body[at];
            t.status = static_cast<HandleStatus>(body[at + 1]);
            const auto size = recordSize(t.status);
            if (at + size > recordEnd)
                throw ProtocolError("Aurora truncated tool body at offset " +
                                    std::to_string(off + at));
            if (handles[t.handle])
                throw ProtocolError("Aurora duplicate handle at offset " +
                                    std::to_string(off + at));
            handles[t.handle] = true;
            if (t.status == HandleStatus::Valid) {
                for (unsigned k = 0; k < 4; ++k)
                    t.quaternion[k] = getFloat(body + at + 2 + 4 * k);
                for (unsigned k = 0; k < 3; ++k)
                    t.positionMm[k] = getFloat(body + at + 18 + 4 * k);
                t.quality = getFloat(body + at + 30);
                t.portStatus = readLe32(body + at + 34);
                t.frameNumber = readLe32(body + at + 38);
                validNumbers(t);
                if (onValid)
                    onValid({t.handle, t.positionMm, t.portStatus, t.frameNumber});
                ++stats.valid;
            } else if (t.status == HandleStatus::Missing)
                ++stats.missing;
            else
                ++stats.disabled;
            at += size;
        }
        if (at != recordEnd)
            throw ProtocolError("Aurora BX unexpected trailing data at offset " +
                                std::to_string(off + at));
        ++stats.frames;
        lastOffset = off;
        lastLength = length;
        off += length;
    }
    if (lastReply) {
        if (!stats.frames)
            throw ProtocolError("Aurora BX sequence is empty");
        *lastReply = decodeBx(p + lastOffset, lastLength);
    }
}

std::array<std::uint8_t, SingleToolBxSize>
encodeSingleValidTool(std::uint8_t handle, const std::array<float, 3> &positionMm,
                      std::uint32_t frameNumber, std::uint32_t portStatus,
                      const std::array<float, 4> &quaternion, float indicator,
                      std::uint16_t systemStatus) {
    Tool tool;
    tool.handle = handle;
    tool.status = HandleStatus::Valid;
    tool.quaternion = quaternion;
    tool.positionMm = positionMm;
    tool.quality = indicator;
    tool.portStatus = portStatus;
    tool.frameNumber = frameNumber;
    validNumbers(tool);
    std::array<std::uint8_t, SingleToolBxSize> b{};
    auto *p = b.data();
    writeLe16(p, 0xa5c4);
    writeLe16(p + 2, 45);
    writeLe16(p + 4, crc16(p, 4));
    p[6] = 1;
    p[7] = handle;
    p[8] = 1;
    for (unsigned k = 0; k < 4; ++k)
        putFloat(p + 9 + 4 * k, quaternion[k]);
    for (unsigned k = 0; k < 3; ++k)
        putFloat(p + 25 + 4 * k, positionMm[k]);
    putFloat(p + 37, indicator);
    writeLe32(p + 41, portStatus);
    writeLe32(p + 45, frameNumber);
    writeLe16(p + 49, systemStatus);
    writeLe16(p + 51, crc16(p + 6, 45));
    return b;
}

Bytes encodeBx(const Reply &r) {
    if (r.tools.size() > 255)
        throw ProtocolError("Aurora handle count exceeds one byte");
    std::size_t n = 11;
    std::set<std::uint8_t> handles;
    for (const auto &t : r.tools) {
        n += recordSize(t.status);
        if (!handles.insert(t.handle).second)
            throw ProtocolError("Aurora duplicate handle");
        if (t.status == HandleStatus::Valid)
            validNumbers(t);
    }
    if (n > MaxBxReplyBytes)
        throw ProtocolError("Aurora BX buffer limit");
    Bytes b(n);
    auto *p = b.data();
    writeLe16(p, 0xa5c4);
    writeLe16(p + 2, std::uint16_t(n - 8));
    writeLe16(p + 4, crc16(p, 4));
    p[6] = std::uint8_t(r.tools.size());
    std::size_t off = 7;
    for (const auto &t : r.tools) {
        p[off] = t.handle;
        p[off + 1] = std::uint8_t(t.status);
        if (t.status == HandleStatus::Valid) {
            for (unsigned k = 0; k < 4; ++k)
                putFloat(p + off + 2 + 4 * k, t.quaternion[k]);
            for (unsigned k = 0; k < 3; ++k)
                putFloat(p + off + 18 + 4 * k, t.positionMm[k]);
            putFloat(p + off + 30, t.quality);
            writeLe32(p + off + 34, t.portStatus);
            writeLe32(p + off + 38, t.frameNumber);
        } else if (t.status == HandleStatus::Missing) {
            writeLe32(p + off + 2, t.portStatus);
            writeLe32(p + off + 6, t.frameNumber);
        }
        off += recordSize(t.status);
    }
    writeLe16(p + off, r.systemStatus);
    writeLe16(p + n - 2, crc16(p + 6, n - 8));
    return b;
}

Bytes generateSample(std::size_t count, std::uint32_t sourceFrame) {
    if (count > 255)
        throw ProtocolError("Aurora simulated handle count exceeds 255");
    Reply r;
    r.tools.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        Tool t;
        t.handle = std::uint8_t(i + 1);
        t.status = HandleStatus::Valid;
        t.quaternion = {1, 0, 0, 0};
        t.positionMm = {float(i), float(std::sin(double(sourceFrame % 3200) * 0.01)), 100.0f};
        t.quality = 0.1f;
        t.portStatus = 0x31;
        t.frameNumber = sourceFrame;
        r.tools.push_back(t);
    }
    return encodeBx(r);
}

std::string asciiCommand(const std::string &command, const std::string &parameters) {
    if (command.empty() || command.find_first_of("\r\n :") != std::string::npos ||
        parameters.find_first_of("\r\n") != std::string::npos)
        throw ProtocolError("Aurora invalid command text");
    auto s = command + ":" + parameters;
    return s + hex4(crc16(reinterpret_cast<const std::uint8_t *>(s.data()), s.size())) + "\r";
}

std::string decodeAsciiReply(const Bytes &b) {
    if (b.size() < 5 || b.size() > MaxBxReplyBytes || b.back() != '\r')
        throw ProtocolError("Aurora invalid ASCII reply framing");
    auto n = b.size() - 5;
    std::uint16_t expected = 0;
    for (std::size_t i = n; i < n + 4; ++i)
        expected = std::uint16_t(expected * 16 + hexDigit(char(b[i])));
    if (crc16(b.data(), n) != expected)
        throw ProtocolError("Aurora ASCII CRC16 mismatch");
    return std::string(b.begin(), b.begin() + n);
}

void StreamDecoder::pushEvent(StreamEvent e) {
    if (events_.size() == 32) {
        events_.pop_front();
        ++discardedEvents_;
    }
    events_.push_back(std::move(e));
}

void StreamDecoder::reset() {
    buffer_.clear();
    events_.clear();
}

bool StreamDecoder::pop(StreamEvent &e) {
    if (events_.empty())
        return false;
    e = std::move(events_.front());
    events_.pop_front();
    return true;
}

void StreamDecoder::feed(const std::uint8_t *p, std::size_t n) {
    if (!p && n)
        throw ProtocolError("Aurora null stream input");
    while (n) {
        if (buffer_.size() == MaxBxReplyBytes) {
            bool keep = buffer_.back() == 0xc4;
            buffer_.clear();
            if (keep)
                buffer_.push_back(0xc4);
            ++rejected_;
        }
        auto take = std::min(n, MaxBxReplyBytes - buffer_.size());
        buffer_.insert(buffer_.end(), p, p + take);
        p += take;
        n -= take;
        parse();
    }
}

void StreamDecoder::parse() {
    while (!buffer_.empty()) {
        if (buffer_[0] == 0xc4) {
            if (buffer_.size() < 2)
                return;
            if (buffer_[1] != 0xa5) {
                buffer_.erase(buffer_.begin());
                ++rejected_;
                continue;
            }
            if (buffer_.size() < 6)
                return;
            auto n = std::size_t(readLe16(buffer_.data() + 2)) + 8;
            if (n < 11 || n > MaxBxReplyBytes ||
                crc16(buffer_.data(), 4) != readLe16(buffer_.data() + 4)) {
                buffer_.erase(buffer_.begin());
                ++rejected_;
                continue;
            }
            if (buffer_.size() < n)
                return;
            StreamEvent e;
            e.bytes.assign(buffer_.begin(), buffer_.begin() + n);
            try {
                e.reply = decodeBx(e.bytes);
                pushEvent(std::move(e));
                buffer_.erase(buffer_.begin(), buffer_.begin() + n);
            } catch (const ProtocolError &) {
                buffer_.erase(buffer_.begin());
                ++rejected_;
            }
            continue;
        }
        auto cr = std::find(buffer_.begin(), buffer_.end(), '\r');
        auto binary = std::find(buffer_.begin(), buffer_.end(), std::uint8_t(0xc4));
        if (binary != buffer_.end() && (cr == buffer_.end() || binary < cr)) {
            buffer_.erase(buffer_.begin(), binary);
            ++rejected_;
            continue;
        }
        if (cr == buffer_.end())
            return;
        Bytes b(buffer_.begin(), cr + 1);
        buffer_.erase(buffer_.begin(), cr + 1);
        try {
            StreamEvent e;
            e.text = decodeAsciiReply(b);
            e.bytes = std::move(b);
            e.kind = e.text.rfind("ERROR", 0) == 0 ? EventKind::Error
                     : e.text == "RESET"           ? EventKind::Reset
                                                   : EventKind::Ascii;
            pushEvent(std::move(e));
        } catch (const ProtocolError &) {
            ++rejected_;
        }
    }
}
} // namespace rt::aurora
