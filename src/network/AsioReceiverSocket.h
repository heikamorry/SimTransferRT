#pragma once
#include "protocol/ProtocolTypes.h"
#include <memory>

namespace rt {
// 由接收线程独占；IPv4 地址使用主机字节序。反馈与接收共用同一个绑定端口。
class AsioReceiverSocket final {
public:
    AsioReceiverSocket(std::uint32_t bindIp, std::uint16_t port);
    ~AsioReceiverSocket();
    void waitReadable();
    // 返回 -1 表示当前没有数据；零字节 UDP 包仍然算作一次读取。
    int read(std::uint8_t* data, std::size_t capacity, std::uint32_t& sourceIp, std::uint16_t& sourcePort);
    bool sendFeedback(const Datagram&, std::uint32_t ip, std::uint16_t port);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}
