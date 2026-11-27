#include "v2/Runner.h"
#include "v2/RunnerReport.h"
#include "v2/Engine.h"
#include "v2/ProducerWait.h"
#include "sources/AuroraCodec.h"
#include "sources/AuroraSource.h"
#include "core/SamplingClock.h"
#include "protocol/PacketCodec.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace rt::v2 {
namespace {
using runner_detail::SourceStats;
using runner_detail::State;
using runner_detail::TraceClock;

struct SenderWait {
    const char *kind = nullptr;
    const char *result = nullptr;
    std::uint64_t beginUs = 0;
    std::uint64_t returnUs = 0;
};

std::uint64_t nativeThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();
#elif defined(__linux__)
    return static_cast<std::uint64_t>(syscall(SYS_gettid));
#else
    return 0;
#endif
}

TraceClock captureTraceClock() {
    TraceClock c;
    c.senderTid = nativeThreadId();
#ifdef _WIN32
    c.processId = GetCurrentProcessId();
    LARGE_INTEGER freq{};
    LARGE_INTEGER before{};
    LARGE_INTEGER after{};
    FILETIME utc{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&before);
    GetSystemTimePreciseAsFileTime(&utc);
    QueryPerformanceCounter(&after);
    c.frequency = freq.QuadPart;
    c.beforeTicks = before.QuadPart;
    c.afterTicks = after.QuadPart;
    c.utcFiletime = (static_cast<std::uint64_t>(utc.dwHighDateTime) << 32) | utc.dwLowDateTime;
#elif defined(__linux__)
    c.processId = static_cast<std::uint64_t>(getpid());
#endif
    return c;
}

// Producers, status submissions and Stop can wake the sender even if the
// notification arrives just before it begins waiting. Only the sender waits.
class SenderWake {
  public:
    void notify(std::atomic<std::uint64_t> *issue = nullptr) {
#ifdef _WIN32
        if (issue)
            issue->store(rt::monotonicUs(), std::memory_order_relaxed);
        SetEvent(work_.get());
#else
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = true;
        }
        if (issue)
            issue->store(rt::monotonicUs(), std::memory_order_relaxed);
        changed_.notify_one();
#endif
    }

    void until(std::uint64_t deadline, SenderWait *trace) {
        const auto now = rt::monotonicUs();
        if (now >= deadline)
            return;
        const auto us = deadline - now;
#ifdef _WIN32
        LARGE_INTEGER interval{};
        interval.QuadPart = -static_cast<LONGLONG>(us * 10);
        if (timer_ && SetWaitableTimer(timer_.get(), &interval, 0, nullptr, nullptr, FALSE)) {
            HANDLE handles[] = {work_.get(), timer_.get()};
            if (trace) {
                trace->kind = "work_event_or_timer";
                trace->beginUs = rt::monotonicUs();
            }
            const auto result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (trace) {
                trace->returnUs = rt::monotonicUs();
                trace->result = result == WAIT_OBJECT_0       ? "work_event"
                                : result == WAIT_OBJECT_0 + 1 ? "timer"
                                                              : "wait_failed";
            }
        } else {
            if (trace) {
                trace->kind = "work_event_or_timeout";
                trace->beginUs = rt::monotonicUs();
            }
            const auto result =
                WaitForSingleObject(work_.get(), static_cast<DWORD>((us + 999) / 1000));
            if (trace) {
                trace->returnUs = rt::monotonicUs();
                trace->result = result == WAIT_OBJECT_0  ? "work_event"
                                : result == WAIT_TIMEOUT ? "timeout"
                                                         : "wait_failed";
            }
        }
#else
        std::unique_lock<std::mutex> lock(mutex_);
        if (trace) {
            trace->kind = "condition_variable";
            trace->beginUs = rt::monotonicUs();
        }
        const bool work = changed_.wait_for(lock, std::chrono::microseconds(us), [this] {
            return pending_;
        });
        if (trace) {
            trace->returnUs = rt::monotonicUs();
            trace->result = work ? "work_signal" : "timeout";
        }
        pending_ = false;
#endif
    }
#ifdef _WIN32
    SenderWake() {
        if (!work_)
            throw std::runtime_error("Cannot create sender wake event");
    }

  private:
    using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;
    Handle work_{CreateEventW(nullptr, FALSE, FALSE, nullptr), CloseHandle};
    Handle timer_{CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS),
                  CloseHandle};
#else
  private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool pending_ = false;
#endif
};

// The sender can dequeue before the producer reaches SetEvent. The shared
// diagnostic timestamp outlives that race without extending the mailbox lock.
struct TraceTicket {
    FrameHandoff handoff;
    std::atomic<std::uint64_t> notifyIssueUs{0};
};

struct QueuedFrame {
    PreparedPtr frame;
    std::shared_ptr<TraceTicket> ticket;
};

struct SourceMailbox {
    std::mutex mutex;
    std::deque<QueuedFrame> queue;
    SourceStats stats;
    // Used when detailed handoff tracing is off. write is published after the
    // immutable frame, read only after consumer releases that cell. At most
    // four resident frames, exactly the same drop-new policy as the deque.
    std::array<PreparedPtr, 4> ring{};
    std::array<std::uint64_t, 4> enqueuedUs{};
    alignas(64) std::atomic<unsigned> write{0};
    alignas(64) std::atomic<unsigned> read{0};

