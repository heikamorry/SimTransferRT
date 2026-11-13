#include "ui/MainWindow.h"
#include "ui/ConfigPanel.h"
#include "ui/FrameTableModel.h"
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QTableView>
#include <QHeaderView>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QFileDialog>
#include <QSaveFile>
#include <QMessageBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QStatusBar>
#include <QSettings>
namespace rt {
    MainWindow::MainWindow(QWidget*p):QMainWindow(p),controller_(this) {
        setWindowTitle(QStringLiteral("SimTransfer RT · 多模块实时 UDP 发送平台"));
        resize(1500,920);
        setMinimumSize(1180,720);
        auto*root=new QWidget;
        setCentralWidget(root);
        auto*layout=new QVBoxLayout(root);
        layout->setContentsMargins(20,16,20,16);
        layout->setSpacing(12);
        auto*head=new QHBoxLayout;
        auto*brand=new QLabel("SimTransfer RT");
        brand->setObjectName("brand");
        head->addWidget(brand);
        head->addStretch();
        auto*badge=new QLabel(QStringLiteral("SIMULATED TS / 接收结果未知"));
        badge->setObjectName("badge");
        head->addWidget(badge);
        layout->addLayout(head);
        auto*actions=new QHBoxLayout;
        start_=new QPushButton(QStringLiteral("开始实时发送"));
        start_->setObjectName("primary");
        stop_=new QPushButton(QStringLiteral("停止发送"));
        stop_->setEnabled(false);
        actions->addWidget(start_);
        actions->addWidget(stop_);
        state_=new QLabel(QStringLiteral("就绪"));
        state_->setObjectName("state");
        actions->addWidget(state_);
        actions->addStretch();
        auto*exportB=new QPushButton(QStringLiteral("导出本地报告"));
        actions->addWidget(exportB);
        auto*clear=new QPushButton(QStringLiteral("清空日志"));
        actions->addWidget(clear);
        layout->addLayout(actions);
        auto*vertical=new QSplitter(Qt::Vertical);
        auto*body=new QSplitter(Qt::Horizontal);
        config_=new ConfigPanel;
        config_->setObjectName("card");
        config_->setMinimumWidth(350);
        body->addWidget(config_);
        auto*center=new QWidget;
        center->setObjectName("card");
        auto*cl=new QVBoxLayout(center);
        auto*ct=new QLabel(QStringLiteral("最近缓存帧 · 分片监视（10Hz刷新）"));
        ct->setObjectName("sectionTitle");
        cl->addWidget(ct);
        auto*table=new QTableView;
        model_=new FrameTableModel(this);
        table->setModel(model_);
        table->verticalHeader()->hide();
        table->setAlternatingRowColors(true);
        table->setShowGrid(false);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(3,QHeaderView::Stretch);
        cl->addWidget(table,1);
        body->addWidget(center);
        auto*stats=new QWidget;
        stats->setObjectName("card");
        auto*sl=new QVBoxLayout(stats);
        auto*st=new QLabel(QStringLiteral("实时统计"));
        st->setObjectName("sectionTitle");
        sl->addWidget(st);
        const QList<QPair<QString,QString>> items= {
            {
                "frames",QStringLiteral("已构建逻辑帧")
            }, {
                "packets",QStringLiteral("本地接受的数据报")
            }, {
                "nacks",QStringLiteral("CRC通过的NACK")
            }, {
                "retry",QStringLiteral("实际补发数据报")
            }, {
                "miss",QStringLiteral("NACK缓存未命中")
            }, {
                "invalid",QStringLiteral("非法反馈 / 来源拒绝")
            }, {
                "skip",QStringLiteral("离线 / 超长 / 调度跳采")
            }, {
                "drops",QStringLiteral("故障注入丢片")
            }, {
                "expired",QStringLiteral("首轮未发即过期")
            }, {
                "late",QStringLiteral("首轮发送超期次数")
            }, {
                "jitter",QStringLiteral("最大采样调度迟到")
            }, {
                "duration",QStringLiteral("最近首轮实际跨度")
            }, {
                "port",QStringLiteral("NACK监听 / DF")
            }
        };
        for(const auto&item:items) {
            auto*label=new QLabel(item.second);
            label->setObjectName("muted");
            sl->addWidget(label);
            auto*value=new QLabel("—");
            value->setObjectName("metric");
            sl->addWidget(value);
            metrics_.insert(item.first,value);
        }
        sl->addStretch();
        auto*scroll=new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setWidget(stats);
        scroll->setMinimumWidth(230);
        body->addWidget(scroll);
        body->setStretchFactor(1,1);
        body->setSizes( {
            370,850,240
        });
        vertical->addWidget(body);
        auto*logBox=new QWidget;
        logBox->setObjectName("card");
        auto*ll=new QVBoxLayout(logBox);
        ll->addWidget(new QLabel(QStringLiteral("运行日志")));
        log_=new QPlainTextEdit;
        log_->setReadOnly(true);
        log_->setMaximumBlockCount(2000);
        ll->addWidget(log_);
        vertical->addWidget(logBox);
        vertical->setSizes( {
            650,180
        });
        layout->addWidget(vertical,1);
        connect(start_,&QPushButton::clicked,this,&MainWindow::start);
        connect(stop_,&QPushButton::clicked,&controller_,&TransferController::stop);
        connect(clear,&QPushButton::clicked,log_,&QPlainTextEdit::clear);
        connect(exportB,&QPushButton::clicked,this,&MainWindow::exportReport);
        connect(config_,&ConfigPanel::maskRequested,&controller_,&TransferController::applyMask);
        connect(&controller_,&TransferController::snapshotChanged,this,&MainWindow::apply);
        connect(&controller_,&TransferController::logMessage,this,&MainWindow::appendLog);
        connect(&controller_,&TransferController::rejected,this,[this](const QString&e) {
            config_->setRunning(false);start_->setEnabled(true);stop_->setEnabled(false);QMessageBox::warning(this,QStringLiteral("不能开始"),e);
        });
        connect(&controller_,&TransferController::runEnded,this,[this] {
            start_->setEnabled(true);stop_->setEnabled(false);config_->setRunning(false);config_->setNextId(latest_.nextLogicId);
        });
        QSettings s;
        restoreGeometry(s.value("rt/geometry").toByteArray());
        appendLog(QStringLiteral("新协议：直接发 DATA；NACK→缓存补发。无需接收端握手。"));
        appendLog(QStringLiteral("默认严格采用原文缓存索引；FIFO为可选勘误。计时精度需要目标Windows和网卡实测。"));
    }
    MainWindow::~MainWindow() {
        controller_.shutdown();
    }
    void MainWindow::start() {
        try {
            const auto c=config_->config();
            start_->setEnabled(false);
            stop_->setEnabled(true);
            config_->setRunning(true);
            controller_.start(c);
        }
        catch(const std::exception&e) {
            QMessageBox::warning(this,QStringLiteral("配置错误"),QString::fromUtf8(e.what()));
        }
    }
    void MainWindow::appendLog(const QString&text) {
        log_->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss.zzz")+"  "+text);
    }
    void MainWindow::apply(const Snapshot&s) {
        latest_=s;
        model_->apply(s);
        state_->setText(s.running?(s.draining?QStringLiteral("缓存补发收尾"):QStringLiteral("发送中")):(s.detail.find("Local failure:")==0?QStringLiteral("本地故障"):QStringLiteral("已停止 / 本地结束")));
        metrics_["frames"]->setText(QString::number(s.stats.built));
        metrics_["packets"]->setText(QString::number(s.stats.datagrams));
        metrics_["nacks"]->setText(QString::number(s.stats.nacks));
        metrics_["retry"]->setText(QString::number(s.stats.retransmissions));
        metrics_["miss"]->setText(QString::number(s.stats.cacheMisses));
        metrics_["invalid"]->setText(QString::number(s.stats.invalidNacks)+" / "+QString::number(s.stats.sourceRejected));
        metrics_["skip"]->setText(QString::number(s.stats.skippedNoModules)+" / "+QString::number(s.stats.buildRejected)+" / "+QString::number(s.stats.skippedOverrun));
        metrics_["drops"]->setText(QString::number(s.stats.injectedDrops));
        metrics_["expired"]->setText(QString::number(s.stats.unsentExpired));
        metrics_["late"]->setText(QString::number(s.stats.deadlineMisses));
        metrics_["jitter"]->setText(QString::number(s.stats.maxSampleLatenessUs)+" us");
        metrics_["duration"]->setText(QString::number(s.stats.lastInitialDurationUs)+" us");
        metrics_["port"]->setText(QString::number(s.localPort)+(s.dfEnabled?" / ON":" / OFF"));
        statusBar()->showMessage(QString::fromStdString(s.detail));
    }
    void MainWindow::exportReport() {
        const auto path=QFileDialog::getSaveFileName(this,QStringLiteral("导出发送端本地统计"),"sim-rt-report.json","JSON (*.json)");
        if(path.isEmpty())return;
        QSaveFile f(path);
        const auto data=QByteArray::fromStdString(snapshotJson(latest_));
        if(!f.open(QIODevice::WriteOnly)||f.write(data)!=data.size()||!f.commit())QMessageBox::warning(this,QStringLiteral("保存失败"),f.errorString());
        else appendLog(QStringLiteral("报告已保存：")+path);
    }
    void MainWindow::closeEvent(QCloseEvent*e) {
        controller_.shutdown();
        QSettings s;
        s.setValue("rt/geometry",saveGeometry());
        e->accept();
    }
}
