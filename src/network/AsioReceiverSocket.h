#pragma once
#include "protocol/ProtocolTypes.h"
#include <memory>
#include <optional>

namespace rt {
// 由接收线程独占；IPv4 地址使用主机字节序。反馈与接收共用同一个绑定端口。
class AsioReceiverSocket final {
  public:
    AsioReceiverSocket(std::uint32_t bindIp, std::uint16_t port);
    ~AsioReceiverSocket();
    void waitReadable(std::uint64_t deadlineUs);
    // Nonblocking observation when reporting NACK generation. Does not consume data.
    std::optional<bool> readableNow() const;
    // 返回 -1 表示当前没有数据；零字节 UDP 包仍然算作一次读取。
    // 缓冲区至少为 MaxDatagram + 1 字节，以便协议层拒绝被截断的超长报文。
    int read(std::uint8_t *data, std::size_t capacity, std::uint32_t &sourceIp,
             std::uint16_t &sourcePort);
    bool sendFeedback(const Datagram &, std::uint32_t ip, std::uint16_t port);

  private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
} // namespace rt
