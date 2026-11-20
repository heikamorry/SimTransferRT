#include "network/AsioUdpTransport.h"
#include "network/IUdpTransport.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace {
void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }

rt::StreamConfig config(std::uint16_t bindPort, std::uint16_t targetPort) {
    rt::StreamConfig c;
    c.bindIp = "127.0.0.1"; c.targetIp = "127.0.0.1";
    c.commandPort = bindPort; c.targetPort = targetPort;
    return c;
}
rt::IncomingDatagram readOne(rt::IUdpTransport& socket) {
    rt::IncomingDatagram datagram;
    for (int i = 0; i < 5000; ++i) {
        if (socket.receive(datagram)) return datagram;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    throw std::runtime_error("loopback datagram not received");
}
void roundTrip(bool async) {
    auto receiver = rt::makeAsioUdpTransport(false);
    receiver->open(config(0, 1));
    auto sender = rt::makeAsioUdpTransport(async);
    sender->open(config(0, receiver->localPort()));
    require(sender->localPort() != 0 && sender->dfEnabled(), "socket identity");
    const std::vector<std::uint8_t> payload{1, 2, 3, 4, 5, 6, 7};
    if (!async) {
        require(sender->submitDatagram(payload.data(), payload.size(), 1) ==
                    rt::SendDisposition::Complete, "sync send");
    } else {
        std::vector<rt::SendCompletion> done;
        for (std::uint64_t token = 1; token <= 8; ++token) {
            require(sender->submitDatagram(payload.data(), payload.size(), token) ==
                        rt::SendDisposition::Pending, "async submit");
        }
        for (int i = 0; i < 20000 && done.size() < 8; ++i) {
            rt::SendCompletion completion;
            while (sender->pollCompletion(completion)) done.push_back(completion);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        require(done.size() == 8, "all async completions observed");
        for (const auto& completion : done)
            require(completion.disposition == rt::SendDisposition::Complete,
                    "async completion is not success");
        require(sender->inFlight() == 0, "in-flight drained after reaping");
    }
    for (int i = 0; i < (async ? 8 : 1); ++i) {
        const auto received = readOne(*receiver);
        require(received.bytes == payload && received.sourcePort == sender->localPort(),
                "payload and source port preserved");
    }
    // Backpressure must not be reported as success and must leave the socket usable.
    std::vector<std::uint8_t> big(1400, 0x5a);
    std::size_t accepted = 0, pressured = 0;
    for (int i = 0; i < 200; ++i) {
        const auto result = sender->submitDatagram(big.data(), big.size(), 1000 + i);
        if (result == rt::SendDisposition::Complete || result == rt::SendDisposition::Pending) ++accepted;
        else if (result == rt::SendDisposition::Backpressure) { ++pressured; break; }
        else throw std::runtime_error("unexpected send failure");
    }
    require(accepted > 0, "some datagrams accepted before pressure");
    sender->cancelOutstanding();
    rt::SendCompletion completion;
    for (int i = 0; i < 20000; ++i) {
        while (sender->pollCompletion(completion)) {}
        if (!sender->inFlight()) break;
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    require(sender->inFlight() == 0, "cancelled operations are reaped, not leaked");
    (void)pressured;
}
void exclusiveBind() {
    auto owner = rt::makeAsioUdpTransport(false);
    owner->open(config(0, 1));
    auto second = rt::makeAsioUdpTransport(false);
    bool failed = false;
    try {
        second->open(config(owner->localPort(), 1));
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed, "a port owned by another socket must not be silently shared");
}
void feedback() {
    auto sender = rt::makeAsioUdpTransport(false);
    sender->open(config(0, 1));
    auto peer = rt::makeAsioUdpTransport(false);
    peer->open(config(0, sender->localPort()));
    const std::vector<std::uint8_t> payload{9, 8, 7};
    require(peer->submitDatagram(payload.data(), payload.size(), 1) == rt::SendDisposition::Complete,
            "feedback send");
    rt::IncomingDatagram datagram;
    for (int i = 0; i < 5000; ++i) {
        const auto outcome = sender->receiveFeedback(datagram);
        if (outcome == rt::ReceiveOutcome::Received) break;
        require(outcome != rt::ReceiveOutcome::Error, "feedback read error");
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    require(datagram.bytes == payload && datagram.sourcePort == peer->localPort(),
            "feedback payload and source");
}
} // namespace

int main() {
    try {
        roundTrip(false);
        roundTrip(true);
        exclusiveBind();
        feedback();
        std::cout << "Asio transport sync, async, backpressure and cancellation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