    unsigned push(PreparedPtr frame, bool stampEnqueue) {
        const auto w = write.load(std::memory_order_relaxed);
        const auto used = w - read.load(std::memory_order_acquire);
        if (used == ring.size())
            return 0;
        ring[w % ring.size()] = std::move(frame);
        if (stampEnqueue)
            enqueuedUs[w % ring.size()] = rt::monotonicUs();
        write.store(w + 1, std::memory_order_release);
        return used + 1;
    }

    PreparedPtr pop(std::uint64_t *publishedUs = nullptr) {
        const auto r = read.load(std::memory_order_relaxed);
        if (r == write.load(std::memory_order_acquire))
            return {};
        auto frame = std::move(ring[r % ring.size()]);
        if (publishedUs)
            *publishedUs = enqueuedUs[r % ring.size()];
        read.store(r + 1, std::memory_order_release);
        return frame;
    }

    std::atomic<std::uint64_t> nextSample{0};
    std::atomic<bool> done{false};
};

Bytes readFile(const std::string &path, std::size_t max) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("Cannot open replay file: " + path);
    const auto n = f.tellg();
    if (n <= 0 || static_cast<std::uint64_t>(n) > max)
        throw std::runtime_error("Replay payload size outside allowed range");
    Bytes b(static_cast<std::size_t>(n));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char *>(b.data()), static_cast<std::streamsize>(b.size())))
        throw std::runtime_error("Cannot read complete replay payload");
    return b;
}
} // namespace

struct Session::Impl {
    TransportFactory factory;
    rt::aurora::NdiSourceFactory ndiFactory;
    LinkObserver observe;
    SenderWake wake;
    std::thread worker;
    ProducerStop producerStop;
    const std::atomic<bool> &stopRequested = producerStop.flag();
    std::atomic<bool> abortRequested{false};
    std::atomic<bool> active{false};
    mutable std::mutex mutex;
    std::shared_ptr<const State> state = std::make_shared<const State>();
    std::mutex statusMutex;
    std::deque<std::pair<Bytes, std::uint64_t>> statuses;
    std::atomic<bool> hasStatus{false}; // hint only; payload ownership stays under statusMutex
    std::uint64_t acceptedStatuses = 0;
    std::uint64_t rejectedStatuses = 0;
    std::uint64_t cancelledStatuses = 0;

    Impl(TransportFactory f, rt::aurora::NdiSourceFactory n, LinkObserver linkObserver)
        : factory(std::move(f)), ndiFactory(std::move(n)), observe(std::move(linkObserver)) {}

    void publishState(std::shared_ptr<const State> next) {
        std::lock_guard<std::mutex> lock(mutex);
        state.swap(next); // The previous state is released after the guard.
    }

    void execute(Config c);
    void source(Config c, unsigned id, std::uint64_t origin, SourceMailbox &mail,
                std::size_t ndiBytes, std::size_t epBytes,
                const std::atomic<unsigned> &observedRate);
};

