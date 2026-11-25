#include "support/V2Cli.h"
#include <QByteArray>
#include <QCoreApplication>
#include <QSaveFile>
#include <stdexcept>

namespace {
void saveReport(const std::string &path, const std::string &json) {
    QSaveFile output(QString::fromStdString(path));
    const auto bytes = QByteArray::fromStdString(json);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() ||
        !output.commit())
        throw std::runtime_error("cannot save V2 report");
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setOrganizationName("SimTransferLab");
    app.setApplicationName("SimTransferRTCli");
    return rt::v2::runCli(argc, argv, saveReport, "");
}
