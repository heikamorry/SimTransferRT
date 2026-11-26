#include "network/AsioUdpTransport.h"
#include "network/AsioOperationStorage.h"
#include "network/AsioReceiverSocket.h"
#include "network/IUdpTransport.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace {
void require(bool ok, const char *what) {
    if (!ok)
        throw std::runtime_error(what);
}

rt::StreamConfig config(std::uint16_t bindPort, std::uint16_t targetPort) {
    rt::StreamConfig c;
    c.bindIp = "127.0.0.1";
    c.targetIp = "127.0.0.1";
    c.commandPort = bindPort;
    c.targetPort = targetPort;
    return c;
}

rt::IncomingDatagram readOne(rt::IUdpTransport &socket) {
    rt::IncomingDatagram datagram;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (socket.receive(datagram))
            return datagram;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    throw std::runtime_error("loopback datagram not received");
}

void operationStorageFallbacks() {
    rt::AsioOperationStorage first;
    rt::AsioOperationStorage second;
    constexpr auto alignment = alignof(std::max_align_t);
    constexpr auto extendedAlignment = alignment * 2;
    auto *overaligned = first.allocate(32, extendedAlignment);
    require(reinterpret_cast<std::uintptr_t>(overaligned) % extendedAlignment == 0,
            "fallback preserves extended alignment");
    first.deallocate(overaligned, 32, extendedAlignment);
    auto *oversized = first.allocate(2048, alignment);
    first.deallocate(oversized, 2048, alignment);

    auto *block = first.allocate(32, alignment);
    auto *overlap = first.allocate(32, alignment);
    auto *independent = second.allocate(32, alignment);
    require(block != overlap && block != independent &&
                reinterpret_cast<std::uintptr_t>(block) % alignment == 0,
            "live allocations are aligned and distinct");
    first.deallocate(block, 32, alignment);
    auto *reused = first.allocate(32, alignment);
    require(reused == block, "the original block is reusable while a fallback remains live");
    first.deallocate(overlap, 32, alignment);
    first.deallocate(reused, 32, alignment);
    second.deallocate(independent, 32, alignment);
    require(first.stats().hits == 2 && first.stats().fallbacks == 3 &&
                second.stats().hits == 1 && second.stats().fallbacks == 0,
            "allocation and fallback counts are per instance");
}

void completionPool(bool reuse) {
    auto receiver = rt::makeAsioUdpTransport(false);
    receiver->open(config(0, 1));
    auto sender = rt::makeAsioUdpTransport(true);
    if (reuse)
        sender->setOperationStorageReuse(true);
    sender->open(config(0, receiver->localPort()));
    rt::SendCompletion completion;
    require(!sender->pollCompletion(completion), "poll before first submission");
    std::array<bool, 64> observed{};
    const std::uint8_t payload = 37;
    for (std::size_t i = 0; i < observed.size(); ++i)
        require(sender->submitDatagram(&payload, 1, i) == rt::SendDisposition::Pending,
                "each pool slot holds a real pending submission");
    require(sender->submitDatagram(&payload, 1, observed.size()) ==
                rt::SendDisposition::Backpressure,
            "a full pool applies backpressure");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (sender->inFlight() && std::chrono::steady_clock::now() < deadline) {
        if (!sender->pollCompletion(completion)) {
            std::this_thread::yield();
            continue;
        }
        require(completion.token < observed.size() && !observed[completion.token] &&
                    completion.disposition == rt::SendDisposition::Complete &&
                    completion.observedUs > 0,
                "each real handler has a unique token, timestamp and success result");
        observed[completion.token] = true;
    }
    require(std::all_of(observed.begin(), observed.end(),
                        [](bool value) {
                            return value;
                        }) &&
                sender->inFlight() == 0 && !sender->pollCompletion(completion),
            "all local handlers were reaped");
    const auto stats = sender->operationStorageStats();
    require(stats.enabled == reuse && (reuse ? stats.hits > 0 && stats.fallbacks > 0
                                            : stats.hits == 0 && stats.fallbacks == 0),
            "64 overlapping sends exercise both bound allocator paths only when enabled");
    if (reuse) {
        require(sender->submitDatagram(&payload, 1, 100) == rt::SendDisposition::Pending &&
                    sender->operationStorageStats().hits > stats.hits,
                "reaping a full pool makes operation storage reusable");
        require(sender->submitDatagram(&payload, 1, 101) == rt::SendDisposition::Pending,
                "overlapping operation before shutdown");
        bool refused = false;
        try {
            sender->setOperationStorageReuse(false);
        } catch (const std::logic_error &) {
            refused = true;
        }
        require(refused, "an open transport must retain its live associated allocator");
        // Destroy with both an inline and a fallback operation still outstanding.
        const auto port = sender->localPort();
        sender.reset();
        auto replacement = rt::makeAsioUdpTransport(false);
        replacement->open(config(port, receiver->localPort()));
        require(replacement->localPort() == port, "shutdown released the pending sender");
    }
    std::cout << "64 distinct local send completions verified (allocator "
              << (reuse ? "on" : "off") << ", hits " << stats.hits
              << ", fallbacks " << stats.fallbacks << ")\n";
}

