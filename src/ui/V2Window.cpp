#include "ui/V2Window.h"
#ifdef SIM_MODEL_PREVIEW
#include "model/PreviewPanel.h"
#include "ui/BxPreviewQueue.h"
#endif
#include "ui/MedicalBackdrop.h"
#include "v2/Runner.h"
#include "v2/Receiver.h"
#include "v2/ReceiveLoop.h"
#include "core/SamplingClock.h"
#include "core/Diagnostics.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPaintEngine>
#include <QPaintEvent>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include "network/AsioReceiverSocket.h"
#include "network/AsioUdpTransport.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <thread>
#include <chrono>
#include <deque>

namespace rt {
namespace {
std::uint32_t parseIpv4(const QString &text) {
    const auto parts = text.split('.');
    if (parts.size() != 4)
        throw std::invalid_argument("IPv4 address required");
    std::uint32_t value = 0;
    for (const auto &part : parts) {
        bool ok = false;
        const auto number = part.toUInt(&ok);
        if (!ok || number > 255)
            throw std::invalid_argument("IPv4 address required");
        value = (value << 8) | number;
    }
    return value;
}

QString formatIpv4(std::uint32_t value) {
    return QStringLiteral("%1.%2.%3.%4")
        .arg((value >> 24) & 255)
        .arg((value >> 16) & 255)
        .arg((value >> 8) & 255)
        .arg(value & 255);
}

QString text(const QJsonValue &v) {
    if (v.isString())
        return v.toString();
    if (v.isUndefined() || v.isNull())
        return QStringLiteral("未检测");
    if (v.isDouble())
        return QString::number(v.toDouble(), 'g', 14);
    if (v.isBool())
        return v.toBool() ? QStringLiteral("是") : QStringLiteral("否");
    return QString::fromUtf8(
        QJsonDocument(v.isArray() ? QJsonDocument(v.toArray()) : QJsonDocument(v.toObject()))
            .toJson(QJsonDocument::Compact));
}

QLineEdit *line(const QString &value, const char *name) {
    auto *v = new QLineEdit(value);
    v->setObjectName(name);
    return v;
}

QSpinBox *spin(int lo, int hi, int value, const char *name) {
    auto *s = new QSpinBox;
    s->setRange(lo, hi);
    s->setValue(value);
    s->setObjectName(name);
    return s;
}

QString verdict(const QString &s) {
    if (s == "running")
        return QStringLiteral("进行中");
    if (s == "pass")
        return QStringLiteral("通过");
    if (s == "fail")
        return QStringLiteral("失败");
    return QStringLiteral("未检测");
}

QTableWidget *table(const QStringList &labels, const char *name) {
    auto *t = new QTableWidget(0, labels.size());
    t->setObjectName(name);
    t->setHorizontalHeaderLabels(labels);
    t->verticalHeader()->hide();
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setAlternatingRowColors(true);
    t->setWordWrap(true);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    t->horizontalHeader()->setStretchLastSection(true);
    return t;
}

void cell(QTableWidget *t, int r, int c, const QString &s) {
    if (auto *item = t->item(r, c)) {
        if (item->text() != s)
            item->setText(s);
    } else
        t->setItem(r, c, new QTableWidgetItem(s));
}

struct ReceiverUpdate {
    v2::ReceiverSnapshot snapshot;
    QString bindAddress;
    QString peerAddress;
    QString backend;
    quint16 port = 0;
    quint16 peerPort = 0;
    std::uint64_t generation = 0;
    std::uint64_t observedUs = 0;
    std::uint64_t rejectedSources = 0;
    std::uint64_t feedbackErrors = 0;
    v2::ReceiveLoopStats loop;
    bool final = false;
};

class WidgetFramesReady final : public QEvent {
  public:
    static QEvent::Type eventType() {
        static const auto type = static_cast<QEvent::Type>(QEvent::registerEventType());
        return type;
    }
    WidgetFramesReady(std::uint64_t generation, std::shared_ptr<ui::WidgetWakeTiming> wake)
        : QEvent(eventType()), generation(generation), wake(std::move(wake)) {}
    const std::uint64_t generation;
    const std::shared_ptr<ui::WidgetWakeTiming> wake;
};

using GuiReceiverSocket = AsioReceiverSocket;

inline v2::Session::TransportFactory guiTransportFactory() {
    // The GUI sender and receiver are hardwired to Boost.Asio. There is no
    // runtime switch and no Qt socket in the V2 path.
    return [] {
        return makeAsioUdpTransport(true);
    };
}

QString routeKey(const v2::Config &c) {
    return QString::fromStdString(c.bindIp) + "/" + QString::fromStdString(c.targetIp) + ":" +
           QString::number(c.targetPort);
}

QJsonObject ndiDisplay(const aurora::Reply &reply) {
    QJsonArray rows;
    for (const auto &tool : reply.tools) {
        QString state = QStringLiteral("disabled");
        if (tool.status == aurora::HandleStatus::Missing)
            state = "missing";
        if (tool.status == aurora::HandleStatus::Valid)
            state = tool.transformUsable() ? "valid" : "invalid status";
        QJsonArray position;
        QJsonArray quaternion;
        for (auto value : tool.positionMm)
            position.append(value);
        for (auto value : tool.quaternion)
            quaternion.append(value);
        rows.append(QJsonObject{{"handle", tool.handle},
                                {"state", state},
                                {"position", position},
                                {"quaternion", quaternion},
                                {"quality", tool.quality},
                                {"port_status", static_cast<qint64>(tool.portStatus)},
                                {"source_frame", static_cast<qint64>(tool.frameNumber)}});
    }
    return {{"tools", rows}};
}

QJsonObject epDisplay(const v2::Frame &frame) {
    QJsonObject ep{{"format", QStringLiteral("字节值预览；不是已解码的真实电生理波形")}};
    {
        const auto &bytes = frame.bytes;
        QJsonArray samples;
        const auto stride = std::max<std::size_t>(1, bytes.size() / 256);
        for (std::size_t i = 0; i < bytes.size() && samples.size() < 256; i += stride)
            samples.append(bytes[i]);
        ep["bytes"] = static_cast<qint64>(bytes.size());
        ep["samples"] = samples;
    }
    return ep;
}

QJsonObject displayReport(const ReceiverUpdate &update, QJsonObject view) {
    const auto &snapshot = update.snapshot;
    for (unsigned i = 0; i < 2; ++i) {
        const auto key = i == 0 ? "ndi" : "ep";
        const auto &latest = snapshot.latest[i];
        auto item = view[key].toObject();
        // Statistics snapshots can precede frames already delivered to the widgets.
        if (update.final || !item.contains("frame_id") ||
            (latest.frame && latest.frame->frameId == item["frame_id"].toInteger()))
            item["stale"] = latest.stale || update.final;
        view[key] = item;
    }
    QJsonObject status;
    if (snapshot.hasStatus) {
        status["sequence"] = static_cast<qint64>(snapshot.status.sequence);
        status["text"] =
            QString::fromUtf8(reinterpret_cast<const char *>(snapshot.status.payload.data()),
                              static_cast<qsizetype>(snapshot.status.payload.size()));
    }
    view["status"] = status;
    view["stopped"] = update.final;
    const bool associated = snapshot.associationAvailable && !update.final;
    view["association_available"] = associated;
    if (associated)
        view["association_delta_us"] = static_cast<qint64>(snapshot.nearestTimestampDeltaUs);
    else
        view.remove("association_delta_us");
    const auto endpoint = update.peerPort
                              ? update.peerAddress + ":" + QString::number(update.peerPort)
                              : QStringLiteral("尚未收到合法报文");
    return {{"display", view},
            {"receiver_generation", static_cast<qint64>(update.generation)},
            {"source_endpoint", endpoint},
            {"receiver_port", update.port},
            {"receiver_bind", update.bindAddress},
            {"observed_host_us", QString::number(update.observedUs)},
            {"source_rejected", static_cast<qint64>(update.rejectedSources)},
            {"feedback_send_errors", static_cast<qint64>(update.feedbackErrors)},
            {"receive_loop",
             QJsonObject{{"extra_batches", static_cast<qint64>(update.loop.extraBatches)},
                         {"extra_datagrams", static_cast<qint64>(update.loop.extraDatagrams)},
                         {"forced_timer_polls", static_cast<qint64>(update.loop.forcedTimerPolls)},
                         {"nack_batches_socket_readable",
                          static_cast<qint64>(update.loop.nackBatchesReadable)},
                         {"nack_batches_socket_not_readable",
                          static_cast<qint64>(update.loop.nackBatchesNotReadable)},
                         {"nack_batches_socket_readability_unknown",
                          static_cast<qint64>(update.loop.nackBatchesReadabilityUnknown)}}},
            {"final", update.final}};
}

class WaveView final : public QWidget {
  public:
    explicit WaveView(QWidget *parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setMinimumHeight(140);
        setObjectName("v2Waveform");
    }

    void showSamples(QJsonArray samples) {
        if (samples_ == samples) {
            repeatedSamples_ = true;
            return;
        }
        samples_ = std::move(samples);
        firstPaint_ = true;
        repeatedSamples_ = false;
        cachedWave_ = QImage();
        update();
    }

