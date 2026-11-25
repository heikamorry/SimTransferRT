#include "v2/Engine.h"
#include "v2/ReceiveLoop.h"
#include "v2/Receiver.h"
#include <iostream>
#include <stdexcept>

namespace {
using namespace rt::v2;

void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

Frame makeFrame(std::uint8_t module, std::size_t bytes) {
    Frame frame;
    frame.moduleId = module;
    frame.frameId = 1;
    frame.timestampUs = 1;
    frame.bytes.resize(bytes, 42);
    return frame;
}

void readFragment(Receiver &receiver, const Frame &frame, std::uint16_t index, std::uint64_t now) {
    const auto packet = encodeData(frame, index);
    check(receiver.receiveAndPollObserved(packet.bytes.data(), packet.size, now).empty(),
          "in-order data produced unexpected immediate NACK");
}

// Compare the old polling order with the production receive loop on the same fragment sequence.
// Time advances after fragment 64 while 79 more original datagrams are already readable.
ReceiveModuleStats runQueuedEpTail(bool previousOrder) {
    const auto frame = makeFrame(EpModule, 204800);
    check(frame.fragmentCount() == 143, "fixture must match the 200 KiB EP load");
    std::uint64_t now = 100;
    Receiver receiver(false, [&] {
        return now;
    });
    std::uint16_t next = 0;
    auto readOne = [&] {
        if (next == frame.fragmentCount())
            return false;
        readFragment(receiver, frame, next++, now);
        if (next == 64)
            now += 2001;
        return true;
    };
    auto poll = [&] {
        receiver.poll(now);
    };
    ReceiveLoopStats loop;
    while (next != frame.fragmentCount()) {
        if (previousOrder) {
            for (unsigned n = 0; n != 64 && readOne(); ++n) {
            }
            poll();
        } else {
            serviceReceiveBatch(
                loop,
                [&] {
                    return now;
                },
                [] {
                    return false;
                },
                readOne, poll);
        }
    }
    return receiver.snapshot(now).modules[1];
}

void testQueuedOriginalTail() {
    const auto before = runQueuedEpTail(true);
    const auto after = runQueuedEpTail(false);
    check(before.requestedFragments == 79 && before.nackPackets == 1 &&
              after.requestedFragments == 0 && after.nackPackets == 0 && after.completed == 1 &&
              before.completed == 1,
          "already queued EP originals should prevent the 79-index false tail request");
    std::cout << "200 KiB queued EP tail: requested fragments " << before.requestedFragments
              << " -> " << after.requestedFragments << ", completed frames " << before.completed
              << " -> " << after.completed << '\n';
}

// Continuous unrelated traffic at the three retry deadlines and the final expiry.
// At the expiry boundary the otherwise identical run either queues the tail as the
// 65th readable packet, or never supplies it.
void testDeadlineWithBacklog(bool tailQueued) {
    std::uint64_t now = 100;
    Receiver receiver(false, [&] {
        return now;
    });
    const auto ep = makeFrame(EpModule, 1437);
    const auto ndi = makeFrame(NdiModule, 1);
    readFragment(receiver, ep, 0, now);
    ReceiveLoopStats loop;
    unsigned feedback = 0;
    for (auto due : {2100u, 12100u, 22100u, 32100u}) {
        now = due;
        unsigned reads = 0, polls = 0;
        auto readOne = [&] {
            ++reads;
            check(reads <= 128, "continuous traffic starved the timer poll");
            if (tailQueued && due == 32100 && reads == 65)
                readFragment(receiver, ep, 1, now);
            else
                readFragment(receiver, ndi, 0, now);
            return true;
        };
        serviceReceiveBatch(
            loop,
            [&] {
                return now;
            },
            [] {
                return false;
            },
            readOne,
            [&] {
                ++polls;
                for (const auto &packet : receiver.poll(now)) {
                    const auto request = decodeNack(packet.bytes.data(), packet.size);
                    check(request.moduleId == EpModule && request.frameId == 1 &&
                              request.indices == std::vector<std::uint16_t>{1},
                          "time-based request changed module, frame or fragment");
                    ++feedback;
                }
            });
        check(polls == 1 && reads >= 65, "service timers once after bounded backlog draining");
    }
    const auto stats = receiver.snapshot(now).modules[1];
    check(loop.extraBatches == 4 && loop.forcedTimerPolls == 4 && loop.extraDatagrams == 256,
          "report bounded extra reads and timer polls under uninterrupted backlog");
    check(feedback == 3 && stats.minRetryIntervalUs == 10000 && stats.maxRetryIntervalUs == 10000,
          "genuine missing tail gets all three retry rounds under unrelated traffic");
    check(stats.completed == unsigned(tailQueued) && stats.expired == unsigned(!tailQueued),
          "only the frame with an available original tail survives the last deadline");
}

void testTimeBudgetAndStop() {
    std::uint64_t now = 100;
    bool stopped = false;
    unsigned reads = 0, polls = 0;
    ReceiveLoopStats loop;
    // Each handled datagram consumes a whole time budget; continued readability permits one extra.
    auto slowRead = [&] {
        ++reads;
        now += 600;
        return true;
    };
    auto poll = [&] {
        ++polls;
    };
    serviceReceiveBatch(
        loop,
        [&] {
            return now;
        },
        [&] {
            return stopped;
        },
        slowRead, poll);
    check(reads == 2 && polls == 1 && loop.extraDatagrams == 1 && loop.forcedTimerPolls == 1,
          "time budget bounds additional work independently of packet budget");
    auto stopDuringRead = [&] {
        ++reads;
        stopped = true;
        return true;
    };
    serviceReceiveBatch(
        loop,
        [&] {
            return now;
        },
        [&] {
            return stopped;
        },
        stopDuringRead, poll);
    check(reads == 3 && polls == 1, "stop prevents further reads and timer-generated feedback");
    stopped = false;
    serviceReceiveBatch(
        loop,
        [&] {
            return now;
        },
        [&] {
            return stopped;
        },
        [] {
            return false;
        },
        poll);
    check(polls == 2 && loop.extraBatches == 1,
          "an empty socket services timers without extra work");
}

void testVirtualLoopback(std::size_t bytes, bool dropFirstTail) {
    std::uint64_t now = 100;
    auto clock = [&] {
        return now;
    };
    auto frame = makeFrame(EpModule, bytes);
    frame.timestampUs = now;
    for (std::size_t i = 0; i < bytes; ++i)
        frame.bytes[i] = static_cast<std::uint8_t>(i * 17 + i / rt::PayloadStep);
    const auto count = frame.fragmentCount();
    StaticEpPayload source(frame.bytes);
    Engine sender(Config{});
    sender.submit(source.sample(frame.frameId, now, now));
    Receiver receiver(false, clock);
    receiver.setFrameConsumer([&](const Frame &received, std::uint64_t) {
        check(received.moduleId == EpModule && received.frameId == frame.frameId &&
                  received.timestampUs == frame.timestampUs && received.bytes == frame.bytes,
              "virtual link consumer received different frame bytes or identity");
    });

    // One encoded DATA datagram in flight. NACKs go back through the native feedback decoder.
    rt::Bytes wire;
    auto submit = [&](const std::uint8_t *p, std::size_t size) {
        const auto data = decodeData(p, size);
        if (!(dropFirstTail && !data.retransmission && data.fragmentIndex + 1 == count))
            wire.assign(p, p + size);
        return true; // an intentional link drop still completed at the sender
    };
    auto feedback = [&](const std::vector<rt::Datagram> &packets) {
        for (const auto &packet : packets) {
            const auto request = decodeNack(packet.bytes.data(), packet.size);
            check(request.frameId == frame.frameId && request.moduleId == EpModule &&
                      request.indices == std::vector<std::uint16_t>{std::uint16_t(count - 1)},
                  "NACK requests only the lost tail");
            sender.feedback(packet.bytes.data(), packet.size, now);
        }
    };
    ReceiveLoopStats loop;
    auto service = [&] {
        serviceReceiveBatch(
            loop, clock,
            [] {
                return false;
            },
            [&] {
                if (wire.empty())
                    return false;
                feedback(receiver.receiveAndPollObserved(wire.data(), wire.size(), now));
                wire.clear();
                return true;
            },
            [&] {
                feedback(receiver.poll(now));
            });
    };
    for (unsigned i = 0; i < count; ++i) {
        now = std::max(now, sender.nextDueUs());
        check(sender.step(now, submit, clock), "virtual link stalled during initial transmission");
        service();
    }
    if (dropFirstTail) {
        check(receiver.snapshot(now).modules[1].completed == 0,
              "lost tail must prevent completion");
        now += 2000;
        service(); // idle timer delivers the NACK to the sender
        check(sender.step(now, submit, clock), "requested tail was not retransmitted");
        service();
    }
    const auto tx = sender.snapshot().modules[1];
    const auto rx = receiver.snapshot(now).modules[1];
    const auto recoveries = dropFirstTail ? 1u : 0u;
    check(tx.initialComplete == 1 && tx.nacks == recoveries && tx.retransmissions == recoveries &&
              tx.datagrams == count + recoveries && rx.nackPackets == recoveries &&
              rx.fragmentOutcomes[1].accepted == recoveries && rx.datagrams == count &&
              rx.completed == 1 && rx.consumed == 1 && rx.expired == 0,
          "virtual link accounting or bytes delivered to consumer disagree");
    std::cout << "In-process virtual link (no UDP socket): " << bytes << " bytes, " << count
              << " fragments, " << rx.nackPackets << " NACK, " << tx.retransmissions
              << " retry, bytewise delivery verified\n";
}
} // namespace

int main() {
    try {
        testQueuedOriginalTail();
        testDeadlineWithBacklog(false);
        testDeadlineWithBacklog(true);
        testTimeBudgetAndStop();
        for (const auto size : {200u * 1024u, 1024u * 1024u}) {
            testVirtualLoopback(size, false);
            testVirtualLoopback(size, true);
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
