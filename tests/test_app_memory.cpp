// Native Qt GUI diagnostics, including a paired sender/receiver soak in one window.
#include "cycle_memory.h"
#include "core/SamplingClock.h"
#include "model/PreviewPanel.h"
#include "ui/MedicalBackdrop.h"
#include "ui/V2Window.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaEnum>
#include <QOpenGLWidget>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QSysInfo>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWindow>
#include <QVTKOpenGLNativeWidget.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkWeakPointer.h>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}

struct PaintTarget {
    QPointer<QWidget> widget;
    QString name, className, namedParent;
};

// One outer UpdateRequest. Only non-overlapping, outermost nested Paint calls enter this list.
struct UpdatePaints {
    static constexpr std::size_t limit = 256;
    struct Span {
        qint64 begin, end;
        int target;
    };
    std::vector<Span> spans;
    qint64 allPaintUs = 0, firstOmittedUs = 0;
    int omitted = 0;

    void reset() {
        spans.clear();
        allPaintUs = firstOmittedUs = 0;
        omitted = 0;
    }

    void add(qint64 begin, qint64 end, int target) {
        allPaintUs += end - begin;
        if (spans.size() < limit)
            spans.push_back({begin, end, target});
        else if (omitted++ == 0)
            firstOmittedUs = begin;
    }

    QJsonObject describe(qint64 left, qint64 right, qint64 outerUs,
                         const std::vector<PaintTarget> &targets) const {
        struct Group {
            int calls = 0;
            qint64 time = 0, longest = 0;
        };
        std::map<int, Group> groups;
        qint64 measured = 0;
        for (const auto &span : spans) {
            const qint64 duration = std::min(span.end, right) - std::max(span.begin, left);
            if (duration <= 0)
                continue;
            auto &group = groups[span.target];
            ++group.calls;
            group.time += duration;
            group.longest = std::max(group.longest, duration);
            measured += duration;
        }
        std::vector<std::pair<int, Group>> ordered(groups.begin(), groups.end());
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
            return a.second.time > b.second.time;
        });
        QJsonArray rows;
        qint64 unlisted = measured;
        for (const auto &[id, group] : ordered) {
            if (rows.size() == 10)
                break;
            const auto &target = targets.at(id);
            rows.append(QJsonObject{{"target_id", id}, {"object_name", target.name},
                                    {"class", target.className}, {"named_parent", target.namedParent},
                                    {"paint_calls", group.calls}, {"overlap_us", group.time},
                                    {"longest_overlap_us", group.longest}});
            unlisted -= group.time;
        }
        const qint64 unknown = omitted ? std::max<qint64>(0, right - std::max(firstOmittedUs, left)) : 0;
        return {{"outer_all_paint_us", allPaintUs}, {"outer_outside_paint_us", outerUs - allPaintUs},
                {"outer_omitted_paints", omitted}, {"overlap_paint_us", measured},
                {"overlap_outside_paint_us", right - left - measured - unknown},
                {"overlap_unpartitioned_after_limit_us", unknown}, {"other_targets_us", unlisted},
                {"top_targets", rows}};
    }
};

// Ranges between the actual model viewport's Qt signals may overlap the Paint ranges.
struct ViewportCompose {
    static constexpr std::size_t limit = 32768;
    std::vector<std::pair<qint64, qint64>> spans;
    qint64 pending = 0, seen = 0, evictedThrough = 0;
    qint64 unmatchedStarts = 0, unmatchedEnds = 0;

    void begin(qint64 stamp) {
        if (pending)
            ++unmatchedStarts;
        pending = stamp;
    }
    void finish(qint64 stamp) {
        if (!pending) {
            ++unmatchedEnds;
            return;
        }
        const std::pair<qint64, qint64> completed{pending, stamp};
        if (spans.size() < limit)
            spans.push_back(completed);
        else {
            auto &old = spans[seen % limit];
            evictedThrough = old.second;
            old = completed;
        }
        ++seen;
        pending = 0;
    }
    QJsonObject describe(qint64 left, qint64 right) const {
        QJsonArray overlaps;
        qint64 total = 0;
        for (qint64 i = seen - qint64(spans.size()); i < seen; ++i) {
            const auto &[begin, end] = spans[i % limit];
            if (begin >= right)
                break;
            const qint64 first = std::max(begin, left), last = std::min(end, right);
            if (first < last) {
                total += last - first;
                overlaps.append(QJsonObject{{"about_to_compose_us", begin}, {"frame_swapped_us", end},
                                            {"overlap_start_us", first}, {"overlap_end_us", last}});
            }
        }
        return {{"overlap_us", total}, {"intervals", overlaps},
                {"outside_retention_us", std::max<qint64>(0, std::min(right, evictedThrough) - left)}};
    }
};

