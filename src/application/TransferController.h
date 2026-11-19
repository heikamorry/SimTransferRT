#pragma once
#include "application/SessionManager.h"
namespace rt {
    class TransferController final:public QObject {
        Q_OBJECT
        public:
        explicit TransferController(QObject* parent=nullptr);
        void start(const StreamConfig& config);
        void stop();
        void applyMask(std::uint32_t mask);
        void shutdown();
        bool busy()const {
            return manager_.busy();
        }
        signals:
        void snapshotChanged(const rt::Snapshot&);
        void logMessage(const QString&);
        void rejected(const QString&);
        void runEnded();
        private:SessionManager manager_;
    };
}