void roundTrip(bool async, bool reuse = false) {
    auto receiver = rt::makeAsioUdpTransport(false);
    receiver->open(config(0, 1));
    auto sender = rt::makeAsioUdpTransport(async);
    if (reuse)
        sender->setOperationStorageReuse(true);
    sender->open(config(0, receiver->localPort()));
    require(sender->localPort() != 0 && sender->dfEnabled(), "socket identity");
    const std::vector<std::uint8_t> payload{1, 2, 3, 4, 5, 6, 7};
    if (!async) {
        require(sender->submitDatagram(payload.data(), payload.size(), 1) ==
                    rt::SendDisposition::Complete,
                "sync send");
    } else {
        // The engine polls while idle, including before its first submission.
        rt::SendCompletion idleCompletion;
        require(!sender->pollCompletion(idleCompletion), "idle completion queue is empty");
        std::vector<rt::SendCompletion> done;
        for (std::uint64_t token = 1; token <= 8; ++token) {
            require(sender->submitDatagram(payload.data(), payload.size(), token) ==
                        rt::SendDisposition::Pending,
                    "async submit");
        }
        for (int i = 0; i < 20000 && done.size() < 8; ++i) {
            rt::SendCompletion completion;
            while (sender->pollCompletion(completion))
                done.push_back(completion);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        require(done.size() == 8, "all async completions observed");
        for (const auto &completion : done)
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
    std::size_t accepted = 0;
    std::size_t pressured = 0;
    for (int i = 0; i < 200; ++i) {
        const auto result = sender->submitDatagram(big.data(), big.size(), 1000 + i);
        if (result == rt::SendDisposition::Complete || result == rt::SendDisposition::Pending)
            ++accepted;
        else if (result == rt::SendDisposition::Backpressure) {
            ++pressured;
            break;
        } else
            throw std::runtime_error("unexpected send failure");
    }
    require(accepted > 0, "some datagrams accepted before pressure");
    sender->cancelOutstanding();
    rt::SendCompletion completion;
    for (int i = 0; i < 20000; ++i) {
        while (sender->pollCompletion(completion)) {
        }
        if (!sender->inFlight())
            break;
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
    } catch (const std::exception &) {
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
    require(peer->submitDatagram(payload.data(), payload.size(), 1) ==
                rt::SendDisposition::Complete,
            "feedback send");
    rt::IncomingDatagram datagram;
    for (int i = 0; i < 5000; ++i) {
        const auto outcome = sender->receiveFeedback(datagram);
        if (outcome == rt::ReceiveOutcome::Received)
            break;
        require(outcome != rt::ReceiveOutcome::Error, "feedback read error");
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    require(datagram.bytes == payload && datagram.sourcePort == peer->localPort(),
            "feedback payload and source");
}

void oversizedReceiverDatagram() {
    auto peer = rt::makeAsioUdpTransport(false);
    peer->open(config(0, 1));
    rt::AsioReceiverSocket receiver(0x7f000001, 0);

    // Discover the receiver's ephemeral port through its feedback socket.
    rt::Datagram probe;
    probe.size = 1;
    require(receiver.sendFeedback(probe, 0x7f000001, peer->localPort()), "receiver probe");
    const auto receiverPort = readOne(*peer).sourcePort;

    boost::asio::io_context context;
    boost::asio::ip::udp::socket source(context, boost::asio::ip::udp::v4());
    const boost::asio::ip::udp::endpoint target(boost::asio::ip::address_v4::loopback(),
                                                receiverPort);
    const std::vector<std::uint8_t> oversized(4096, 0x5a);
    source.send_to(boost::asio::buffer(oversized), target);
    const std::vector<std::uint8_t> normal{1, 2, 3};
    source.send_to(boost::asio::buffer(normal), target);

    std::array<std::uint8_t, rt::MaxDatagram + 1> buffer{};
    std::uint32_t sourceIp = 0;
    std::uint16_t sourcePort = 0;
    auto read = [&] {
        for (int i = 0; i < 5000; ++i) {
            const auto size = receiver.read(buffer.data(), buffer.size(), sourceIp, sourcePort);
            if (size >= 0)
                return size;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        throw std::runtime_error("receiver datagram not received");
    };
    require(read() > static_cast<int>(rt::MaxDatagram),
            "oversized input must remain oversized for protocol rejection");
    require(sourceIp == 0x7f000001 && sourcePort == source.local_endpoint().port(),
            "oversized datagram source preserved");
    require(read() == static_cast<int>(normal.size()) &&
                std::equal(normal.begin(), normal.end(), buffer.begin()),
            "normal receiving must continue after an oversized datagram");
}
} // namespace

int main(int argc, char **argv) {
    try {
        operationStorageFallbacks();
        completionPool(false);
        completionPool(true);
        if (argc == 2 && std::strcmp(argv[1], "--local-completion-only") == 0)
            return 0;
        roundTrip(false);
        roundTrip(true);
        roundTrip(true, true);
        exclusiveBind();
        feedback();
        oversizedReceiverDatagram();
        std::cout << "Asio transport sync, async, backpressure and cancellation passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