  protected:
    void paintEvent(QPaintEvent *event) override {
        QPainter p(this);
        if (firstPaint_ || samples_.isEmpty() || !repeatedSamples_ || !p.paintEngine() ||
            p.paintEngine()->type() != QPaintEngine::Raster || p.opacity() != 1 ||
            p.compositionMode() != QPainter::CompositionMode_SourceOver) {
            firstPaint_ = false;
            paintWave(p);
            return;
        }
        const auto transform = p.deviceTransform();
        const qreal dpr = devicePixelRatioF();
        if (transform.type() > QTransform::TxScale || transform.m11() != dpr ||
            transform.m22() != dpr || std::floor(transform.dx()) != transform.dx() ||
            std::floor(transform.dy()) != transform.dy()) {
            paintWave(p);
            return;
        }
        if (cachedSize_ != size() || cachedWave_.devicePixelRatio() != dpr)
            cachedWave_ = QImage();
        if (cachedWave_.isNull()) {
            // A new value is painted directly. A partial repaint never rasterizes a full cache.
            constexpr int maxPixels = 2 * 1024 * 1024; // One ARGB image, up to 8 MiB.
            const qreal w = std::ceil(width() * dpr);
            const qreal h = std::ceil(height() * dpr);
            if (event->region() != QRegion(rect()) || p.hasClipping() ||
                width() < 32 || height() < 40 || w * h > maxPixels) {
                paintWave(p);
                return;
            }
            cachedWave_ = QImage(static_cast<int>(w), static_cast<int>(h),
                                 QImage::Format_ARGB32_Premultiplied);
            if (cachedWave_.isNull()) {
                paintWave(p);
                return;
            }
            cachedSize_ = size();
            cachedWave_.setDevicePixelRatio(dpr);
            cachedWave_.fill(Qt::transparent);
            QPainter raster(&cachedWave_);
            paintWave(raster);
        }
        // The line is inset by 10 x 15 logical pixels. Paint a fractional outer edge
        // against the actual destination, then blit the cached opaque interior.
        const QRect interior(0, 0, width() - (width() * dpr != cachedWave_.width()),
                             height() - (height() * dpr != cachedWave_.height()));
        if (!interior.contains(event->region().boundingRect())) {
            p.fillRect(rect(), QColor("#f3f7fa"));
            p.setClipRect(interior, Qt::IntersectClip);
        }
        p.drawImage(QPointF(), cachedWave_);
    }

  private:
    void paintWave(QPainter &p) const {
        p.fillRect(rect(), QColor("#f3f7fa"));
        p.setPen(QColor("#5a6a7a"));
        if (samples_.isEmpty()) {
            p.drawText(rect(), Qt::AlignCenter,
                       QStringLiteral("等待电生理数据 · 真实波形格式尚未提供"));
            return;
        }
        double low = 0;
        double high = 0;
        for (const auto &v : samples_) {
            low = std::min(low, v.toDouble());
            high = std::max(high, v.toDouble());
        }
        if (high == low)
            high = low + 1;
        QPainterPath path;
        for (int i = 0; i < samples_.size(); ++i) {
            const QPointF point(
                10.0 + (width() - 20.0) * i / std::max<qsizetype>(1, samples_.size() - 1),
                height() - 15.0 -
                    (height() - 30.0) * (samples_[i].toDouble() - low) / (high - low));
            if (i == 0)
                path.moveTo(point);
            else
                path.lineTo(point);
        }
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor("#147d92"), 1.5));
        p.drawPath(path);
    }

    QJsonArray samples_;
    bool firstPaint_ = true;
    bool repeatedSamples_ = false;
    QSize cachedSize_;
    QImage cachedWave_;
};
} // namespace

struct V2Window::Impl {
    V2WindowRunOptions options;
    v2::Session session{guiTransportFactory()};
    QComboBox *profile = nullptr;
    QWidget *configBox = nullptr;
    QLineEdit *target = nullptr;
    QLineEdit *bind = nullptr;
    QLineEdit *ndiFile = nullptr;
    QLineEdit *epFile = nullptr;
    QLineEdit *receiverBind = nullptr;
    QLineEdit *statusBody = nullptr;
    QSpinBox *port = nullptr;
    QSpinBox *commandPort = nullptr;
    QSpinBox *epBytes = nullptr;
    QSpinBox *tools = nullptr;
    QSpinBox *ndiHandoffGuard = nullptr;
    QSpinBox *duration = nullptr;
    QSpinBox *pathLimit = nullptr;
    QSpinBox *receiverPort = nullptr;
    QCheckBox *ndiOn = nullptr;
    QCheckBox *epOn = nullptr;
    QCheckBox *overload = nullptr;
    QPushButton *start = nullptr;
    QPushButton *stop = nullptr;
    QPushButton *receive = nullptr;
    QPushButton *post = nullptr;
    QLabel *budget = nullptr;
    QLabel *runState = nullptr;
    QLabel *receiveState = nullptr;
    QLabel *epState = nullptr;
    QLabel *statusState = nullptr;
    QLabel *association = nullptr;
    QPlainTextEdit *log = nullptr;
    QTableWidget *senderStats = nullptr;
    QTableWidget *acceptance = nullptr;
    QTableWidget *toolsTable = nullptr;
    WaveView *wave = nullptr;
    QTimer timer;
    QJsonObject senderReport;
    QJsonObject receiverReport;
    QJsonObject receiverBaseline;
    QString autoPortKey;
    quint16 previousSourcePort = 0;
    bool reusingAutoPort = false;
    QString baselineRun;
    v2::Config baselineConfig;
    std::uint64_t receiverGeneration = 0;
    std::uint64_t baselineGeneration = 0;
    bool baselineReady = false;
    bool archivePending = false;
    std::uint64_t archiveDueUs = 0;
    QJsonObject lastDisplay;
    std::uint64_t lastSenderPollUs = 0;
    StageProfile senderRefresh;
    StageProfile receiverRefresh;
    StageProfile receiverSnapshot;
    StageProfile receiverPresentation;
    StageProfile previewLog;
    StageProfile previewRequest;
    std::array<std::uint64_t, 2> displayedFrames{};
    std::array<std::uint64_t, 2> displayMaxDelayUs{};
    struct DisplayEndpoint {
        std::uint32_t frameId{};
        std::uint64_t completed{}, updated{};
        int tab{};
        bool visible{}, minimized{};
    };
    std::array<DisplayEndpoint, 2> displayPeak{};
    QTabWidget *tabs = nullptr;
    std::array<std::uint64_t, 2> displayMaxWaitUs{};
    std::array<std::uint64_t, 2> displayMaxApplyUs{};
    std::array<std::uint64_t, 2> displayLate{};
    std::array<ui::WidgetFrameTiming, 2> worstWidgetFrame{};
    std::array<std::uint32_t, 2> displayedIds{};
    std::array<bool, 2> haveDisplayed{};
    std::uint32_t nextNdi = 1;
    std::uint32_t nextEp = 1;
    std::uint32_t nextStatus = 1;
    bool sending = false;
    bool finishing = false;
    std::atomic<bool> receiverStop{true};
    std::atomic<bool> receiverRunning{false};
    std::thread receiverWorker;
    std::mutex receiverMutex;
    std::optional<ReceiverUpdate> receiverLatest;

    struct WidgetFrame {
        unsigned module;
        QJsonObject view;
        std::uint64_t enqueuedUs{};
        unsigned queueDepth{};
        bool inheritedWake{};
    };

    std::deque<WidgetFrame> widgetFrames;
    std::array<std::uint64_t, 2> widgetQueueDrops{};
    bool widgetWakePosted = false;
    std::uint64_t nextWakeSequence = 0;
    std::shared_ptr<ui::WidgetWakeTiming> pendingWidgetWake;
    std::optional<ReceiverUpdate> presentedUpdate;
    QString receiverError;
#ifdef SIM_MODEL_PREVIEW
    model::PreviewPanel *preview = nullptr;
    BxPreviewQueue previewQueue;
#endif
};

V2Window::V2Window(QWidget *parent, V2WindowRunOptions options)
    : QMainWindow(parent), p_(std::make_unique<Impl>()) {
    p_->options = options;
    setWindowTitle(QStringLiteral("SimTransfer RT · V2 多模块传输"));
    setWindowIcon(ui::appIcon());
    resize(1350, 900);
    setMinimumSize(1020, 720);
    auto *root = new ui::MedicalBackdrop;
    setCentralWidget(root);
    auto *layout = new QVBoxLayout(root);
    layout->setContentsMargins(18, 14, 18, 14);
    layout->setSpacing(10);
    layout->addWidget(createHeader());
    auto *scope =
        new QLabel(QStringLiteral("独立采集准备 → 有界队列 → 统一发送。当前可验证模拟 / "
                                  "回放输入；真实设备采样、共同硬件时钟和物理出线需另行验证。"));
    scope->setObjectName("hint");
    scope->setWordWrap(true);
    layout->addWidget(scope);
    auto *tabs = new QTabWidget;
    p_->tabs = tabs;
    layout->addWidget(tabs, 1);
    tabs->addTab(createSendPage(), QStringLiteral("发送与网络"));
    auto *receivePage = createReceivePage();
    tabs->addTab(receivePage, QStringLiteral("接收与显示"));
    auto *reportPage = createReportPage();
    tabs->addTab(reportPage, QStringLiteral("逐项验收"));
    connect(tabs, &QTabWidget::currentChanged, this, [this, tabs, receivePage, reportPage] {
        if (p_->sending && tabs->currentWidget() != receivePage)
            refreshSender(true);
        else if (tabs->currentWidget() == reportPage)
            p_->acceptance->resizeRowsToContents();
    });
    layout->addWidget(createLogCard());
    initializeControls();
}

QWidget *V2Window::createHeader() {
    auto *header = new QFrame;
    header->setObjectName("headerCard");
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(16, 10, 16, 10);
    hl->setSpacing(12);
    auto *mark = new QLabel;
    mark->setPixmap(ui::medicalMark(40));
    mark->setFixedSize(40, 40);
    hl->addWidget(mark);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(1);
    auto *title = new QLabel(QStringLiteral("SimTransfer RT · V2"));
    title->setObjectName("brand");
    titles->addWidget(title);
    auto *subtitle = new QLabel(QStringLiteral("多模块 UDP 传输模拟器 · 医疗设备数据链路验证台"));
    subtitle->setObjectName("subtitle");
    titles->addWidget(subtitle);
    hl->addLayout(titles);
    hl->addWidget(new ui::MedicalWatermark(header), 1);
    auto *badge = new QLabel(QStringLiteral("模拟数据 · 非临床用途"));
    badge->setObjectName("badge");
    hl->addWidget(badge);
    return header;
}

