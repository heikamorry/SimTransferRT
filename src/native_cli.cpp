#include "application/StreamRunner.h"
#include "network/NativeUdpTransport.h"
#include "support/CliOptions.h"
#include "support/V2CliOptions.h"
#include "v2/Runner.h"
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
            const auto result=rt::v2::run(options.config,[]{return std::make_unique<rt::NativeUdpTransport>();},
                                          []{return interrupted!=0;},options.handoffTriggerReport,options.fragmentTriggerReport);
            std::cout<<result.reportJson<<'\n';
            if(!options.report.empty()){
                std::ofstream out(std::filesystem::u8path(options.report),std::ios::binary);
                if(!out||!(out<<result.reportJson<<'\n'))throw std::runtime_error("cannot save V2 report");
            }
            return result.success?0:2;
        }
        // No version flag retains all legacy script defaults; explicit v1 is equivalent.
        std::vector<char*> legacyArgs;
        for(int i=0;i<argc;++i){if(std::string(argv[i])=="--protocol"){++i;continue;}legacyArgs.push_back(argv[i]);}
        argc=static_cast<int>(legacyArgs.size());argv=legacyArgs.data();
        auto o=rt::parseOptions(argc,argv);
        if(o.help) {
            std::cout<<rt::cliHelp();
            return 0;
        }
        std::signal(SIGINT,signalHandler);
        std::signal(SIGTERM,signalHandler);
#ifdef SIGBREAK
        std::signal(SIGBREAK,signalHandler);
#endif
        rt::NativeUdpTransport udp;
        rt::CommandDispatcher dispatcher;
        rt::RunnerHooks hooks;
        // A flag read must remain available even if every sampling deadline is
        // overdue and the runner defers ordinary control/feedback work.
        hooks.cancelRequested=[] {return interrupted!=0;};
        const auto final=rt::runStream(o.config,udp,dispatcher,[](const auto&) {},[](const auto&s) {
            std::cerr<<s<<'\n';
        },hooks);
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
