#include "v2/Engine.h"
#include "protocol/V2PacketCodec.h"
#include "protocol/Checksum.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>
using namespace rt::v2;

namespace {
void require(bool b, const char *text) {
    if (!b)
        throw std::runtime_error(text);
}

PreparedPtr make(unsigned module, unsigned id, std::size_t n = 1437, std::uint64_t sampled = 1000) {
    Frame f;
    f.moduleId = static_cast<std::uint8_t>(module);
    f.frameId = id;
    f.timestampUs = sampled;
    f.bytes.resize(n, 37);
    return prepareFrame(std::move(f), sampled, sampled, sampled);
}

void testStaticEpPayload() {
    for (const std::size_t n :
         {std::size_t(1), 2 * rt::PayloadStep, rt::PayloadStep + 1, std::size_t(1) << 20}) {
        Frame frame;
        frame.moduleId = EpModule;
        frame.bytes.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            frame.bytes[i] = static_cast<std::uint8_t>((i * 17 + i / rt::PayloadStep + 3) & 255);
        StaticEpPayload source(frame.bytes);
        const auto beforeWrap = source.sample(UINT32_MAX, 10, 0x1122334455667788ULL);
        const auto afterWrap = source.sample(0, 20, 0x8877665544332211ULL);
        require(beforeWrap->payload == afterWrap->payload,
                "EP frames must share immutable packet storage");
        require(beforeWrap->payload->crc == rt::crc32(frame.bytes),
                "EP template payload ledger CRC");
        // Revisit the old frame after a later sample, just as cached retransmissions do.
        for (const auto &prepared : {afterWrap, beforeWrap}) {
            frame.frameId = prepared->frameId;
            frame.timestampUs = prepared->timestampUs;
            for (std::uint16_t i = 0; i < frame.fragmentCount(); ++i) {
                for (const bool retry : {false, true}) {
                    const auto actual = prepared->packet(i, retry);
                    const auto expected = encodeData(frame, i, retry);
                    require(actual.size == expected.size &&
                                std::equal(actual.bytes.begin(), actual.bytes.begin() + actual.size,
                                           expected.bytes.begin()),
                            "EP template stamp must equal canonical DATA encoding through wrap, "
                            "tail and NACK");
                }
            }
        }
    }
}

void testPriority() {
    Config c;
    Engine e(c);
    e.submit(make(1, 1));
    e.submit(make(2, 1));
    e.submitStatus({1, 2}, 1000);
    std::vector<rt::Bytes> out;
    std::uint64_t clock = 1000;
    auto send = [&](const auto *p, std::size_t n) {
        out.emplace_back(p, p + n);
        return true;
    };
    auto time = [&] {
        return clock;
    };
    for (unsigned i = 0; i < 5; ++i) {
        require(e.step(clock, send, time), "packet expected");
        clock = std::max(clock + 1, e.nextDueUs());
    }
    require(out[0][6] == 3, "status must go first");
    require(decodeData(out[1].data(), out[1].size()).moduleId == 1, "earlier NDI deadline");
    require(e.snapshot().modules[0].initialComplete == 1 &&
                e.snapshot().modules[1].initialComplete == 1,
            "independent completion");
    Nack n;
    n.moduleId = 2;
    n.frameId = 1;
    n.indices = {0};
    auto nack = encodeNack(n);
    e.feedback(nack.bytes.data(), nack.size, clock);
    e.submit(make(1, 2, 1, clock));
    require(e.step(clock, send, time), "initial pending");
    require(decodeData(out.back().data(), out.back().size()).moduleId == 1,
            "initial ahead of retry");
    clock = std::max(clock + 1, e.nextDueUs());
    e.step(clock, send, time);
    auto p = decodeData(out.back().data(), out.back().size());
    require(p.moduleId == 2 && p.retransmission, "retry correct module");
}

void testDelayedCompletion() {
    struct Delayed : rt::IUdpTransport {
        std::uint64_t token = 0;
        bool ready = false;
        std::uint64_t observed = 20300;
        rt::SendDisposition outcome = rt::SendDisposition::Complete;

        void open(const rt::StreamConfig &) override {}

        bool send(const std::uint8_t *, std::size_t) override {
            return false;
        }

        rt::SendDisposition submitDatagram(const std::uint8_t *, std::size_t,
                                           std::uint64_t id) override {
            token = id;
            return rt::SendDisposition::Pending;
        }

        bool pollCompletion(rt::SendCompletion &out) override {
            if (!ready || !token)
                return false;
            out = {token, outcome, observed};
            token = 0;
            return true;
        }

        std::size_t inFlight() const noexcept override {
            return token ? 1 : 0;
        }

        bool receive(rt::IncomingDatagram &) override {
            return false;
        }

        std::uint16_t localPort() const override {
            return 0;
        }

        bool dfEnabled() const override {
            return false;
        }

        std::string lastError() const override {
            return {};
        }
    } transport;

    Engine engine(Config{});
    engine.submit(make(EpModule, 1, 1, 1000));
    auto clock = [] {
        return std::uint64_t{1000};
    };
    require(engine.stepTransport(1000, transport, clock), "submitted first fragment");
    require(!engine.stepTransport(1001, transport, clock), "scheduler revisits pending completion");
    require(!engine.snapshot().modules[1].initialComplete, "pending callback is not completion");
    transport.ready = true;
    engine.stepTransport(20400, transport, clock);
    const auto m = engine.snapshot().modules[1];
    require(m.initialComplete == 1 && m.initialDuration.maxUs == 19300 && m.deadlineMisses == 1,
            "only the real handler timestamp completes or misses the deadline");

    Config c;
    c.fragmentTrace = true;
    for (const bool earlyFailure : {false, true}) {
        Engine traced(c);
        traced.submit(make(EpModule, 1, 1, 1000));
        transport.ready = true;
        transport.observed = earlyFailure ? 1015 : 20300;
        transport.outcome = earlyFailure ? rt::SendDisposition::Failed : rt::SendDisposition::Complete;
        std::uint64_t tick = 1000;
        auto observedClock = [&] {
            const auto now = tick;
            tick += 10;
            return now;
        };
        require(traced.stepTransport(1000, transport, observedClock), "traced asynchronous submit");
        const auto span = traced.snapshot().worstFragmentSpan[1];
        require(span && span->fragments.size() == 1, "one asynchronous original retained");
        const auto &p = span->fragments.front();
        require(p.pendingCompletion && p.success == !earlyFailure && p.callStartUs == 1010 &&
                    p.submitReturnUs == 1020 && p.endUs == transport.observed &&
                    span->ownCallUs == transport.observed - p.callStartUs,
                "submission return cannot replace a late or already-observed completion endpoint");
    }
}

void testBudget() {
    for (unsigned rate : {100u, 1000u, 2500u}) {
        Config c;
        c.linkMbps = rate;
        Engine e(c);
        e.submit(make(2, 1, 204800));
        std::uint64_t clock = 1000;
        unsigned packets = 0;
        auto send = [&](const auto *, std::size_t) {
            ++packets;
            return true;
        };
        while (e.pendingInitial()) {
            if (!e.step(clock, send, [&] {
                    return clock;
                }))
                throw std::runtime_error("scheduler stalled");
            clock = std::max(clock, e.nextDueUs());
        }
        require(packets == 143, "143 EP fragments");
        const auto d = e.snapshot().modules[1].initialDuration;
        require(d.maxUs <= 18000, "capacity timing");
        require(d.maxUs >= (rate == 100    ? 17000
                            : rate == 1000 ? 1700
                                           : 680),
                "selected profile actually paces");
    }
}

void testCacheStopAndLate() {
    Config c;
    Engine e(c);
    for (unsigned i = 0; i < 5; ++i)
        e.submit(make(1, i));
    require(e.snapshot().modules[0].cacheHighWater == 4, "four-frame bound");
    Nack n;
    n.moduleId = 1;
    n.frameId = 0;
    n.indices = {0};
    auto d = encodeNack(n);
    e.feedback(d.bytes.data(), d.size, 1000);
    require(e.snapshot().modules[0].cacheMisses == 1, "old cache eviction");
    std::uint64_t now = 100000;
    e.step(
        now,
        [](auto *, auto) {
            return true;
        },
        [&] {
            return now;
        });
    require(e.snapshot().worstEvents.front().originalDueUs == 1000,
            "original plan not moved after stall");
    e.stop();
    auto m = e.snapshot().modules[0];
    require(m.initialIncomplete == 5, "all incomplete accounted");
    require(m.unsentEvicted == 2 && m.unsentAtStop == 7, "unsent counted");
}

void testPreflight() {
    Config c;
    c.linkMbps = 100;
    c.ndiTools = 40;
    auto p = preflight(c);
    require(p.feasible && p.synchronousWireUs > 17700 && p.synchronousWireUs < 17703,
            "100M budget");
    c.ndiTools = 255;
    require(!preflight(c).feasible, "synchronous overload rejected");
    c.linkMbps = 2500;
    c.observedLinkMbps = 100;
    require(effectiveMbps(c) == 100, "local lower link wins");
    c.pathLimitMbps = 50;
    require(effectiveMbps(c) == 50, "path lower cap wins");
}

void testLateStatusAndFaults() {
    Config c;
    c.faults.reverseInitial = true;
    c.faults.duplicateFirst.insert(1);
    Engine e(c);
    e.submit(make(2, 3));
    e.submitStatus({7}, 10000);
    std::uint64_t now = 10000;
    std::vector<rt::Bytes> packets;
    auto send = [&](const auto *p, std::size_t n) {
        packets.emplace_back(p, p + n);
        return true;
    };
    e.step(now, send, [&] {
        return now;
    });
    for (unsigned k = 0; k < 3; ++k) {
        now = std::max(now + 1, e.nextDueUs());
        require(e.step(now, send,
                       [&] {
                           return now;
                       }),
                "fault packet");
    }
    require(packets.size() == 4, "duplicate emitted");
    require(decodeData(packets[1].data(), packets[1].size()).fragmentIndex == 1, "reverse first");
    require(decodeData(packets[2].data(), packets[2].size()).fragmentIndex == 1,
            "duplicate fragment");
    require(e.snapshot().modules[1].originalPlanLateness.maxUs >= 9000,
            "late status cannot hide original data lateness");
}

void testRetryDuringDelayedAcquisition() {
    Engine engine(Config{});
    engine.submit(make(2, 8, 1));
    std::vector<rt::Bytes> output;
    auto send = [&](const auto *bytes, std::size_t size) {
        output.emplace_back(bytes, bytes + size);
        return true;
    };
    std::uint64_t now = 1000;
    auto clock = [&] {
        return now;
    };
    require(engine.step(now, send, clock), "EP first copy");
    auto request = encodeNack(Nack{2, 8, {0}});
    engine.feedback(request.bytes.data(), request.size, now);
    engine.setNextSample(1, 2000);
    now = 1999;
    require(!engine.step(now, send, clock), "reserve upcoming NDI sample boundary");
    now = 2001;
    require(engine.step(now, send, clock),
            "blocked NDI acquisition must not reserve a full deadline window");
    require(decodeData(output.back().data(), output.back().size()).retransmission,
            "EP retry served");
}

void testInitialDeadlineEvidence() {
    Config c;
    c.handoffTrace = true;
    Engine engine(c);
    std::uint64_t now = 2000;
    auto clock = [&] {
        return now;
    };
    auto send = [](auto *, auto) {
        return true;
    };
    engine.submit(make(2, 77));
    require(engine.step(now, send, clock), "first fragment");
    now = 20001;
    require(engine.step(now, send, clock), "late last fragment");
    auto request = encodeNack(Nack{2, 77, {0}});
    for (unsigned i = 0; i < 40; ++i) {
        engine.feedback(request.bytes.data(), request.size, now);
        now += 1000;
        require(engine.step(now, send, clock), "retry burst");
    }
    auto snapshot = engine.snapshot();
    require(snapshot.initialDeadlineEvents.size() == 1, "independent deadline evidence");
    const auto &event = snapshot.initialDeadlineEvents.front();
    require(event.moduleId == 2 && event.frameId == 77 && event.complete, "late frame identity");
    require(event.plannedSampleUs == 1000 && event.sampleUs == 1000 && event.readyUs == 1000 &&
                event.firstAttemptUs == 2000 && event.endUs == 20001 && event.deadlineUs == 19000 &&
                event.overrunUs() == 1001,
            "deadline event measures preparation and both sends");
    require(event.firstDispatch && event.firstDispatch->readyToFirstUs() == 1000,
            "deadline row retains its own first-dispatch evidence");
    require(snapshot.modules[1].readyToFirst.count == 1,
            "retries must not add first-attempt samples");
}

void testMultipartNdiBehindEarlierEpDeadline() {
    Config c;
    c.handoffTrace = true;
    c.linkMbps = 1000;
    Engine engine(c);
    constexpr std::uint64_t epSample = 1000;
    constexpr std::uint64_t ndiSample = 4334;
    engine.submit(make(EpModule, 8, 1u << 20, epSample));
    std::uint64_t now = epSample;
    auto clock = [&] {
        return now;
    };
    auto send = [](const auto *, std::size_t) {
        return true;
    };
    auto sendNext = [&] {
        require(engine.step(now, send, clock), "an initial fragment is due");
        if (engine.pendingInitial())
            now = std::max(now + 1, engine.nextDueUs());
    };
    while (now < ndiSample)
        sendNext();
    FrameHandoff handoff;
    handoff.submittedUs = now;
    engine.submit(make(NdiModule, 9, rt::PayloadStep + 1, ndiSample), handoff);
    while (engine.pendingInitial())
        sendNext();

    auto snapshot = engine.snapshot();
    const auto &ndi = snapshot.worstFirstDispatch[0].at(0);
    const auto &ep = ndi.headsAtSubmit[1];
    require(ep.moduleId == EpModule && ep.frameId == 8 && ep.deadlineUs == 19000 &&
                ep.nextFragment > 0,
            "record the already running EP with an earlier deadline than NDI");
    require(ndi.intervening.ep == ep.fragmentsRemaining && ndi.intervening.ep > 400 &&
                ndi.intervening.ndi == 0 && ndi.intervening.retry == 0 &&
                ndi.intervening.status == 0,
            "count the EP remainder submitted between NDI admission and selection");
    require(ndi.previousAtSubmit.index + 1 == ep.nextFragment &&
                ndi.previousAtFirst.moduleId == EpModule && ndi.previousAtFirst.frameId == 8 &&
                ndi.previousAtFirst.index == 730,
            "retain the preceding fragments at admission and first dispatch");
    require(ndi.selectedUs >= handoff.submittedUs && ndi.firstAttemptUs == ndi.selectedUs &&
                ndi.readyToFirstUs() > 5000 && ndi.readyToFirstUs() < 6500,
            "multi-fragment NDI retains packet-boundary EDF");
    require(snapshot.modules[0].readyToFirst.maxUs == ndi.readyToFirstUs() &&
                snapshot.modules[0].deadlineMisses == 0 && snapshot.modules[1].deadlineMisses == 0,
            "multi-millisecond arbitration delay is visible even within both deadline limits");
}

// EP deadline 19000 and NDI deadline 19051 reproduce the observed 51 us difference.
struct EpNdiBoundary {
    Engine engine;
    std::uint8_t module = 0;
    std::uint32_t frame = 0;
    std::uint16_t index = 0;
    bool retry = false;

