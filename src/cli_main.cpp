#include "application/TransferController.h"
#include "support/CliOptions.h"
#include "support/V2CliOptions.h"
#include "v2/Runner.h"
#include "network/QtUdpTransport.h"
#include <QCoreApplication>
#include <QTimer>
#include <QSaveFile>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <iostream>
#include <csignal>
namespace {
    volatile std::sig_atomic_t interrupted=0;
    void signalHandler(int) {
        interrupted=1;
    }
}
int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);
    app.setOrganizationName("SimTransferLab");
    app.setApplicationName("SimTransferRTCli");
    try {
        bool useV2=false;
        for(int i=1;i<argc;++i)if(std::string(argv[i])=="--protocol"){
            if(i+1>=argc)throw std::invalid_argument("--protocol requires v1 or v2");
            const std::string version=argv[++i];
            if(version!="v1"&&version!="v2")throw std::invalid_argument("--protocol requires v1 or v2");
            useV2=version=="v2";
        }
        if(useV2){
            const auto options=rt::v2::parseV2Options(argc,argv);
            if(options.help){std::cout<<rt::v2::v2CliHelp();return 0;}
            std::signal(SIGINT,signalHandler);std::signal(SIGTERM,signalHandler);
#ifdef SIGBREAK
            std::signal(SIGBREAK,signalHandler);
#endif
            const auto result=rt::v2::run(options.config,[]{return std::make_unique<rt::QtUdpTransport>();},[]{return interrupted!=0;});
            std::cout<<result.reportJson<<'\n';
            if(!options.report.empty()){
                QSaveFile f(QString::fromStdString(options.report));const auto bytes=QByteArray::fromStdString(result.reportJson);
                if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size()||!f.commit())throw std::runtime_error("cannot save V2 report");
            }
            return result.success?0:2;
        }
        std::vector<char*> legacyArgs;
        for(int i=0;i<argc;++i){if(std::string(argv[i])=="--protocol"){++i;continue;}legacyArgs.push_back(argv[i]);}
        argc=static_cast<int>(legacyArgs.size());argv=legacyArgs.data();
        const auto o=rt::parseOptions(argc,argv);
        if(o.help) {
            std::cout<<rt::cliHelp();
            return 0;
        }
        rt::TransferController controller;
        const auto startedUtc=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        rt::Snapshot last;
        QObject::connect(&controller,&rt::TransferController::snapshotChanged,&app,[&](const rt::Snapshot&s) {
            last=s;
        });
        QObject::connect(&controller,&rt::TransferController::logMessage,&app,[](const QString&s) {
            qInfo().noquote()<<s;
        });
        QObject::connect(&controller,&rt::TransferController::rejected,&app,[&](const QString&s) {
            qCritical().noquote()<<s;app.exit(2);
        });
        QObject::connect(&controller,&rt::TransferController::runEnded,&app,[&] {
            auto report=QJsonDocument::fromJson(QByteArray::fromStdString(rt::snapshotJson(last))).object();
            report["qt_version"]=qVersion();report["started_utc"]=startedUtc;
            report["ended_utc"]=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
            const auto data=QJsonDocument(report).toJson();std::cout<<data.constData();bool failed=last.stats.sendErrors||last.detail.find("Local failure:")==0;
            if(!o.report.empty()) {
                QSaveFile f(QString::fromStdString(o.report));if(!f.open(QIODevice::WriteOnly)||f.write(data)!=data.size()||!f.commit()) {
                    qCritical()<<"Cannot save report";failed=true;
                }
            }
            app.exit(failed?2:0);
        });
        std::signal(SIGINT,signalHandler);
        std::signal(SIGTERM,signalHandler);
#ifdef SIGBREAK
        std::signal(SIGBREAK,signalHandler);
#endif
        QTimer stopPoll;
        stopPoll.setInterval(100);
        QObject::connect(&stopPoll,&QTimer::timeout,&app,[&] {
            if(interrupted)controller.stop();
        });
        stopPoll.start();
        QTimer::singleShot(0,&app,[&] {
            controller.start(o.config);
        });
        return app.exec();
    }
    catch(const std::exception&e) {
        std::cerr<<e.what()<<'\n';
        return 2;
    }
}