void Session::Impl::source(Config c, unsigned id, std::uint64_t origin, SourceMailbox &mail,
                           std::size_t ndiBytes, std::size_t epBytes,
                           const std::atomic<unsigned> &observedRate) {
    SourceStats stats;
    stats.nextId = id == 1 ? c.initialNdiId : c.initialEpId;
    TimingHistogram interval;
    TimingHistogram deviation;
    TimingHistogram late;
    if (c.handoffTrace || c.fragmentTrace)
        stats.producerTid = nativeThreadId();
    std::unique_ptr<rt::aurora::INdiSource> ndi;
    auto publish = [&] {
        stats.intervals = interval.summary();
        stats.deviation = deviation.summary();
        stats.lateness = late.summary();
        std::lock_guard<std::mutex> lock(mail.mutex);
        mail.stats = stats;
    };
    try {
        const bool enabled = id == 1 ? c.ndiEnabled : c.epEnabled;
        if (!enabled) {
            publish();
            mail.done = true;
            wake.notify();
            return;
        }
        const auto file = id == 1 ? c.ndiReplayFile : c.epReplayFile;
        Bytes replay;
        Bytes ep;
        if (!file.empty()) {
            stats.kind = "replay";
            replay = readFile(file, id == 1 ? 16384 : rt::MaxFrameBytes);
            if (id == 1)
                rt::aurora::decodeBx(replay);
        }
        if (id == 1) {
            auto sourceFactory =
                ndiFactory
                    ? ndiFactory
                    : (!replay.empty() ? rt::aurora::replaySourceFactory(replay, c.ndiHz)
                                       : rt::aurora::simulationSourceFactory(c.ndiTools, c.ndiHz));
            ndi = sourceFactory();
            if (!ndi)
                throw std::runtime_error("NDI factory returned no data source");
            stats.kind = ndi->kind();
            ndi->start(stopRequested);
            stats.actualDeviceHz = stats.kind == "device" ? ndi->actualHz() : 0;
            stats.counterHz = ndi->counterHz();
        }
        if (id == 2 && replay.empty()) {
            ep.resize(c.epBytes);
            for (std::size_t j = 0; j < ep.size(); ++j)
                ep[j] = static_cast<std::uint8_t>(j * 17 + 3);
        }
        const std::size_t payload =
            file.empty() ? (id == 1 ? 11 + 42 * c.ndiTools : ep.size()) : replay.size();
        stats.payloadBytes = payload;
        const auto check = preflight(c, id == 1 ? payload : ndiBytes, id == 2 ? payload : epBytes,
                                     observedRate.load());
        if (!check.feasible && !c.allowOversubscribed)
            throw std::runtime_error(check.reason);
        std::optional<StaticEpPayload> epTemplate;
        if (id == EpModule)
            epTemplate.emplace(replay.empty() ? std::move(ep) : std::move(replay));
        const auto hz = id == 1 ? c.ndiHz : c.epHz;
        const auto phase = id == 1 ? c.ndiPhaseUs : c.epPhaseUs;
        ProducerWait sampleWait(producerStop, stats.wait, hz, ProducerWait::Timer::Automatic,
                                c.sampleLeadCapUs);
        const bool ringPath = c.mailboxFastPath && !c.handoffTrace;
        std::uint64_t slot = 0;
        std::uint64_t previous = 0;
        std::uint64_t nextStatsUs = 0;
        auto advance = [&](std::uint64_t now, PreparedPtr p = {}) {
            auto next = slot + 1;
            const auto latest = (now - origin - phase) * hz / 1000000ULL + 1;
            if (latest > next) {
                stats.skipped += latest - next;
                next = latest;
            }
            slot = next;
            if (ringPath) {
                mail.nextSample.store(origin + phase + (slot * 1000000ULL) / hz,
                                      std::memory_order_release);
                if (p) {
                    const auto depth = mail.push(std::move(p), c.fragmentTrace && id == 1);
                    if (!depth)
                        ++stats.queueDrops;
                    stats.queueHighWater = std::max<std::uint64_t>(stats.queueHighWater, depth);
                }
                wake.notify();
            } else {
                auto ticket = c.handoffTrace && p ? std::make_shared<TraceTicket>() : nullptr;
                {
                    std::lock_guard<std::mutex> lock(mail.mutex);
                    if (p) {
                        if (mail.queue.size() == 4) {
                            ++stats.queueDrops;
                            ticket.reset();
                        } else {
                            mail.queue.push_back({std::move(p), ticket});
                            if (ticket)
                                ticket->handoff.enqueuedUs = rt::monotonicUs();
                        }
                    }
                    stats.queueHighWater =
                        std::max<std::uint64_t>(stats.queueHighWater, mail.queue.size());
                    mail.nextSample = origin + phase + (slot * 1000000ULL) / hz;
                }
                wake.notify(ticket ? &ticket->notifyIssueUs : nullptr);
            }
            // Publish distributions after the frame is visible to the sender.
            // Every slot is measured; the final source publication is unconditional.
            if (now >= nextStatsUs) {
                publish();
                nextStatsUs = now + 100000;
            }
        };
        while (!stopRequested.load(std::memory_order_acquire)) {
            const auto planned = origin + phase + (slot * 1000000ULL) / hz;
            mail.nextSample = planned;
            if (c.durationUs && planned >= origin + c.durationUs)
                break;
            if (c.frameLimit && stats.sampled >= c.frameLimit)
                break;
            std::uint64_t now = 0;
            if (!sampleWait.until(planned, now))
                break;
            if (c.durationUs && now >= origin + c.durationUs) {
                const auto total =
                    (c.durationUs > phase ? ((c.durationUs - phase) * hz + 999999) / 1000000 : 0);
                if (total > slot)
                    stats.skipped += total - slot;
                break;
            }
            const auto sampled = now;
            ++stats.sampled;
            if (!stats.firstSampleUs)
                stats.firstSampleUs = sampled;
            stats.lastSampleUs = sampled;
            late.add(sampled > planned ? sampled - planned : 0, 999);
            if (previous) {
                const auto gap = sampled - previous;
                interval.add(gap);
                const auto nominal = static_cast<std::uint64_t>(std::llround(1000000.0 / hz));
                deviation.add(gap > nominal ? gap - nominal : nominal - gap, 999);
            }
            previous = sampled;
            Frame frame;
            frame.moduleId = static_cast<std::uint8_t>(id);
            frame.timestampUs = sampled;
            std::uint64_t observedCounter = 0;
            if (id == 1) {
                auto observation = ndi->read(rt::monotonicUs, stopRequested);
                frame.bytes = std::move(observation.bytes);
                if (observation.newSourceFrame)
                    ++stats.newSourceFrames;
                else
                    ++stats.duplicateSource;
                if (observation.semanticChange)
                    ++stats.semanticChanges;
                bool firstCounter = true;
                bool mixed = false;
                for (const auto &tool : observation.toolTimes) {
                    stats.sourceCounterGaps += tool.counter.missingSamples;
                    stats.sourceCounterResets += tool.counter.reset ? 1 : 0;
                    if (firstCounter) {
                        observedCounter = tool.counter.unwrapped;
                        firstCounter = false;
                    } else
                        mixed = mixed || observedCounter != tool.counter.unwrapped;
                }
                if (mixed)
                    ++stats.mixedCounterFrames;
                stats.commonClockMapped = observation.sourceTime.has_value();
                if (observation.sourceTime)
                    stats.clockErrorBoundUs =
                        std::max(stats.clockErrorBoundUs, observation.sourceTime->errorBoundUs);
                if (stats.kind == "device") {
                    if (!observation.sourceTime) {
                        ++stats.unmappedSourceFrames;
                        advance(rt::monotonicUs());
                        continue;
                    }
                    frame.timestampUs = observation.sourceTime->timestampUs;
                    if (!observation.newSourceFrame && !observation.semanticChange) {
                        advance(rt::monotonicUs());
                        continue;
                    }
                }
                // Reject an oversized device reply here, before it can reach the
                // shared sender and interrupt the independent EP/status streams.
                stats.payloadBytes = frame.bytes.size();
                const auto currentBudget =
                    preflight(c, frame.bytes.size(), epBytes, observedRate.load());
                if (!currentBudget.feasible && !c.allowOversubscribed)
                    throw std::runtime_error("NDI observed length fails capacity screening: " +
                                             currentBudget.reason);
            }
            frame.frameId = stats.nextId++;
            auto p = epTemplate
                         ? epTemplate->sample(frame.frameId, planned, sampled)
                         : prepareFrame(std::move(frame), planned, sampled, 0, observedCounter);
            now = rt::monotonicUs();
            stats.prepareMaxUs = std::max(stats.prepareMaxUs, now - sampled);
            ++stats.built;
            stats.ledger.add(*p);
            advance(now, std::move(p));
        }
    } catch (const std::exception &e) {
        if (stopRequested)
            ++stats.cancelledAttempts;
        else {
            ++stats.errors;
            stats.error = e.what();
        }
    }
    if (ndi) {
        try {
            ndi->shutdown();
        } catch (const std::exception &e) {
            ++stats.errors;
            if (stats.error.empty())
                stats.error = e.what();
        }
    }
    try {
        publish();
    } catch (...) {
    }
    mail.nextSample = 0;
    mail.done.store(true, std::memory_order_release);
    wake.notify();
}