void testUpdateTiming() {
    const std::vector<PaintTarget> targets = {{nullptr, "panel", "QWidget", "root"}};
    UpdatePaints paints;
    paints.add(10, 30, 0);
    paints.add(50, 80, 0);
    const auto paint = paints.describe(20, 70, 100, targets);
    const auto target = paint["top_targets"].toArray().at(0).toObject();
    check(paint["outer_all_paint_us"].toInteger() == 50 && paint["overlap_paint_us"].toInteger() == 30 &&
              paint["overlap_outside_paint_us"].toInteger() == 20 &&
              target["longest_overlap_us"].toInteger() == 20 && target["paint_calls"].toInt() == 2,
          "Nested Paint interval clipping or aggregation failed");
    paints.reset();
    for (int i = 0; i < int(UpdatePaints::limit) + 2; ++i)
        paints.add(2 * i + 1, 2 * i + 2, 0);
    const auto overflow = paints.describe(510, 516, 516, targets);
    check(overflow["overlap_paint_us"].toInteger() == 1 &&
              overflow["overlap_outside_paint_us"].toInteger() == 2 &&
              overflow["overlap_unpartitioned_after_limit_us"].toInteger() == 3 &&
              overflow["outer_omitted_paints"].toInteger() == 2,
          "Truncated Paint intervals must remain distinguishable from unpainted time");
    ViewportCompose compose;
    compose.finish(1);
    compose.begin(12);
    compose.begin(18);
    compose.finish(65);
    const auto group = compose.describe(20, 70);
    const auto span = group["intervals"].toArray().at(0).toObject();
    check(group["overlap_us"].toInteger() == 45 && span["about_to_compose_us"].toInteger() == 18 &&
              span["overlap_start_us"].toInteger() == 20 && span["frame_swapped_us"].toInteger() == 65 &&
              compose.unmatchedStarts == 1 && compose.unmatchedEnds == 1,
          "Independent Qt composition timestamps must survive clipping");
}

// Instrument only the diagnostic GUI driver. A top-level notify may itself dispatch events;
// nested delivery is included in its enclosing interval, so overlaps never double count.
class SoakApplication final : public QApplication {
  public:
    using QApplication::QApplication;

    void beginEventTrace(QMainWindow &window, QOpenGLWidget &viewport) {
        records_.reserve(capacity);
        paints_.spans.reserve(UpdatePaints::limit);
        window_ = &window;
        compose_.spans.reserve(ViewportCompose::limit);
        viewportName_ = viewport.objectName();
        connect(&viewport, &QOpenGLWidget::aboutToCompose, this, [this] {
            if (enabled_.load(std::memory_order_relaxed) && std::this_thread::get_id() == guiTid_)
                compose_.begin(rt::monotonicUs());
        }, Qt::DirectConnection);
        connect(&viewport, &QOpenGLWidget::frameSwapped, this, [this] {
            if (enabled_.load(std::memory_order_relaxed) && std::this_thread::get_id() == guiTid_)
                compose_.finish(rt::monotonicUs());
        }, Qt::DirectConnection);
        // Resolve names once, before the measurement. Index zero covers unnamed/new targets.
        paintTargets_.push_back({nullptr, {}, {}, {}});
        auto widgets = window.findChildren<QWidget *>();
        widgets.prepend(&window);
        for (auto *widget : widgets) {
            const auto name = widget->objectName();
            if (name.isEmpty() || paintTargets_.size() >= 1024)
                continue;
            auto *parent = widget->parentWidget();
            while (parent && parent->objectName().isEmpty())
                parent = parent->parentWidget();
            paintIds_.insert(widget, static_cast<int>(paintTargets_.size()));
            paintTargets_.push_back({widget, name, QString::fromLatin1(widget->metaObject()->className()),
                                     parent ? parent->objectName() : QString{}});
        }
        begin_ = rt::monotonicUs();
        enabled_.store(true, std::memory_order_relaxed);
    }