QWidget *V2Window::createSendPage() {
    auto *sendPage = new QWidget;
    auto *sendLayout = new QVBoxLayout(sendPage);
    sendLayout->setContentsMargins(0, 8, 0, 0);
    sendLayout->setSpacing(9);
    p_->configBox = new QFrame;
    p_->configBox->setObjectName("card");
    auto *grid = new QHBoxLayout(p_->configBox);
    grid->setContentsMargins(16, 12, 16, 12);
    grid->setSpacing(26);
    auto *left = new QFormLayout;
    auto *right = new QFormLayout;
    left->setSpacing(7);
    right->setSpacing(7);
    grid->addLayout(left);
    grid->addLayout(right);
    auto *targets = new QVBoxLayout;
    targets->setSpacing(6);
    auto *targetsTitle = new QLabel(QStringLiteral("验收目标"));
    targetsTitle->setObjectName("sectionTitle");
    targets->addWidget(targetsTitle);
    auto chip = [](const QString &value, const QString &caption) {
        auto *box = new QFrame;
        box->setObjectName("chip");
        auto *row = new QHBoxLayout(box);
        row->setContentsMargins(12, 7, 14, 7);
        row->setSpacing(9);
        auto *v = new QLabel(value);
        v->setObjectName("chipValue");
        row->addWidget(v);
        auto *c = new QLabel(caption);
        c->setObjectName("chipLabel");
        c->setWordWrap(true);
        row->addWidget(c, 1);
        return box;
    };
    targets->addWidget(chip(QStringLiteral("≤ 15 ms"), QStringLiteral("NDI 采样到完整首发")));
    targets->addWidget(chip(QStringLiteral("≤ 18 ms"), QStringLiteral("电生理采样到完整首发")));
    targets->addWidget(chip(QStringLiteral("< 1 ms"), QStringLiteral("采样与包间隔抖动")));
    targets->addStretch(1);
    auto *targetNote =
        new QLabel(QStringLiteral("计时为本机用户态观测；真实网卡出线时刻需独立验证。"));
    targetNote->setObjectName("muted");
    targetNote->setWordWrap(true);
    targets->addWidget(targetNote);
    grid->addLayout(targets, 1);
    sendLayout->addWidget(p_->configBox);
    p_->profile = new QComboBox;
    p_->profile->setObjectName("v2NetworkProfile");
    p_->profile->addItem(QStringLiteral("100Mbps（备选）"), 100);
    p_->profile->addItem(QStringLiteral("1GbE（推荐）"), 1000);
    p_->profile->addItem(QStringLiteral("2.5GbE（备选）"), 2500);
    left->addRow(QStringLiteral("网络档位"), p_->profile);
    QSettings settings;
    p_->target = line(settings.value("v2/target", "127.0.0.1").toString(), "v2Target");
    left->addRow(QStringLiteral("目标 IPv4"), p_->target);
    p_->bind = line(settings.value("v2/bind", "127.0.0.1").toString(), "v2Bind");
    left->addRow(QStringLiteral("绑定 IPv4"), p_->bind);
    p_->port = spin(1, 65535, settings.value("v2/port", 5650).toInt(), "v2TargetPort");
    left->addRow(QStringLiteral("数据端口"), p_->port);
    p_->commandPort =
        spin(0, 65535, settings.value("v2/commandPort", 5500).toInt(), "v2CommandPort");
    left->addRow(QStringLiteral("反馈端口（0 为自动）"), p_->commandPort);
    p_->pathLimit = spin(0, 1000000, 0, "v2PathLimit");
    p_->pathLimit->setSpecialValueText(QStringLiteral("未知"));
    left->addRow(QStringLiteral("已知路径上限（Mbps）"), p_->pathLimit);
    p_->duration = spin(0, 86400, 0, "v2Duration");
    p_->duration->setSpecialValueText(QStringLiteral("持续运行"));
    left->addRow(QStringLiteral("时长（秒）"), p_->duration);
    p_->ndiOn = new QCheckBox(QStringLiteral("启用 NDI · 模拟 60Hz / 回放"));
    p_->ndiOn->setChecked(true);
    right->addRow(p_->ndiOn);
    p_->tools = spin(0, 255, 4, "v2NdiTools");
    right->addRow(QStringLiteral("模拟工具数（非设备能力）"), p_->tools);
    p_->ndiHandoffGuard = spin(0, 400, v2::Config{}.ndiHandoffGuardUs, "v2NdiHandoffGuard");
    p_->ndiHandoffGuard->setSpecialValueText(QStringLiteral("关闭"));
    p_->ndiHandoffGuard->setToolTip(QStringLiteral("在预计 NDI 帧就绪前短时轮询，增加发送线程 CPU 用量。"));
    right->addRow(QStringLiteral("NDI 交接提前量（µs）"), p_->ndiHandoffGuard);
    p_->ndiFile = line({}, "v2NdiReplay");
    auto *ndiBrowse = new QPushButton(QStringLiteral("选择 BX"));
    auto *nr = new QHBoxLayout;
    nr->addWidget(p_->ndiFile);
    nr->addWidget(ndiBrowse);
    right->addRow(QStringLiteral("NDI 回放（空为模拟）"), nr);
    p_->epOn = new QCheckBox(QStringLiteral("启用电生理 · 50Hz"));
    p_->epOn->setChecked(true);
    right->addRow(p_->epOn);
    p_->epBytes = spin(1, 2097152, 204800, "v2EpBytes");
    right->addRow(QStringLiteral("电生理帧长（字节）"), p_->epBytes);
    p_->epFile = line({}, "v2EpReplay");
    auto *epBrowse = new QPushButton(QStringLiteral("选择数据"));
    auto *er = new QHBoxLayout;
    er->addWidget(p_->epFile);
    er->addWidget(epBrowse);
    right->addRow(QStringLiteral("电生理回放（空为模拟）"), er);
    p_->overload = new QCheckBox(QStringLiteral("显式过载测试（容量不足仍运行，验收不放宽）"));
    right->addRow(p_->overload);
    p_->budget = new QLabel;
    p_->budget->setObjectName("v2Budget");
    p_->budget->setWordWrap(true);
    sendLayout->addWidget(p_->budget);
    auto *actions = new QHBoxLayout;
    p_->start = new QPushButton(QStringLiteral("开始 V2 发送"));
    p_->start->setObjectName("v2Start");
    p_->start->setMinimumSize(148, 36);
    p_->stop = new QPushButton(QStringLiteral("停止"));
    p_->stop->setMinimumHeight(36);
    p_->stop->setObjectName("v2Stop");
    p_->stop->setEnabled(false);
    actions->addWidget(p_->start);
    actions->addWidget(p_->stop);
    p_->runState = new QLabel(QStringLiteral("就绪"));
    actions->addWidget(p_->runState);
    actions->addStretch();
    sendLayout->addLayout(actions);
    auto *statusRow = new QHBoxLayout;
    p_->statusBody = line(QStringLiteral("ready"), "v2StatusBody");
    p_->statusBody->setMaxLength(1444);
    p_->post = new QPushButton(QStringLiteral("发送完整状态快照"));
    p_->post->setEnabled(false);
    statusRow->addWidget(p_->statusBody);
    statusRow->addWidget(p_->post);
    sendLayout->addLayout(statusRow);
    p_->senderStats =
        table({QStringLiteral("模块"), QStringLiteral("已构建"), QStringLiteral("完整首发"),
               QStringLiteral("跳采"), QStringLiteral("首发超期"), QStringLiteral("补发包"),
               QStringLiteral("首发最大耗时 μs")},
              "v2SenderStats");
    sendLayout->addWidget(p_->senderStats, 1);
    connect(ndiBrowse, &QPushButton::clicked, this, [this] {
        const auto f =
            QFileDialog::getOpenFileName(this, QStringLiteral("选择一次完整 Aurora BX 回复"));
        if (!f.isEmpty())
            p_->ndiFile->setText(f);
    });
    connect(epBrowse, &QPushButton::clicked, this, [this] {
        const auto f = QFileDialog::getOpenFileName(this, QStringLiteral("选择电生理帧数据"));
        if (!f.isEmpty())
            p_->epFile->setText(f);
    });
    return sendPage;
}

