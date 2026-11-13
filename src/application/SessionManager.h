#pragma once
#include "application/StreamRunner.h"
#include <QObject>
#include <QMetaType>
#include <QTimer>
#include <atomic>
#include <mutex>
#include <QThread>
#include <deque>
Q_DECLARE_METATYPE(rt::Snapshot)
namespace rt {
    // QObject stays on the GUI/application thread. All network work lives in worker_.
    // A bounded mailbox decouples high-rate I/O from 10Hz UI snapshots.
    class SessionManager final:public QObject {
        Q_OBJECT
        public:
        explicit SessionManager(QObject* parent=nullptr);
        ~SessionManager()override;
        void begin(const StreamConfig& config);
        void stop();
        void applyMask(std::uint32_t mask);
        void shutdown();
        bool busy()const noexcept {
            return busy_;
        }
        const std::deque<Snapshot>& history()const noexcept {
            return history_;
        }
        signals:
        void snapshotChanged(const rt::Snapshot& snapshot);
        void logMessage(const QString& message);
        void runEnded();
        private:
        CommandDispatcher dispatcher_;
        QTimer timer_;
        std::unique_ptr<QThread> worker_;
        std::atomic<bool> done_ {
            true
        };
        std::mutex mutex_;
        Snapshot latest_;
        bool dirty_=false,busy_=false;
        std::deque<std::string> logs_;
        std::deque<Snapshot> history_;
        void publish(const Snapshot& s);
        void log(const std::string& s);
        void poll();
    };
}
