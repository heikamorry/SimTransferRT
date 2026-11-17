#pragma once
#include "v2/Config.h"
#include <QMainWindow>
#include <QJsonObject>
#include <memory>
namespace rt {
class V2Window final : public QMainWindow {
public:
    explicit V2Window(QWidget* parent=nullptr);
    ~V2Window() override;
    v2::Config selectedConfig() const;
    // Shared presentation entry points also used by deterministic UI tests.
    void presentSenderReport(const QJsonObject& report);
    void presentReceiverReport(const QJsonObject& report);
    void setSenderControlsLocked(bool locked);
    QJsonObject reportForExport() const;
protected:
    void closeEvent(QCloseEvent*) override;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    void startSending();
    void stopSending();
    void toggleReceiver();
    void refresh();
    void updateBudget();
    void exportReport();
    void archiveRun();
};
}