QWidget *V2Window::createReceivePage() {
    QSettings settings;
    auto *receivePage = new QWidget;
    auto *rl = new QVBoxLayout(receivePage);
    rl->setContentsMargins(0, 8, 0, 0);
    rl->setSpacing(8);
    auto *rxControls = new QHBoxLayout;
    rxControls->setSpacing(8);
    p_->receiverBind = line("127.0.0.1", "v2ReceiverBind");
    p_->receiverPort = spin(1, 65535, 5650, "v2ReceiverPort");
    p_->receive = new QPushButton(QStringLiteral("启动接收"));
    p_->receive->setObjectName("v2Receive");
    rxControls->addWidget(new QLabel(QStringLiteral("绑定地址")));
    rxControls->addWidget(p_->receiverBind, 1);
    rxControls->addWidget(p_->receiverPort);
    rxControls->addWidget(p_->receive);
    rl->addLayout(rxControls);
    p_->receiveState = new QLabel(
        QStringLiteral("接收未启动。重启会清空本地会话；请与发送端协调并换用新端口隔离旧报文。"));
    p_->receiveState->setObjectName("muted");
    p_->receiveState->setWordWrap(true);
    rl->addWidget(p_->receiveState);
    p_->toolsTable =
        table({QStringLiteral("工具"), QStringLiteral("有效性"), QStringLiteral("位置 x/y/z（mm）"),
               QStringLiteral("四元数 w/x/y/z"), QStringLiteral("质量指标"),
               QStringLiteral("端口状态"), QStringLiteral("设备帧号")},
              "v2NdiToolsTable");
#ifdef SIM_MODEL_PREVIEW
    auto *models = new QSplitter(Qt::Horizontal, receivePage);
    models->setObjectName("localModelSplitter");
    models->setChildrenCollapsible(false);
    p_->toolsTable->setMinimumWidth(220);
    p_->toolsTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    p_->toolsTable->horizontalHeader()->setStretchLastSection(false);
    models->addWidget(p_->toolsTable);
    p_->preview = new model::PreviewPanel(models);
    models->addWidget(p_->preview);
    connect(p_->preview, &model::PreviewPanel::localActionStarted, this, [this] {
        std::lock_guard<std::mutex> lock(p_->receiverMutex);
        p_->previewQueue.localActionStarted();
    });
    connect(p_->preview, &model::PreviewPanel::bxObjectFinished, this,
            [this](bool ok, const QString &detail) {
                p_->log->appendPlainText(ok ? QStringLiteral("BX 解码完成，点云已显示：") + detail
                                            : QStringLiteral("传输完成，但 BX 预览未完成：") +
                                                  detail);
            });
    models->setStretchFactor(0, 2);
    models->setStretchFactor(1, 3);
    models->setSizes({430, 750});
    models->restoreState(settings.value("models/splitter").toByteArray());
    connect(models, &QSplitter::splitterMoved, models, [models] {
        QSettings().setValue("models/splitter", models->saveState());
    });
    rl->addWidget(models, 1);
#else
    rl->addWidget(p_->toolsTable, 1);
#endif
    // Live coordinates change every frame. Content-based sizing repeatedly measures every tool.
    p_->toolsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    const std::array<int, 7> toolColumnWidths{80, 110, 300, 340, 110, 110, 120};
    for (int i = 0; i < static_cast<int>(toolColumnWidths.size()); ++i)
        p_->toolsTable->setColumnWidth(i, toolColumnWidths[i]);
    p_->epState = new QLabel(QStringLiteral("电生理尚未收到"));
    p_->epState->setObjectName("v2EpState");
    p_->epState->setWordWrap(true);
    p_->epState->setMinimumWidth(250);
    p_->wave = new WaveView;
    p_->wave->setMinimumHeight(56);
    p_->wave->setMaximumHeight(68);
    auto *waveCard = new QFrame;
    waveCard->setObjectName("card");
    auto *waveLayout = new QHBoxLayout(waveCard);
    waveLayout->setContentsMargins(12, 7, 12, 8);
    waveLayout->setSpacing(12);
    auto *waveHead = new QVBoxLayout;
    waveHead->setSpacing(1);
    auto *waveTitle = new QLabel(QStringLiteral("电生理字节预览"));
    waveTitle->setObjectName("sectionTitle");
    waveHead->addWidget(waveTitle);
    waveHead->addWidget(p_->epState, 1);
    waveLayout->addLayout(waveHead, 2);
    waveLayout->addWidget(p_->wave, 3);
    rl->addWidget(waveCard);
    p_->statusState = new QLabel(QStringLiteral("状态尚未收到"));
    p_->statusState->setObjectName("v2StatusState");
    p_->statusState->setWordWrap(true);
    p_->association = new QLabel(QStringLiteral("共同硬件时间域未验证；匹配仅作模拟/回放观察"));
    p_->association->setObjectName("muted");
    p_->association->setWordWrap(true);
    auto *rxStatusRow = new QHBoxLayout;
    rxStatusRow->setSpacing(12);
    rxStatusRow->addWidget(p_->statusState, 1);
    rxStatusRow->addWidget(p_->association, 1);
    rl->addLayout(rxStatusRow);
    return receivePage;
}

QWidget *V2Window::createReportPage() {
    auto *reportPage = new QWidget;
    auto *reportLayout = new QVBoxLayout(reportPage);
    reportLayout->setContentsMargins(0, 8, 0, 0);
    reportLayout->setSpacing(8);
    auto *reportActions = new QHBoxLayout;
    reportActions->setSpacing(8);
    auto *exp = new QPushButton(QStringLiteral("导出当前收发报告"));
    exp->setObjectName("primary");
    auto *archives = new QPushButton(QStringLiteral("打开每轮运行档案"));
    reportActions->addWidget(exp);
    reportActions->addWidget(archives);
    reportActions->addStretch();
    reportLayout->addLayout(reportActions);
    p_->acceptance =
        table({QStringLiteral("要求"), QStringLiteral("测量口径"), QStringLiteral("阈值"),
               QStringLiteral("实测值"), QStringLiteral("结果"), QStringLiteral("原因")},
              "v2Acceptance");
    reportLayout->addWidget(p_->acceptance, 1);
    connect(archives, &QPushButton::clicked, this, [] {
        const auto directory =
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/v2-runs";
        QDir().mkpath(directory);
        QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
    });
    connect(exp, &QPushButton::clicked, this, &V2Window::exportReport);
    return reportPage;
}

QWidget *V2Window::createLogCard() {
    auto *logCard = new QFrame;
    logCard->setObjectName("card");
    auto *logLayout = new QVBoxLayout(logCard);
    logLayout->setContentsMargins(12, 8, 12, 10);
    logLayout->setSpacing(4);
    auto *logHead = new QHBoxLayout;
    auto *logTitle = new QLabel(QStringLiteral("运行日志"));
    logTitle->setObjectName("sectionTitle");
    logHead->addWidget(logTitle);
    logHead->addStretch();
    auto *clearLog = new QPushButton(QStringLiteral("清空"));
    clearLog->setObjectName("ghost");
    clearLog->setCursor(Qt::PointingHandCursor);
    logHead->addWidget(clearLog);
    logLayout->addLayout(logHead);
    p_->log = new QPlainTextEdit;
    p_->log->setReadOnly(true);
    p_->log->setMaximumBlockCount(300);
    p_->log->setFixedHeight(72);
    logLayout->addWidget(p_->log);
    connect(clearLog, &QPushButton::clicked, p_->log, &QPlainTextEdit::clear);
    return logCard;
}

void V2Window::initializeControls() {
    QSettings settings;
    int index = p_->profile->findData(settings.value("v2/networkMbps", 1000).toUInt());
    if (index < 0) {
        index = 1;
        p_->log->appendPlainText(QStringLiteral("保存的网络档位无效，已恢复为 1GbE。"));
    }
    p_->profile->setCurrentIndex(index);
    connect(p_->profile, &QComboBox::currentIndexChanged, this, [this] {
        QSettings().setValue("v2/networkMbps", p_->profile->currentData());
        updateBudget();
    });
    for (auto *s : {p_->epBytes, p_->tools, p_->pathLimit})
        connect(s, &QSpinBox::valueChanged, this, [this] {
            updateBudget();
        });
    for (auto *s : {p_->ndiOn, p_->epOn})
        connect(s, &QCheckBox::toggled, this, [this] {
            updateBudget();
        });
    for (auto *s : {p_->ndiFile, p_->epFile})
        connect(s, &QLineEdit::textChanged, this, [this] {
            updateBudget();
        });
    connect(p_->start, &QPushButton::clicked, this, &V2Window::startSending);
    connect(p_->stop, &QPushButton::clicked, this, &V2Window::stopSending);
    connect(p_->receive, &QPushButton::clicked, this, &V2Window::toggleReceiver);
    connect(p_->post, &QPushButton::clicked, this, [this] {
        const auto b = p_->statusBody->text().toUtf8();
        if (b.size() > 1444 || !p_->session.postStatus(Bytes(b.begin(), b.end())))
            p_->log->appendPlainText(
                QStringLiteral("状态未入队：超过 1444 字节、队列已满或运行已停止。"));
    });
    p_->timer.setTimerType(Qt::PreciseTimer);
    p_->timer.setInterval(5);
    connect(&p_->timer, &QTimer::timeout, this, &V2Window::refresh);
    p_->timer.start();
    updateBudget();
}

V2Window::~V2Window() {
    p_->receiverStop = true;
#ifdef SIM_MODEL_PREVIEW
    {
        std::lock_guard<std::mutex> lock(p_->receiverMutex);
        p_->previewQueue.close();
    }
#endif
    p_->session.stop();
    if (p_->receiverWorker.joinable())
        p_->receiverWorker.join();
    refreshReceiver();
    const auto result = p_->session.wait();
    if (p_->sending && !result.reportJson.empty()) {
        p_->senderReport =
            QJsonDocument::fromJson(QByteArray::fromStdString(result.reportJson)).object();
        p_->sending = false;
        p_->archivePending = true;
    }
    if (p_->archivePending)
        archiveRun();
}

v2::Config V2Window::selectedConfig() const {
    v2::Config c;
    c.fragmentTrace = p_->options.fragmentTrace;
    c.asioPooledHandler = p_->options.asioPooledHandler;
    c.linkMbps = p_->profile->currentData().toUInt();
    c.targetIp = p_->target->text().trimmed().toStdString();
    c.bindIp = p_->bind->text().trimmed().toStdString();
    c.allowedNackIp = c.targetIp;
    c.targetPort = static_cast<std::uint16_t>(p_->port->value());
    c.commandPort = static_cast<std::uint16_t>(p_->commandPort->value());
    c.epBytes = static_cast<std::uint32_t>(p_->epBytes->value());
    c.ndiTools = static_cast<std::uint32_t>(p_->tools->value());
    c.ndiHandoffGuardUs = static_cast<unsigned>(p_->ndiHandoffGuard->value());
    c.ndiEnabled = p_->ndiOn->isChecked();
    c.epEnabled = p_->epOn->isChecked();
    c.pathLimitMbps = static_cast<unsigned>(p_->pathLimit->value());
    c.durationUs = static_cast<std::uint64_t>(p_->duration->value()) * 1000000;
    c.allowOversubscribed = p_->overload->isChecked();
    c.ndiReplayFile = p_->ndiFile->text().trimmed().toStdString();
    c.epReplayFile = p_->epFile->text().trimmed().toStdString();
    c.initialNdiId = p_->nextNdi;
    c.initialEpId = p_->nextEp;
    c.initialStatusSequence = p_->nextStatus;
    return c;
}

void V2Window::setSenderControlsLocked(bool locked) {
    p_->configBox->setEnabled(!locked);
    p_->start->setEnabled(!locked);
    p_->stop->setEnabled(locked);
    p_->post->setEnabled(locked);
}

