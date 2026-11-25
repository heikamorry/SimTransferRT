#pragma once
#include "network/AsioUdpTransport.h"
#include <cstdint>
#include <memory>

namespace test {
// Loopback peer used by GUI tests. Qt Network is not linked on this branch.
class UdpSocket {
  public:
    UdpSocket(std::uint16_t bindPort, std::uint16_t targetPort) {
        socket_ = rt::makeAsioUdpTransport(false);
        rt::StreamConfig config;
        config.bindIp = "127.0.0.1";
        config.targetIp = "127.0.0.1";
        config.commandPort = bindPort;
        config.targetPort = targetPort;
        socket_->open(config);
    }

    ~UdpSocket() {
        if (socket_)
            socket_->cancelOutstanding();
    }

    UdpSocket(const UdpSocket &) = delete;
    UdpSocket &operator=(const UdpSocket &) = delete;

    std::uint16_t localPort() const {
        return socket_->localPort();
    }

    bool send(const void *data, std::size_t size) {
        return socket_->submitDatagram(static_cast<const std::uint8_t *>(data), size, ++token_) ==
               rt::SendDisposition::Complete;
    }

    void close() {
        socket_.reset();
    }

  private:
    std::unique_ptr<rt::IUdpTransport> socket_;
    std::uint64_t token_ = 0;
};
} // namespace test
