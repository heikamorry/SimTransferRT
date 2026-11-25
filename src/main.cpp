#include "ui/MedicalBackdrop.h"
#include "ui/V2Window.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QSaveFile>
#include <QTimer>
#include <cstdio>
#ifdef SIM_MODEL_PREVIEW
#include "model/PreviewPanel.h"
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QVTKOpenGLNativeWidget.h>
#include <filesystem>
#endif

namespace {
int reportError(const QString &message, int code) {
    std::fprintf(stderr, "%s\n", message.toUtf8().constData());
    return code;
}

int saveScreenshot(QWidget &window, const QString &path) {
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

#ifdef SIM_MODEL_PREVIEW
bool sameFile(const QString &left, const QString &right) {
    std::error_code error;
    return std::filesystem::equivalent(QFileInfo(left).filesystemFilePath(),
                                       QFileInfo(right).filesystemFilePath(), error);
}

int showStartupModel(rt::V2Window &window, QApplication &app, const QString &model,
                     const QString &screenshot, bool smoke) {
    auto *panel = window.findChild<rt::model::PreviewPanel *>();
    if (!panel)
        return reportError("模型预览窗口初始化失败", 1);
    if (auto *tabs = window.findChild<QTabWidget *>()) {
        for (int page = 0; page < tabs->count(); ++page) {
            if (tabs->widget(page)->isAncestorOf(panel)) {
                tabs->setCurrentIndex(page);
                break;
            }
        }
    }
    if (!screenshot.isEmpty() || smoke) {
        QObject::connect(
            panel, &rt::model::PreviewPanel::fileTaskFinished, &window,
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
            },
            Qt::SingleShotConnection);
    }
    QTimer::singleShot(0, panel, [panel, model] {
        panel->openFile(model);
    });
    return app.exec();
}
#endif
} // namespace

int main(int argc, char **argv) {
#ifdef SIM_MODEL_PREVIEW
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
#endif
    QApplication app(argc, argv);
    app.setOrganizationName("SimTransferLab");
    app.setApplicationName("SimTransferRT");
    app.setWindowIcon(rt::ui::appIcon());
    QFile style(":/styles/light.qss");
    if (style.open(QIODevice::ReadOnly))
        app.setStyleSheet(QString::fromUtf8(style.readAll()));
    QCommandLineParser parser;
    parser.addOptions({
        {QStringLiteral("model"), "打开本地模型", "file"},
        {QStringLiteral("screenshot"), "保存窗口截图", "image"},
        {QStringLiteral("smoke-test"), "启动检查完成后退出"},
    });
    if (!parser.parse(app.arguments()))
        return reportError(parser.errorText(), 2);
    const bool smoke = parser.isSet("smoke-test");
    const QString model = parser.value("model");
    const QString screenshot = parser.value("screenshot");
    for (const auto &option : {QStringLiteral("model"), QStringLiteral("screenshot")}) {
        const QString path = parser.value(option);
        if (parser.isSet(option) && (path.isEmpty() || path.startsWith("--")))
            return reportError("--" + option + " 需要文件路径", 2);
    }
    if (!model.isEmpty()) {
#ifdef SIM_MODEL_PREVIEW
        if (!screenshot.isEmpty() && sameFile(model, screenshot))
            return reportError("截图路径与模型路径相同：" + model, 2);
#else
        return reportError("当前构建未启用本地模型预览", 2);
#endif
    }
    rt::V2Window window;
    window.show();
#ifdef SIM_MODEL_PREVIEW
    if (!model.isEmpty())
        return showStartupModel(window, app, model, screenshot, smoke);
#endif
    if (!screenshot.isEmpty()) {
        QTimer::singleShot(500, &window, [&window, &app, screenshot] {
            app.exit(saveScreenshot(window, screenshot));
        });
    } else if (smoke) {
        QTimer::singleShot(400, &app, &QApplication::quit);
    }
    return app.exec();
}
