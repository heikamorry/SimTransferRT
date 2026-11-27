#include "model/Model.h"
#include "model/PreviewPanel.h"
#include "protocol/V2PacketCodec.h"
#include "ui/BxPreviewQueue.h"
#include "ui/V2Window.h"
#include "asio_test_socket.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QOpenGLFunctions>
#include <QOpenGLContext>
#include <QOpenGLWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>
#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkDataSet.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkMapper.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkWeakPointer.h>
#include <algorithm>
#include <iostream>
#ifdef Q_OS_WIN
#include "cycle_memory.h"
#include <QLibrary>
#include <dxgi1_4.h>
#include <psapi.h>
#include <windows.h>
#include <wrl/client.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif
using namespace rt::model;

void expect(bool ok, const QString &txt) {
    if (!ok)
        throw std::runtime_error(txt.toUtf8().constData());
}

void testPreviewQueue() {
    rt::BxPreviewQueue queue;
    auto offer = [&](std::uint64_t completed, std::uint32_t id) {
        std::optional<rt::BxPreview> frame{rt::BxPreview{{}, completed, id}};
        queue.offer(frame);
    };
    queue.begin();
    offer(0, 1);
    const auto first = queue.take(10, false, true);
    expect(first && first->frameId == 1, "first preview");
    offer(11, 2);
    expect(!queue.take(12, false, true), "rate-limited preview");
    queue.finish(false);
    expect(!queue.take(13, false, true), "wait for in-flight receive before final preview");
    offer(14, 3);
    const auto final = queue.take(15, true, false);
    expect(final && final->frameId == 3 && !queue.take(16, false, false),
           "final latest BX bypasses preview rate limit and supersedes old network work");
    queue.begin();
    offer(17, 4);
    queue.finish(false);
    queue.localActionStarted();
    offer(18, 5);
    expect(!queue.take(19, false, false), "manual file work suppresses final BX");
    queue.begin();
    offer(20, 6);
    queue.finish(true);
    expect(!queue.take(21, false, false), "stopping protects an existing local file task");
    queue.begin();
    offer(22, 7);
    queue.begin();
    expect(!queue.take(23, false, true), "restart discards stale pending BX");
    queue.close();
    offer(24, 8);
    expect(!queue.take(25, false, false), "closed view rejects late completions");
}

qint64 memory(bool peak) {
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS m{};
    GetProcessMemoryInfo(GetCurrentProcess(), &m, sizeof m);
    return peak ? m.PeakWorkingSetSize : m.WorkingSetSize;
#else
    if (peak) {
        rusage r{};
        getrusage(RUSAGE_SELF, &r);
        return qint64(r.ru_maxrss) * 1024;
    }
    QFile f("/proc/self/statm");
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    return f.readAll().split(' ').value(1).toLongLong() * sysconf(_SC_PAGESIZE);
#endif
}

QJsonArray gpuMemory() {
    QJsonArray adapters;
#ifdef Q_OS_WIN
    using Microsoft::WRL::ComPtr;
    static QLibrary dxgi("dxgi");
    static const auto create =
        reinterpret_cast<decltype(&CreateDXGIFactory1)>(dxgi.resolve("CreateDXGIFactory1"));
    ComPtr<IDXGIFactory1> factory;
    if (!create || FAILED(create(IID_PPV_ARGS(&factory))))
        return adapters;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> item;
        if (factory->EnumAdapters1(i, &item) != S_OK)
            break;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(item->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            continue;
        ComPtr<IDXGIAdapter3> adapter;
        if (FAILED(item.As(&adapter)))
            continue;
        DXGI_QUERY_VIDEO_MEMORY_INFO local{};
        DXGI_QUERY_VIDEO_MEMORY_INFO shared{};
        if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) ||
            FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared)))
            continue;
        adapters.append(
            QJsonObject{{"adapter", QString::fromWCharArray(desc.Description)},
                        {"index", int(i)},
                        {"luid", QString("%1:%2")
                                     .arg(quint32(desc.AdapterLuid.HighPart), 8, 16, QChar('0'))
                                     .arg(desc.AdapterLuid.LowPart, 8, 16, QChar('0'))},
                        {"localBytes", qint64(local.CurrentUsage)},
                        {"nonLocalBytes", qint64(shared.CurrentUsage)}});
    }