void Session::Impl::execute(Config c) {
    State local;
#ifdef _WIN32
    // This dedicated sender thread exits with the session.
    local.senderPriorityRaised =
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL) != FALSE;
    if (!local.senderPriorityRaised)
        local.senderPriorityError = GetLastError();
#endif
    local.config = c;
    std::array<SourceMailbox, 2> sources;
    std::array<std::thread, 2> producers;
    if (c.handoffTrace || c.fragmentTrace)
        local.traceClock = captureTraceClock();
    sources[0].stats.nextId = c.initialNdiId;
    sources[1].stats.nextId = c.initialEpId;
    local.engine.nextStatusSequence = c.initialStatusSequence;
    std::unique_ptr<Engine> engineOwner;
    std::unique_ptr<IUdpTransport>
        udp; // lives through cancellation and delivery of terminal completions
    std::thread observer;
    std::atomic<bool> observerStop{false};
    std::atomic<unsigned> observedRate{0};
    std::mutex observationMutex;
    LinkObservation latestObservation;
    auto publish = [&](const Engine *engine) {
        {
            std::lock_guard<std::mutex> lock(observationMutex);
            if (latestObservation.observedAtUs)
                local.link = latestObservation;
        }
        // local keeps session metadata; event vectors belong to each published snapshot.
        auto next = std::make_shared<State>(local);
        if (engine)
            next->engine = engine->snapshot();
        if (udp)
            next->operationStorage = udp->operationStorageStats();
        for (unsigned i = 0; i < 2; ++i) {
            std::lock_guard<std::mutex> lock(sources[i].mutex);
            next->sources[i] = sources[i].stats;
        }
        {
            std::lock_guard<std::mutex> lock(statusMutex);
            next->statusIngressAccepted = acceptedStatuses;
            next->statusIngressRejected = rejectedStatuses;
            next->statusIngressCancelled = cancelledStatuses;
            next->statusIngressPending = statuses.size();
        }
        const auto now = rt::monotonicUs();
        next->elapsedUs = local.startedUs && now > local.startedUs ? now - local.startedUs : 0;
        publishState(std::move(next));
    };
    try {
        local.link = observe(c);
        latestObservation = local.link;
        observedRate = local.link.mbps;
        local.engine.effectiveMbps = effectiveMbps(c, local.link.mbps);
        auto fileBytes = [](const std::string &path) {
            std::error_code error;
            if (path.empty())
                return std::size_t(0);
            const auto size = std::filesystem::file_size(std::filesystem::u8path(path), error);
            return error ? std::size_t(0) : static_cast<std::size_t>(size);
        };
        std::size_t knownNdiBytes = fileBytes(c.ndiReplayFile);
        std::size_t knownEpBytes = fileBytes(c.epReplayFile);
        local.capacity = preflight(c, knownNdiBytes, knownEpBytes, local.link.mbps);
        if (!local.capacity.feasible && !c.allowOversubscribed)
            throw std::runtime_error(local.capacity.reason);
        udp = factory();
        if (!udp)
            throw std::runtime_error("Transport factory returned no socket");
        StreamConfig transportConfig;
        transportConfig.targetIp = c.targetIp;
        transportConfig.bindIp = c.bindIp;
        transportConfig.targetPort = c.targetPort;
        transportConfig.commandPort = c.commandPort;
        if (c.asioPooledHandler)
            udp->setOperationStorageReuse(true);
        udp->open(transportConfig);
        local.localPort = udp->localPort();
        local.df = udp->dfEnabled();
        local.transport = udp->name();
        local.sendApi = udp->sendApi();
        local.boostVersion = udp->libraryVersion();
        local.sendBuffer = udp->sendBufferBytes();
        local.receiveBuffer = udp->receiveBufferBytes();
        local.resetError = udp->connResetError();
        local.asynchronous = local.sendApi.find("async_send_to") != std::string::npos;
        engineOwner = std::make_unique<Engine>(c, local.link.mbps);
        auto &engine = *engineOwner;
        local.startedUs = rt::monotonicUs() + 50000;
        for (unsigned i = 0; i < 2; ++i)
            producers[i] = std::thread([&, i, initialNdi = knownNdiBytes,
                                        initialEp = knownEpBytes] {
                source(c, i + 1, local.startedUs, sources[i], initialNdi, initialEp, observedRate);
            });
        observer = std::thread([&] {
            while (!observerStop) {
                for (unsigned n = 0; n < 10 && !observerStop; ++n)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (observerStop)
                    break;
                try {
                    auto observation = observe(c);
                    if (observation.mbps)
                        observedRate = observation.mbps;
                    std::lock_guard<std::mutex> lock(observationMutex);
                    latestObservation = std::move(observation);
                } catch (const std::exception &e) {
                    std::lock_guard<std::mutex> lock(observationMutex);
                    latestObservation.reason =
                        std::string("Link observer failed; retaining last known rate: ") + e.what();
                } catch (...) {
                    std::lock_guard<std::mutex> lock(observationMutex);
                    latestObservation.reason = "Link observer failed; retaining last known rate";
                }
            }
        });
        std::uint64_t nextPublish = 0;
        std::uint64_t nextStatus = c.statusIntervalUs ? local.startedUs : UINT64_MAX;
        std::uint64_t drainStart = 0;
        constexpr std::uint64_t drainLimitUs = 2000000;
        auto stopAt = c.durationUs ? local.startedUs + c.durationUs + drainLimitUs : UINT64_MAX;
        bool finishing = false;
        constexpr unsigned maxDispatchesPerPoll = 8;
        std::uint64_t lastServiceUs = 0;
        unsigned dispatchesSinceService = 0;
        auto nextService = [&](std::uint64_t packetDue) {
            return lastServiceUs + (packetDue == UINT64_MAX ? 500u : 100u);
        };
        const bool ringPath = c.mailboxFastPath && !c.handoffTrace;
        // An idle sender may briefly check the mailbox around a published NDI sample slot.
        // At the maximum rate, cap pre/post-slot checks at 1/32 and 1/64 of a period.
        const auto ndiLeadUs = std::min(c.ndiHandoffGuardUs, 1000000u / c.ndiHz / 32);
        const auto ndiTailUs = ndiLeadUs / 2;
        std::uint64_t lastGuardSlot = 0;
        SenderWait lastWait;
        while (!abortRequested.load(std::memory_order_acquire)) {
            auto now = rt::monotonicUs();
            if (!finishing && stopRequested.load(std::memory_order_acquire)) {
                finishing = true;
                nextStatus = UINT64_MAX;
                stopAt = std::min(stopAt, now + drainLimitUs);
            }
            bool sizeChanged = false;
            for (unsigned i = 0; i < 2; ++i) {
                PreparedPtr p;
                std::shared_ptr<TraceTicket> ticket;
                std::uint64_t publishedUs = 0;
                std::uint64_t ndiDequeuedUs = 0;
                std::uint64_t nextSample = 0;
                if (ringPath) {
                    p = sources[i].pop(c.fragmentTrace && i == 0 ? &publishedUs : nullptr);
                    nextSample = sources[i].nextSample.load(std::memory_order_acquire);
                } else {
                    auto &sourceBox = sources[i];
                    std::lock_guard<std::mutex> lock(sourceBox.mutex);
                    if (!sourceBox.queue.empty()) {
                        if (c.handoffTrace) {
                            // Mark every newly visible frame, including ones that
                            // remain behind the head until a later sender turn.
                            const auto poll = rt::monotonicUs();
                            for (auto &queued : sourceBox.queue) {
                                auto &h = queued.ticket->handoff;
                                if (h.senderObservedUs)
                                    continue;
                                h.senderObservedUs = now >= h.enqueuedUs ? now : poll;
                                if (lastWait.returnUs && h.enqueuedUs <= lastWait.returnUs) {
                                    h.waitKind = lastWait.kind;
                                    h.waitResult = lastWait.result;
                                    h.waitBeginUs = lastWait.beginUs;
                                    h.waitReturnUs = lastWait.returnUs;
                                }
                            }
                        }
                        p = std::move(sourceBox.queue.front().frame);
                        ticket = std::move(sourceBox.queue.front().ticket);
                        sourceBox.queue.pop_front();
                        if (ticket)
                            ticket->handoff.dequeuedUs = rt::monotonicUs();
                    }
                    nextSample = sourceBox.nextSample;
                }
                if (p) {
                    if (i == 0) {
                        const auto dequeued = rt::monotonicUs();
                        ndiDequeuedUs = dequeued;
                        local.ndiReadyToDequeueMaxUs =
                            std::max(local.ndiReadyToDequeueMaxUs, dequeued - p->readyUs);
                        if (lastGuardSlot && p->plannedSampleUs == lastGuardSlot &&
                            dequeued <= lastGuardSlot + ndiTailUs)
                            ++local.ndiGuardHits;
                    }
                    auto &known = i == 0 ? knownNdiBytes : knownEpBytes;
                    if (known != p->payload->byteSize) {
                        known = p->payload->byteSize;
                        sizeChanged = true;
                    }
                    if (ticket) {
                        auto h = std::move(ticket->handoff);
                        h.notifyIssueUs = std::shared_ptr<const std::atomic<std::uint64_t>>(
                            ticket, &ticket->notifyIssueUs);
                        h.submittedUs = rt::monotonicUs();
                        engine.submit(std::move(p), std::move(h));
                    } else if (publishedUs) {
                        FrameHandoff handoff;
                        handoff.enqueuedUs = publishedUs;
                        handoff.dequeuedUs = ndiDequeuedUs;
                        engine.submit(std::move(p), std::move(handoff));
                    } else
                        engine.submit(std::move(p));
                }
                engine.setNextSample(static_cast<std::uint8_t>(i + 1), nextSample);
            }
            if (c.handoffTrace)
                lastWait = {};
            if (!c.mailboxFastPath || hasStatus.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(statusMutex);
                if (!statuses.empty()) {
                    auto item = std::move(statuses.front());
                    statuses.pop_front();
                    engine.submitStatus(std::move(item.first), item.second);
                }
                hasStatus.store(!statuses.empty(), std::memory_order_release);
            }
            if (c.durationUs && now >= local.startedUs + c.durationUs)
                nextStatus = UINT64_MAX;
            if (now >= nextStatus) {
                Bytes b(16);
                rt::writeLe32(b.data(), 1);
                rt::writeLe32(b.data() + 4, c.ndiEnabled ? 1 : 0);
                rt::writeLe32(b.data() + 8, c.epEnabled ? 1 : 0);
                rt::writeLe32(b.data() + 12, c.linkMbps);
                engine.submitStatus(std::move(b), now);
                nextStatus += c.statusIntervalUs;
                if (nextStatus <= now)
                    nextStatus = now + c.statusIntervalUs;
            }
            const auto currentObserved = observedRate.load();
            const auto budget = effectiveMbps(c, currentObserved);
            if (sizeChanged || budget != local.capacity.effectiveMbps) {
                local.capacity = preflight(c, knownNdiBytes, knownEpBytes, currentObserved);
                if (!local.capacity.feasible && !c.allowOversubscribed)
                    throw std::runtime_error(
                        "Current payload sizes exceed the effective link budget");
            }
            engine.setEffectiveMbps(budget, now);
            // Exactly one send per turn. Stop and producer queues are revisited at every packet
            // boundary.
            const auto stepUs = now;
            const bool sent = engine.stepTransport(stepUs, *udp, rt::monotonicUs);
            if (sent) {
                drainStart = 0;
                ++dispatchesSinceService;
            }
            now = rt::monotonicUs();
            auto packetDue = engine.nextDueUs();
            const auto ndiSlot = ndiLeadUs && !finishing && packetDue == UINT64_MAX
                                     ? sources[0].nextSample.load(std::memory_order_acquire)
                                     : 0;
            const auto guardBegin = ndiSlot > ndiLeadUs ? ndiSlot - ndiLeadUs : 0;
            const auto guardEnd = ndiSlot + ndiTailUs;
            const bool nearNdi = ndiSlot && now >= guardBegin && now < guardEnd;
            // Publish between bursts where possible. A continuously busy stream
            // still publishes within 100 ms of the normal reporting deadline.
            const bool publishDue = now >= nextPublish &&
                                    ((packetDue == UINT64_MAX && !nearNdi) ||
                                     now - nextPublish >= 100000);
            // A short pacing gap does not end a burst. Bound feedback latency by
            // both dispatch count and elapsed time, then give the socket an idle poll.
            if (dispatchesSinceService >= maxDispatchesPerPoll || now >= nextService(packetDue) ||
                publishDue || now >= stopAt ||
                (packetDue == UINT64_MAX && dispatchesSinceService)) {
                lastServiceUs = rt::monotonicUs();
                dispatchesSinceService = 0;
                IncomingDatagram incoming;
                for (unsigned n = 0;
                     n < 4 && !abortRequested && (!n || rt::monotonicUs() - lastServiceUs < 40);
                     ++n) {
                    ++local.feedbackPollCalls;
                    const auto outcome = udp->receiveFeedback(incoming);
                    if (outcome == ReceiveOutcome::Error) {
                        ++local.feedbackReadErrors;
                        local.lastFeedbackReadError = udp->lastError();
                        break;
                    }
                    if (outcome == ReceiveOutcome::Empty) {
                        ++local.feedbackEmptyPolls;
                        break;
                    }
                    if (incoming.sourceIp != c.allowedNackIp ||
                        (c.allowedNackPort && incoming.sourcePort != c.allowedNackPort))
                        engine.rejectFeedbackSource();
                    else
                        engine.feedback(incoming.bytes.data(), incoming.bytes.size(),
                                        rt::monotonicUs());
                    if (engine.nextDueUs() <= rt::monotonicUs())
                        break;
                }
                now = rt::monotonicUs();
                if (publishDue) {
                    publish(&engine);
                    nextPublish = now + 100000;
                }
                const bool done = sources[0].done && sources[1].done &&
                                  (finishing || c.ndiEnabled || c.epEnabled ||
                                   (c.durationUs && now >= local.startedUs + c.durationUs));
                if (done)
                    nextStatus = UINT64_MAX;
                if (done && engine.nextDueUs() == UINT64_MAX && !udp->inFlight() &&
                    !hasStatus.load(std::memory_order_acquire)) {
                    bool empty = true;
                    for (auto &box : sources) {
                        if (ringPath)
                            empty = empty && box.read.load() == box.write.load();
                        else {
                            std::lock_guard<std::mutex> lock(box.mutex);
                            empty = empty && box.queue.empty();
                        }
                    }
                    if (empty) {
                        if (!drainStart)
                            drainStart = now;
                        if (now - drainStart >= 100000) {
                            local.endedReason = finishing
                                                    ? "user_finish_drained"
                                                    : "sources_finished_and_retry_grace_elapsed";
                            break;
                        }
                    }
                }
                if (now >= stopAt) {
                    local.endedReason = "drain_timeout";
                    local.error = "Timed out draining accepted work and receiver feedback";
                    break;
                }
                packetDue = engine.nextDueUs(); // Feedback may have scheduled a retry.
            }
            // A pacing slot may have matured since the step's clock sample. Only
            // a step declined at an already-due slot needs the guarded retry delay.
            if (packetDue <= now && (sent || packetDue > stepUs))
                continue;
            // A guarded retry revisits the producer queues at the next packet
            // opportunity. Pace short on-wire gaps as before. With no pending
            // packets, wait on the wake event until the next idle feedback poll.
            auto due = packetDue <= now ? now + 20 : packetDue;
            const auto publishWake = now < nextPublish ? nextPublish : nextPublish + 100000;
            due = std::min({due, nextStatus, publishWake, nextService(packetDue), stopAt});
            if (packetDue == UINT64_MAX && ndiSlot && now < guardBegin)
                due = std::min(due, guardBegin);
            if (packetDue == UINT64_MAX && ndiSlot && now >= guardBegin && now < guardEnd) {
                // Never hold the sender until the NDI slot: revisit EP, status, feedback
                // and stop between short checks, even if the NDI source stalls.
                if (lastGuardSlot != ndiSlot) {
                    lastGuardSlot = ndiSlot;
                    ++local.ndiGuardWindows;
                }
                const auto begin = rt::monotonicUs();
                rt::waitBrieflyUntil(std::min({due, now + 25, guardEnd}));
                const auto end = rt::monotonicUs();
                local.ndiGuardWaitWallUs += end - begin;
                if (c.handoffTrace)
                    lastWait = {"ndi_handoff_guard", "guard_return", begin, end};
            } else if (packetDue != UINT64_MAX && due <= now + 180) {
                if (c.handoffTrace) {
                    lastWait.kind = "brief_pacing";
                    lastWait.beginUs = rt::monotonicUs();
                }
                rt::waitBrieflyUntil(due);
                if (c.handoffTrace) {
                    lastWait.returnUs = rt::monotonicUs();
                    lastWait.result = "pacing_return";
                }
            } else
                wake.until(due, c.handoffTrace ? &lastWait : nullptr);
        }
        if (local.endedReason.empty())
            local.endedReason = "user_stop";
    } catch (const std::exception &e) {
        local.error = e.what();
        local.endedReason = "local_failure";
    } catch (...) {
        local.error = "Unknown worker error";
        local.endedReason = "local_failure";
    }
    producerStop.request();
    for (auto &t : producers)
        if (t.joinable())
            t.join();
    observerStop = true;
    if (observer.joinable())
        observer.join();
    for (auto &sourceBox : sources) {
        std::lock_guard<std::mutex> lock(sourceBox.mutex);
        sourceBox.stats.queueDrops += sourceBox.queue.size();
        sourceBox.queue.clear();
        while (sourceBox.pop())
            ++sourceBox.stats.queueDrops;
    }
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        cancelledStatuses += statuses.size();
        statuses.clear();
    }
    if (udp)
        udp->cancelOutstanding();
    if (engineOwner)
        engineOwner->stop();
    if (local.feedbackReadErrors && local.error.empty())
        local.error = "UDP feedback read error: " + local.lastFeedbackReadError;
    local.finished = true;
    local.endedUs = rt::monotonicUs();
    // A throw here would escape the worker thread and terminate the process while
    // busy() still reports true. Keep the flag transition unconditional.
    try {
        publish(engineOwner.get());
    } catch (const std::exception &e) {
        if (local.error.empty())
            local.error = std::string("publish failed: ") + e.what();
    } catch (...) {
        if (local.error.empty())
            local.error = "publish failed";
    }
    active.store(false, std::memory_order_release);
}

