#pragma once
#include "v2/Config.h"

namespace rt::v2 {
struct CliOptions {
    Config config;
    std::string transport;
    std::string report;
    std::string handoffTriggerReport;
    std::string fragmentTriggerReport;
    bool help = false;
};

CliOptions parseV2Options(int argc, char **argv);
std::string v2CliHelp();
} // namespace rt::v2