    explicit EpNdiBoundary(Config c = {}) : engine(c) {
        engine.submit(make(EpModule, 217, 1u << 20, 1000));
    }

    void ndi(unsigned id, std::uint64_t planned, std::uint64_t sampled = 4051) {
        Frame f{NdiModule, id, sampled, rt::Bytes(179, 37)};
        engine.submit(prepareFrame(std::move(f), planned, sampled, sampled));
    }

    void sendAt(std::uint64_t now) {
        auto send = [&](const std::uint8_t *bytes, std::size_t size) {
            if (bytes[6] == StatusType) {
                module = StatusModule;
                return true;
            }
            const auto p = decodeData(bytes, size);
            module = p.moduleId;
            frame = p.frameId;
            index = p.fragmentIndex;
            retry = p.retransmission;
            return true;
        };
        require(engine.step(now, send,
                            [&] {
                                return now;
                            }),
                "expected one eligible packet");
    }

    void prefix(unsigned count, std::uint64_t first, std::uint64_t last) {
        for (unsigned i = 0; i < count; ++i) {
            sendAt(first + (last - first) * i / (count - 1));
            require(module == EpModule && frame == 217 && index == i && !retry,
                    "original EP prefix in order");
        }
    }
};

void testSingleNdiWithEpSlack() {
    EpNdiBoundary f;
    f.prefix(197, 1000, 4136);
    f.ndi(7, 3300);
    f.ndi(8, 3300);
    require(f.engine.submitStatus({1}, 4140), "status queued");
    f.sendAt(4140);
    require(f.module == StatusModule, "immediate status keeps first priority");
    auto nack = encodeNack(Nack{EpModule, 217, {0}});
    f.engine.feedback(nack.bytes.data(), nack.size, 4141);
    f.sendAt(4152);
    require(f.module == NdiModule && f.frame == 7 && f.index == 0,
            "oldest single-fragment NDI takes one EP boundary");
    for (unsigned i = 197; i < 731; ++i) {
        f.sendAt(4168 + (i - 197) * 16);
        require(f.module == EpModule && f.index == i && !f.retry,
                "remaining 534 originals stay ordered; one yield per EP");
    }
    f.sendAt(13000);
    require(f.module == NdiModule && f.frame == 8, "same-module NDI FIFO preserved");
    f.sendAt(13020);
    require(f.module == EpModule && f.retry && f.index == 0, "retry stays behind initial frames");
    const auto stats = f.engine.snapshot().modules;
    require(stats[0].deadlineMisses == 0 && stats[1].deadlineMisses == 0,
            "both deadlines met in the slack fixture");
}

void testSingleNdiProtection() {
    for (auto planned : {3997u, 3300u}) {
        EpNdiBoundary slow;
        slow.prefix(197, 1013, 7502);
        // The first anchor matches the measured run; the earlier anchor
        // isolates the effect of observed service speed from the plan guard.
        slow.ndi(7, planned);
        slow.sendAt(7934);
        require(slow.module == EpModule && slow.index == 197,
                "197 fragments in 6921 us cannot budget the remaining 534");
    }

    EpNdiBoundary anchored;
    anchored.prefix(197, 1000, 4136);
    anchored.ndi(7, 4051);
    anchored.sendAt(4152);
    require(anchored.module == EpModule, "no artificial forward jump of the original EP schedule");

    EpNdiBoundary slowerLink;
    slowerLink.prefix(197, 1000, 4136);
    slowerLink.ndi(7, 3300);
    slowerLink.engine.setEffectiveMbps(100, 4152);
    slowerLink.sendAt(4152);
    require(slowerLink.module == EpModule,
            "current wire budget overrides previously fast observed service");

    Config faults;
    faults.faults.duplicateFirst = {197};
    EpNdiBoundary duplicate(faults);
    duplicate.prefix(197, 1000, 4136);
    duplicate.ndi(7, 3300);
    duplicate.sendAt(4152);
    require(duplicate.module == EpModule && duplicate.index == 197, "fault injection keeps EDF");
    duplicate.sendAt(4168);
    require(duplicate.module == EpModule && duplicate.index == 197,
            "injected duplicate is preserved");
}

void testSingleNdiDeadlineBoundary() {
    EpNdiBoundary f;
    f.prefix(530, 1000, 18986);
    f.ndi(7, 4051);
    f.sendAt(18999);
    require(f.module == EpModule && f.index == 530,
            "one microsecond of EP slack still protects its deadline");
    f.sendAt(19000);
    require(f.module == NdiModule && f.frame == 7,
            "EP deadline reached; observed single NDI cost fits its remaining 51 us");

    EpNdiBoundary late;
    late.prefix(530, 1000, 18986);
    late.ndi(7, 4051);
    late.sendAt(19025);
    require(late.module == EpModule && late.index == 530,
            "a future NDI deadline alone is insufficient for rescue");
    late.sendAt(19051);
    require(late.module == EpModule && late.index == 531,
            "EDF stays active when neither frame can still finish in time");
}

void testFirstDispatchBehindPreviousEp() {
    Config c;
    c.handoffTrace = true;
    c.linkMbps = 1000;
    Engine engine(c);
    constexpr std::uint64_t oldSample = 1000;
    constexpr std::uint64_t newSample = 21000;
    constexpr std::size_t frameBytes = 1u << 20;
    engine.submit(make(EpModule, 217, frameBytes, oldSample));
    // Late service of #217 leaves a backlog when the next 50 Hz sample arrives.
    std::uint64_t now = 18000;
    std::uint64_t newFirstUs = 0;
    unsigned sent = 0;
    auto clock = [&] {
        return now;
    };
    auto send = [&](const auto *bytes, std::size_t size) {
        const auto packet = decodeData(bytes, size);
        require(packet.moduleId == EpModule && !packet.retransmission, "original EP DATA expected");
        if (sent < 731) {
            require(packet.frameId == 217 && packet.fragmentIndex == sent,
                    "finish old EP fragments in order");
        } else {
            require(sent == 731 && packet.frameId == 218 && packet.fragmentIndex == 0,
                    "new EP starts after old tail");
            newFirstUs = now;
        }
        ++sent;
        return true;
    };
    auto sendDue = [&] {
        require(engine.step(now, send, clock), "one EP fragment due");
        now = engine.nextDueUs();
    };
    for (unsigned i = 0; i < 201; ++i)
        sendDue();
    require(now < newSample, "201 fragments already submitted before the new EP arrives");
    now = newSample;
    FrameHandoff handoff;
    handoff.submittedUs = now;
    engine.submit(make(EpModule, 218, frameBytes, newSample), handoff);
    for (unsigned i = 0; i < 530; ++i)
        sendDue();
    require(sent == 731 && engine.snapshot().modules[1].readyToFirst.count == 1,
            "no first-attempt sample for #218 while draining its predecessor");
    const auto gate = now;
    require(!engine.step(gate - 1, send, clock),
            "respect the final pacing gate after the old EP tail");
    sendDue();

    const auto snapshot = engine.snapshot();
    const auto &events = snapshot.worstFirstDispatch[1];
    const auto found = std::find_if(events.begin(), events.end(), [](const auto &row) {
        return row.frameId == 218;
    });
    require(found != events.end(), "retain the new EP first-attempt row");
    const auto &event = *found;
    const auto &head = event.headsAtSubmit[1];
    require(head.moduleId == EpModule && head.frameId == 217 && head.nextFragment == 201 &&
                head.fragmentsRemaining == 530,
            "identify the same-module backlog at admission");
    require(event.frameId == 218 && event.previousAtSubmit.frameId == 217 &&
                event.previousAtSubmit.index == 200 && event.previousAtFirst.frameId == 217 &&
                event.previousAtFirst.index == 730 && event.intervening.ep == 530,
            "link the new first dispatch to the actual predecessor fragments");
    require(event.selectedUs == gate && event.pacingDueUs == gate &&
                event.firstAttemptUs == newFirstUs &&
                event.readyToFirstUs() == newFirstUs - newSample,
            "first-attempt timestamp agrees with the real first-fragment callback");
    // At 1 Gb/s these 529 full fragments and one short tail cost 6512 us on
    // the wire. The scheduler permits up to one full datagram of catch-up credit.
    require(event.readyToFirstUs() >= 6499 && event.readyToFirstUs() <= 6512,
            "remaining EP serialization time");
}

void testFirstDispatchPacingAndRetention() {
    Config c;
    c.handoffTrace = true;
    c.linkMbps = 100;
    Engine engine(c);
    auto send = [](const auto *, std::size_t) {
        return true;
    };
    std::uint64_t now = 1000;
    auto clock = [&] {
        return now;
    };
    engine.submit(make(EpModule, 7, 3 * rt::PayloadStep, now));
    require(engine.step(now, send, clock) && engine.step(now, send, clock),
            "initial allowed catch-up");
    FrameHandoff handoff;
    handoff.submittedUs = now;
    engine.submit(make(NdiModule, 11, 1, now), handoff);
    require(!engine.step(now, send, clock), "pacing blocks immediate selection");
    now = engine.nextDueUs();
    require(engine.step(now, send, clock), "NDI selected when pacing gate opens");
    const auto snapshot = engine.snapshot();
    const auto &ndi = snapshot.worstFirstDispatch[0].at(0);
    require(ndi.pacingDueUs == now && ndi.admissionPacingDueUs > handoff.submittedUs &&
                ndi.selectedUs == now && ndi.intervening.ep == 0,
            "a pacing gate is distinct from extra EP fragment dispatches");

    Engine many(c);
    for (unsigned id = 0; id < 40; ++id) {
        now = (id + 1) * 100000ULL;
        const auto ready = now - (id + 1) * 100;
        for (auto module : {NdiModule, EpModule})
            many.submit(make(module, id, 1, ready));
        require(many.step(now, send, clock), "NDI first dispatch");
        ++now;
        require(many.step(now, send, clock), "EP first dispatch");
    }
    auto ranked = many.snapshot();
    for (unsigned i = 0; i < 2; ++i) {
        require(ranked.modules[i].readyToFirst.count == 40 &&
                    ranked.worstFirstDispatch[i].size() == 32,
                "each module keeps all samples in its histogram and at most 32 event rows");
        for (const auto &event : ranked.worstFirstDispatch[i])
            require(event.frameId >= 8,
                    "retain only the worst ready-to-first values in this module");
    }
    Engine off(Config{});
    off.submit(make(NdiModule, 1, 1, now));
    require(off.step(now, send, clock), "untraced first dispatch");
    auto untraced = off.snapshot();
    require(untraced.modules[0].readyToFirst.count == 0 && untraced.worstFirstDispatch[0].empty(),
            "handoff diagnostics are opt-in");
}

void testFragmentSpanPartition() {
    Config c;
    c.fragmentTrace = true;
    c.faults.duplicateFirst = {0};
    c.faults.dropFirst = {1, 3};
    Engine engine(c);
    engine.submit(make(EpModule, 8, 4 * rt::PayloadStep, 1000));
    std::uint64_t tick = 0;
    auto clock = [&] {
        return tick++;
    };
    auto send = [&](const std::uint8_t *data, std::size_t size) {
        tick += 10;
        if (data[6] == StatusType)
            return true;
        auto p = decodeData(data, size);
        return p.moduleId != EpModule || p.fragmentIndex != 2; // An actual failed call.
    };
    auto dispatch = [&](std::uint64_t when) {
        tick = when;
        require(engine.step(when, send, clock), "trace fixture dispatch");
    };
    dispatch(1000); // EP fragment 0
    dispatch(1100); // Injected duplicate of EP fragment 0
    engine.submitStatus({1}, 1200);
    dispatch(1200); // Status
    dispatch(1300); // Dropped EP fragment 1
    engine.submit(make(NdiModule, 9, 1, 1100));
    dispatch(1500); // NDI has an earlier deadline than EP
    const auto pending = engine.snapshot();
    require(pending.worstNdiWait && pending.worstNdiWait->frameId == 9 && !pending.worstNdiWaitEp &&
                !pending.worstFragmentSpan[1],
            "never publish a mutable partial EP span");
    dispatch(1700); // EP fragment 2 actually calls the transport and fails
    dispatch(1900); // Dropped EP fragment 3
    const auto final = engine.snapshot();
    require(final.worstFragmentSpan[1] != nullptr, "retain a finalized EP span");
    const auto &ep = *final.worstFragmentSpan[1];
    require(final.worstNdiWaitEp == final.worstFragmentSpan[1] &&
                final.worstNdiReadyToEnd == final.worstNdiWait && !pending.worstNdiWaitEp,
            "publish matching EP after completion without mutating an older snapshot");
    require(!ep.complete && ep.fragments.size() == 4 && ep.callCount == 2 && ep.spanUs() == 901,
            "all original attempts, including dropped and failed, contribute to the observed span");
    require(final.fragmentSpanDistribution[1].count == 1 &&
                final.fragmentSpanDistribution[1].maxUs == 901,
            "a finished series of original attempts contributes even when a transport call fails");
    require(ep.beforeFirstCallUs == 1 && ep.ownCallUs == 22 && ep.betweenCallsUs == 689 &&
                ep.afterLastCallUs == 189,
            "exact partition into pre-call, actual transport calls, between-call gaps and trailing "
            "drop");
    require(ep.fragments[0].callStartUs == 1001 && ep.fragments[0].endUs == 1012 &&
                ep.fragments[0].success && !ep.fragments[1].called && ep.fragments[2].called &&
                !ep.fragments[2].success && !ep.fragments[3].called,
            "actual transport timestamps exclude injected drops and include failed real calls");
    for (const auto &fragment : ep.fragments)
        require(!fragment.pendingCompletion &&
                    fragment.submitReturnUs == (fragment.called ? fragment.endUs : 0),
                "immediate success and failure reuse return time; injected drops have no return");
    require(ep.intervening.ndi == 1 && ep.intervening.ep == 0 && ep.intervening.status == 1 &&
                ep.intervening.retry == 0 && ep.intervening.duplicate == 1,
            "disjoint foreign dispatch counts");
    std::uint64_t pacing = 0;
    for (std::size_t i = 1; i < ep.fragments.size(); ++i) {
        const auto &row = ep.fragments[i];
        const auto previous = ep.fragments[i - 1].endUs;
        const auto expected = std::clamp(row.pacingDueUs, previous, row.attemptUs) - previous;
        require(row.index == i && row.pacingGapUs == expected,
                "pacing bound clipped to this attempt gap");
        pacing += expected;
    }
    require(ep.pacingGapUs == pacing, "per-fragment bounds sum to the frame bound");
    require(engine.snapshot().worstFragmentSpan[1] == final.worstFragmentSpan[1],
            "periodic snapshots share the finalized interval list");
}

void testFragmentTraceLimits() {
    Config c;
    c.fragmentTrace = true;
    c.faults.dropAlways = {0, 1};
    Engine drops(c);
    drops.submit(make(EpModule, 1, 2 * rt::PayloadStep, 1000));
    auto noSend = [](const auto *, std::size_t) -> bool {
        throw std::runtime_error("unexpected real send");
    };
    for (std::uint64_t t : {1000ULL, 1050ULL})
        require(drops.step(t, noSend,
                           [&] {
                               return t;
                           }),
                "injected drop");
    auto failed = drops.snapshot().worstFragmentSpan[1];
    require(failed && !failed->complete && failed->callCount == 0 && failed->ownCallUs == 0 &&
                failed->betweenCallsUs == 0 && failed->beforeFirstCallUs == 50 &&
                failed->afterLastCallUs == 0,
            "all-dropped span needs no call timestamp");

    c.faults = {};
    Engine large(c);
    large.submit(make(EpModule, 2, rt::MaxFrameBytes, 2000));
    std::uint64_t now = 2000;
    while (large.pendingInitial()) {
        require(large.step(
                    now,
                    [](auto *, auto) {
                        return true;
                    },
                    [&] {
                        return now;
                    }),
                "maximum frame dispatch");
        now = large.nextDueUs();
    }
    auto full = large.snapshot().worstFragmentSpan[1];
    require(full && full->complete &&
                full->fragments.size() == rt::fragmentCount(rt::MaxFrameBytes) &&
                std::size_t(full->fragments.back().index) + 1 == full->fragments.size(),
            "no truncation at the 2 MiB frame limit");
    Engine off(Config{});
    off.submit(make(NdiModule, 1, 1, 1000));
    unsigned reads = 0;
    require(off.step(
                1000,
                [](auto *, auto) {
                    return true;
                },
                [&] {
                    ++reads;
                    return 1000;
                }),
            "untraced send");
    const auto untraced = off.snapshot();
    require(reads == 2 && !untraced.worstFragmentSpan[0] &&
                untraced.fragmentSpanDistribution[0].count == 0 &&
                !untraced.worstPacketGaps[0].current.kind,
            "opt-out preserves the two clock reads and records no span samples");
}

void testFragmentSpanDistribution() {
    Config c;
    c.fragmentTrace = true;
    Engine engine(c);
    std::uint64_t now = 30000;
    auto singleFrame = [&](std::uint8_t module, unsigned id, std::uint64_t duration) {
        engine.submit(make(module, id, 1, now));
        const auto start = now;
        auto send = [&](auto *, auto) {
            now += duration;
            return true;
        };
        require(engine.step(start, send,
                            [&] {
                                return now;
                            }),
                "submit timed one-fragment frame");
        now += 30000;
    };
    for (unsigned i = 0; i < 998; ++i)
        singleFrame(NdiModule, i, 50);
    singleFrame(NdiModule, 998, 17999);
    singleFrame(NdiModule, 999, 18001);
    singleFrame(EpModule, 1, 17999);
    singleFrame(EpModule, 2, 18000);
    singleFrame(EpModule, 3, 18001);
    engine.submit(make(EpModule, 4, rt::PayloadStep + 1, now));
    require(engine.step(
                now,
                [](auto *, auto) {
                    return true;
                },
                [&] {
                    return now;
                }),
            "partially submit a fourth EP frame");
    engine.stop();
    const auto distributions = engine.snapshot().fragmentSpanDistribution;
    const auto &ndi = distributions[0];
    require(ndi.count == 1000 && ndi.p999Us == 18000 && ndi.maxUs == 18001 && ndi.overLimit == 1,
            "NDI P99.9 uses all observations and the 100 us histogram bucket");
    const auto &ep = distributions[1];
    require(ep.count == 3 && ep.p999Us == 18001 && ep.maxUs == 18001 && ep.overLimit == 2,
            "include exact 18 ms in EP threshold, excluding the partially attempted frame");
}

void testWorstGapPairs() {
    Config c;
    c.fragmentTrace = true;
    c.initialStatusSequence = 91;
    c.faults.duplicateFirst = {0};
    c.faults.dropFirst = {1};
    Engine engine(c);
    engine.submit(make(EpModule, 7, 3 * rt::PayloadStep));
    auto step = [&](std::uint64_t t, bool success = true) {
        auto clock = t;
        require(engine.step(
                    t,
                    [&](auto *, auto) {
                        ++clock;
                        return success;
                    },
                    [&] {
                        return clock;
                    }),
                "gap fixture dispatch");
    };
    step(1000);
    step(1100); // duplicate of index 0
    step(1200); // injected drop of index 1
    const auto early = engine.snapshot();
    const auto &duplicate = early.worstPacketGaps[0];
    require(early.globalPacketGapDeviation.count == 1 &&
                !early.worstPacketGaps[EpModule].current.kind && duplicate.current.kind &&
                std::string(duplicate.current.kind) == "duplicate" &&
                duplicate.previous.index == 0 && duplicate.actualUs == 100 &&
                duplicate.deviationUs == early.globalPacketGapDeviation.maxUs,
            "global includes duplicate, both scopes exclude injected drop");
    step(1600, false); // a real, unsuccessful send attempt counts in both streams
    const auto originals = engine.snapshot();
    const auto &ep = originals.worstPacketGaps[EpModule];
    require(originals.modules[1].packetGapDeviation.count == 1 && ep.previous.startUs == 1000 &&
                ep.current.startUs == 1600 && ep.current.endUs == 1601 && ep.previous.id == 7 &&
                ep.previous.index == 0 && ep.current.index == 2 && !ep.current.success &&
                ep.actualUs == 600 && ep.expectedUs == ep.current.planUs - ep.previous.planUs &&
                ep.deviationUs == originals.modules[1].packetGapDeviation.maxUs,
            "module maximum spans non-adjacent originals and records failed attempts");
    const auto nack = encodeNack({EpModule, 7, {1}});
    engine.feedback(nack.bytes.data(), nack.size, 900); // deliberately older request anchor
    step(5000);
    require(engine.submitStatus({1}, 900), "queue status with the same original anchor");
    step(15000);
    const auto mixed = engine.snapshot();
    const auto &global = mixed.worstPacketGaps[0];
    const auto &pairs = mixed.globalGapViolationPairs;
    require(pairs.size() == 2 && mixed.globalGapViolations == 2 &&
                pairs[0].previous.moduleId == EpModule && pairs[0].previous.id == 7 &&
                pairs[0].previous.index == 2 && !pairs[0].previous.success &&
                pairs[0].previous.callStartUs == 1600 && pairs[0].previous.submitReturnUs == 1601 &&
                pairs[0].current.index == 1 && std::string(pairs[0].current.kind) == "retry" &&
                pairs[1].previous.startUs == pairs[0].current.startUs &&
                pairs[1].current.moduleId == StatusModule && !pairs[1].current.callStartUs,
            "violations preserve adjacent packets, failed sends and original call timing");
    require(mixed.globalPacketGapDeviation.count == 4 && global.previous.moduleId == EpModule &&
                global.previous.index == 1 && global.previous.kind &&
                std::string(global.previous.kind) == "retry" &&
                global.current.moduleId == StatusModule && global.current.id == 91 &&
                std::string(global.current.kind) == "status" && global.expectedUs == 0 &&
                global.actualUs == 10000 &&
                global.deviationUs == mixed.globalPacketGapDeviation.maxUs &&
                mixed.modules[1].packetGapDeviation.count == 1 &&
                !mixed.worstPacketGaps[NdiModule].current.kind,
            "global retry/status boundary preserves original anchors and independent module "
            "population");
    Engine exact(c);
    for (std::uint64_t t : {1000ULL, 2000ULL, 3000ULL}) {
        require(exact.submitStatus({1}, t) && exact.step(
                                                  t,
                                                  [](auto *, auto) {
                                                      return true;
                                                  },
                                                  [&] {
                                                      return t;
                                                  }),
                "exactly paced status fixture");
    }
    const auto tied = exact.snapshot();
    require(tied.globalPacketGapDeviation.count == 2 && tied.globalPacketGapDeviation.maxUs == 0 &&
                tied.worstPacketGaps[0].previous.id == 91 &&
                tied.worstPacketGaps[0].current.id == 92,
            "retain the first valid pair when all deviations are zero");
}

void testGlobalGapViolationRetention() {
    auto run = [](bool trace) {
        Config config;
        config.fragmentTrace = trace;
        Engine engine(config);
        std::uint64_t now = 2000;
        auto dispatch = [&] {
            require(engine.submitStatus({1}, 100), "queue status with fixed anchor");
            require(engine.step(now, [](auto *, auto) { return true; }, [&] { return now; }),
                    "send status for bounded violation trace");
        };
        dispatch();
        now += 999;
        dispatch();
        now += 1000;
        dispatch();
        for (std::size_t i = 0; i < MaxGlobalGapViolationPairs + 4; ++i) {
            now += 1001 + i;
            dispatch();
        }
        return engine.snapshot();
    };
    const auto traced = run(true);
    const auto untraced = run(false);
    const auto &pairs = traced.globalGapViolationPairs;
    require(traced.globalGapViolations == MaxGlobalGapViolationPairs + 5 &&
                pairs.size() == MaxGlobalGapViolationPairs && pairs.front().deviationUs == 1000 &&
                pairs.back().deviationUs == 1000 + MaxGlobalGapViolationPairs - 1 &&
                traced.worstPacketGaps[0].deviationUs == 1000 + MaxGlobalGapViolationPairs + 4,
            "retain first threshold-inclusive violations; worst pair includes overflow");
    require(untraced.globalGapViolationPairs.empty() &&
                untraced.globalGapViolations == traced.globalGapViolations &&
                untraced.globalPacketGapDeviation.count == traced.globalPacketGapDeviation.count &&
                untraced.globalPacketGapDeviation.maxUs == traced.globalPacketGapDeviation.maxUs,
            "diagnostics preserve gap population and count when tracing is disabled");
}

void testNdiHandoffInViolationPairs() {
    Config config;
    config.fragmentTrace = true;
    Engine engine(config);
    FrameHandoff handoff;
    handoff.enqueuedUs = 1200;
    handoff.dequeuedUs = 1300;
    engine.submit(make(NdiModule, 5, 1, 1000), handoff);
    auto sendAt = [&](std::uint64_t now) {
        require(engine.step(now, [](auto *, auto) { return true; }, [&] { return now; }),
                "dispatch frame for violation handoff evidence");
    };
    sendAt(2000);
    require(engine.submitStatus({1}, 1000), "queue status after NDI");
    sendAt(5000);
    const auto nack = encodeNack({NdiModule, 5, {0}});
    engine.feedback(nack.bytes.data(), nack.size, 5000);
    sendAt(15000);
    const auto snapshot = engine.snapshot();
    const auto &pairs = snapshot.globalGapViolationPairs;
    require(pairs.size() == 2 && pairs[0].previous.id == 5 &&
                pairs[0].previous.enqueuedUs == 1200 && pairs[0].previous.dequeuedUs == 1300 &&
                !pairs[0].current.enqueuedUs && !pairs[0].current.dequeuedUs &&
                std::string(pairs[1].current.kind) == "retry" &&
                pairs[1].current.enqueuedUs == 1200 && pairs[1].current.dequeuedUs == 1300,
            "both NDI DATA and later retry retain handoff stamps without handoff tracing");
}

void testPreviousEpFramesInViolationPairs() {
    Config config;
    config.fragmentTrace = true;
    Engine engine(config);
    auto sendAt = [&](std::uint64_t now) {
        require(engine.step(now, [](auto *, auto) { return true; }, [&] { return now; }),
                "dispatch packet for previous EP frame trace");
    };
    engine.submit(make(EpModule, 71, 4 * rt::PayloadStep));
    sendAt(1000);
    sendAt(4000);
    const auto active = engine.snapshot();
    require(active.gapPreviousEpFrames.size() == 1 && active.gapPreviousEpFrames[0].awaitingFinalize &&
                !active.gapPreviousEpFrames[0].span,
            "active EP frame does not expose a mutable span");
    sendAt(7000);
    sendAt(10000);
    engine.submit(make(EpModule, 72, 1));
    sendAt(13000);
    require(engine.submitStatus({1}, 1000), "queue status after EP");
    sendAt(16000);
    const auto done = engine.snapshot();
    const auto &pairs = done.globalGapViolationPairs;
    const auto &frames = done.gapPreviousEpFrames;
    require(pairs.size() == 5 && frames.size() == 2 && frames[0].frameId == 71 &&
                frames[0].span && frames[0].span->fragments.size() == 4 &&
                !frames[0].awaitingFinalize && frames[1].frameId == 72 && frames[1].span &&
                frames[1].span->fragments.size() == 1 && !active.gapPreviousEpFrames[0].span,
            "snapshots retain each frozen EP span independently of the worst frame");
    for (const auto &pair : pairs) {
        require(pair.previousEpFrameIndex &&
                    frames.at(*pair.previousEpFrameIndex).frameId == pair.previous.id,
                "each EP pair references the correct distinct frame");
    }
    engine.submit(make(EpModule, 73, 3 * rt::PayloadStep));
    sendAt(19000);
    require(engine.submitStatus({1}, 1000), "queue status while EP is unfinished");
    sendAt(22000);
    engine.stop();
    const auto stopped = engine.snapshot();
    require(!stopped.globalGapViolationPairs.at(5).previousEpFrameIndex &&
                stopped.gapPreviousEpFrames.size() == 3 && stopped.gapPreviousEpFrames[2].frameId == 73 &&
                !stopped.gapPreviousEpFrames[2].awaitingFinalize && !stopped.gapPreviousEpFrames[2].span,
            "non-EP predecessors have no link and a cancelled EP span is explicitly unavailable");
}

void testWorstPacketEventOrder() {
    Engine engine(Config{});
    // Repeated scores exercise both equal-score rejection and replacement of
    // the first of several tied minimum slots. Compare every published prefix.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> expected;
    for (unsigned i = 0; i < 128; ++i) {
        const std::uint64_t now = (i + 1) * 1000;
        const std::uint64_t score = ((i * 7 + 9) % 11) * 10;
        require(engine.submitStatus({1}, now - score), "status timing fixture");
        require(engine.step(
                    now,
                    [](auto *, auto) {
                        return true;
                    },
                    [&] {
                        return now;
                    }),
                "status dispatched");
        if (expected.size() < 32) {
            expected.emplace_back(score, now);
        } else {
            auto smallest = std::min_element(expected.begin(), expected.end(),
                                             [](const auto &a, const auto &b) {
                                                 return a.first < b.first;
                                             });
            if (score > smallest->first)
                *smallest = {score, now};
        }
        const auto actual = engine.snapshot().worstEvents;
        require(actual.size() == expected.size(), "bounded packet event size");
        for (std::size_t j = 0; j < expected.size(); ++j) {
            require(actual[j].scoreUs == expected[j].first &&
                        actual[j].sendStartUs == expected[j].second,
                    "preserve packet event ordering and original tie behavior");
        }
    }
    require(engine.nextDueUs() == UINT64_MAX, "idle engine has no next due time");
    const auto unexpectedClock = []() -> std::uint64_t {
        throw std::runtime_error("idle engine read send clock");
    };
    require(!engine.step(
                200000,
                [](auto *, auto) {
                    return true;
                },
                unexpectedClock),
            "idle step stays idle");
}
} // namespace

int main() {
    try {
        testStaticEpPayload();
        testPriority();
        testDelayedCompletion();
        testBudget();
        testCacheStopAndLate();
        testPreflight();
        testLateStatusAndFaults();
        testRetryDuringDelayedAcquisition();
        testInitialDeadlineEvidence();
        testMultipartNdiBehindEarlierEpDeadline();
        testSingleNdiWithEpSlack();
        testSingleNdiProtection();
        testSingleNdiDeadlineBoundary();
        testFirstDispatchBehindPreviousEp();
        testFirstDispatchPacingAndRetention();
        testFragmentSpanPartition();
        testFragmentTraceLimits();
        testFragmentSpanDistribution();
        testWorstGapPairs();
        testGlobalGapViolationRetention();
        testNdiHandoffInViolationPairs();
        testPreviousEpFramesInViolationPairs();
        testWorstPacketEventOrder();
        std::cout << "V2 engine tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
