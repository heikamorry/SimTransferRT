#include "application/StreamRunner.h"
#include "network/NativeUdpTransport.h"
#include "support/CliOptions.h"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <csignal>
#include <atomic>
namespace {
    volatile std::sig_atomic_t interrupted=0;
    void signalHandler(int) {
        interrupted=1;
    }
}
int main(int argc,char**argv) {
    try {
        auto o=rt::parseOptions(argc,argv);
        if(o.help) {
            std::cout<<rt::cliHelp();
            return 0;
        }
        std::signal(SIGINT,signalHandler);
        std::signal(SIGTERM,signalHandler);
        rt::NativeUdpTransport udp;
        rt::CommandDispatcher dispatcher;
        const auto final=rt::runStream(o.config,udp,dispatcher,[&](const auto&) {
            if(interrupted) {
                rt::Command c;c.kind=rt::Command::Kind::Stop;dispatcher.post(c);
            }
        },[](const auto&s) {
            std::cerr<<s<<'\n';
        });
        const auto report=rt::snapshotJson(final);
        std::cout<<report;
        if(!o.report.empty()) {
            std::ofstream out(std::filesystem::u8path(o.report));
            if(!out)throw std::runtime_error("cannot open report file");
            out<<report;
        }
        return final.stats.sendErrors?2:0;
    }
    catch(const std::exception&e) {
        std::cerr<<"ERROR: "<<e.what()<<'\n';
        return 2;
    }
}