    QJsonObject endEventTrace(const QJsonObject &joint) {
        enabled_.store(false, std::memory_order_relaxed);
        const qint64 end = rt::monotonicUs();
        const auto rows =
            joint["receiver"].toObject()["ui_observations"].toObject()["modules"].toArray();
        QJsonArray windows;
        for (const auto &row : rows) {
            const auto module = row.toObject();
            windows.append(QJsonObject{
                {"module_id", module["module_id"]},
                {"queue_window", describeWindow(module["worst_frame"].toObject(), end)}});
        }
        return {{"threshold_exclusive_us", thresholdUs}, {"capacity", capacity},
                {"started_us", begin_}, {"ended_us", end}, {"long_events_seen", seen_},
                {"overwritten_events", seen_ - static_cast<qint64>(records_.size())},
                {"scope", "Elapsed time inside top-level GUI notify, including nested calls and "
                          "thread preemption. Gaps can include short callbacks, native work or idle time."},
                {"paint_scope", "Widget paints nested in an UpdateRequest on the soak main window. "
                                "Each outermost paint includes its nested delivery. Times are wall time. "
                                "Target 0 represents an unregistered or unnamed target. Target rows use "
                                "only the overlap with the retained frame's queue interval."},
                {"paint_span_capacity_per_update", int(UpdatePaints::limit)},
                {"compose_scope", "Qt aboutToCompose to frameSwapped for the model viewport. "
                                  "These wall intervals may overlap Paint; do not sum them."},
                {"compose_target", viewportName_}, {"compose_pairs", compose_.seen},
                {"compose_unmatched_starts", compose_.unmatchedStarts + (compose_.pending ? 1 : 0)},
                {"compose_unmatched_ends", compose_.unmatchedEnds},
                {"compose_pair_capacity", int(ViewportCompose::limit)},
                {"modules", windows}};
    }

  protected:
    bool notify(QObject *receiver, QEvent *event) override {
        if (!enabled_.load(std::memory_order_relaxed) || std::this_thread::get_id() != guiTid_ ||
            !receiver || !event)
            return QApplication::notify(receiver, event);
        if (inNotify_) {
            if (!inUpdate_ || inPaint_ || event->type() != QEvent::Paint || !receiver->isWidgetType())
                return QApplication::notify(receiver, event);
            QScopedValueRollback<bool> inner(inPaint_, true);
            const int target = paintTarget(static_cast<QWidget *>(receiver));
            const auto begin = rt::monotonicUs();
            const bool delivered = QApplication::notify(receiver, event);
            paints_.add(begin, rt::monotonicUs(), target);
            return delivered;
        }
        QScopedValueRollback<bool> guard(inNotify_, true);
        Record record;
        record.type = static_cast<int>(event->type());
        record.mainUpdate = receiver == window_ && event->type() == QEvent::UpdateRequest;
        QScopedValueRollback<bool> scope(inUpdate_, record.mainUpdate);
        if (record.mainUpdate)
            paints_.reset();
        // notify() may destroy either object. Copy the class name and type before delivery.
        qstrncpy(record.target, receiver->metaObject()->className(), sizeof(record.target));
        record.start = rt::monotonicUs();
        const bool delivered = QApplication::notify(receiver, event);
        record.end = rt::monotonicUs();
        if (record.end - record.start > thresholdUs) {
            if (record.mainUpdate)
                record.paints = paints_;
            if (records_.size() < capacity) {
                records_.push_back(std::move(record));
            } else {
                auto &old = records_[seen_ % capacity];
                evictedThrough_ = old.end;
                old = std::move(record);
            }
            ++seen_;
        }
        return delivered;
    }

  private:
    static constexpr int thresholdUs = 2000;
    static constexpr int capacity = 4096;
    struct Record {
        qint64 start, end;
        int type;
        char target[96]{};
        bool mainUpdate = false;
        UpdatePaints paints;
    };

    int paintTarget(QWidget *widget) const {
        for (; widget; widget = widget->parentWidget()) {
            auto it = paintIds_.constFind(widget);
            if (it != paintIds_.cend() && paintTargets_[it.value()].widget.data() == widget)
                return it.value();
            if (!widget->objectName().isEmpty())
                break;
        }
        return 0;
    }

