// Opt-in native Windows GUI memory run. The paired sender and receiver use separate processes.
#include "cycle_memory.h"
#include "model/PreviewPanel.h"
#include "ui/MedicalBackdrop.h"
#include "ui/V2Window.h"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QVTKOpenGLNativeWidget.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkWeakPointer.h>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}

template <class T> T *control(QWidget &window, const char *name = nullptr) {
    auto *widget = window.findChild<T *>(QString::fromLatin1(name));
    check(widget != nullptr, name ? name : "missing widget");
    return widget;
}

QJsonObject reading() {
    const auto memory = cycle_memory::readCurrent();
    const auto process = GetCurrentProcess();
    DWORD handles = 0;
    check(GetProcessHandleCount(process, &handles), "GetProcessHandleCount");
    return {{"workingSetBytes", memory.workingSetBytes},
            {"privateBytes", memory.privateBytes},
            {"handles", static_cast<qint64>(handles)},
            {"gdiObjects", static_cast<qint64>(GetGuiResources(process, GR_GDIOBJECTS))},
            {"userObjects", static_cast<qint64>(GetGuiResources(process, GR_USEROBJECTS))}};
}

QJsonObject peak(cycle_memory::Sampler &sampler) {
    const auto result = sampler.finish();
    return {{"workingSetBytes", result.workingSetBytes},
            {"privateBytes", result.privateBytes},
            {"samples", result.samples}};
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
    check(QApplication::platformName() == "windows", "requires the Windows graphics platform");
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
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    try {
        const auto args = app.arguments();
        const bool includeReceiver =
            args.size() == 7 && args.at(6) == QStringLiteral("--include-receiver-report");
        check(args.size() == 6 || includeReceiver,
              "usage: SimAppMemoryTests sender|receiver|joint port output.json "
              "joint-10s-blocks cycles [--include-receiver-report]");
        const auto role = args.at(1);
        check(!includeReceiver || role == "receiver", "receiver report requires the receiver role");
        const int port = args.at(2).toInt();
        const int blocks = args.at(4).toInt();
        const int cycles = args.at(5).toInt();
        check(role == "sender" || role == "receiver" || role == "joint", "invalid role");
        check(port > 0 && port <= 65535 && blocks > 0 && blocks <= 360 && cycles > 0 &&
                  cycles <= 100,
              "invalid port, duration or cycle count");
        check(!QFileInfo::exists(args.at(3)), "output file already exists");
        app.setOrganizationName("SimTransferTests");
        app.setApplicationName("AppMemory");
        QStandardPaths::setTestModeEnabled(true);
        const auto base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        check(QDir().mkpath(base), "could not create test directory");
        QTemporaryDir work(base + "/run-XXXXXX");
        check(work.isValid(), "could not create private run directory");
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
            rt::V2Window window;
            viewport = control<QVTKOpenGLNativeWidget>(window, "modelViewport");
            renderWindow = vtkGenericOpenGLRenderWindow::SafeDownCast(viewport->renderWindow());
            check(renderWindow != nullptr, "VTK render window missing");
            result = run(window, role, port, blocks, cycles);
            if (includeReceiver)
                result["receiver"] = window.reportForExport()["receiver"].toObject();
        }
        QTest::qWait(1500);
        result["afterWindowDestruction"] =
            QJsonObject{{"memory", reading()},
                        {"qtViewportReleased", viewport.isNull()},
                        {"vtkRenderWindowReleased", renderWindow == nullptr}};
        const auto bytes = QJsonDocument(result).toJson();
        QSaveFile file(args.at(3));
        check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(),
              "cannot save measurements");
        check(viewport.isNull() && renderWindow == nullptr, "graphics window survived teardown");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