void V2Window::updateBudget() {
    try {
        const auto c = selectedConfig();
        const auto ndiSize =
            p_->ndiFile->text().isEmpty() ? 0 : QFileInfo(p_->ndiFile->text()).size();
        const auto epSize = p_->epFile->text().isEmpty() ? 0 : QFileInfo(p_->epFile->text()).size();
        const auto b =
            v2::preflight(c, static_cast<std::size_t>(ndiSize), static_cast<std::size_t>(epSize));
        const auto verdict =
            b.feasible ? QStringLiteral(
                             "容量预检通过；主机停顿、状态突发、设备时序和物理出线仍需独立验证。")
                       : QStringLiteral(
                             "容量预检未通过：声明负载超过平均容量或 15/18 ms 最小串行化预算。");
        p_->budget->setText(
            QStringLiteral("所选 %1 Mbps；本机连接速率在启动时检测；有效预算 %2 Mbps。理论线路占用 "
                           "%3%；NDI %4 字节 / 电生理 %5 字节；同相出线下限 %6 ms。\n%7 NDI "
                           "≤15ms、电生理 ≤18ms，附加抖动目标 <1ms 保持不变。")
                .arg(c.linkMbps)
                .arg(b.effectiveMbps)
                .arg(b.utilization * 100, 0, 'f', 3)
                .arg(b.ndiBytes)
                .arg(b.epBytes)
                .arg(b.synchronousWireUs / 1000, 0, 'f', 3)
                .arg(verdict));
        p_->budget->setToolTip(QString::fromStdString(b.reason));
    } catch (const std::exception &e) {
        p_->budget->setText(QString::fromUtf8(e.what()));
    }
}

void V2Window::startSending() {
    if (p_->archivePending) {
        archiveRun();
        if (p_->archivePending)
            return;
    }
    try {
        auto c = selectedConfig();
        c.runId = QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz").toStdString();
        v2::validateConfig(c);
        QSettings s;
        s.setValue("v2/target", p_->target->text());
        s.setValue("v2/bind", p_->bind->text());
        s.setValue("v2/port", p_->port->value());
        s.setValue("v2/commandPort", p_->commandPort->value());
        s.setValue("v2/networkMbps", c.linkMbps);
        refreshReceiver();
        p_->receiverBaseline = receiverReportForExport();
        p_->baselineRun = QString::fromStdString(c.runId);
        p_->baselineConfig = c;
        p_->baselineGeneration = p_->receiverGeneration;
        // A running receiver pins its peer. Keep the OS-selected source port on consecutive runs.
        if (c.commandPort)
            p_->autoPortKey.clear();
        p_->reusingAutoPort = !c.commandPort && p_->autoPortKey == routeKey(c);
        if (p_->reusingAutoPort)
            c.commandPort = p_->previousSourcePort;
        // New-run display evidence has the same baseline as receive/consume counts.
        // Keep last displayed IDs so the preceding run's stale view is not counted again.
        p_->displayedFrames = {};
        p_->displayMaxDelayUs = {};
        p_->displayPeak = {};
        p_->displayMaxWaitUs = {};
        p_->displayMaxApplyUs = {};
        p_->displayLate = {};
        p_->worstWidgetFrame = {};
        p_->senderRefresh = {};
        p_->receiverRefresh = {};
        p_->receiverSnapshot = {};
        p_->receiverPresentation = {};
        p_->previewLog = {};
        p_->previewRequest = {};
        {
            std::lock_guard<std::mutex> lock(p_->receiverMutex);
            p_->widgetQueueDrops = {};
        }
        const auto modules = p_->receiverBaseline["modules"].toObject();
        const auto observed = p_->receiverBaseline["observed_host_us"].toString().toULongLong();
        const auto now = monotonicUs();
        p_->baselineReady = p_->receiverRunning &&
                            p_->receiverBaseline["receiver_generation"].toInteger(-1) ==
                                static_cast<qint64>(p_->receiverGeneration) &&
                            p_->receiverBaseline["receiver_port"].toInt() == c.targetPort &&
                            observed && now >= observed && now - observed < 100000 &&
                            modules.contains("1") && modules.contains("2") &&
                            modules["1"].toObject()["activeFrames"].toInteger() == 0 &&
                            modules["2"].toObject()["activeFrames"].toInteger() == 0 &&
                            p_->receiverBaseline["source_rejected"].toInteger() == 0;
        p_->session.start(c);
        p_->sending = true;
        p_->finishing = false;
        p_->stop->setText(QStringLiteral("停止发送"));
        setSenderControlsLocked(true);
        p_->runState->setText(QStringLiteral("准备 / 发送中"));
        p_->log->appendPlainText(QStringLiteral("开始 V2 运行 %1（传输后端：Boost.Asio "
                                                "异步）。所有模块序号续用；采样来源：模拟或回放。")
                                     .arg(QString::fromStdString(c.runId)));
    } catch (const std::exception &e) {
        p_->log->appendPlainText(QString::fromUtf8(e.what()));
        p_->runState->setText(QStringLiteral("启动失败"));
    }
}

void V2Window::stopSending() {
    p_->post->setEnabled(false);
    if (p_->finishing) {
        p_->session.stop();
        p_->stop->setEnabled(false);
        p_->runState->setText(QStringLiteral("正在取消剩余发送"));
        return;
    }
    p_->finishing = true;
    p_->session.finish();
    p_->stop->setText(QStringLiteral("立即停止"));
    p_->runState->setText(QStringLiteral("正在完成已构建帧并处理补发"));
}

void V2Window::presentSenderReport(const QJsonObject &report) {
    if (p_->sending && !p_->baselineConfig.commandPort &&
        p_->baselineGeneration == p_->receiverGeneration &&
        report["run_id"].toString() == p_->baselineRun) {
        const auto port = report["local_port"].toInt();
        if (port > 0) {
            p_->previousSourcePort = static_cast<quint16>(port);
            p_->autoPortKey = routeKey(p_->baselineConfig);
        } else if (p_->reusingAutoPort && report["finished"].toBool() &&
                   !report["error"].toString().isEmpty()) {
            p_->reusingAutoPort = false;
            const auto feasible = report["preflight"].toObject().value("feasible");
            const bool rejectedBeforeOpen =
                !p_->baselineConfig.allowOversubscribed && feasible.isBool() && !feasible.toBool();
            if (!rejectedBeforeOpen) {
                try {
                    GuiReceiverSocket probe(
                        parseIpv4(QString::fromStdString(p_->baselineConfig.bindIp)),
                        p_->previousSourcePort);
                } catch (const std::exception &) {
                    p_->autoPortKey.clear();
                    p_->log->appendPlainText(
                        QStringLiteral("复查时无法绑定上轮自动端口。下次启动将重新分配；请重新启动"
                                       "锁定旧端口的接收端。"));
                }
            }
        }
    }
    p_->senderReport = report;
    const auto modules = report["modules"].toObject();
    p_->senderStats->setRowCount(2);
    const auto config = report["config"].toObject();
    const auto link = report["link_observation"].toObject();
    if (!config.isEmpty())
        p_->budget->setText(
            QStringLiteral("所选档位 %1；本机检测速率 %2 Mbps；有效调度预算 %3 Mbps；已知路径上限 "
                           "%4 Mbps（0 为未知）。\n%5\nNDI ≤15ms、电生理 ≤18ms；附加抖动目标 "
                           "<1ms。链路能力与实际出线时序分别验收。")
                .arg(text(config["network_profile"]), text(link["local_link_mbps"]),
                     text(config["effective_link_mbps"]), text(config["path_limit_mbps"]),
                     link["loopback"].toBool()
                         ? QStringLiteral("当前是本机环回，真实网口与物理出线未检测。")
                         : QStringLiteral("已观测到非环回路由；物理出线时刻仍需独立验证。")));
    for (int i = 0; i < 2; ++i) {
        const auto m = modules[QString::number(i + 1)].toObject();
        cell(p_->senderStats, i, 0, i ? QStringLiteral("电生理") : QStringLiteral("NDI"));
        int col = 1;
        for (const auto *key :
             {"built", "initial_complete", "skipped_slots", "deadline_misses", "retransmissions"})
            cell(p_->senderStats, i, col++, text(m[key]));
        cell(p_->senderStats, i, 6, text(m["initial_duration"].toObject()["max_us"]));
    }
    const auto rows = report["acceptance"].toArray();
    p_->acceptance->setRowCount(rows.size());
    int row = 0;
    for (const auto &value : rows) {
        const auto r = value.toObject();
        int col = 0;
        for (const auto *key : {"requirement", "measurement", "threshold", "measured"})
            cell(p_->acceptance, row, col++, text(r[key]));
        cell(p_->acceptance, row, 4, verdict(r["status"].toString()));
        cell(p_->acceptance, row, 5, text(r["reason"]));
        ++row;
    }
    if (p_->acceptance->isVisible())
        p_->acceptance->resizeRowsToContents();
}

void V2Window::presentReceiverReport(const QJsonObject &report) {
    p_->presentedUpdate.reset();
    p_->receiverReport = report;
    applyReceiverView(report["display"].toObject());
}

