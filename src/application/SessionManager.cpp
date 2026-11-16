#include "application/SessionManager.h"
#include "network/QtUdpTransport.h"
namespace rt {
    SessionManager::SessionManager(QObject* p):QObject(p) {
        timer_.setInterval(100);
        connect(&timer_,&QTimer::timeout,this,[this] {
            poll();
        });
    }
    SessionManager::~SessionManager() {
        shutdown();
    }
    void SessionManager::publish(const Snapshot&s) {
        std::shared_ptr<const Snapshot> next=std::make_shared<const Snapshot>(s);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            latest_.swap(next);
            dirty_=true;
        } // Old detail storage is released outside the lock.
    }
    void SessionManager::log(const std::string&s) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(logs_.size()==128)logs_.pop_front();
        logs_.push_back(s);
    }
    void SessionManager::begin(const StreamConfig& c) {
        if(busy_)throw std::runtime_error("a stream is already running");
        if(worker_) {
            worker_->wait();
            worker_.reset();
        }
        dispatcher_.clear();
        Snapshot preparing;
        preparing.nextLogicId=c.initialLogicId;
        preparing.config=c;preparing.runId=c.runId;
        preparing.running=true;
        preparing.detail="Preparing module data (no wire handshake)";
        publish(preparing);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            logs_.clear();
        }
        busy_=true;
        done_=false;
        timer_.start();
        try {
            worker_.reset(QThread::create([this,c] {
                try {
                    QtUdpTransport udp; // Created, used and destroyed exclusively on this worker thread.
                    runStream(c,udp,dispatcher_,[this](const Snapshot&s) {
                        publish(s);
                    },[this](const std::string&s) {
                        log(s);
                    });
                }
                catch(const std::exception&e) {
                    std::shared_ptr<const Snapshot> previous; {
                        std::lock_guard<std::mutex> lock(mutex_);previous=latest_;
                    }
                    Snapshot failure=*previous;
                    failure.running=false;failure.detail=std::string("Local failure: ")+e.what();publish(failure);log(failure.detail);
                }
                catch(...) {
                    Snapshot failure;failure.detail="Local failure: unknown exception";publish(failure);log(failure.detail);
                }
                done_.store(true,std::memory_order_release);
            }));
            worker_->start();
        }
        catch(...) {
            busy_=false;
            done_=true;
            timer_.stop();
            throw;
        }
        poll();
    }
    void SessionManager::stop() {
        Command c;
        c.kind=Command::Kind::Stop;
        dispatcher_.post(std::move(c));
    }
    void SessionManager::applyMask(std::uint32_t mask) {
        if(!busy_)return;
        Command c;
        c.kind=Command::Kind::SetMask;
        c.mask=mask;
        if(!dispatcher_.post(std::move(c)))emit logMessage(QStringLiteral("控制队列已满，模块变更未接受"));
    }
    void SessionManager::poll() {
        std::shared_ptr<const Snapshot> snapshot;
        bool dirty=false;
        std::deque<std::string> logs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dirty=dirty_;
            if(dirty)snapshot=latest_;
            dirty_=false;
            logs.swap(logs_);
        }
        for(const auto&l:logs)emit logMessage(QString::fromStdString(l));
        if(dirty)emit snapshotChanged(*snapshot);
        if(busy_&&done_.load(std::memory_order_acquire)) {
            if(worker_) {
                worker_->wait();
                worker_.reset();
            }
            busy_=false;
            timer_.stop();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot=latest_;
            }
            Snapshot summary=*snapshot;
            summary.frames.clear();
            summary.timingTrace={}; // History retains counters, not anomaly event payloads.
            history_.push_back(std::move(summary));
            if(history_.size()>20)history_.pop_front();
            emit snapshotChanged(*snapshot);
            emit runEnded();
        }
    }
    void SessionManager::shutdown() {
        if(worker_) {
            stop();
            worker_->wait();
            worker_.reset();
        }
        done_=true;
        busy_=false;
        timer_.stop();
        dispatcher_.clear();
    }
}
