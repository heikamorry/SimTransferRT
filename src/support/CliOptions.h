#pragma once
#include "core/Types.h"
namespace rt {
    struct CliOptions {
        StreamConfig config;
        std::string report;
        bool help=false;
    };
    CliOptions parseOptions(int argc,char** argv);
    std::string cliHelp();
}