std::uint64_t V2Window::applyReceiverView(const QJsonObject &view, unsigned observedModule) {
    if (view == p_->lastDisplay)
        return 0;
    const bool stopped = view["stopped"].toBool();
    const bool stopChanged = stopped != p_->lastDisplay["stopped"].toBool();
    if (stopChanged || view["ndi"] != p_->lastDisplay["ndi"]) {
        const auto ndi = view["ndi"].toObject();
        const auto tools = ndi["tools"].toArray();
        if (p_->toolsTable->rowCount() != tools.size())
            p_->toolsTable->setRowCount(tools.size());
        int row = 0;
        for (const auto &value : tools) {
            const auto t = value.toObject();
            const auto state = t["state"].toString();
            const bool valid = state == "valid" && !ndi["stale"].toBool();
            cell(p_->toolsTable, row, 0, text(t["handle"]));
            cell(p_->toolsTable, row, 1,
                 stopped                 ? QStringLiteral("接收已停止")
                 : ndi["stale"].toBool() ? QStringLiteral("已过期")
                                         : state);
            cell(p_->toolsTable, row, 2,
                 valid ? text(t["position"]) : QStringLiteral("无有效位置"));
            cell(p_->toolsTable, row, 3,
                 valid ? text(t["quaternion"]) : QStringLiteral("无有效姿态"));
            cell(p_->toolsTable, row, 4, valid ? text(t["quality"]) : QStringLiteral("不适用"));
            cell(p_->toolsTable, row, 5, text(t["port_status"]));
            cell(p_->toolsTable, row, 6, text(t["source_frame"]));
            ++row;
        }
    }
    if (stopChanged || view["ep"] != p_->lastDisplay["ep"]) {
        const auto ep = view["ep"].toObject();
        const auto freshness = stopped                ? QStringLiteral("接收已停止")
                               : ep["stale"].toBool() ? QStringLiteral("已过期")
                                                      : QStringLiteral("最近已消费");
        p_->epState->setText(
            QStringLiteral("电生理帧 %1，%2 字节；%3；%4")
                .arg(text(ep["frame_id"]), text(ep["bytes"]), freshness,
                     ep["format"].toString(QStringLiteral("业务布局未知，不能判定真实波形"))));
        p_->wave->showSamples(ep["stale"].toBool() ? QJsonArray{} : ep["samples"].toArray());
    }
    if (p_->lastDisplay.isEmpty() || stopChanged || view["status"] != p_->lastDisplay["status"]) {
        const auto status = view["status"].toObject();
        const auto format = stopped ? QStringLiteral("接收已停止，最后状态序号 %1：%2")
                                    : QStringLiteral("状态序号 %1：%2");
        p_->statusState->setText(format.arg(text(status["sequence"]), text(status["text"])));
    }
    if (p_->lastDisplay.isEmpty() || stopChanged ||
        view["association_delta_us"] != p_->lastDisplay["association_delta_us"]) {
        p_->association->setText(stopped ? QStringLiteral("接收已停止，无当前关联")
            : QStringLiteral("跨模块最近邻时间差：%1 μs；共同硬件时间域未验证，真实同步验收未检测。")
                  .arg(text(view["association_delta_us"])));
    }
    p_->lastDisplay = view;
    if (stopped)
        return 0;
    const auto updated = monotonicUs();
    std::uint64_t observedUpdate = 0;
    for (unsigned i = 0; i < 2; ++i) {
        const auto item = view[i == 0 ? "ndi" : "ep"].toObject();
        if (!item.contains("frame_id"))
            continue;
        const auto id = static_cast<std::uint32_t>(item["frame_id"].toInteger());
        if (!p_->haveDisplayed[i] || id != p_->displayedIds[i]) {
            p_->haveDisplayed[i] = true;
            p_->displayedIds[i] = id;
            ++p_->displayedFrames[i];
            const auto completed =
                static_cast<std::uint64_t>(item["host_completed_us"].toInteger());
            if (completed && updated >= completed) {
                const auto delay = updated - completed;
                if (observedModule == i + 1)
                    observedUpdate = updated;
                if (!p_->displayPeak[i].updated || delay > p_->displayMaxDelayUs[i]) {
                    p_->displayMaxDelayUs[i] = delay;
                    p_->displayPeak[i] = {id, completed, updated, p_->tabs->currentIndex(),
                                          isVisible(), isMinimized()};
                }
                if (delay > (i == 0 ? 16667ull : 20000ull))
                    ++p_->displayLate[i];
            }
        }
    }
    return observedUpdate;
}

void V2Window::refreshSender(bool force) {
    if (!p_->sending)
        return;
    const auto now = monotonicUs();
    const bool finished = !p_->session.busy();
    if (!finished && !force &&
        ((!p_->senderStats->isVisible() && !p_->acceptance->isVisible()) ||
         now - p_->lastSenderPollUs < 500000))
        return;
    p_->lastSenderPollUs = now;
    const auto result = finished ? p_->session.wait() : p_->session.snapshot();
    if (!result.reportJson.empty())
        presentSenderReport(
            QJsonDocument::fromJson(QByteArray::fromStdString(result.reportJson)).object());
    if (finished) {
        p_->nextNdi = result.nextNdiId;
        p_->nextEp = result.nextEpId;
        p_->nextStatus = result.nextStatusSequence;
        p_->sending = false;
        p_->stop->setText(QStringLiteral("停止发送"));
        p_->archivePending = true;
        p_->archiveDueUs = now + 200000;
        setSenderControlsLocked(false);
        p_->runState->setText(result.success ? QStringLiteral("已停止，正在整理本轮报告")
                                             : QStringLiteral("已停止，请查看错误及验收"));
        if (!result.error.empty())
            p_->log->appendPlainText(QString::fromStdString(result.error));
    }
    p_->senderRefresh.add(monotonicUs() - now);
}

void V2Window::refresh() {
    refreshSender();
    refreshReceiver(ui::WidgetFrameDrain::Timer);
    if (!p_->receiverRunning && p_->receiverWorker.joinable()) {
        p_->receiverWorker.join();
        refreshReceiver();
        p_->receive->setText(QStringLiteral("启动接收"));
        p_->receive->setEnabled(true);
        p_->receiverBind->setEnabled(true);
        p_->receiverPort->setEnabled(true);
        if (!p_->receiveState->text().startsWith(QStringLiteral("接收停止")))
            p_->receiveState->setText(QStringLiteral(
                "接收已停止。重启会清空本地会话；请与发送端协调并换用新端口隔离旧报文。"));
    }
    if (p_->archivePending && monotonicUs() >= p_->archiveDueUs)
        archiveRun();
}

bool V2Window::event(QEvent *event) {
    if (event->type() == WidgetFramesReady::eventType()) {
        const auto &ready = *static_cast<const WidgetFramesReady *>(event);
        if (ready.wake)
            ready.wake->eventEntered.store(monotonicUs());
        if (ready.generation == p_->receiverGeneration)
            refreshReceiver(ui::WidgetFrameDrain::FrameEvent, ready.wake ? ready.wake->sequence : 0);
        return true;
    }
    return QMainWindow::event(event);
}

void V2Window::refreshReceiver(ui::WidgetFrameDrain drain, std::uint64_t drainWakeSequence) {
    const bool framesOnly = drain == ui::WidgetFrameDrain::FrameEvent;
    const auto refreshStarted = monotonicUs();
    std::optional<ReceiverUpdate> latest;
    std::deque<Impl::WidgetFrame> frames;
    std::uint64_t batchTaken = 0;
    std::shared_ptr<const ui::WidgetWakeTiming> batchWake;
    QString message;
#ifdef SIM_MODEL_PREVIEW
    std::optional<BxPreview> preview;
    const bool previewVisible =
        !framesOnly && p_->preview &&
        (!p_->receiverRunning || (p_->preview->isVisible() && !isMinimized()));
#endif
    {
        std::lock_guard<std::mutex> lock(p_->receiverMutex);
        if (p_->options.fragmentTrace && !p_->widgetFrames.empty())
            batchWake = p_->pendingWidgetWake;
        if (framesOnly) {
            p_->widgetWakePosted = false;
            p_->pendingWidgetWake.reset();
        } else {
            latest.swap(p_->receiverLatest);
            message.swap(p_->receiverError);
#ifdef SIM_MODEL_PREVIEW
            if (previewVisible)
                preview = p_->previewQueue.take(refreshStarted, p_->preview->busy(),
                                                p_->receiverRunning);
#endif
        }
        frames.swap(p_->widgetFrames);
        if (p_->options.fragmentTrace && !frames.empty())
            batchTaken = monotonicUs();
    }
    p_->receiverSnapshot.add(monotonicUs() - refreshStarted);
    for (const auto &frame : frames) {
        const auto index = frame.module - 1;
        const auto started = monotonicUs();
        const auto completed =
            static_cast<std::uint64_t>(frame.view["host_completed_us"].toInteger());
        auto view = p_->lastDisplay;
        view["stopped"] = false;
        view[frame.module == 1 ? "ndi" : "ep"] = frame.view;
        const auto updated = applyReceiverView(view, frame.module);
        if (frame.enqueuedUs && updated)
            p_->worstWidgetFrame[index].retainWorst({
                p_->displayedIds[index], frame.queueDepth, drain, completed, frame.enqueuedUs,
                batchTaken, started, updated, batchWake, drainWakeSequence, frame.inheritedWake});
        p_->displayMaxWaitUs[index] = std::max(p_->displayMaxWaitUs[index], started - completed);
        p_->displayMaxApplyUs[index] =
            std::max(p_->displayMaxApplyUs[index], monotonicUs() - started);
    }
    if (latest && latest->generation == p_->receiverGeneration) {
        const auto presentationStarted = monotonicUs();
        presentReceiverReport(displayReport(*latest, p_->lastDisplay));
        p_->presentedUpdate = std::move(latest);
        p_->receiverPresentation.add(monotonicUs() - presentationStarted);
    }
    if (!message.isEmpty())
        p_->receiveState->setText(message);
#ifdef SIM_MODEL_PREVIEW
    if (preview) {
        const auto logStarted = monotonicUs();
        p_->log->appendPlainText(
            QStringLiteral("NDI 对象 #%1 传输完成（%2 字节，主机完成 %3 μs），提交 BX 解码。")
                .arg(preview->frameId)
                .arg(preview->bytes.size())
                .arg(preview->completedUs));
        p_->previewLog.add(monotonicUs() - logStarted);
        const auto requestStarted = monotonicUs();
        p_->preview->showBxObject(preview->bytes,
                                  QStringLiteral("网络接收 · NDI 帧 #%1").arg(preview->frameId));
        p_->previewRequest.add(monotonicUs() - requestStarted);
    }
#endif
    p_->receiverRefresh.add(monotonicUs() - refreshStarted);
}