    QJsonValue describeWindow(const QJsonObject &frame, qint64 traceEnd) const {
        if (frame.isEmpty())
            return QJsonValue::Null;
        const qint64 left = frame["enqueued_us"].toInteger();
        const qint64 right = frame["batch_taken_us"].toInteger();
        check(begin_ <= left && left <= right && right <= traceEnd, "GUI queue window outside trace");
        qint64 cursor = left;
        qint64 covered = 0;
        QJsonArray events, gaps;
        const auto types = QMetaEnum::fromType<QEvent::Type>();
        auto gapUntil = [&](qint64 next, const char *reason) {
            if (next > cursor) {
                gaps.append(QJsonObject{{"start_us", cursor}, {"end_us", next},
                                        {"reason", QString::fromLatin1(reason)}});
                cursor = next;
            }
        };
        gapUntil(std::min(right, evictedThrough_), "retention_limit");
        for (qint64 i = seen_ - static_cast<qint64>(records_.size()); i < seen_; ++i) {
            const auto &r = records_[i % capacity];
            const qint64 start = std::max(left, r.start);
            const qint64 end = std::min(right, r.end);
            if (start >= end)
                continue;
            gapUntil(start, "no_long_notify_observed");
            const auto *name = types.valueToKey(r.type);
            QJsonObject item{{"start_us", r.start}, {"end_us", r.end}, {"event_type", r.type},
                             {"event_name", name ? QJsonValue(QString::fromLatin1(name)) : QJsonValue(QJsonValue::Null)},
                             {"target_class", QString::fromLatin1(r.target)}, {"overlap_us", end - start}};
            if (r.mainUpdate) {
                item["widget_paints"] = r.paints.describe(start, end, r.end - r.start, paintTargets_);
                item["viewport_compose"] = compose_.describe(start, end);
            }
            events.append(item);
            cursor = end;
            covered += end - start;
        }
        gapUntil(right, "no_long_notify_observed");
        return QJsonObject{{"frame_id", frame["frame_id"]}, {"start_us", left}, {"end_us", right},
                           {"covered_us", covered}, {"uncovered_us", right - left - covered},
                           {"events", events}, {"gaps", gaps}};
    }

    const std::thread::id guiTid_ = std::this_thread::get_id();
    std::atomic<bool> enabled_{false};
    bool inNotify_ = false;
    bool inUpdate_ = false, inPaint_ = false;
    QMainWindow *window_ = nullptr;
    UpdatePaints paints_;
    QString viewportName_;
    ViewportCompose compose_;
    QHash<const QWidget *, int> paintIds_;
    std::vector<PaintTarget> paintTargets_;
    qint64 begin_ = 0, seen_ = 0, evictedThrough_ = 0;
    std::vector<Record> records_;
};

template <class T> T *control(QWidget &window, const char *name = nullptr) {
    auto *widget = window.findChild<T *>(QString::fromLatin1(name));
    check(widget != nullptr, name ? name : "missing widget");
    return widget;
}

QJsonObject reading() {
    const auto memory = cycle_memory::readCurrent();
#ifdef _WIN32
    const auto process = GetCurrentProcess();
    DWORD handles = 0;
    check(GetProcessHandleCount(process, &handles), "GetProcessHandleCount");
    return {{"workingSetBytes", memory.workingSetBytes},
            {"privateBytes", memory.privateBytes},
            {"handles", static_cast<qint64>(handles)},
            {"gdiObjects", static_cast<qint64>(GetGuiResources(process, GR_GDIOBJECTS))},
            {"userObjects", static_cast<qint64>(GetGuiResources(process, GR_USEROBJECTS))}};
#else
    return {{"rssBytes", qint64(memory.workingSetBytes)}, {"anonymousRssBytes", qint64(memory.privateBytes)}};
#endif
}

QJsonObject peak(cycle_memory::Sampler &sampler) {
    const auto result = sampler.finish();
#ifdef _WIN32
    return {{"workingSetBytes", result.workingSetBytes},
            {"privateBytes", result.privateBytes},
            {"samples", result.samples}};
#else
    return {{"rssBytes", qint64(result.workingSetBytes)}, {"anonymousRssBytes", qint64(result.privateBytes)},
            {"samples", qint64(result.samples)}};
#endif
}

class WindowRun : public QObject {
  public:
    WindowRun(rt::V2Window &window, int port)
        : window_(window), port_(port), tabs_(control<QTabWidget>(window)),
          start_(control<QPushButton>(window, "v2Start")),
          receive_(control<QPushButton>(window, "v2Receive")),
          rxPort_(control<QSpinBox>(window, "v2ReceiverPort")) {
        control<QSpinBox>(window, "v2TargetPort")->setValue(port);
        control<QSpinBox>(window, "v2CommandPort")->setValue(0);
        rxPort_->setValue(port);
        auto *panel = control<rt::model::PreviewPanel>(window, "localModelPanel");
        QObject::connect(panel, &rt::model::PreviewPanel::bxObjectFinished, this,
                         [this](bool ok, const QString &) {
                             if (ok)
                                 ++previews_;
                         });
    }