Session::Session(TransportFactory f, rt::aurora::NdiSourceFactory n, LinkObserver observer)
    : p_(std::make_unique<Impl>(std::move(f), std::move(n), std::move(observer))) {}

Session::~Session() {
    stop();
    if (p_->worker.joinable())
        p_->worker.join();
}

void Session::setTransportFactory(TransportFactory factory) {
    if (busy())
        throw std::logic_error("Cannot switch transport during a V2 session");
    if (!factory)
        throw std::invalid_argument("Missing V2 transport factory");
    if (p_->worker.joinable())
        p_->worker.join();
    p_->factory = std::move(factory);
}

void Session::start(const Config &config) {
    if (busy())
        throw std::runtime_error("V2 session is already active or stopping");
    if (p_->worker.joinable())
        p_->worker.join();
    validateConfig(config);
    auto c = config;
    if (c.runId.empty())
        c.runId = "v2-" + std::to_string(rt::monotonicUs());
    auto initial = std::make_shared<State>();
    initial->config = c;
    initial->capacity = preflight(c);
    initial->sources[0].nextId = c.initialNdiId;
    initial->sources[1].nextId = c.initialEpId;
    initial->engine.nextStatusSequence = c.initialStatusSequence;
    initial->engine.effectiveMbps = effectiveMbps(c);
    p_->publishState(std::move(initial));
    {
        std::lock_guard<std::mutex> lock(p_->statusMutex);
        p_->statuses.clear();
        p_->hasStatus = false;
        p_->acceptedStatuses = 0;
        p_->rejectedStatuses = 0;
        p_->cancelledStatuses = 0;
    }
    p_->abortRequested = false;
    p_->producerStop.reset();
    p_->active = true;
    try {
        p_->worker = std::thread([this, c] {
            p_->execute(c);
        });
    } catch (...) {
        p_->active = false;
        throw;
    }
}