QJsonObject V2Window::receiverReportForExport() const {
    QJsonObject report;
    if (p_->presentedUpdate) {
        const auto bytes =
            QByteArray::fromStdString(v2::receiverJson(p_->presentedUpdate->snapshot));
        report = QJsonDocument::fromJson(bytes).object();
    }
    for (auto it = p_->receiverReport.constBegin(); it != p_->receiverReport.constEnd(); ++it)
        report.insert(it.key(), it.value());
    if (report.isEmpty())
        return report;
    report["display"] = p_->lastDisplay;
    std::array<std::uint64_t, 2> drops;
    {
        std::lock_guard<std::mutex> lock(p_->receiverMutex);
        drops = p_->widgetQueueDrops;
    }
    QJsonArray rows;
    for (unsigned i = 0; i < 2; ++i) {
        QJsonObject row{
            {"module_id", int(i + 1)},
            {"widget_updated_frames", static_cast<qint64>(p_->displayedFrames[i])},
            {"widget_queue_drops", static_cast<qint64>(drops[i])},
            {"max_complete_to_gui_start_us", static_cast<qint64>(p_->displayMaxWaitUs[i])},
            {"max_gui_apply_us", static_cast<qint64>(p_->displayMaxApplyUs[i])},
            {"max_complete_to_widget_update_us", static_cast<qint64>(p_->displayMaxDelayUs[i])},
            {"updates_after_one_module_period", static_cast<qint64>(p_->displayLate[i])}};
        if (p_->options.fragmentTrace)
            row["worst_frame"] = p_->worstWidgetFrame[i].json();
        const auto &peak = p_->displayPeak[i];
        if (peak.updated)
            row["max_update_endpoint"] = QJsonObject{
                {"frame_id", qint64(peak.frameId)}, {"completed_us", qint64(peak.completed)},
                {"widget_updated_us", qint64(peak.updated)}, {"tab_index", peak.tab},
                {"visible", peak.visible}, {"minimized", peak.minimized}};
        else
            row["max_update_endpoint"] = QJsonValue::Null;
        rows.append(row);
    }
    report["ui_observations"] = QJsonObject{
        {"modules", rows},
        {"measurement",
         "Host receive-complete to Qt widget data update; not physical screen presentation"},
        {"max_update_endpoint_scope", "Window state observed at the update endpoint. "
                                      "Tab 0=sender, 1=receiver, 2=report."},
        {"physical_render_status", "not_tested"}};
    auto profileJson = [](const StageProfile &stage) {
        return QJsonObject{{"calls", static_cast<qint64>(stage.calls)},
                           {"total_us", static_cast<qint64>(stage.totalUs)},
                           {"max_us", static_cast<qint64>(stage.maxUs)}};
    };
    QJsonObject work{{"sender_refresh", profileJson(p_->senderRefresh)},
                     {"receiver_refresh", profileJson(p_->receiverRefresh)},
                     {"receiver_snapshot", profileJson(p_->receiverSnapshot)},
                     {"receiver_presentation", profileJson(p_->receiverPresentation)},
                     {"preview_log", profileJson(p_->previewLog)},
                     {"preview_request", profileJson(p_->previewRequest)}};
#ifdef SIM_MODEL_PREVIEW
    if (p_->preview)
        work["model_preview"] = p_->preview->timingReport();
#endif
    report["ui_work"] = work;
    return report;
}

void V2Window::archiveRun() {
    if (!p_->archivePending)
        return;
    p_->archiveDueUs = monotonicUs() + 30000000;
    const auto directory = p_->options.archiveDirectory.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/v2-runs"
        : p_->options.archiveDirectory;
    const auto id = p_->senderReport["run_id"].toString();
    if (id.isEmpty() || !QDir().mkpath(directory)) {
        p_->log->appendPlainText(QStringLiteral("自动归档失败，请手动导出当前报告。"));
        return;
    }
    QString destination = directory + "/" + id + ".json";
    for (unsigned suffix = 1; QFileInfo::exists(destination); ++suffix)
        destination = directory + "/" + id + "-" + QString::number(suffix) + ".json";
    const auto bytes = QJsonDocument(reportForExport()).toJson();
    QSaveFile f(destination);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) {
        p_->log->appendPlainText(QStringLiteral("自动归档失败，请手动导出：") + f.errorString());
        return;
    }
    p_->archivePending = false;
    p_->runState->setText(QStringLiteral("已停止，本轮报告已独立归档"));
    p_->log->appendPlainText(QStringLiteral("本轮档案：") + QDir::toNativeSeparators(destination));
}

void V2Window::exportReport() {
    const auto file = QFileDialog::getSaveFileName(this, QStringLiteral("导出 V2 收发报告"),
                                                   "sim-v2-report.json", "JSON (*.json)");
    if (file.isEmpty())
        return;
    const auto out = reportForExport();
    const auto bytes = QJsonDocument(out).toJson();
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
        p_->log->appendPlainText(QStringLiteral("报告保存失败：") + f.errorString());
    else {
        if (!p_->sending)
            p_->archivePending = false;
        p_->log->appendPlainText(QStringLiteral("已导出：") + file);
    }
}

QJsonObject V2Window::reportForExport() const {
    const auto sender = p_->sending
                            ? QJsonDocument::fromJson(
                                  QByteArray::fromStdString(p_->session.snapshot().reportJson)).object()
                            : p_->senderReport;
    const auto rx = receiverReportForExport();
    QJsonObject out{
        {"report_schema_version", 1},
        {"product_version", QString::fromLatin1(SIM_PRODUCT_VERSION)},
        {"protocol_version", 2},
        {"sender", sender},
        {"receiver", rx},
        {"exported_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"note", QStringLiteral("本文件保留本地收发证据；累计收包不抵消跳采或首发超期。真实硬件时钟"
                                "、物理出线及真实电生理业务仍需独立验证。")}};
    auto rows = sender["acceptance"].toArray();
    for (const auto &row : rx["acceptance"].toArray())
        rows.append(row);
    const auto &base = p_->receiverBaseline;
    const auto localPort = sender["local_port"].toInt();
    const auto &c = p_->baselineConfig;
    const auto expectedEndpoint =
        QString::fromStdString(c.bindIp) + ":" + QString::number(localPort);
    const auto receiverEnd = rx["observed_host_us"].toString().toULongLong();
    const auto senderEnd = sender["ended_host_us"].toString().toULongLong();
    const bool loopbackTarget = c.targetIp.rfind("127.", 0) == 0 && c.bindIp.rfind("127.", 0) == 0;
    const auto receiverBind = rx["receiver_bind"].toString();
    const bool boundTarget =
        receiverBind == "0.0.0.0" || receiverBind == QString::fromStdString(c.targetIp);
    const bool eligible =
        p_->baselineReady && !p_->sending && sender["finished"].toBool() &&
        p_->baselineRun == sender["run_id"].toString() &&
        p_->baselineGeneration == p_->receiverGeneration &&
        rx["receiver_generation"].toInteger(-1) == static_cast<qint64>(p_->baselineGeneration) &&
        loopbackTarget && boundTarget && rx["receiver_port"].toInt() == c.targetPort &&
        localPort > 0 && rx["source_endpoint"].toString() == expectedEndpoint &&
        rx["source_rejected"].toInteger() == 0 && receiverEnd >= senderEnd &&
        receiverEnd - senderEnd >= 100000;
    const auto sm = sender["modules"].toObject();
    const auto rm = rx["modules"].toObject();
    const auto bm = base["modules"].toObject();
    auto count = [](const QJsonValue &v) -> std::optional<std::uint64_t> {
        if (v.isDouble() && v.toInteger(-1) >= 0)
            return static_cast<std::uint64_t>(v.toInteger());
        if (v.isString()) {
            bool valid = false;
            const auto value = v.toString().toULongLong(&valid);
            if (valid)
                return value;
        }
        return std::nullopt;
    };
    for (unsigned i = 0; i < 2; ++i) {
        const auto key = QString::number(i + 1);
        const auto s = sm[key].toObject();
        const auto r = rm[key].toObject();
        const auto b = bm[key].toObject();
        const auto sl = s["ledger"].toObject();
        const auto rl = r["ledger"].toObject();
        const auto bl = b["ledger"].toObject();
        const auto built = count(s["built"]);
        auto delta = [&](const QJsonValue &after,
                         const QJsonValue &before) -> std::optional<std::uint64_t> {
            const auto end = count(after);
            const auto start = count(before);
            if (!eligible || !end || !start || *end < *start)
                return std::nullopt;
            return *end - *start;
        };
        const auto received = delta(rl["count"], bl["count"]);
        const auto consumed = delta(r["consumed"], b["consumed"]);
        bool complete = built && count(sl["count"]) && received && consumed;
        bool agrees = true;
        for (const auto *field : {"id_sum", "crc_sum", "stamp_sum"}) {
            const auto end = count(rl[field]);
            const auto start = count(bl[field]);
            const auto sent = count(sl[field]);
            if (!end || !start || !sent)
                complete = false;
            else if (*end - *start != *sent)
                agrees = false; // additive checksums wrap modulo 2^64
        }
        const auto status = !complete || !*built ? QStringLiteral("not_tested")
                                                 : (agrees && count(sl["count"]) == built &&
                                                            received == built && consumed == built
                                                        ? QStringLiteral("pass")
                                                        : QStringLiteral("fail"));
        auto number = [](std::optional<std::uint64_t> v) {
            return v ? QString::number(*v) : QStringLiteral("未检测");
        };
        rows.append(QJsonObject{
            {"id", "gui_local_joint_delivery"},
            {"module_id", int(i + 1)},
            {"requirement", i == 0 ? "NDI local paired receive and consume"
                                   : "EP local paired receive and consume"},
            {"measurement", "Same GUI-run receiver baseline delta; loopback endpoint and receiver "
                            "generation checked; uint64 count/ID/CRC32/timestamp aggregates"},
            {"threshold",
             "Built, completed and consumed counts equal; all bounded ledger aggregates agree"},
            {"measured", QStringLiteral("built=%1; completed_delta=%2; consumed_delta=%3")
                             .arg(number(built))
                             .arg(number(received))
                             .arg(number(consumed))},
            {"status", status},
            {"reason",
             !complete
                 ? QStringLiteral("未建立完整同轮证据：需接收先启动且无在途帧、本机端点匹配、接收未"
                                  "重启、发送结束后至少观察100ms；仅run_id相同不构成线上会话证据。")
                 : QStringLiteral("采用有界汇总对账，不是逐帧字节相同的数学证明；不抵消跳采、首发超"
                                  "期，真实硬件与物理链路另验。")}});
        const auto updates = p_->displayedFrames[i];
        rows.append(QJsonObject{
            {"id", "gui_widget_updates"},
            {"module_id", int(i + 1)},
            {"requirement", i == 0 ? "NDI Qt widget update" : "EP Qt widget update"},
            {"measurement", "Host complete-to-widget-data-update time; distinct observed frames; "
                            "physical presentation not measured"},
            {"threshold", i == 0 ? "<=16667us and consumed frames observed"
                                 : "<=20000us and consumed frames observed"},
            {"measured", QStringLiteral("updates=%1; consumed_delta=%2; max=%3us; late=%4")
                             .arg(updates)
                             .arg(number(consumed))
                             .arg(p_->displayMaxDelayUs[i])
                             .arg(p_->displayLate[i])},
            {"status", !complete || !*consumed
                           ? "not_tested"
                           : (updates == consumed && !p_->displayLate[i] ? "pass" : "fail")},
            {"reason", "Qt data updates are software evidence only; unknown real EP waveform "
                       "layout and hardware screen presentation remain not_tested"}});
    }
    out["acceptance"] = rows;
    out["joint_scope"] =
        QJsonObject{{"eligible", eligible},
                    {"receiver_generation", static_cast<qint64>(p_->receiverGeneration)},
                    {"baseline_generation", static_cast<qint64>(p_->baselineGeneration)},
                    {"baseline_run_id", p_->baselineRun},
                    {"expected_source_endpoint", expectedEndpoint}};
    return out;
}

