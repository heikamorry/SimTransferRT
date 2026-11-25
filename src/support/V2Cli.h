#pragma once
#include <string>

namespace rt::v2 {
using ReportWriter = void (*)(const std::string &path, const std::string &json);
int runCli(int argc, char **argv, ReportWriter saveReport, const char *errorPrefix);
} // namespace rt::v2