void Session::finish() noexcept {
    p_->producerStop.request();
    p_->wake.notify();
}

void Session::stop() noexcept {
    p_->abortRequested.store(true, std::memory_order_release);
    finish();
}

bool Session::busy() const noexcept {
    return p_->active.load(std::memory_order_acquire);
}

bool Session::epHandoffExceeded(std::uint64_t thresholdUs) const {
    std::lock_guard<std::mutex> lock(p_->mutex);
    const auto &state = *p_->state;
    const auto &ep = state.engine.modules[EpModule - 1].readyToFirst;
    return state.config.handoffTrace && ep.count && ep.maxUs > thresholdUs;
}

bool Session::epFragmentSpanReached(std::uint64_t thresholdUs) const {
    std::lock_guard<std::mutex> lock(p_->mutex);
    const auto &state = *p_->state;
    const auto &ep = state.engine.worstFragmentSpan[EpModule - 1];
    return state.config.fragmentTrace && ep && ep->spanUs() >= thresholdUs;
}

RunResult Session::snapshot() const {
    std::shared_ptr<const State> latest;
    {
        std::lock_guard<std::mutex> lock(p_->mutex);
        latest = p_->state;
    }
    const State &s = *latest;
    RunResult r;
    r.reportJson = runner_detail::report(s);
    r.error = s.error;
    r.nextNdiId = s.sources[0].nextId;
    r.nextEpId = s.sources[1].nextId;
    r.nextStatusSequence = s.engine.nextStatusSequence;
    r.success = s.finished && s.error.empty() && s.engine.statusSendErrors == 0;
    for (unsigned i = 0; i < 2; ++i)
        r.success = r.success && !s.sources[i].errors && !s.engine.modules[i].sendErrors;
    return r;
}