    void startReceiver() {
        tabs_->setCurrentIndex(1);
        receive_->click();
        check(QTest::qWaitFor(
                  [&] {
                      const auto rx = window_.reportForExport()["receiver"].toObject();
                      return rx["receiver_port"].toInt() == port_ && !rx["final"].toBool(true);
                  },
                  5000),
              "receiver did not bind");
    }

    void startSender(bool joint) {
        tabs_->setCurrentIndex(0);
        start_->click();
        check(!start_->isEnabled(), "sender failed to start");
        if (joint)
            tabs_->setCurrentIndex(1);
    }

    void waitForTraffic() const {
        check(QTest::qWaitFor(
                  [&] {
                      return previews_ > 0;
                  },
                  45000),
              "no live BX preview from the peer");
    }

    QJsonArray observe(int intervals, int intervalMs, bool expectPreviews) {
        QJsonArray rows;
        for (int i = 0; i < intervals; ++i) {
            const auto before = previews_;
            cycle_memory::Sampler sampler;
            QTest::qWait(intervalMs);
            const auto peaks = peak(sampler);
            const auto previews = previews_ - before;
            check(!expectPreviews || previews > 0, "live BX preview stalled");
            rows.append(QJsonObject{{"peak", peaks}, {"end", reading()}, {"bxPreviews", previews}});
        }
        return rows;
    }

    QJsonObject stop(bool sender, bool receiver) {
        if (sender) {
            control<QPushButton>(window_, "v2Stop")->click();
            check(QTest::qWaitFor(
                      [&] {
                          return start_->isEnabled();
                      },
                      10000),
                  "sender did not join");
        }
        if (receiver) {
            receive_->click();
            check(QTest::qWaitFor(
                      [&] {
                          return rxPort_->isEnabled();
                      },
                      10000),
                  "receiver did not join");
        }
        QTest::qWait(2500);
        const auto after = reading();
        const auto report = window_.reportForExport();
        QJsonObject result{{"afterStop", after}};
        if (sender) {
            const auto tx = report["sender"].toObject();
            check(tx["finished"].toBool() && tx["error"].toString().isEmpty(), "sender failed");
            const auto archives =
                QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                "/v2-runs/";
            check(QFileInfo::exists(archives + tx["run_id"].toString() + ".json"),
                  "sender report was not archived");
            result["sent"] = counts(tx, "initial_complete");
        }
        if (receiver) {
            const auto rx = report["receiver"].toObject();
            check(rx["final"].toBool(), "receiver did not publish final counters");
            result["received"] = counts(rx, "consumed");
        }
        return result;
    }

    void visitReport() {
        tabs_->setCurrentIndex(2);
        QTest::qWait(500);
        tabs_->setCurrentIndex(1);
    }

  private:
    static QJsonObject counts(const QJsonObject &report, const QString &key) {
        const auto modules = report["modules"].toObject();
        const auto ndi = modules["1"].toObject()[key].toInteger();
        const auto ep = modules["2"].toObject()[key].toInteger();
        check(ndi > 0 && ep > 0, "one or both modules did not deliver frames");
        return {{"ndi", ndi}, {"ep", ep}};
    }

    rt::V2Window &window_;
    int port_;
    QTabWidget *tabs_;
    QPushButton *start_;
    QPushButton *receive_;
    QSpinBox *rxPort_;
    int previews_ = 0;
};

struct SoakWindowResidency {
    qint64 started = 0, last = 0;
    int page = 0, visibility = 0;
    int pageChanges = 0, visibilityChanges = 0;
    qint64 residency[3][3]{};

    void observe(qint64 now, int newPage, bool visible, bool minimized) {
        check(newPage >= 0 && newPage < 3, "unexpected soak tab index");
        const int state = !visible ? 2 : minimized ? 1 : 0;
        if (last) {
            residency[page][visibility] += now - last;
            pageChanges += newPage != page;
            visibilityChanges += state != visibility;
        } else {
            started = now;
        }
        page = newPage;
        visibility = state;
        last = now;
    }

