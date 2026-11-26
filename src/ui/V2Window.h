#pragma once
#include "v2/Config.h"
#include "ui/WidgetFrameTiming.h"
#include <QMainWindow>
#include <QJsonObject>
#include <memory>

namespace rt {
struct V2WindowRunOptions {
    bool fragmentTrace = false;
    bool asioPooledHandler = false;
};

class V2Window final : public QMainWindow {
  public:
    explicit V2Window(QWidget *parent = nullptr, V2WindowRunOptions options = {});
    ~V2Window() override;
    v2::Config selectedConfig() const;
    // Shared presentation entry points also used by deterministic UI tests.
    void presentSenderReport(const QJsonObject &report);
    void presentReceiverReport(const QJsonObject &report);
    void setSenderControlsLocked(bool locked);
    QJsonObject reportForExport() const;

  protected:
    bool event(QEvent *) override;
    void closeEvent(QCloseEvent *) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    QWidget *createHeader();
    QWidget *createSendPage();
    QWidget *createReceivePage();
    QWidget *createReportPage();
    QWidget *createLogCard();
    void initializeControls();
    void startSending();
    void stopSending();
    void toggleReceiver();
    void refresh();
    void refreshSender(bool force = false);
    void refreshReceiver(ui::WidgetFrameDrain drain = ui::WidgetFrameDrain::Explicit,
                         std::uint64_t drainWakeSequence = 0);
    std::uint64_t applyReceiverView(const QJsonObject &, unsigned observedModule = 0);
    QJsonObject receiverReportForExport() const;
    void updateBudget();
    void exportReport();
    void archiveRun();
};
} // namespace rt
