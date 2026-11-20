#include "support/V2CliOptions.h"
#include "v2/Runner.h"
#include "network/V2TransportSelection.h"
#include <QCoreApplication>
#include <QSaveFile>
#include <QByteArray>
#include <iostream>
#include <csignal>
#include <stdexcept>
namespace {
volatile std::sig_atomic_t interrupted=0;
void signalHandler(int) { interrupted=1; }
}
int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);
    app.setOrganizationName("SimTransferLab");
    app.setApplicationName("SimTransferRTCli");
    try {
        const auto options=rt::v2::parseV2Options(argc,argv);
        if(options.help){std::cout<<rt::v2::v2CliHelp();return 0;}
        std::signal(SIGINT,signalHandler);std::signal(SIGTERM,signalHandler);
#ifdef SIGBREAK
        std::signal(SIGBREAK,signalHandler);
#endif
        const auto result=rt::v2::run(options.config,rt::v2::selectTransport(options.transport),
                                      []{return interrupted!=0;},options.handoffTriggerReport,options.fragmentTriggerReport);
        std::cout<<result.reportJson<<'\n';
        if(!options.report.empty()){
            QSaveFile f(QString::fromStdString(options.report));
            const auto bytes=QByteArray::fromStdString(result.reportJson);
            if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size()||!f.commit())
                throw std::runtime_error("cannot save V2 report");
        }
        return result.success?0:2;
    }
    catch(const std::exception&e) {
        std::cerr<<e.what()<<'\n';
        return 2;
    }
}