    QJsonObject json() const {
        QJsonObject tabs;
        const char *names[] = {"sender", "receiver", "report"};
        for (int i = 0; i < 3; ++i) {
            const auto &row = residency[i];
            tabs[names[i]] = QJsonObject{{"visible_us", row[0]}, {"minimized_us", row[1]},
                                         {"hidden_us", row[2]}, {"total_us", row[0] + row[1] + row[2]}};
        }
        return {{"start_us", started}, {"end_us", last}, {"total_us", last - started},
                {"tab_changes", pageChanges}, {"visibility_changes", visibilityChanges}, {"tabs", tabs},
                {"scope", "Before sender start until the GUI detects sender completion. "
                          "Observed using tab/window signals and the 100 ms monitor. "
                          "Visible means shown and not minimized; occlusion is not measured."}};
    }
};

void testWindowResidency() {
    SoakWindowResidency sample;
    sample.observe(100, 0, true, false);
    sample.observe(120, 1, true, false);
    sample.observe(150, 1, true, true);
    sample.observe(190, 2, false, true);
    sample.observe(200, 2, false, false);
    const auto report = sample.json();
    const auto tabs = report["tabs"].toObject();
    check(report["total_us"].toInteger() == 100 && report["tab_changes"].toInt() == 2 &&
              report["visibility_changes"].toInt() == 2 &&
              tabs["sender"].toObject()["visible_us"].toInteger() == 20 &&
              tabs["receiver"].toObject()["visible_us"].toInteger() == 30 &&
              tabs["receiver"].toObject()["minimized_us"].toInteger() == 40 &&
              tabs["report"].toObject()["hidden_us"].toInteger() == 10,
          "window residency must account for every interval exactly once");
}

// Keep the normal Qt event loop dispatching for the complete measured interval.
QJsonObject runLoopback(rt::V2Window &window, int port, int seconds) {
    WindowRun driver(window, port);
    control<QLineEdit>(window, "v2Target")->setText("127.0.0.1");
    control<QLineEdit>(window, "v2Bind")->setText("127.0.0.1");
    control<QLineEdit>(window, "v2ReceiverBind")->setText("127.0.0.1");
    control<QSpinBox>(window, "v2Duration")->setValue(seconds);
    const auto cfg = window.selectedConfig();
    check(cfg.ndiEnabled && cfg.epEnabled && cfg.ndiHz == 60 && cfg.epHz == 50 &&
              cfg.epBytes == 204800 && cfg.linkMbps == 1000,
          "unexpected loopback test configuration");
    window.show();
    driver.startReceiver();
    auto *tabs = control<QTabWidget>(window);
    SoakWindowResidency residency;
    auto sampleWindow = [&] {
        residency.observe(rt::monotonicUs(), tabs->currentIndex(), window.isVisible(), window.isMinimized());
    };
    QObject observer;
    const auto pageChange = QObject::connect(tabs, &QTabWidget::currentChanged, &observer, sampleWindow);
    const auto visibilityChange = QObject::connect(window.windowHandle(), &QWindow::visibilityChanged,
                                                   &observer, sampleWindow);
    sampleWindow();
    driver.startSender(true);
    auto *start = control<QPushButton>(window, "v2Start");
    QEventLoop loop;
    QTimer monitor;
    QElapsedTimer elapsed;
    elapsed.start();
    QObject::connect(&monitor, &QTimer::timeout, &loop, [&] {
        sampleWindow();
        if (start->isEnabled() || elapsed.elapsed() >= (seconds + 10) * 1000)
            loop.quit();
    });
    monitor.start(100);
    loop.exec();
    sampleWindow();
    QObject::disconnect(pageChange);
    QObject::disconnect(visibilityChange);
    check(start->isEnabled(), "sender exceeded duration and drain deadline");
    // Export after receiver grace, including during a failed/zero-delivery run.
    QTimer::singleShot(300, &loop, &QEventLoop::quit);
    monitor.stop();
    loop.exec();
    control<QPushButton>(window, "v2Receive")->click();
    check(QTest::qWaitFor(
              [&] {
                  return control<QSpinBox>(window, "v2ReceiverPort")->isEnabled();
              },
              5000),
          "receiver did not stop");
    auto report = window.reportForExport();
    report["test_duration_seconds"] = seconds;
    report["test_qt_platform"] = QApplication::platformName();
    report["test_cpu_architecture"] = QSysInfo::currentCpuArchitecture();
    report["test_window_residency"] = residency.json();
    const auto tx = report["sender"].toObject();
    bool passed = tx["finished"].toBool() && tx["error"].toString().isEmpty() &&
                  tx["elapsed_us"].toInteger() >= qint64(seconds) * 1000000 &&
                  report["joint_scope"].toObject()["eligible"].toBool();
    auto rows = report["acceptance"].toArray();
    for (const auto &v : rows)
        if (v.toObject()["status"].toString() == "fail")
            passed = false;
    auto rowPassed = [&](int module, const char *id) {
        for (const auto &v : rows) {
            const auto row = v.toObject();
            if (row["module_id"].toInt() == module && row["id"].toString() == id)
                return row["status"].toString() == "pass";
        }
        return false;
    };
    passed = passed && rowPassed(0, "capacity") && rowPassed(0, "global_packet_gap_deviation");
    for (const auto &key : {"1", "2"}) {
        for (const auto id :
             {"sampling_integrity", "sampling_jitter", "initial_submission", "packet_gap_jitter",
              "receiver_consume", "gui_local_joint_delivery", "gui_widget_updates"})
            passed = passed && rowPassed(key[0] - '0', id);
        const auto m = tx["modules"].toObject()[key].toObject();
        const auto rx = report["receiver"].toObject()["modules"].toObject()[key].toObject();
        const auto expected = qint64(seconds) * (key[0] == '1' ? cfg.ndiHz : cfg.epHz);
        passed = passed && m["built"].toInteger() == expected &&
                 m["initial_complete"].toInteger() == expected &&
                 rx["consumed"].toInteger() == expected;
    }
    report["loopback_passed"] = passed;
    return report;
}

