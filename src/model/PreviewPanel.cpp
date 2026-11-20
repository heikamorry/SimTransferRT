#include "PreviewPanel.h"
#include "Scene.h"
#include <QAction>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace rt::model {
namespace {
QString defaultArchive() {
    return QSettings()
        .value("models/archiveDir",
               QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                   "/model-conversions")
        .toString();
}
QString known(QString text) {
    return text.isEmpty() ? QStringLiteral("未知") : text;
}
QString fieldValue(float v) {
    return QString::number(v, 'g', 9);
}
template <size_t N> QString numbers(const std::array<float, N> &v) {
    QStringList out;
    for (auto f : v)
        out << fieldValue(f);
    return out.join(", ");
}
} // namespace
struct PreviewPanel::Impl {
    struct Work {
        quint64 generation;
        QString file, format, dir, label;
        Snapshot source;
        QByteArray bytes;
        bool object = false;
    };
    PreviewPanel *panel;
    Scene *scene = nullptr;
    QComboBox *mode = nullptr, *attribute = nullptr;
    QSpinBox *pointSize = nullptr;
    QLabel *info = nullptr, *state = nullptr;
    QPushButton *save = nullptr, *cancelButton = nullptr, *details = nullptr;
    QProgressBar *progress = nullptr;
    QTableWidget *trackingTable = nullptr;
    Snapshot current;
    QString detailText, lastArchive, lastOutput;
    bool active = false;
    quint64 committing = 0;
    std::atomic<bool> quitting{false};
    std::atomic<quint64> generation{0}, cancelled{0};
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Work> pending;
    std::thread worker;
    explicit Impl(PreviewPanel *w) : panel(w) {}
    void setBusy(bool on) {
        active = on;
        save->setEnabled(bool(current) && !on);
        cancelButton->setEnabled(on);
        progress->setVisible(on);
    }
    void queue(QString file, QString format, Snapshot src = {}, QString dir = {}) {
        if (quitting)
            return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto gen = ++generation;
            // Coalesce superseded open requests. Accepted conversions always create a job record.
            while (!pending.empty() && !pending.back().source)
                pending.pop_back();
            Work work;
            work.generation = gen;
            work.file = std::move(file);
            work.format = std::move(format);
            work.dir = std::move(dir);
            work.source = std::move(src);
            pending.push_back(std::move(work));
        }
        setBusy(true);
        progress->setValue(0);
        state->setText("正在处理…");
        ready.notify_one();
    }
    void queueObject(QByteArray bytes, QString label) {
        if (quitting || bytes.isEmpty())
            return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto gen = ++generation;
            // A preview replacement policy: only the newest network object waits.
            while (!pending.empty() && !pending.back().source)
                pending.pop_back();
            Work work;
            work.generation = gen;
            work.label = std::move(label);
            work.bytes = std::move(bytes);
            work.object = true;
            pending.push_back(std::move(work));
        }
        setBusy(true);
        progress->setValue(0);
        state->setText("正在解码网络 BX…");
        ready.notify_one();
    }
    template <class F> void post(quint64 gen, F fn) {
        QMetaObject::invokeMethod(
            panel,
            [this, gen, fn = std::move(fn)]() mutable {
                if (!quitting && generation == gen)
                    fn();
            },
            Qt::QueuedConnection);
    }
    void loop() {
        for (;;) {
            Work w;
            {
                std::unique_lock<std::mutex> lock(mutex);
                ready.wait(lock, [&] { return quitting || !pending.empty(); });
                if (pending.empty() && quitting)
                    return;
                w = std::move(pending.front());
                pending.pop_front();
            }
            auto gen = w.generation;
            auto last = std::chrono::steady_clock::time_point{};
            Task task{[this, gen] { return quitting || generation != gen || cancelled == gen; },
                      [this, gen, &last](int n, const QString &msg) {
                          auto now = std::chrono::steady_clock::now();
                          if (now - last < std::chrono::milliseconds(80))
                              return;
                          last = now;
                          post(gen, [this, n, msg] {
                              progress->setValue(n);
                              state->setText(msg);
                          });
                      },
                      [this, gen] {
                          std::lock_guard<std::mutex> lock(mutex);
                          if (quitting || generation != gen || cancelled == gen)
                              throw Cancelled();
                          committing = gen;
                      }};
            try {
                if (w.source) {
                    auto r = convert(w.source, w.file, w.dir, task);
                    post(gen, [this, r, src = w.source, fmt = formatForPath(w.file)] {
                        lastArchive = r.archive;
                        lastOutput = r.output;
                        detailText = QString("%1 → %2\n%3\n任务 %4\n%5\n%6\n输出：%7\n档案：%8")
                                         .arg(src->descriptor["origin"].toString(), fmt,
                                              assess(*src, fmt).summary(), r.id, r.status,
                                              r.message, r.output.isEmpty() ? "未提交" : r.output,
                                              r.archive.isEmpty() ? "未保存" : r.archive);
                        state->setText(r.status == "success" ? "已完成 · " + r.output : r.message);
                        state->setToolTip(detailText);
                        setBusy(false);
                        emit panel->fileTaskFinished(r.status == "success");
                    });
                } else if (w.object) {
                    Snapshot source;
                    try {
                        source = loadBxObject(w.bytes, task, w.label);
                    } catch (const Cancelled &) {
                        throw;
                    } catch (const std::exception &e) {
                        QString msg = QString::fromUtf8(e.what());
                        post(gen, [this, msg] {
                            finishError("传输完成，但 BX 解码失败：" + msg);
                            emit panel->bxObjectFinished(false, msg);
                        });
                        continue;
                    }
                    Display display;
                    try {
                        task.report(85, "构建点云");
                        display = makeDisplay(*source, task);
                        task.check();
                    } catch (const Cancelled &) {
                        throw;
                    } catch (const std::exception &e) {
                        QString msg = QString::fromUtf8(e.what());
                        post(gen, [this, msg] {
                            finishError("BX 解码完成，但预览失败：" + msg);
                            emit panel->bxObjectFinished(false, msg);
                        });
                        continue;
                    }
                    post(gen, [this, source, display = std::move(display)]() mutable {
                        present(std::move(source), display);
                        setBusy(false);
                        const auto &d = current->descriptor;
                        emit panel->bxObjectFinished(
                            true, QString("%1 帧 · %2 个有效位置 · %3 点")
                                      .arg(d.value("frames").toInteger())
                                      .arg(d.value("validPositions").toInteger())
                                      .arg(d.value("pointCount").toInteger()));
                    });
                } else {
                    auto source = load(w.file, task, w.format);
                    task.report(80, "准备静态几何和属性");
                    auto display = makeDisplay(*source, task);
                    task.check();
                    post(gen, [this, source, display = std::move(display), gen]() mutable {
                        if (cancelled == gen) {
                            finishError("已取消");
                            return;
                        }
                        present(std::move(source), display);
                        setBusy(false);
                        emit panel->fileTaskFinished(true);
                    });
                }
            } catch (const Cancelled &) {
                post(gen, [this] { finishError("已取消"); });
            } catch (const std::exception &e) {
                QString msg = QString::fromUtf8(e.what());
                post(gen, [this, msg] { finishError("失败：" + msg); });
            }
        }
    }
    void finishError(const QString &msg) {
        setBusy(false);
        state->setText(msg);
        detailText = msg;
        state->setToolTip(msg);
        emit panel->fileTaskFinished(false);
    }
    void present(Snapshot source, const Display &display) {
        current = std::move(source);
        attribute->blockSignals(true);
        attribute->clear();
        attribute->addItem("原始颜色", -1);
        attribute->addItem("统一颜色", -2);
        int attrs = 0;
        qint64 points = 0, cells = 0;
        QStringList fields;
        for (size_t i = 0; i < current->fields.size(); ++i) {
            auto &f = current->fields[i];
            fields << QString("%1 / %2 · %3 × %4 · %5 tuples")
                          .arg(f.section, f.name, typeName(f.type))
                          .arg(f.components)
                          .arg(f.tuples());
            if (f.section == "Points" || topology(f.section))
                continue;
            ++attrs;
            if (!f.list && (f.section == "PointData" || f.section == "CellData"))
                attribute->addItem(QString("伪彩 · %1/%2 [0] · #%3").arg(f.section, f.name).arg(i),
                                   int(i));
        }
        attribute->blockSignals(false);
        for (int i = 0; i < current->pieces(); ++i) {
            points += current->points(i);
            cells += current->cells(i);
        }
        trackingTable->setRowCount(0);
        trackingTable->setVisible(bool(current->tracking));
        if (current->tracking) {
            int row = 0;
            for (const auto &f : current->tracking->tools) {
                trackingTable->insertRow(row);
                trackingTable->setRowHeight(row, 20);
                bool valid = f.status == aurora::HandleStatus::Valid,
                     port = f.status != aurora::HandleStatus::Disabled;
                QStringList values{QString::number(f.handle),
                                   valid  ? (f.transformUsable() ? "有效" : "端口状态不可用")
                                   : port ? "Missing"
                                          : "Disabled",
                                   valid ? numbers(f.positionMm) : "—",
                                   valid ? numbers(f.quaternion) : "—",
                                   valid ? fieldValue(f.quality) : "—",
                                   port ? QString::number(f.portStatus, 16) : "—",
                                   port ? QString::number(f.frameNumber) : "—"};
                for (int i = 0; i < values.size(); ++i)
                    trackingTable->setItem(row, i, new QTableWidgetItem(values[i]));
                ++row;
                if (f.transformUsable())
                    ++points;
            }
            // Header plus every row; no internal scrolling for a single reply.
            int needed = trackingTable->horizontalHeader()->sizeHint().height() +
                         2 * trackingTable->frameWidth();
            for (int i = 0; i < row; ++i)
                needed += trackingTable->rowHeight(i);
            trackingTable->setFixedHeight(std::min(200, needed + 2));
        }
        auto &desc = current->descriptor;
        const bool bxCloud = desc.value("kind").toString() == "BxPointCloud";
        QString origin = desc["origin"].toString();
        if (current->loadedFormat == "HDF5")
            origin = "HDF5 / " + origin;
        const auto frames = desc.value("frames").toInteger();
        const auto valid = desc.value("validPositions").toInteger();
        const auto missing = desc.value("missing").toInteger();
        const auto disabled = desc.value("disabled").toInteger();
        if (bxCloud) {
            const auto source =
                desc.value("source").toString() == "network" ? "网络接收" : "本地文件";
            QString bounds;
            const auto box = desc.value("bounds").toArray();
            if (box.size() == 6)
                bounds = QString("范围 X[%1, %2] · Y[%3, %4] · Z[%5, %6] mm")
                             .arg(box[0].toDouble(), 0, 'g', 7)
                             .arg(box[1].toDouble(), 0, 'g', 7)
                             .arg(box[2].toDouble(), 0, 'g', 7)
                             .arg(box[3].toDouble(), 0, 'g', 7)
                             .arg(box[4].toDouble(), 0, 'g', 7)
                             .arg(box[5].toDouble(), 0, 'g', 7);
            info->setText(QString("%1 · %2 · BX 点云\n%3 帧 · %4 有效位置 · %5 Missing · "
                                  "%6 Disabled · %7 点 · 单位 mm")
                              .arg(QFileInfo(current->path).fileName(), source)
                              .arg(frames)
                              .arg(valid)
                              .arg(missing)
                              .arg(disabled)
                              .arg(points));
            if (!bounds.isEmpty())
                info->setText(info->text() + "\n" + bounds);
            detailText = current->path + "\n" + info->text() + "\n" + fields.join('\n');
            detailText += QString("\n来源：%1 · 帧 %2 · 有效位置 %3 · Missing %4 · Disabled %5 · "
                                  "点云记录 %6")
                              .arg(source)
                              .arg(frames)
                              .arg(valid)
                              .arg(missing)
                              .arg(disabled)
                              .arg(points);
            state->setText(
                (valid ? QString("已解码 %1 帧 · %2 个有效位置 · 点云已显示（%3 点）")
                             .arg(frames)
                             .arg(valid)
                             .arg(points)
                       : QString("已解码 %1 帧，但没有有效位置（Missing %2 / Disabled %3）")
                             .arg(frames)
                             .arg(missing)
                             .arg(disabled)) +
                QStringLiteral(" · 仅点云预览，不导出模型"));
            save->setToolTip("将已解码的 BX 点云写入过程档案；不导出模型文件");
        } else {
            info->setText(QString("%1 · %2 · %3\n%4 点 · %5 单元 · %6 属性 · 单位 %7 · 坐标系 %8")
                              .arg(QFileInfo(current->path).fileName(), origin,
                                   desc["kind"].toString())
                              .arg(points)
                              .arg(cells)
                              .arg(attrs)
                              .arg(known(desc["units"].toString()),
                                   known(desc["frame"].toString())));
            detailText = current->path + "\n" + info->text() + "\n" + fields.join('\n');
            if (current->tracking)
                detailText +=
                    "\n系统状态 0x" + QString::number(current->tracking->systemStatus, 16);
            state->setText(
                QString("已读取 · %1 分块").arg(current->pieces()) +
                (display.hiddenPoints
                     ? QString(" · 隐藏 %1 个非有限坐标").arg(display.hiddenPoints)
                     : QString()) +
                (display.invalidPoses
                     ? QString(" · %1 个四元数无法显示姿态").arg(display.invalidPoses)
                     : QString()) +
                (current->tracking
                     ? QString(" · 系统状态 0x%1").arg(current->tracking->systemStatus, 0, 16)
                     : QString()));
            save->setToolTip("严格转换并回读校验，同时写入 HDF5 过程档案");
        }
        if (!current->previousJob.isEmpty())
            detailText += "\n档案任务 " + current->previousJob["jobId"].toString() + " · " +
                          current->previousJob["status"].toString() + "\n" +
                          current->previousJob["detail"].toString();
        for (auto item : desc["resourceStatus"].toArray())
            detailText += "\n贴图 " + item.toObject()["reference"].toString() + " · " +
                          item.toObject()["state"].toString();
        state->setToolTip(detailText);
        scene->showData(display, current);
        scene->select(-1, mode->currentIndex());
    }
};
PreviewPanel::PreviewPanel(QWidget *parent) : QWidget(parent), p(std::make_unique<Impl>(this)) {
    setObjectName("localModelPanel");
    setMinimumWidth(340);
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 0, 0, 0);
    layout->setSpacing(6);
    auto tools = new QHBoxLayout;
    tools->setSpacing(8);
    auto open = new QToolButton;
    open->setObjectName("modelOpen");
    open->setText("打开 ▾");
    open->setPopupMode(QToolButton::InstantPopup);
    open->setToolButtonStyle(Qt::ToolButtonTextOnly);
    open->setCursor(Qt::PointingHandCursor);
    open->setMinimumSize(106, 34);
    open->setToolTip("打开 PLY / VTP / legacy VTK、Aurora BX 帧序列或 HDF5 过程档案");
    auto menu = new QMenu(open);
    open->setMenu(menu);
    tools->addWidget(open);
    auto action = [&](QString title, QString filter, QString type) {
        menu->addAction(title, this, [this, title, filter, type] {
            QString path = QFileDialog::getOpenFileName(this, title, {}, filter + ";;所有文件 (*)");
            if (!path.isEmpty())
                openFile(path, type);
        });
    };
    action("打开模型", "模型 (*.ply *.vtp *.vtk)", {});
    action("打开 NDI/BX 文件", "Aurora BX (*.bx *.ndi *.bin)", "Aurora BX");
    action("打开过程档案", "HDF5 (*.h5 *.hdf5)", "HDF5");
    menu->addSeparator();
    menu->addAction("卸载当前文件", this, &PreviewPanel::unload);
    p->save = new QPushButton("转换 / 另存为");
    p->save->setObjectName("modelConvert");
    p->save->setMinimumHeight(34);
    p->save->setToolTip("严格转换并回读校验，同时写入 HDF5 过程档案");
    tools->addWidget(p->save);
    p->cancelButton = new QPushButton("取消");
    p->cancelButton->setObjectName("ghost");
    p->cancelButton->setMinimumHeight(34);
    tools->addWidget(p->cancelButton);
    tools->addStretch();
    auto reset = new QPushButton("重置视图");
    reset->setObjectName("subtle");
    reset->setMinimumHeight(34);
    tools->addWidget(reset);
    layout->addLayout(tools);
    auto view = new QHBoxLayout;
    view->setSpacing(8);
    p->mode = new QComboBox;
    p->mode->addItems({"表面", "点云", "线框"});
    p->mode->setMinimumHeight(32);
    p->attribute = new QComboBox;
    p->attribute->setMinimumContentsLength(6);
    p->attribute->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    p->attribute->addItem("原始颜色", -1);
    p->attribute->setMinimumHeight(32);
    view->addWidget(p->mode);
    view->addWidget(p->attribute, 1);
    auto* sizeLabel = new QLabel("点大小");
    sizeLabel->setObjectName("muted");
    view->addWidget(sizeLabel);
    p->pointSize = new QSpinBox;
    p->pointSize->setObjectName("modelPointSize");
    p->pointSize->setRange(1, 20);
    p->pointSize->setValue(8);
    p->pointSize->setFixedWidth(64);
    p->pointSize->setMinimumHeight(32);
    p->pointSize->setToolTip("点云与点模式的显示大小；仅本地渲染设置，不写入 BX");
    view->addWidget(p->pointSize);
    p->details = new QPushButton("详情");
    p->details->setMinimumHeight(32);
    view->addWidget(p->details);
    layout->addLayout(view);
    p->scene = new Scene(this);
    p->scene->setMinimumSize(0, 0);
    p->scene->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    layout->addWidget(p->scene, 1);
    p->trackingTable = new QTableWidget(0, 7);
    p->trackingTable->setObjectName("localBxTools");
    p->trackingTable->setHorizontalHeaderLabels({"工具", "有效性", "位置 (mm)", "四元数 w/x/y/z",
                                                 "质量指标", "端口状态 (hex)", "设备帧号"});
    p->trackingTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    p->trackingTable->verticalHeader()->hide();
    p->trackingTable->setAlternatingRowColors(true);
    p->trackingTable->setShowGrid(false);
    p->trackingTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    p->trackingTable->horizontalHeader()->setStretchLastSection(true);
    p->trackingTable->setFixedHeight(150);
    p->trackingTable->verticalHeader()->setDefaultSectionSize(20);
    p->trackingTable->verticalHeader()->setMinimumSectionSize(18);
    p->trackingTable->horizontalHeader()->setFixedHeight(32);
    p->trackingTable->hide();
    layout->addWidget(p->trackingTable);
    p->info = new QLabel("本地三维预览");
    p->info->setObjectName("modelInfo");
    p->info->setWordWrap(true);
    layout->addWidget(p->info);
    p->state = new QLabel("选择模型、Aurora BX 或过程档案");
    p->state->setObjectName("modelStatus");
    p->state->setWordWrap(true);
    p->state->setMaximumHeight(34);
    p->state->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(p->state);
    p->progress = new QProgressBar;
    p->progress->setTextVisible(false);
    p->progress->setFixedHeight(8);
    layout->addWidget(p->progress);
    p->setBusy(false);
    connect(reset, &QPushButton::clicked, p->scene, &Scene::reset);
    connect(p->cancelButton, &QPushButton::clicked, this, &PreviewPanel::cancel);
    auto refresh = [this] {
        p->scene->select(p->attribute->currentData().toInt(), p->mode->currentIndex());
    };
    connect(p->attribute, &QComboBox::currentIndexChanged, this, refresh);
    connect(p->mode, &QComboBox::currentIndexChanged, this, refresh);
    connect(p->pointSize, &QSpinBox::valueChanged, p->scene, &Scene::setPointSize);
    connect(p->save, &QPushButton::clicked, this, [this] {
        const bool bxCloud =
            p->current && p->current->descriptor.value("kind").toString() == "BxPointCloud";
        const QString filters =
            bxCloud ? "过程档案 (*.h5)"
                    : "VTP (*.vtp);;PLY (*.ply);;legacy VTK (*.vtk);;Aurora BX (*.bx);;"
                      "过程档案 (*.h5)";
        QString path = QFileDialog::getSaveFileName(this, bxCloud ? "归档 BX 点云" : "转换为新文件",
                                                    {}, filters);
        if (path.isEmpty() || !p->current)
            return;
        const auto fmt = formatForPath(path);
        const auto a = assess(*p->current, fmt);
        QMessageBox box(QMessageBox::Information, "严格转换能力检查", a.summary(),
                        QMessageBox::Cancel, this);
        box.setInformativeText(QString("%1 → %2\n%3").arg(p->current->loadedFormat, fmt, path));
        auto go =
            box.addButton(a.allowed() ? "转换并验证" : "保存拒绝记录", QMessageBox::AcceptRole);
        box.exec();
        if (box.clickedButton() == go)
            saveFile(path);
    });
    menu->addAction("选择过程档案目录…", this, [this] {
        auto dir = QFileDialog::getExistingDirectory(this, "选择过程档案目录", defaultArchive());
        if (!dir.isEmpty())
            QSettings().setValue("models/archiveDir", dir);
    });
    menu->addAction("打开档案所在目录", this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(
            p->lastArchive.isEmpty() ? defaultArchive()
                                     : QFileInfo(p->lastArchive).absolutePath()));
    });
    menu->addAction("打开输出所在目录", this, [this] {
        if (!p->lastOutput.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(p->lastOutput).absolutePath()));
    });
    connect(p->details, &QPushButton::clicked, this, [this] {
        QDialog dialog(this);
        dialog.setWindowTitle("文件与转换详情");
        dialog.resize(640, 420);
        QVBoxLayout box(&dialog);
        QPlainTextEdit text(p->detailText);
        text.setReadOnly(true);
        box.addWidget(&text);
        QDialogButtonBox buttons(QDialogButtonBox::Close);
        box.addWidget(&buttons);
        connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.exec();
    });
    p->worker = std::thread([this] { p->loop(); });
}
void PreviewPanel::openFile(const QString &path, const QString &type) {
    p->queue(path, type);
}
void PreviewPanel::showBxObject(const QByteArray &bytes, const QString &sourceLabel) {
    p->queueObject(bytes, sourceLabel);
}
void PreviewPanel::saveFile(const QString &path, const QString &dir) {
    if (!p->current)
        return;
    if (p->active) {
        p->state->setText("当前文件任务尚未完成，请等待或取消后再转换");
        return;
    }
    auto assessment = assess(*p->current, formatForPath(path));
    p->detailText = assessment.summary();
    p->queue(path, {}, p->current, dir.isEmpty() ? defaultArchive() : dir);
    p->state->setText(assessment.allowed() ? "能力检查通过 · 正在转换"
                                           : "能力检查拒绝 · 正在归档原因");
}
void PreviewPanel::cancel() {
    if (p->active) {
        std::lock_guard<std::mutex> lock(p->mutex);
        if (p->committing == p->generation) {
            p->state->setText("正在提交已校验文件");
            return;
        }
        p->cancelled = p->generation.load();
        p->state->setText("正在取消文件任务…");
    }
}
void PreviewPanel::unload() {
    ++p->generation;
    p->current.reset();
    p->scene->showData({}, {});
    p->scene->render();
    p->attribute->clear();
    p->trackingTable->hide();
    p->trackingTable->setRowCount(0);
    p->info->setText("本地三维预览");
    p->state->setText("已卸载");
    p->detailText.clear();
    p->setBusy(false);
}
bool PreviewPanel::busy() const {
    return p->active;
}
double PreviewPanel::cameraDistance() const {
    return p->scene->cameraDistance();
}
void PreviewPanel::stop() {
    {
        std::lock_guard<std::mutex> lock(p->mutex);
        p->quitting = true;
    }
    p->ready.notify_all();
    if (p->worker.joinable())
        p->worker.join();
}
PreviewPanel::~PreviewPanel() {
    stop();
}
} // namespace rt::model