#endif
    return adapters;
}

int main(int argc, char **argv) {
    rt::model::PreviewPanel::setDefaultSurfaceFormat();
    QApplication app(argc, argv);
    QTemporaryDir tmp;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tmp.path());
    app.setOrganizationName("SimTransferRTTests");
    app.setApplicationName("ModelGui");
    try {
        QFile style(":/styles/light.qss");
        expect(style.open(QIODevice::ReadOnly), "application stylesheet is available");
        app.setStyleSheet(QString::fromUtf8(style.readAll()));
        auto args = app.arguments();
        const bool networkOnly = args.removeOne("--network-only");
        const bool localOnly = args.removeOne("--local-only");
        expect(!(networkOnly && localOnly), "select one model GUI test mode");
        QString fixturePath = args.value(1);
        if (fixturePath.isEmpty()) {
            QDir executableDir(app.applicationDirPath());
            for (const auto &relative : {"model-fixtures", "../tests/model-fixtures",
                                         "tests/model-fixtures", "../model-fixtures"}) {
                QDir candidate(executableDir.filePath(relative));
                if (candidate.exists("ready.stamp")) {
                    fixturePath = candidate.absolutePath();
                    break;
                }
            }
        }
        if (fixturePath.isEmpty() || !QDir(fixturePath).exists("tools.bx")) {
            std::cerr << "Model test fixtures could not be found.\n"
                         "Usage: SimModelGuiTests [model-fixtures-directory [evidence-directory "
                         "[large-model-file [cycles]]]]\n";
            return 2;
        }
        QDir files(fixturePath);
        QDir evidence(args.value(2, tmp.path()));
        expect(evidence.mkpath("."), "evidence directory");
        rt::V2Window w;
        w.resize(1380, 900);
        w.show();
        auto panel = w.findChild<PreviewPanel *>();
        expect(panel, "panel");
        auto tabs = w.findChild<QTabWidget *>();
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->widget(i)->isAncestorOf(panel))
                tabs->setCurrentIndex(i);
        auto split = w.findChild<QSplitter *>("localModelSplitter");
        auto live = w.findChild<QTableWidget *>("v2NdiToolsTable");
        expect(split && split->orientation() == Qt::Horizontal && split->widget(0) == live &&
                   split->widget(1) == panel,
               "splitter");
        auto viewport = panel->findChild<QOpenGLWidget *>("modelViewport");
        expect(viewport, "native OpenGL viewport");
        QTest::qWait(150);
        auto state = panel->findChild<QLabel *>("modelStatus");
        auto info = panel->findChild<QLabel *>("modelInfo");
        auto until = [&](auto predicate, int ms = 30000) {
            QElapsedTimer timer;
            timer.start();
            while (!predicate() && timer.elapsed() < ms)
                QTest::qWait(8);
            expect(predicate(), "timeout " + state->text());
        };
        auto open = [&](QString path) {
            QSignalSpy spy(panel, &PreviewPanel::fileTaskFinished);
            panel->openFile(path);
            until([&] {
                return !spy.isEmpty();
            });
            expect(spy[0][0].toBool(), state->text());
        };
        auto saveShot = [&](QString name) {
            expect(w.grab().save(evidence.filePath(name)), "screenshot");
        };
        testPreviewQueue();
        auto dark = viewport->grabFramebuffer();
        expect(!dark.isNull(), "OpenGL readback");
#ifdef Q_OS_LINUX
        const auto *context = viewport->context();
        expect(context && context->isValid(), "valid native graphics context");
        if (QSurfaceFormat::defaultFormat().renderableType() == QSurfaceFormat::OpenGLES)
            expect(context->isOpenGLES() && context->format().majorVersion() >= 3,
                   "VTK GLES build requires an actual OpenGL ES 3 context");