RunResult Session::wait() {
    if (p_->worker.joinable())
        p_->worker.join();
    return snapshot();
}

bool Session::postStatus(const Bytes &b, std::uint64_t when) {
    if (!busy() || p_->stopRequested || b.size() > 1444)
        return false;
    if (!when)
        when = rt::monotonicUs();
    {
        std::lock_guard<std::mutex> lock(p_->statusMutex);
        if (p_->stopRequested || p_->statuses.size() >= 64) {
            ++p_->rejectedStatuses;
            return false;
        }
        p_->statuses.emplace_back(b, when);
        ++p_->acceptedStatuses;
        p_->hasStatus.store(true, std::memory_order_release);
    }
    p_->wake.notify();
    return true;
}

namespace {
void publishTrigger(const std::filesystem::path &destination, const std::string &json) {
    // The watcher treats existence as completion. Stage the complete document
    // on the same volume, with an exclusive staging directory per destination.
    auto staging = destination;
    staging += ".writing";
    if (!std::filesystem::create_directory(staging))
        throw std::runtime_error("trigger report staging directory already exists: " +
                                 staging.u8string());
    const auto temp = staging / "report.json";
    std::error_code ec;
    try {
        std::ofstream out(temp, std::ios::binary);
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out << json << '\n';
        out.close();
        if (std::filesystem::exists(destination))
            throw std::runtime_error("trigger report already exists: " + destination.u8string());
        std::filesystem::rename(temp, destination);
    } catch (...) {
        std::filesystem::remove(temp, ec);
        std::filesystem::remove(staging, ec);
        throw;
    }
    std::filesystem::remove(staging, ec);
}
} // namespace