void V2Window::closeEvent(QCloseEvent *event) {
    p_->session.stop();
    p_->receiverStop = true;
#ifdef SIM_MODEL_PREVIEW
    {
        std::lock_guard<std::mutex> lock(p_->receiverMutex);
        p_->previewQueue.close();
    }
    if (p_->preview)
        p_->preview->stop();
#endif
    event->accept();
}

void V2Window::toggleReceiver() {
    if (p_->receiverRunning) {
        p_->receiverStop = true;
#ifdef SIM_MODEL_PREVIEW
        {
            std::lock_guard<std::mutex> lock(p_->receiverMutex);
            p_->previewQueue.finish(p_->preview->fileTaskActive());
        }
#endif
        p_->receive->setEnabled(false);
        p_->receiveState->setText(QStringLiteral("正在停止接收…"));
        return;
    }
    if (p_->receiverWorker.joinable())
        p_->receiverWorker.join();
    std::uint32_t bindAddress = 0;
    try {
        bindAddress = parseIpv4(p_->receiverBind->text().trimmed());
    } catch (const std::exception &) {
        p_->receiveState->setText(QStringLiteral("请输入有效的 IPv4 绑定地址。"));
        return;
    }
    const auto port = static_cast<quint16>(p_->receiverPort->value());
    p_->receiverStop = false;
    p_->receiverRunning = true;
    p_->receiverBind->setEnabled(false);
    p_->receiverPort->setEnabled(false);
    p_->receive->setText(QStringLiteral("停止接收"));
    p_->receiveState->setText(QStringLiteral("接收准备中；只接受本次首个有效数据源的同一端点。"));
    p_->lastDisplay = {};
    p_->displayedFrames = {};
    p_->displayMaxDelayUs = {};
    p_->displayPeak = {};
    p_->displayMaxWaitUs = {};
    p_->displayMaxApplyUs = {};
    p_->displayLate = {};
    p_->worstWidgetFrame = {};
    p_->haveDisplayed = {};
    p_->senderRefresh = {};
    p_->receiverRefresh = {};
    p_->receiverSnapshot = {};
    p_->receiverPresentation = {};
    p_->previewLog = {};
    p_->previewRequest = {};
    p_->toolsTable->setRowCount(0);
    p_->epState->setText(QStringLiteral("电生理尚未收到"));
    p_->wave->showSamples({});
    p_->statusState->setText(QStringLiteral("状态尚未收到"));
    p_->association->setText(QStringLiteral("等待两个模块的新数据"));
    const auto generation = ++p_->receiverGeneration;
    p_->autoPortKey.clear();
    p_->receiverReport = {};
    p_->presentedUpdate.reset();
    p_->baselineReady = false;
    {
        std::lock_guard<std::mutex> lock(p_->receiverMutex);
        p_->receiverLatest.reset();
        p_->widgetFrames.clear();
        p_->widgetQueueDrops = {};
        p_->widgetWakePosted = false;
        p_->pendingWidgetWake.reset();
        p_->receiverError.clear();
#ifdef SIM_MODEL_PREVIEW
        p_->previewQueue.begin();
#endif
    }
#ifdef SIM_MODEL_PREVIEW
    p_->preview->beginReceiveSession();
#endif
    p_->receiverWorker = std::thread([this, bindAddress, port, generation] {
        try {
            GuiReceiverSocket socket(bindAddress, port);
            v2::Receiver receiver(true, [] {
                return monotonicUs();
            });
            receiver.setFrameConsumer(
                [this, generation, &receiver](const v2::Frame &frame, std::uint64_t completed) {
                    auto view =
                        frame.moduleId == 1 ? ndiDisplay(receiver.latestNdi()) : epDisplay(frame);
                    view["frame_id"] = static_cast<qint64>(frame.frameId);
                    view["host_completed_us"] = static_cast<qint64>(completed);
                    view["stale"] = false;
#ifdef SIM_MODEL_PREVIEW
                    std::optional<BxPreview> preview;
                    if (p_->preview && frame.moduleId == 1)
                        preview.emplace(
                            BxPreview{QByteArray(reinterpret_cast<const char *>(frame.bytes.data()),
                                                 qsizetype(frame.bytes.size())),
                                      completed, frame.frameId});
#endif
                    bool notify = false;
                    std::shared_ptr<ui::WidgetWakeTiming> wake;
                    {
                        std::lock_guard<std::mutex> lock(p_->receiverMutex);
#ifdef SIM_MODEL_PREVIEW
                        if (preview)
                            p_->previewQueue.offer(preview);
#endif
                        if (p_->widgetFrames.size() >= 64) {
                            ++p_->widgetQueueDrops[frame.moduleId - 1];
                            return;
                        }
                        p_->widgetFrames.push_back({frame.moduleId, std::move(view)});
                        notify = !p_->widgetWakePosted;
                        if (p_->options.fragmentTrace) {
                            auto &item = p_->widgetFrames.back();
                            item.queueDepth = static_cast<unsigned>(p_->widgetFrames.size());
                            item.enqueuedUs = monotonicUs();
                            item.inheritedWake = !notify;
                            if (notify) {
                                wake = std::make_shared<ui::WidgetWakeTiming>(
                                    ++p_->nextWakeSequence, frame.moduleId, frame.frameId);
                                p_->pendingWidgetWake = wake;
                            }
                        }
                        p_->widgetWakePosted = true;
                    }
                    if (notify) {
                        auto *event = new WidgetFramesReady(generation, wake);
                        if (wake)
                            wake->postStarted.store(monotonicUs());
                        QCoreApplication::postEvent(this, event, Qt::HighEventPriority);
                        if (wake)
                            wake->postReturned.store(monotonicUs());
                    }
                });
            std::uint32_t peer = 0;
            quint16 peerPort = 0;
            bool pinned = false;
            std::uint64_t rejectedSources = 0;
            std::uint64_t feedbackErrors = 0;
            v2::ReceiveLoopStats loop;
            std::uint64_t nextPublish = 0;
            const auto bindAddressText = formatIpv4(bindAddress);
            QString peerAddress;
            auto publish = [&](bool final) {
                const auto now = monotonicUs();
                std::optional<ReceiverUpdate> update{ReceiverUpdate{
                    receiver.snapshot(now), bindAddressText, peerAddress, QStringLiteral("asio"),
                    port, peerPort, generation, now, rejectedSources, feedbackErrors, loop, final}};
                std::lock_guard<std::mutex> lock(p_->receiverMutex);
                p_->receiverLatest.swap(update); // Release the overwritten frame after unlocking.
            };
            auto sendFeedback = [&](const std::vector<Datagram> &packets) {
                if (packets.empty())
                    return;
                // One observation per generated group of NACKs. Data may belong to another frame.
                const auto readable = socket.readableNow();
                if (!readable.has_value())
                    ++loop.nackBatchesReadabilityUnknown;
                else if (*readable)
                    ++loop.nackBatchesReadable;
                else
                    ++loop.nackBatchesNotReadable;
                for (const auto &packet : packets) {
                    if (p_->receiverStop)
                        break;
                    if (!socket.sendFeedback(packet, peer, peerPort))
                        ++feedbackErrors;
                }
            };
            {
                std::lock_guard<std::mutex> lock(p_->receiverMutex);
                p_->receiverError =
                    QStringLiteral(
                        "接收已启动（Boost.Asio）：%1:%2；NDI / 电生理独立消费，状态即时应用。")
                        .arg(bindAddressText)
                        .arg(port);
            }
            std::array<std::uint8_t, 1473> buffer;
            auto readOne = [&] {
                std::uint32_t source = 0;
                quint16 sourcePort = 0;
                const auto length = socket.read(buffer.data(), buffer.size(), source, sourcePort);
                if (length < 0)
                    return false;
                if (pinned && (source != peer || sourcePort != peerPort)) {
                    ++rejectedSources;
                    return true;
                }
                const auto *bytes = buffer.data();
                if (!pinned) {
                    try {
                        if (length < 8)
                            throw ProtocolError("short packet");
                        if (bytes[6] == 1)
                            (void)v2::decodeData(bytes, static_cast<std::size_t>(length));
                        else if (bytes[6] == 3)
                            (void)v2::decodeStatus(bytes, static_cast<std::size_t>(length));
                        else
                            throw ProtocolError("not data/status");
                        peer = source;
                        peerAddress = formatIpv4(source);
                        peerPort = sourcePort;
                        pinned = true;
                    } catch (const ProtocolError &) {
                        receiver.receive(bytes, static_cast<std::size_t>(length), monotonicUs());
                        return true;
                    }
                }
                sendFeedback(receiver.receiveAndPollObserved(
                    bytes, static_cast<std::size_t>(length), monotonicUs()));
                return true;
            };
            while (!p_->receiverStop) {
                socket.waitReadable(std::min(receiver.nextDeadlineUs(), nextPublish));
                v2::serviceReceiveBatch(
                    loop, monotonicUs,
                    [&] {
                        return p_->receiverStop.load();
                    },
                    readOne,
                    [&] {
                        sendFeedback(receiver.poll(monotonicUs()));
                    });
                const auto now = monotonicUs();
                if (now >= nextPublish) {
                    publish(false);
                    nextPublish = now + 5000;
                }
            }
            publish(true);
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lock(p_->receiverMutex);
            p_->receiverError = QStringLiteral("接收停止：") + QString::fromUtf8(e.what());
        }
        p_->receiverRunning = false;
    });
}
} // namespace rt
