#include "ui/V2Window.h"
#include "support/V2CliOptions.h"
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QDir>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool test,const char* message) {if(!test)throw std::runtime_error(message);}
rt::v2::CliOptions parse(std::initializer_list<const char*> args) {
    std::vector<std::string> strings{"SimTransferCli"};for(auto a:args)strings.emplace_back(a);
    std::vector<char*> pointers;for(auto& s:strings)pointers.push_back(s.data());
    return rt::v2::parseV2Options(static_cast<int>(pointers.size()),pointers.data());
}
void rejects(std::initializer_list<const char*> args) {bool rejected=false;try{(void)parse(args);}catch(const std::exception&){rejected=true;}require(rejected,"invalid CLI config accepted");}
template<class T> T* child(rt::V2Window& w,const char* name) {auto* p=w.findChild<T*>(name);require(p,"UI control missing");return p;}
bool until(const std::function<bool()>& condition,int timeout=5000) {QElapsedTimer timer;timer.start();while(timer.elapsed()<timeout){QApplication::processEvents();if(condition())return true;QThread::msleep(5);}return condition();}
void cli() {
    require(parse({"--protocol","v2"}).config.linkMbps==1000,"CLI default must be 1G independent of GUI settings");
    for(const auto& p:std::vector<std::pair<const char*,unsigned>>{{"100m",100},{"1g",1000},{"2g5",2500},{"2.5g",2500}})
        require(parse({"--protocol","v2","--network-profile",p.first}).config.linkMbps==p.second,"profile alias failed");
    require(parse({"--network-profile","100m","--link-mbps","100"}).config.linkMbps==100,"matching aliases rejected");
    rejects({"--network-profile","100m","--link-mbps","1000"});rejects({"--link-mbps","1000","--network-profile","100m"});
    rejects({"--network-profile","1g","--network-profile","2g5"});rejects({"--link-mbps","10000"});rejects({"--protocol","v1"});
    rejects({"--duration","nan"});rejects({"--duration","-1"});rejects({"--duration","1x"});rejects({"--ep-bytes","0"});rejects({"--ndi-tools","256"});
    rejects({"--port","0"});rejects({"--fault-module","0"});rejects({"--network-profile"});rejects({"--no-ndi","--no-ep"});
    const auto o=parse({"--protocol","v2","--duration","0.125","--target","192.0.2.10","--command-port","0","--ep-bytes","200000","--ndi-tools","40","--observed-link-mbps","100","--path-limit-mbps","1000"});
    require(o.config.durationUs==125000 && o.config.allowedNackIp==o.config.targetIp,"duration/source defaults failed");
    require(rt::v2::effectiveMbps(o.config)==100 && o.config.epBytes==200000,"effective capacity or exact EP length lost");
}
void settingsAndLocking() {
    QSettings().clear();
    {
        rt::V2Window w;auto* profile=child<QComboBox>(w,"v2NetworkProfile");require(profile->count()==3 && profile->currentData().toUInt()==1000,"GUI first default must be 1G with exactly three profiles");
        profile->setCurrentIndex(profile->findData(2500));require(w.selectedConfig().linkMbps==2500,"GUI profile not in run config");
        w.setSenderControlsLocked(true);require(!profile->isEnabled(),"profile editable during preparation/running/stopping");
        require(!child<QPushButton>(w,"v2Start")->isEnabled(),"second start allowed while busy");
        w.setSenderControlsLocked(false);require(profile->isEnabled(),"profile stayed locked after worker completion");
    }
    {rt::V2Window w;require(w.selectedConfig().linkMbps==2500,"profile not restored");}
    QSettings().setValue("v2/networkMbps",10000);
    {rt::V2Window w;require(w.selectedConfig().linkMbps==1000,"invalid stored profile not reset to 1G");}
}
void presentation() {
    rt::V2Window w;
    QJsonArray rows{QJsonObject{{"requirement","sampling"},{"measurement","all intervals"},{"threshold","<1000us"},{"measured",1000},{"status","fail"},{"reason","threshold reached"}},QJsonObject{{"requirement","hardware"},{"status","not_tested"},{"reason","no common hardware clock"}}};
    w.presentSenderReport(QJsonObject{{"acceptance",rows}});
    auto* acceptance=child<QTableWidget>(w,"v2Acceptance");require(acceptance->columnCount()==6 && acceptance->rowCount()==2,"six acceptance columns missing");
    require(acceptance->item(0,4)->text()==QStringLiteral("失败") && acceptance->item(1,4)->text()==QStringLiteral("未检测"),"failed/unmeasured row rendered as pass");
    const QJsonObject tool{{"handle",1},{"state","valid"},{"position",QJsonArray{10.0,20.0,30.0}},{"quaternion",QJsonArray{1.0,0.0,0.0,0.0}},{"quality",0.0},{"source_frame",8},{"port_status",0}};
    QJsonObject ndi{{"stale",false},{"tools",QJsonArray{tool}}};QJsonObject view{{"ndi",ndi},{"ep",QJsonObject{{"frame_id",3},{"bytes",204800},{"stale",false}}},{"status",QJsonObject{{"sequence",5},{"text","ready"}}}};
    w.presentReceiverReport(QJsonObject{{"display",view}});auto* tools=child<QTableWidget>(w,"v2NdiToolsTable");
    require(tools->rowCount()==1 && tools->item(0,2)->text().contains("10"),"valid position not shown");
    auto missing=tool;missing["state"]="missing";ndi["tools"]=QJsonArray{missing};view["ndi"]=ndi;w.presentReceiverReport(QJsonObject{{"display",view}});
    require(tools->item(0,2)->text()==QStringLiteral("无有效位置"),"Missing reused old or zero coordinate");
    ndi["tools"]=QJsonArray{tool};ndi["stale"]=true;view["ndi"]=ndi;w.presentReceiverReport(QJsonObject{{"display",view}});
    require(tools->item(0,1)->text()==QStringLiteral("已过期") && tools->item(0,2)->text()==QStringLiteral("无有效位置"),"stale position presented as fresh");
    require(child<QLabel>(w,"v2StatusState")->text().contains("ready"),"independent status not applied");
    for(const auto& row:w.reportForExport()["acceptance"].toArray()) {const auto r=row.toObject();if(r["id"].toString()=="gui_local_joint_delivery")require(r["status"].toString()=="not_tested","unpaired GUI reports falsely accepted");}
}
void liveLifecycle() {
    QSettings().clear();rt::V2Window w;w.show();
    const QDir archive(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/v2-runs");const auto archivedBefore=archive.entryList({"*.json"},QDir::Files).size();
    QUdpSocket reserve;require(reserve.bind(QHostAddress::LocalHost,0),"cannot obtain local test port");const auto port=reserve.localPort();reserve.close();
    child<QSpinBox>(w,"v2ReceiverPort")->setValue(port);child<QSpinBox>(w,"v2TargetPort")->setValue(port);child<QSpinBox>(w,"v2CommandPort")->setValue(0);child<QSpinBox>(w,"v2EpBytes")->setValue(512);child<QSpinBox>(w,"v2Duration")->setValue(1);
    auto* receive=child<QPushButton>(w,"v2Receive");receive->click();
    require(until([&]{return w.reportForExport()["receiver"].toObject()["receiver_port"].toInt()==port;}),"receiver failed to publish ready baseline");
    auto* start=child<QPushButton>(w,"v2Start");start->click();require(!child<QComboBox>(w,"v2NetworkProfile")->isEnabled(),"real run did not lock profile");
    require(until([&]{return child<QTableWidget>(w,"v2NdiToolsTable")->rowCount()>0;}),"formal C++ receiver/GUI did not consume BX");
    require(until([&]{return start->isEnabled();}),"timed run failed to stop and unlock");
    require(until([&]{return w.reportForExport()["joint_scope"].toObject()["eligible"].toBool();}),"same-window loopback joint scope was not established");
    int jointlyChecked=0;
    for(const auto& row:w.reportForExport()["acceptance"].toArray()) {const auto r=row.toObject();if(r["id"].toString()=="gui_local_joint_delivery") {require(r["status"].toString()=="pass","paired bounded ledgers/consumer counts disagree");++jointlyChecked;}}
    require(jointlyChecked==2,"joint export omitted one module");
    auto* stats=child<QTableWidget>(w,"v2SenderStats");require(stats->item(0,1)->text().toULongLong()>0 && stats->item(1,1)->text().toULongLong()>0,"independent modules did not run");
    child<QComboBox>(w,"v2NetworkProfile")->setCurrentIndex(0);start->click();child<QPushButton>(w,"v2Stop")->click();
    require(!child<QComboBox>(w,"v2NetworkProfile")->isEnabled(),"profile unlocked before Stop join");
    require(until([&]{return start->isEnabled();}),"quick Stop did not complete");
    receive->click();require(until([&]{return child<QSpinBox>(w,"v2ReceiverPort")->isEnabled();}),"receiver Stop did not complete");
    receive->click();require(until([&]{return !child<QSpinBox>(w,"v2ReceiverPort")->isEnabled();}),"receiver restart failed");
    require(!w.reportForExport()["joint_scope"].toObject()["eligible"].toBool(),"receiver restart reused prior session baseline");
    receive->click();require(until([&]{return child<QSpinBox>(w,"v2ReceiverPort")->isEnabled();}),"receiver second Stop failed");
    require(until([&]{return archive.entryList({"*.json"},QDir::Files).size()>=archivedBefore+2;}),"two runs were not independently archived");
}
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);app.setOrganizationName("SimTransferTests");app.setApplicationName("V2UiTests");
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    try {cli();settingsAndLocking();presentation();liveLifecycle();std::cout<<"V2 CLI, profile persistence, stop locking, consumer presentation and live lifecycle passed\n";return 0;}
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