QJsonObject run(rt::V2Window &window, const QString &role, int port, int jointBlocks, int cycles) {
    WindowRun driver(window, port);
    window.show();
    QTest::qWait(3000);
    QJsonObject out{{"role", role},
                    {"pid", QApplication::applicationPid()},
                    {"port", port},
                    {"qt", qVersion()},
                    {"platform", QApplication::platformName()},
                    {"startedUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                    {"home", reading()},
                    {"targetSampleIntervalMs", cycle_memory::Peaks::intervalMs}};
    const auto backend = QApplication::platformName();
    check(backend == "windows" || backend == "xcb" || backend.startsWith("wayland"),
          "requires a native desktop graphics platform");
    const auto config = window.selectedConfig();
    out["load"] = QJsonObject{{"linkMbps", static_cast<int>(config.linkMbps)},
                              {"ndiHz", static_cast<int>(config.ndiHz)},
                              {"ndiTools", static_cast<int>(config.ndiTools)},
                              {"epHz", static_cast<int>(config.epHz)},
                              {"epBytes", static_cast<int>(config.epBytes)}};
    const bool sender = role != "receiver";
    const bool receiver = role != "sender";
    const bool joint = sender && receiver;
    if (receiver) {
        driver.startReceiver();
        out["listening"] = driver.observe(1, 3000, false);
        if (!joint) {
            std::cout << "receiver-ready" << std::endl;
            driver.waitForTraffic();
        }
    }
    if (sender)
        driver.startSender(joint);
    const int intervalMs = joint ? 10000 : 5000;
    out["activeIntervalMs"] = intervalMs;
    out["active"] = driver.observe(joint ? jointBlocks : (sender ? 5 : 4), intervalMs, receiver);
    // Keep the peer listening until the independent 25-second sender has finished.
    if (!sender)
        QTest::qWait(8000);
    out["firstStop"] = driver.stop(sender, receiver);
    if (joint) {
        driver.visitReport();
        QJsonArray repeated;
        for (int i = 0; i < cycles; ++i) {
            cycle_memory::Sampler sampler;
            driver.startReceiver();
            driver.startSender(true);
            const auto active = driver.observe(1, 4000, true);
            auto row = driver.stop(true, true);
            row["cycle"] = i + 1;
            row["active"] = active.at(0);
            row["cyclePeak"] = peak(sampler);
            repeated.append(row);
        }
        out["cycles"] = repeated;
        out["postStopIdle"] = driver.observe(3, 10000, false);
        auto *panel = control<rt::model::PreviewPanel>(window, "localModelPanel");
        panel->unload();
        out["afterPreviewUnload"] = driver.observe(1, 3000, false).at(0);
    }
    window.close();
    return out;
}
} // namespace

