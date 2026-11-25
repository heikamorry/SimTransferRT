#include "support/V2Cli.h"
#include "support/V2CliOptions.h"
#include "network/V2TransportSelection.h"
#include <csignal>
#include <iostream>

namespace rt::v2 {
namespace {
volatile std::sig_atomic_t interrupted = 0;

void signalHandler(int) {
    interrupted = 1;
}
} // namespace

int runCli(int argc, char **argv, ReportWriter saveReport, const char *errorPrefix) {
    try {
        const auto options = parseV2Options(argc, argv);
        if (options.help) {
            std::cout << v2CliHelp();
            return 0;
        }
        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);
#ifdef SIGBREAK
        std::signal(SIGBREAK, signalHandler);
#endif
        const auto result = run(
            options.config, selectTransport(options.transport),
            [] {
                return interrupted != 0;
            },
            options.handoffTriggerReport, options.fragmentTriggerReport);
        std::cout << result.reportJson << '\n';
        if (!options.report.empty())
            saveReport(options.report, result.reportJson);
        return result.success ? 0 : 2;
    } catch (const std::exception &error) {
        std::cerr << errorPrefix << error.what() << '\n';
        return 2;
    }
}
} // namespace rt::v2