#endif
        // A complete multi-frame BX object over V2 must use the same decoder.
        auto bxFrame = [](std::uint32_t id, float x) {
            rt::aurora::Reply reply;
            rt::aurora::Tool tool;
            tool.handle = 0x0A;
            tool.status = rt::aurora::HandleStatus::Valid;
            tool.quaternion = {1, 0, 0, 0};
            tool.positionMm = {x, 0, 0};
            tool.quality = 0;
            tool.portStatus = 0x31;
            tool.frameNumber = id;
            reply.tools = {tool};
            return rt::aurora::encodeBx(reply);
        };
        auto first = bxFrame(8, 1);
        auto second = bxFrame(16, 2);
        auto third = bxFrame(24, 3);
        rt::v2::Frame object;
        object.moduleId = 1;
        object.bytes = first;
        object.bytes.insert(object.bytes.end(), second.begin(), second.end());
        object.bytes.insert(object.bytes.end(), third.begin(), third.end());
        if (!localOnly) {
            test::UdpSocket probe(0, 1);
            const auto localPort = probe.localPort();
            probe.close();
            test::UdpSocket tx(0, localPort);
            auto rx = w.findChild<QPushButton *>("v2Receive");
            auto port = w.findChild<QSpinBox *>("v2ReceiverPort");
            port->setValue(localPort);
            QTest::mouseClick(rx, Qt::LeftButton);
            until(
                [&] {
                    return w.reportForExport()["receiver"].toObject()["receiver_port"].toInt() ==
                           localPort;
                },
                5000);
            rt::v2::Frame liveFrame;
            liveFrame.bytes = rt::aurora::generateSample(2, 77);
            auto sendLiveFrame = [&] {
                ++liveFrame.frameId;
                auto packet = rt::v2::encodeData(liveFrame, 0);
                return tx.send(packet.bytes.data(), packet.size);
            };
            expect(sendLiveFrame(), "send BX test datagram");
            QTimer liveTimer;
            QObject::connect(&liveTimer, &QTimer::timeout, [&] {
                sendLiveFrame();
            });
            liveTimer.setTimerType(Qt::PreciseTimer);
            liveTimer.start(4);
            auto liveToolsIntact = [&] {
                return live->rowCount() == 2 && live->item(0, 0)->text() == "1" &&
                       live->item(1, 0)->text() == "2" && live->item(0, 6)->text() == "77";
            };
            expect(
                QTest::qWaitFor(liveToolsIntact, 5000),
                "loopback BX did not reach the live NDI table (expected tools 1, 2 and frame 77)");
            liveTimer.stop();
            QTest::qWait(150);
            object.frameId = liveFrame.frameId + 1;
            auto objectPacket = rt::v2::encodeData(object, 0);
            QSignalSpy bxObject(panel, &PreviewPanel::bxObjectFinished);
            expect(tx.send(objectPacket.bytes.data(), objectPacket.size), "send BX object");
            until(
                [&] {
                    // The live table and the asynchronous BX preview refresh independently.
                    return !bxObject.isEmpty() &&
                           info->text().contains(QString("NDI 帧 #%1").arg(object.frameId)) &&
                           state->text().contains("3 点") && live->rowCount() == 1 &&
                           live->item(0, 0) && live->item(0, 0)->text() == "10";
                },
                5000);
            // Pause event delivery while the receiver continues. Preview work must
            // coalesce before entering Qt's event queue, not only inside the panel.
            auto *log = w.findChild<QPlainTextEdit *>();
            expect(log, "run log");
            const auto submissionsBefore = log->toPlainText().count("提交 BX 解码");
            for (int i = 0; i < 24; ++i) {
                ++object.frameId;
                const auto packet = rt::v2::encodeData(object, 0);
                expect(tx.send(packet.bytes.data(), packet.size), "send preview burst");
                QThread::msleep(8);
            }
            QThread::msleep(60);
            until([&] {
                return info->text().contains(QString("NDI 帧 #%1").arg(object.frameId)) &&
                       !panel->busy();
            });
            expect(log->toPlainText().count("提交 BX 解码") - submissionsBefore <= 2,
                   "preview burst queued unbounded GUI work");
            QTest::mouseClick(rx, Qt::LeftButton);
            until([&] {
                return port->isEnabled();
            });
            if (networkOnly) {
                std::cout << "PASS network BX preview" << std::endl;
                return 0;
            }
        }
        QFile objectFile(tmp.filePath("network-object.bx"));
        expect(objectFile.open(QIODevice::WriteOnly) &&
                   objectFile.write(reinterpret_cast<const char *>(object.bytes.data()),
                                    qsizetype(object.bytes.size())) ==
                       qsizetype(object.bytes.size()),
               "write received object");
        objectFile.close();
        const QByteArray objectBytes(reinterpret_cast<const char *>(object.bytes.data()),
                                     qsizetype(object.bytes.size()));
        if (localOnly) {
            QSignalSpy previewDone(panel, &PreviewPanel::bxObjectFinished);
            panel->showBxObject(objectBytes, "offline-network-object");
            until([&] {
                return !previewDone.isEmpty();
            });
            expect(previewDone.front().front().toBool(), "present offline BX fixture");
        }
        const auto beforeCancel = info->text();
        QSignalSpy cancelledObject(panel, &PreviewPanel::bxObjectFinished);
        QSignalSpy unrelatedFileTask(panel, &PreviewPanel::fileTaskFinished);
        panel->showBxObject(objectBytes, "cancelled-network-object");
        // Let decoding finish without dispatching its queued presentation.
        QThread::msleep(100);
        panel->cancel();
        until([&] {
            return !cancelledObject.isEmpty();
        });
        expect(!cancelledObject[0][0].toBool() && info->text() == beforeCancel &&
                   unrelatedFileTask.isEmpty(),
               "cancelled network object committed or completed an unrelated file task");
        QSignalSpy invalidObject(panel, &PreviewPanel::bxObjectFinished);
        panel->showBxObject(QByteArray("invalid BX"), "invalid-network-object");
        until([&] {
            return !invalidObject.isEmpty();
        });
        expect(!invalidObject[0][0].toBool() && unrelatedFileTask.isEmpty() &&
                   info->text() == beforeCancel,
               "failed network decoding changed the view or completed a file task");
        // Old worker callbacks may already be in the Qt event queue at restart.
        panel->showBxObject(objectBytes, "retired-network-object");
        panel->beginReceiveSession();
        expect(!panel->busy(), "restart invalidates an in-flight network preview");
        QSignalSpy newSession(panel, &PreviewPanel::bxObjectFinished);
        panel->showBxObject(objectBytes, "current-network-object");
        until([&] {
            return !newSession.isEmpty();
        });
        expect(newSession.size() == 1 && info->text().contains("current-network-object"),
               "previous session presented after restart");
        auto *nativeLiveView = qobject_cast<QVTKOpenGLNativeWidget *>(viewport);
        expect(nativeLiveView, "native live BX viewport");
        auto *liveRenderer = nativeLiveView->renderWindow()->GetRenderers()->GetFirstRenderer();
        auto *liveActor = vtkActor::SafeDownCast(liveRenderer->GetViewProps()->GetItemAsObject(0));
        expect(liveActor && liveActor->GetMapper(), "live BX actor and mapper");
        vtkWeakPointer<vtkActor> previousLiveActor = liveActor;
        vtkWeakPointer<vtkMapper> previousLiveMapper = liveActor->GetMapper();
        liveRenderer->GetActiveCamera()->Dolly(1.2);
        const auto liveCameraDistance = panel->cameraDistance();
        newSession.clear();
        panel->showBxObject(objectBytes, "current-network-object");
        until([&] {
            return !newSession.isEmpty();
        });
        expect(newSession.front().front().toBool() && previousLiveActor && previousLiveMapper &&
                   liveRenderer->GetViewProps()->GetItemAsObject(0) ==
                       previousLiveActor.GetPointer() &&
                   previousLiveActor->GetMapper() == previousLiveMapper.GetPointer(),
               "successive live BX updates retain graphics actors and mappers");
        expect(panel->cameraDistance() == liveCameraDistance,
               "successive live BX updates preserve the user's camera");
        panel->beginReceiveSession();
        expect(!info->text().contains("current-network-object"),
               "clear last session's displayed BX");
        QSignalSpy localOpen(panel, &PreviewPanel::fileTaskFinished);
        panel->openFile(files.filePath("tools.bx"));
        panel->beginReceiveSession();
        until([&] {
            return !localOpen.isEmpty();
        });
        expect(localOpen.front().front().toBool(), "restart preserved the active local file task");
        const auto localInfo = info->text();
        const auto localStatus = state->text();
        const auto localTooltip = state->toolTip();
        panel->showBxObject(objectBytes, "interrupted-network-object");
        panel->beginReceiveSession();
        expect(info->text() == localInfo && state->text() == localStatus &&
                   state->toolTip() == localTooltip && !panel->busy(),
               "restart preserved the local model and its task details");
        until(
            [&] {
                return !panel->busy();
            },
            5000);
        auto table = panel->findChild<QTableWidget *>("localBxTools");
        expect(!table->isVisible() && table->rowCount() == 0 &&
                   live->rowCount() == (localOnly ? 0 : 1),
               "BX isolation");
        expect(info->text().contains("tools.bx") && info->text().contains("1 帧") &&
                   info->text().contains("3 有效位置") && state->text().contains("3 点") &&
                   state->text().contains("点云已显示"),
               "BX point cloud display");
        auto sizeBox = panel->findChild<QSpinBox *>("modelPointSize");
        expect(sizeBox && sizeBox->value() == 8, "point size control");
        auto smallPoints = viewport->grabFramebuffer();
        sizeBox->setValue(12);
        QTest::qWait(30);
        expect(viewport->grabFramebuffer() != smallPoints, "point size applied");
        sizeBox->setValue(8);
        saveShot("local-bx.png");
        until(
            [&] {
                return !panel->busy();
            },
            5000);
        QSignalSpy bxArchiveSaved(panel, &PreviewPanel::fileTaskFinished);
        panel->saveFile(tmp.filePath("tools-archive.h5"), tmp.path());
        until([&] {
            return !bxArchiveSaved.isEmpty();
        });
        expect(bxArchiveSaved[0][0].toBool(), "BX archive write: " + state->text());
        QSignalSpy bxArchiveOpened(panel, &PreviewPanel::fileTaskFinished);
        panel->openFile(tmp.filePath("tools-archive.h5"));
        until([&] {
            return !bxArchiveOpened.isEmpty();
        });
        expect(bxArchiveOpened[0][0].toBool() && info->text().contains("tools-archive.h5") &&
                   info->text().contains("3 有效位置"),
               "BX archive readback: " + state->text());
        QSignalSpy objectOpen(panel, &PreviewPanel::fileTaskFinished);
        panel->openFile(objectFile.fileName());
        until([&] {
            return !objectOpen.isEmpty();
        });
        expect(objectOpen[0][0].toBool() && info->text().contains("本地文件") &&
                   info->text().contains("3 有效位置") && state->text().contains("3 点"),
               "same BX bytes from a local file");
        QSignalSpy changes(panel, &PreviewPanel::fileTaskFinished);
        panel->openFile(files.filePath("tools.bx"));
        panel->openFile(files.filePath("bad-index.ply"));
        panel->openFile(files.filePath("surface.ply"));
        until([&] {
            return !panel->busy();
        });
        expect(info->text().contains("surface.ply") && info->text().contains("metres"),
               "latest generation");
        QTest::qWait(100);
        auto frame = viewport->grabFramebuffer();
        int sky = 0;
        int colored = 0;
        int modelColored = 0;
        const QRect modelArea(frame.width() / 4, frame.height() / 4, frame.width() / 2,
                              frame.height() / 2);
        for (int y = 0; y < frame.height(); y += 3)
            for (int x = 0; x < frame.width(); x += 3) {
                auto c = frame.pixelColor(x, y);
                if (std::abs(c.red() - 183) < 4 && std::abs(c.green() - 213) < 4 &&
                    std::abs(c.blue() - 234) < 4)
                    ++sky;
                else if (c.green() > 25 && c.green() < 190 && std::abs(c.red() - c.blue()) > 15) {
                    ++colored;
                    if (modelArea.contains(x, y))
                        ++modelColored;
                }
            }
        expect(sky > 40 && colored > 5 && modelColored > 5,
               QString("framebuffer sky=%1 color=%2 model=%3 center=%4")
                   .arg(sky).arg(colored).arg(modelColored)
                   .arg(frame.pixelColor(frame.rect().center()).name(QColor::HexArgb)));
        std::cout << "Surface framebuffer samples: sky=" << sky << " color=" << colored
                  << " model=" << modelColored << '\n';
        saveShot("local-surface.png");
        const auto selectors = panel->findChildren<QComboBox *>();
        auto mode = std::find_if(selectors.begin(), selectors.end(), [](auto combo) {
            return combo->findText("线框") >= 0;
        });
        auto attribute = std::find_if(selectors.begin(), selectors.end(), [](auto combo) {
            return combo->findText("原始颜色") >= 0;
        });
        expect(mode != selectors.end() && attribute != selectors.end(), "display selectors");
        (*mode)->setCurrentIndex((*mode)->findText("线框"));
        expect(viewport->grabFramebuffer() != frame, "wireframe");
        (*mode)->setCurrentIndex((*mode)->findText("点云"));
        auto cloudFrame = viewport->grabFramebuffer();
        expect(cloudFrame != frame, "point mode");
        (*mode)->setCurrentIndex((*mode)->findText("表面"));
        (*attribute)->setCurrentIndex(2);
        expect(viewport->grabFramebuffer() != frame, "attribute pseudocolor");
        (*attribute)->setCurrentIndex(0);
        auto center = viewport->rect().center();
        QTest::mousePress(viewport, Qt::LeftButton, {}, center);
        QTest::mouseMove(viewport, center + QPoint(40, 25), 20);
        QTest::mouseRelease(viewport, Qt::LeftButton, {}, center + QPoint(40, 25));
        QTest::qWait(60);
        expect(viewport->grabFramebuffer() != frame, "rotation");
        auto rotated = viewport->grabFramebuffer();
        QTest::mousePress(viewport, Qt::MiddleButton, {}, center);
        QTest::mouseMove(viewport, center + QPoint(22, 16), 20);
        QTest::mouseRelease(viewport, Qt::MiddleButton, {}, center + QPoint(22, 16));
        QTest::qWait(40);
        expect(viewport->grabFramebuffer() != rotated, "pan");
        auto before = panel->cameraDistance();
        QWheelEvent wheel(center, viewport->mapToGlobal(center), {}, QPoint(0, 120), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(viewport, &wheel);
        QTest::qWait(30);
        expect(panel->cameraDistance() != before, "zoom");
        auto buttons = panel->findChildren<QPushButton *>();
        auto reset = std::find_if(buttons.begin(), buttons.end(), [](auto button) {
            return button->text() == "重置视图";
        });
        expect(reset != buttons.end(), "reset button");
        QTest::mouseClick(*reset, Qt::LeftButton);
        expect(viewport->grabFramebuffer() == frame, "reset view");
        QSignalSpy failedOpen(panel, &PreviewPanel::fileTaskFinished);
        panel->openFile(files.filePath("bad-index.ply"));
        until([&] {
            return !failedOpen.isEmpty();
        });
        expect(!failedOpen[0][0].toBool() && state->text().contains("失败") &&
                   info->text().contains("surface.ply"),
               "bad model error and previous view");
        until(
            [&] {
                return !panel->busy();
            },
            5000);
        QSignalSpy exported(panel, &PreviewPanel::fileTaskFinished);
        panel->saveFile(tmp.filePath("surface-export.bx"), tmp.path());
        until([&] {
            return !exported.isEmpty();
        });
        expect(exported[0][0].toBool() &&
                   QFileInfo(tmp.filePath("surface-export.bx")).size() == 4 * 53,
               "model to BX export");
        QSignalSpy exportedOpen(panel, &PreviewPanel::fileTaskFinished);
        panel->openFile(tmp.filePath("surface-export.bx"));
        until([&] {
            return !exportedOpen.isEmpty();
        });
        expect(exportedOpen[0][0].toBool() && info->text().contains("4 点") &&
                   state->text().contains("点云已显示"),
               "exported BX readback");
        auto cancel = [&] {
            QSignalSpy s(panel, &PreviewPanel::fileTaskFinished);
            panel->openFile(files.filePath("cloud.ply"));
            panel->cancel();
            until([&] {
                return !s.isEmpty();
            });
            expect(!s[0][0].toBool(), "load cancellation");
        };
        cancel();
        QSignalSpy saved(panel, &PreviewPanel::fileTaskFinished);
        panel->saveFile(tmp.filePath("cancelled.vtp"), tmp.path());
        panel->cancel();
        until([&] {
            return !saved.isEmpty();
        });
        expect(!saved[0][0].toBool() && !QFileInfo::exists(tmp.filePath("cancelled.vtp")),
               "conversion cancellation");
        viewport->makeCurrent();
        const auto gl = viewport->context()->functions();
        const QString renderer =
            QString::fromLatin1(reinterpret_cast<const char *>(gl->glGetString(GL_RENDERER)));
        const QString vendor =
            QString::fromLatin1(reinterpret_cast<const char *>(gl->glGetString(GL_VENDOR)));
        viewport->doneCurrent();
        QJsonObject result{{"platform", QGuiApplication::platformName()},
                           {"skySamples", sky},
                           {"coloredSamples", colored},
                           {"modelAreaColoredSamples", modelColored},
                           {"GLRenderer", renderer},
                           {"GLVendor", vendor}};
        if (args.size() > 3) {
            QString big = args[3];
            bool validCount = true;
            const int count = args.size() > 4 ? args[4].toInt(&validCount) : 3;
            expect(validCount && count > 0 && count <= 100 && args.size() <= 5,
                   "expected 1 to 100 large-model cycles");
            QElapsedTimer parse;
            parse.start();
            {
                auto source = load(big, {});
                result["parseAndVerifyMs"] = parse.elapsed();
                result["vertices"] = source->points(0);
                result["cells"] = source->cells(0);
                result["sourceSHA256"] = source->sourceHash;
            }
            QJsonArray cycles;
            for (int i = 0; i < count; ++i) {
                panel->unload();
                QTest::qWait(120);
                qint64 idle = memory(false);
                const auto gpuBefore = gpuMemory();
#ifdef Q_OS_WIN
                cycle_memory::Sampler sampler;
#endif
                QTimer beat;
                beat.setInterval(20);
                QElapsedTimer gap;
                QElapsedTimer elapsed;
                gap.start();
                elapsed.start();
                qint64 maxGap = 0;
                int beats = 0;
                QObject::connect(&beat, &QTimer::timeout, [&] {
                    ++beats;
                    maxGap = std::max(maxGap, gap.restart());
                });
                beat.start();
                open(big);
                QTest::qWait(10);
                auto fb = viewport->grabFramebuffer();
                qint64 drawn = elapsed.elapsed();
                expect(!fb.isNull(), "large frame");
                beat.stop();
                maxGap = std::max(maxGap, gap.elapsed());
                if (!i)
                    saveShot("large-model.png");
                QElapsedTimer drag;
                drag.start();
                QTest::mousePress(viewport, Qt::LeftButton, {}, center);
                QTest::mouseMove(viewport, center + QPoint(18, 8), 10);
                QTest::mouseRelease(viewport, Qt::LeftButton, {}, center + QPoint(18, 8));
                auto dragged = viewport->grabFramebuffer();
                auto dragMs = drag.elapsed();
                expect(dragged != fb, "large model rotated");
                fb = {};
                dragged = {};
                QJsonObject row{{"cycle", i + 1},
                                {"beforeBytes", idle},
                                {"displayBytes", memory(false)},
                                {"gpuBefore", gpuBefore},
                                {"gpuDisplay", gpuMemory()},
                                {"openToRenderMs", drawn},
                                {"peakWorkingSetBytes", memory(true)},
                                {"heartbeatEvents", beats},
                                {"maxHeartbeatGapMs", maxGap},
                                {"dragAndReadbackMs", dragMs}};
                if (!i) {
                    for (int tick = 0; tick < 5; ++tick)
                        QApplication::sendEvent(viewport, &wheel);
                    saveShot("large-detail.png");
                    QTest::mouseClick(*reset, Qt::LeftButton);
                }
                {
                    QTemporaryDir outputs(tmp.filePath("cycle-XXXXXX"));
                    expect(outputs.isValid(), "create temporary conversion directory");
                    QElapsedTimer convertTime;
                    convertTime.start();
                    QSignalSpy completed(panel, &PreviewPanel::fileTaskFinished);
                    panel->saveFile(outputs.filePath("gate.vtp"), outputs.path());
                    until(
                        [&] {
                            return !completed.isEmpty();
                        },
                        420000);
                    expect(completed[0][0].toBool(), state->text());
                    expect(QFileInfo(outputs.filePath("gate.vtp")).size() > 0 &&
                               QDir(outputs.path()).entryList({"*.h5"}, QDir::Files).size() == 1,
                           "conversion produced a VTP and one verified HDF5 archive");
                    row["convertVerifyArchiveMs"] = convertTime.elapsed();
                    row["peakAfterConvertBytes"] = memory(true);
                }
                // Observe the displayed mesh without extending its lifetime.
                vtkWeakPointer<vtkActor> meshActor;
                vtkWeakPointer<vtkMapper> meshMapper;
                vtkWeakPointer<vtkDataSet> meshData;
                auto *nativeView = qobject_cast<QVTKOpenGLNativeWidget *>(viewport);
                expect(nativeView != nullptr, "native VTK viewport");
                auto *renderers = nativeView->renderWindow()->GetRenderers();
                vtkCollectionSimpleIterator rendererIt;
                renderers->InitTraversal(rendererIt);
                while (auto *sceneRenderer = renderers->GetNextRenderer(rendererIt)) {
                    auto *props = sceneRenderer->GetViewProps();
                    vtkCollectionSimpleIterator propIt;
                    props->InitTraversal(propIt);
                    while (auto *prop = props->GetNextProp(propIt)) {
                        auto *actor = vtkActor::SafeDownCast(prop);
                        if (!actor || !actor->GetMapper())
                            continue;
                        auto *mapper = actor->GetMapper();
                        auto *data = mapper->GetInput();
                        if (data && data->GetNumberOfPoints() == result["vertices"].toInteger()) {
                            meshActor = actor;
                            meshMapper = mapper;
                            meshData = data;
                            break;
                        }
                    }
                    if (meshActor)
                        break;
                }
                expect(meshData != nullptr, "found the displayed large mesh");
                panel->unload();
                QTest::qWait(1500);
                row["actorReleased"] = !meshActor;
                row["mapperReleased"] = !meshMapper;
                row["dataReleased"] = !meshData;
                row["meshObjectsReleased"] = !meshActor && !meshMapper && !meshData;
                row["afterUnloadBytes"] = memory(false);
#ifdef Q_OS_WIN
                const auto after = cycle_memory::readCurrent();
                row["afterUnloadBytes"] = after.workingSetBytes;
                row["afterUnloadPrivateBytes"] = after.privateBytes;
                const auto peak = sampler.finish();
                row["cyclePeakWorkingSetBytes"] = peak.workingSetBytes;
                row["cyclePeakPrivateBytes"] = peak.privateBytes;
                row["samples"] = static_cast<qint64>(peak.samples);
                row["sampleIntervalMs"] = peak.intervalMs;
#endif
                row["gpuAfterUnload"] = gpuMemory();
                cycles.append(row);
                std::cout << QJsonDocument(row).toJson(QJsonDocument::Compact).constData()
                          << std::endl;
            }
            result["largeModel"] = QFileInfo(big).fileName();
            result["sourceBytes"] = QFileInfo(big).size();
            result["processId"] = QApplication::applicationPid();
            result["settleAfterUnloadMs"] = 1500;
            result["cycles"] = cycles;
        }
        QDir output(evidence);
        QFile record(output.filePath("ui-measurements.json"));
        expect(record.open(QIODevice::WriteOnly), "write metrics");
        record.write(QJsonDocument(result).toJson());
        record.close();
        panel->openFile(files.filePath("surface.ply"));
        w.close();
        std::cout << "PASS native Qt/VTK integration and lifecycle" << std::endl;
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL " << e.what() << std::endl;
        return 1;
    }
}
