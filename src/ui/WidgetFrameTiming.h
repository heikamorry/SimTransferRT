#pragma once
#include <QJsonObject>
#include <atomic>
#include <cstdint>
#include <memory>

namespace rt::ui {
enum class WidgetFrameDrain { FrameEvent, Timer, Explicit };

// The receiver may still be returning from postEvent after the GUI has taken a batch.
struct WidgetWakeTiming {
    WidgetWakeTiming(std::uint64_t sequence, unsigned module, std::uint32_t frameId)
        : sequence(sequence), module(module), frameId(frameId) {}
    const std::uint64_t sequence;
    const unsigned module;
    const std::uint32_t frameId;
    std::atomic<std::uint64_t> postStarted{0}, postReturned{0}, eventEntered{0};
};

// One correlated sample. All times are absolute microseconds on the same host clock.
struct WidgetFrameTiming {
    std::uint32_t frameId{};
    unsigned queueDepth{};
    WidgetFrameDrain drain{};
    std::uint64_t completed{}, enqueued{}, batchTaken{}, applyStarted{}, updated{};
    std::shared_ptr<const WidgetWakeTiming> wake;
    std::uint64_t drainWakeSequence{};
    bool inheritedWake{};

    void retainWorst(const WidgetFrameTiming &candidate) {
        if (!updated || candidate.updated - candidate.completed > updated - completed)
            *this = candidate;
    }

    QJsonValue json() const {
        if (!updated)
            return QJsonValue::Null;
        const char *source = drain == WidgetFrameDrain::FrameEvent ? "frame_event"
                             : drain == WidgetFrameDrain::Timer    ? "timer"
                                                                   : "explicit";
        QJsonObject row{
            {"frame_id", qint64(frameId)}, {"queue_depth_at_enqueue", int(queueDepth)},
            {"drain", QLatin1String(source)}, {"completed_us", qint64(completed)},
            {"enqueued_us", qint64(enqueued)}, {"batch_taken_us", qint64(batchTaken)},
            {"apply_started_us", qint64(applyStarted)}, {"widget_updated_us", qint64(updated)},
            {"complete_to_enqueue_us", qint64(enqueued - completed)},
            {"enqueue_to_batch_us", qint64(batchTaken - enqueued)},
            {"batch_to_apply_us", qint64(applyStarted - batchTaken)},
            {"apply_to_update_us", qint64(updated - applyStarted)},
            {"complete_to_update_us", qint64(updated - completed)}};
        if (wake) {
            auto stamp = [](std::uint64_t value) -> QJsonValue {
                return value ? QJsonValue(qint64(value)) : QJsonValue(QJsonValue::Null);
            };
            const auto eventEntered = wake->eventEntered.load();
            const auto postReturned = wake->postReturned.load();
            row["wake"] = QJsonObject{
                {"sequence", qint64(wake->sequence)}, {"origin_module_id", int(wake->module)},
                {"origin_frame_id", qint64(wake->frameId)}, {"inherited", inheritedWake},
                {"post_started_us", stamp(wake->postStarted.load())},
                {"post_returned_us", stamp(postReturned)}, {"event_entered_us", stamp(eventEntered)},
                {"drain_event_sequence", stamp(drainWakeSequence)},
                {"drained_by_this_event", wake->sequence == drainWakeSequence}};
        }
        return row;
    }
};
} // namespace rt::ui
