#include "support/V2Cli.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
void saveReport(const std::string &path, const std::string &json) {
    std::ofstream output(std::filesystem::u8path(path), std::ios::binary);
    output << json << '\n';
    output.close();
    if (!output)
        throw std::runtime_error("cannot save V2 report");
}
} // namespace

int main(int argc, char **argv) {
    return rt::v2::runCli(argc, argv, saveReport, "ERROR: ");
}