RunResult run(const Config &c, const Session::TransportFactory &factory,
              const std::function<bool()> &cancelled, const std::string &handoffTriggerReport,
              const std::string &fragmentTriggerReport) {
    const auto handoffPath = std::filesystem::u8path(handoffTriggerReport);
    const auto fragmentPath = std::filesystem::u8path(fragmentTriggerReport);
    bool handoffArmed = !handoffPath.empty();
    bool fragmentArmed = !fragmentPath.empty();
    if (handoffArmed && (!c.handoffTrace || std::filesystem::exists(handoffPath)))
        throw std::invalid_argument("handoff trigger requires tracing and a new report path");
    if (fragmentArmed && (!c.fragmentTrace || std::filesystem::exists(fragmentPath)))
        throw std::invalid_argument("fragment trigger requires tracing and a new report path");
    if (handoffArmed && fragmentArmed) {
        const auto first =
            std::filesystem::weakly_canonical(std::filesystem::absolute(handoffPath));
        const auto second =
            std::filesystem::weakly_canonical(std::filesystem::absolute(fragmentPath));
#ifdef _WIN32
        const bool same =
            CompareStringOrdinal(first.c_str(), -1, second.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
        const bool same = first == second;
#endif
        if (same)
            throw std::invalid_argument(
                "handoff and fragment triggers require distinct report paths");
    }
    Session session(factory);
    session.start(c);
    unsigned polls = 0;
    auto captureIfDue = [&] {
        const bool handoff = handoffArmed && session.epHandoffExceeded(6999);
        const bool fragments = fragmentArmed && session.epFragmentSpanReached(18000);
        if (!handoff && !fragments)
            return;
        const auto json = session.snapshot().reportJson;
        if (handoff) {
            publishTrigger(handoffPath, json);
            handoffArmed = false;
        }
        if (fragments) {
            publishTrigger(fragmentPath, json);
            fragmentArmed = false;
        }
    };
    while (session.busy()) {
        if (cancelled && cancelled())
            session.stop();
        if ((handoffArmed || fragmentArmed) && ++polls == 10) {
            polls = 0;
            captureIfDue();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    captureIfDue();
    return session.wait();
}
} // namespace rt::v2
