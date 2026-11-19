#include "ui/MainWindow.h"
#include "ui/V2Window.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QImageWriter>
#include <QPixmap>
#include <QSaveFile>
#include <QTimer>
#include <cstdio>
#ifdef SIM_MODEL_PREVIEW
#include "model/PreviewPanel.h"
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QVTKOpenGLNativeWidget.h>
#endif

namespace {
int reportError(const QString& message, int code) {
    std::fprintf(stderr, "%s\n", message.toUtf8().constData());
    return code;
}

int saveScreenshot(QWidget& window, const QString& path) {
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix.isEmpty())
        return reportError(path + " — 截图路径需要 .png、.jpg 或 .bmp 扩展名", 2);
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly))
        return reportError(path + " — " + output.errorString(), 1);
    QImageWriter writer(&output, suffix.toLatin1());
    if (!writer.write(window.grab().toImage()))
        return reportError(path + " — " + writer.errorString(), 1);
    if (!output.commit())
        return reportError(path + " — " + output.errorString(), 1);
    return 0;
}
bool samePath(const QString& left, const QString& right) {
    // QFileInfo::operator== compares file-system identity, which is not what a
    // "do not overwrite the model" guard needs. Compare normalized absolute paths.
    return QFileInfo(left).absoluteFilePath().compare(QFileInfo(right).absoluteFilePath(),
                                                      Qt::CaseInsensitive) == 0;
}
}

int main(int argc,char**argv) {
#ifdef SIM_MODEL_PREVIEW
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
#endif
    QApplication app(argc,argv);
    app.setOrganizationName("SimTransferLab");
    app.setApplicationName("SimTransferRT");
    app.setWindowIcon(QIcon(":/icons/app.ico"));
    qRegisterMetaType<rt::Snapshot>();
    QFile style(":/styles/light.qss");
    if(style.open(QIODevice::ReadOnly))app.setStyleSheet(QString::fromUtf8(style.readAll()));
    QCommandLineParser parser;
    parser.addOptions({
        {QStringLiteral("v1"), "打开 V1 窗口"},
        {QStringLiteral("v2"), "打开 V2 窗口"},
        {QStringLiteral("model"), "打开本地模型", "file"},
        {QStringLiteral("screenshot"), "保存窗口截图", "image"},
        {QStringLiteral("smoke-test"), "启动检查完成后退出"},
    });
    if (!parser.parse(app.arguments()))
        return reportError(parser.errorText(), 2);
    const bool v1 = parser.isSet("v1");
    const bool smoke = parser.isSet("smoke-test");
    const QString model = parser.value("model");
    const QString screenshot = parser.value("screenshot");
    for (const auto& option : {QStringLiteral("model"), QStringLiteral("screenshot")}) {
        const QString path = parser.value(option);
        if (parser.isSet(option) && (path.isEmpty() || path.startsWith("--")))
            return reportError("--" + option + " 需要文件路径", 2);
    }
    if (v1 && parser.isSet("v2"))
        return reportError("--v1 和 --v2 只能选一个", 2);
    if (!model.isEmpty()) {
#ifdef SIM_MODEL_PREVIEW
        if (v1)
            return reportError("本地模型预览需要 V2 窗口", 2);
        if (!screenshot.isEmpty() && samePath(model, screenshot))
            return reportError("截图路径与模型路径相同：" + model, 2);
#else
        return reportError("当前构建未启用本地模型预览", 2);
#endif
    }
    std::unique_ptr<QMainWindow> selected;
    if(v1)selected=std::make_unique<rt::MainWindow>();
    else selected=std::make_unique<rt::V2Window>();
    auto& window=*selected;
    window.show();
#ifdef SIM_MODEL_PREVIEW
    if (!model.isEmpty()) {
        auto* panel = window.findChild<rt::model::PreviewPanel*>();
        if (!panel)
            return reportError("模型预览窗口初始化失败", 1);
        if (auto* tabs = window.findChild<QTabWidget*>()) {
            for (int page = 0; page < tabs->count(); ++page) {
                if (tabs->widget(page)->isAncestorOf(panel)) {
                    tabs->setCurrentIndex(page);
                    break;
                }
            }
        }
        if (!screenshot.isEmpty() || smoke) {
            QObject::connect(panel, &rt::model::PreviewPanel::fileTaskFinished, &window,
                [&window, &app, screenshot, model](bool ok) {
                    if (!ok) {
                        app.exit(reportError("模型加载失败：" + model, 1));
                    } else if (screenshot.isEmpty()) {
                        app.quit();
                    } else {
                        QTimer::singleShot(300, &window, [&window, &app, screenshot] {
                            app.exit(saveScreenshot(window, screenshot));
                        });
                    }
                }, Qt::SingleShotConnection);
        }
        panel->openFile(model);
    }
#endif
    if (model.isEmpty()) {
        if (!screenshot.isEmpty()) {
            QTimer::singleShot(500, &window, [&window, &app, screenshot] {
                app.exit(saveScreenshot(window, screenshot));
            });
        } else if (smoke) {
            QTimer::singleShot(400, &app, &QApplication::quit);
        }
    }
    return app.exec();
}
