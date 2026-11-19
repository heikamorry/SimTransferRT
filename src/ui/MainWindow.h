#pragma once
#include "application/TransferController.h"
#include <QMainWindow>
#include <QMap>
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QCloseEvent;
namespace rt {
    class ConfigPanel;
    class FrameTableModel;
    class MainWindow final:public QMainWindow {
        Q_OBJECT
        public:explicit MainWindow(QWidget*p=nullptr);
        ~MainWindow()override;
        protected:void closeEvent(QCloseEvent*)override;
        private:
        ConfigPanel* config_;
        FrameTableModel* model_;
        QPlainTextEdit*log_;
        QLabel*state_;
        QPushButton*start_;
        QPushButton*stop_;
        QMap<QString,QLabel*>metrics_;
        TransferController controller_;
        Snapshot latest_;
        void start();
        void apply(const Snapshot&);
        void appendLog(const QString&);
        void exportReport();
    };
}