int main(int argc, char **argv) {
    rt::model::PreviewPanel::setDefaultSurfaceFormat();
    SoakApplication app(argc, argv);
    try {
        if (app.arguments().mid(1) == QStringList{QStringLiteral("--self-test-update-timing")}) {
            testUpdateTiming();
            testWindowResidency();
            std::cout << "Nested GUI Paint and composition timing tests passed\n";
            return 0;
        }
        QCommandLineParser parser;
        parser.addOptions({
            {QStringLiteral("include-receiver-report"), "Export the receiver report."},
            {QStringLiteral("ndi-handoff-guard-us"), "NDI handoff guard in microseconds.", "us"},
            {QStringLiteral("fragment-trace"), "Record sender fragment timing."},
            {QStringLiteral("asio-pooled-handler"), "Use the Asio handler storage pool."},
        });
        if (!parser.parse(app.arguments()))
            throw std::runtime_error(parser.errorText().toStdString());
        const auto args = parser.positionalArguments();
        const bool includeReceiver = parser.isSet("include-receiver-report");
        const bool withGuard = parser.isSet("ndi-handoff-guard-us");
        rt::V2WindowRunOptions options;
        options.fragmentTrace = parser.isSet("fragment-trace");
        options.asioPooledHandler = parser.isSet("asio-pooled-handler");
        check(args.size() == 5,
              "usage: SimAppMemoryTests sender|receiver|joint port output.json "
              "joint-10s-blocks cycles [--include-receiver-report], or soak port output.json "
              "seconds 1 [--ndi-handoff-guard-us 0..400] [--fragment-trace] [--asio-pooled-handler]");
        const auto role = args.at(0);
        check(!includeReceiver || role == "receiver", "receiver report requires the receiver role");
        const int port = args.at(1).toInt();
        const int blocks = args.at(3).toInt();
        const int cycles = args.at(4).toInt();
        const bool soak = role == "soak";
        check(soak || !(withGuard || options.fragmentTrace || options.asioPooledHandler),
              "sender timing options require the soak role");
        check(soak || role == "sender" || role == "receiver" || role == "joint", "invalid role");
        check(port > 0 && port <= 65535 && blocks > 0 && blocks <= (soak ? 3600 : 360) &&
                  cycles > 0 && cycles <= 100,
              "invalid port, duration or cycle count");
        check(!QFileInfo::exists(args.at(2)), "output file already exists");
        app.setOrganizationName("SimTransferTests");
        app.setApplicationName("AppMemory");
        QStandardPaths::setTestModeEnabled(true);
        QTemporaryDir work(QDir::tempPath() + "/simtransfer-run-XXXXXX");
        check(work.isValid(), "could not create private run directory");
        options.archiveDirectory = work.filePath("v2-runs");
        app.setApplicationName("AppMemory/" + QDir(work.path()).dirName());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, work.path());
        app.setWindowIcon(rt::ui::appIcon());
        QFile style(":/styles/light.qss");
        check(style.open(QIODevice::ReadOnly), "application stylesheet missing");
        app.setStyleSheet(QString::fromUtf8(style.readAll()));
        QJsonObject result;
        QPointer<QVTKOpenGLNativeWidget> viewport;
        vtkWeakPointer<vtkGenericOpenGLRenderWindow> renderWindow;
        {
            rt::V2Window window(nullptr, options);
            if (withGuard) {
                bool ok = false;
                const int guard = parser.value("ndi-handoff-guard-us").toInt(&ok);
                auto *controlGuard = control<QSpinBox>(window, "v2NdiHandoffGuard");
                check(ok && guard >= controlGuard->minimum() && guard <= controlGuard->maximum(),
                      "NDI handoff guard is outside the supported range");
                controlGuard->setValue(guard);
            }
            viewport = control<QVTKOpenGLNativeWidget>(window, "modelViewport");
            renderWindow = vtkGenericOpenGLRenderWindow::SafeDownCast(viewport->renderWindow());
            check(renderWindow != nullptr, "VTK render window missing");
            if (soak && options.fragmentTrace)
                app.beginEventTrace(window, *viewport);
            result =
                soak ? runLoopback(window, port, blocks) : run(window, role, port, blocks, cycles);
            if (soak && options.fragmentTrace)
                result["gui_event_trace"] = app.endEventTrace(result);
            if (includeReceiver)
                result["receiver"] = window.reportForExport()["receiver"].toObject();
        }
        QTest::qWait(1500);
        result["afterWindowDestruction"] =
            QJsonObject{{"memory", reading()},
                        {"qtViewportReleased", viewport.isNull()},
                        {"vtkRenderWindowReleased", renderWindow == nullptr}};
        const auto bytes = QJsonDocument(result).toJson();
        QSaveFile file(args.at(2));
        check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(),
              "cannot save measurements");
        check(viewport.isNull() && renderWindow == nullptr, "graphics window survived teardown");
        return soak && !result["loopback_passed"].toBool() ? 2 : 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
