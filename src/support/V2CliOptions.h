#pragma once
#include "v2/Config.h"
namespace rt::v2 {
struct CliOptions {
    Config config;
    std::string report;
    bool help=false;
};
CliOptions parseV2Options(int argc, char** argv);
std::string v2CliHelp();
}
