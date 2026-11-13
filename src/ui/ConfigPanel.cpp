#include "ui/ConfigPanel.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QSettings>
namespace rt {
    ConfigPanel::ConfigPanel(QWidget*p):QWidget(p) {
        auto*root=new QVBoxLayout(this);
        root->setContentsMargins(12,12,12,12);
        auto*title=new QLabel(QStringLiteral("发送与采样配置"));
        title->setObjectName("sectionTitle");
        root->addWidget(title);
        tabs_=new QTabWidget;
        root->addWidget(tabs_);
        net_=new QWidget;
        auto*f=new QFormLayout(net_);
        f->setLabelAlignment(Qt::AlignLeft);
        target_=new QLineEdit("127.0.0.1");
        bind_=new QLineEdit("127.0.0.1");
        source_=new QLineEdit("127.0.0.1");
        auto mkPort=[](int v) {
            auto*s=new QSpinBox;
            s->setRange(1,65535);
            s->setValue(v);
            return s;
        };
        port_=mkPort(5650);
        cmdPort_=mkPort(5500);
        sourcePort_=mkPort(1);
        sourcePort_->setRange(0,65535);
        sourcePort_->setValue(0);
        sourcePort_->setSpecialValueText(QStringLiteral("不限来源端口"));
        f->addRow(QStringLiteral("数据目标 IP"),target_);
        f->addRow(QStringLiteral("数据目标端口"),port_);
        f->addRow(QStringLiteral("本地绑定 IP"),bind_);
        f->addRow(QStringLiteral("NACK 监听端口"),cmdPort_);
        f->addRow(QStringLiteral("NACK 来源 IP"),source_);
        source_->setToolTip(QStringLiteral("默认只接收目标主机反馈；* 表示允许任意来源，仅用于隔离测试网"));
        f->addRow(QStringLiteral("NACK 来源端口"),sourcePort_);
        f->addRow(QStringLiteral("采样周期"),new QLabel(QStringLiteral("20 ms / 50 Hz（固定）")));
        f->addRow(QStringLiteral("首轮发送窗口"),new QLabel(QStringLiteral("15 ms（计划目标）")));
        f->addRow(QStringLiteral("分片有效数据"),new QLabel(QStringLiteral("1436 B（固定）")));
        f->addRow(QStringLiteral("最大 UDP 载荷"),new QLabel(QStringLiteral("1472 B，DF 必须开启")));
        logic_=new QLineEdit("1");
        f->addRow(QStringLiteral("起始 logic_id"),logic_);
        limit_=new QSpinBox;
        limit_->setRange(0,1000000);
        limit_->setSpecialValueText(QStringLiteral("持续运行"));
        f->addRow(QStringLiteral("本地采样次数"),limit_);
        link_=new QComboBox;
        for(int n: {
            100,1000,2500,10000
        })link_->addItem(QString::number(n)+" Mbit/s",n);
        link_->setCurrentIndex(1);
        f->addRow(QStringLiteral("实际链路速率"),link_);
        cache_=new QComboBox;
        cache_->addItem(QStringLiteral("原文：logic_id % 3"));
        cache_->addItem(QStringLiteral("候选勘误：FIFO 三帧"));
        f->addRow(QStringLiteral("缓存策略"),cache_);
        overload_=new QCheckBox(QStringLiteral("允许超带宽格式测试（不保证15ms）"));
        f->addRow(overload_);
        tabs_->addTab(net_,QStringLiteral("网络"));
        auto*modulesPage=new QWidget;
        auto*ml=new QVBoxLayout(modulesPage);
        auto*help=new QLabel(QStringLiteral("模块体为不透明测试字节；未定义波形/坐标的业务编码。运行中只允许切换在线状态。"));
        help->setWordWrap(true);
        ml->addWidget(help);
        modules_=new QTableWidget(32,4);
        modules_->setHorizontalHeaderLabels( {
            QStringLiteral("在线"),"ID",QStringLiteral("体长度 B"),QStringLiteral("数据文件（空=模拟）")
        });
        modules_->verticalHeader()->hide();
        modules_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);
        modules_->horizontalHeader()->setSectionResizeMode(1,QHeaderView::ResizeToContents);
        modules_->horizontalHeader()->setSectionResizeMode(2,QHeaderView::ResizeToContents);
        modules_->horizontalHeader()->setStretchLastSection(true);
        for(int i=0;i<32;++i) {
            auto*online=new QTableWidgetItem;
            online->setFlags(Qt::ItemIsEnabled|Qt::ItemIsUserCheckable);
            online->setCheckState(i==0?Qt::Checked:Qt::Unchecked);
            modules_->setItem(i,0,online);
            auto*id=new QTableWidgetItem(QString::number(i));
            id->setFlags(Qt::ItemIsEnabled);
            modules_->setItem(i,1,id);
            modules_->setItem(i,2,new QTableWidgetItem(i==0?"65531":"0"));
            modules_->setItem(i,3,new QTableWidgetItem);
        }
        ml->addWidget(modules_,1);
        auto*buttons=new QHBoxLayout;
        for(auto n: {
            65536u,1048576u,2097152u
        }) {
            auto*b=new QPushButton(n==65536?QStringLiteral("64KiB"):n==1048576?QStringLiteral("1MiB"):QStringLiteral("2MiB"));
            buttons->addWidget(b);
            connect(b,&QPushButton::clicked,this,[this,n] {
                if(!running_)preset(n);
            });
        }
        ml->addLayout(buttons);
        auto*file=new QPushButton(QStringLiteral("为选中模块加载文件"));
        ml->addWidget(file);
        connect(file,&QPushButton::clicked,this,&ConfigPanel::chooseModuleFile);
        auto*apply=new QPushButton(QStringLiteral("应用在线位图（下一采样生效）"));
        ml->addWidget(apply);
        connect(apply,&QPushButton::clicked,this,[this] {
            emit maskRequested(selectedMask());
        });
        tabs_->addTab(modulesPage,QStringLiteral("模块"));
        fault_=new QWidget;
        auto*ff=new QFormLayout(fault_);
        drop_=new QLineEdit;
        always_=new QLineEdit;
        corrupt_=new QLineEdit;
        duplicate_=new QLineEdit;
        reverse_=new QCheckBox(QStringLiteral("逆序首发（故障测试，不符合正常顺序）"));
        ff->addRow(QStringLiteral("每帧首轮丢片"),drop_);
        ff->addRow(QStringLiteral("持续丢片"),always_);
        ff->addRow(QStringLiteral("首轮损坏"),corrupt_);
        ff->addRow(QStringLiteral("首轮重复"),duplicate_);
        ff->addRow(reverse_);
        auto*fh=new QLabel(QStringLiteral("片号从0开始，逗号分隔，例如 2,4。\n故障作用于每帧；正常联调请全部清空。\n损坏发生在计算CRC之后。"));
        fh->setWordWrap(true);
        ff->addRow(fh);
        tabs_->addTab(fault_,QStringLiteral("故障"));
        QSettings s;
        target_->setText(s.value("rt/target","127.0.0.1").toString());
        bind_->setText(s.value("rt/bind","127.0.0.1").toString());
        source_->setText(s.value("rt/source",target_->text()).toString());
        port_->setValue(s.value("rt/port",5650).toInt());
        cmdPort_->setValue(s.value("rt/commandPort",5500).toInt());
        logic_->setText(s.value("rt/nextId","1").toString());
    }
    void ConfigPanel::preset(std::uint32_t size) {
        for(int i=0;i<32;++i) {
            modules_->item(i,0)->setCheckState(i==0?Qt::Checked:Qt::Unchecked);
            modules_->item(i,2)->setText(i==0?QString::number(size-5):"0");
            modules_->item(i,3)->setText("");
        }
    }
    void ConfigPanel::chooseModuleFile() {
        if(running_)return;
        int row=modules_->currentRow();
        if(row<0)row=0;
        const auto path=QFileDialog::getOpenFileName(this,QStringLiteral("加载模块原始业务数据体"));
        if(path.isEmpty())return;
        modules_->item(row,3)->setText(path);
        modules_->item(row,2)->setText(QString::number(QFileInfo(path).size()));
        modules_->item(row,0)->setCheckState(Qt::Checked);
    }
    std::uint32_t ConfigPanel::selectedMask()const {
        std::uint32_t m=0;
        for(int i=0;i<32;++i)if(modules_->item(i,0)->checkState()==Qt::Checked)m|=std::uint32_t(1)<<i;
        return m;
    }
    StreamConfig ConfigPanel::config()const {
        StreamConfig c;
        c.targetIp=target_->text().trimmed().toStdString();
        c.bindIp=bind_->text().trimmed().toStdString();
        c.allowedNackIp=source_->text().trimmed().toStdString();
        c.targetPort=static_cast<std::uint16_t>(port_->value());
        c.commandPort=static_cast<std::uint16_t>(cmdPort_->value());
        c.allowedNackPort=static_cast<std::uint16_t>(sourcePort_->value());
        bool ok=false;
        auto id=logic_->text().toULongLong(&ok,0);
        if(!ok||id>0xFFFFFFFFull)throw std::invalid_argument("logic_id must be uint32");
        c.initialLogicId=static_cast<std::uint32_t>(id);
        c.frameLimit=static_cast<std::uint64_t>(limit_->value());
        c.linkMbps=link_->currentData().toUInt();
        c.allowOversubscribed=overload_->isChecked();
        c.cachePolicy=cache_->currentIndex()==0?CachePolicy::SpecModulo:CachePolicy::FifoErratum;
        c.modules.clear();
        for(int i=0;i<32;++i) {
            auto n=modules_->item(i,2)->text().toUInt(&ok);
            if(!ok||n>MaxFrameBytes-5)throw std::invalid_argument("module body size must be 0..2097147");
            ModuleConfig m;
            m.id=static_cast<std::uint8_t>(i);
            m.online=modules_->item(i,0)->checkState()==Qt::Checked;
            m.dataBytes=n;
            m.file=modules_->item(i,3)->text().toStdString();
            c.modules.push_back(std::move(m));
        }
        c.faults.dropFirst=parseFragmentIds(drop_->text().toStdString());
        c.faults.dropAlways=parseFragmentIds(always_->text().toStdString());
        c.faults.corruptFirst=parseFragmentIds(corrupt_->text().toStdString());
        c.faults.duplicateFirst=parseFragmentIds(duplicate_->text().toStdString());
        c.faults.reverseInitial=reverse_->isChecked();
        QSettings s;
        s.setValue("rt/target",target_->text());
        s.setValue("rt/bind",bind_->text());
        s.setValue("rt/source",source_->text());
        s.setValue("rt/port",port_->value());
        s.setValue("rt/commandPort",cmdPort_->value());
        return c;
    }
    void ConfigPanel::setRunning(bool busy) {
        running_=busy;
        net_->setEnabled(!busy);
        fault_->setEnabled(!busy);
        for(int row=0;row<32;++row)for(int col: {
            2,3
        })modules_->item(row,col)->setFlags(busy?Qt::ItemIsSelectable|Qt::ItemIsEnabled:Qt::ItemIsSelectable|Qt::ItemIsEnabled|Qt::ItemIsEditable);
    }
    void ConfigPanel::setNextId(std::uint32_t id) {
        logic_->setText(QString::number(id));
        QSettings s;
        s.setValue("rt/nextId",QString::number(id));
    }
}
