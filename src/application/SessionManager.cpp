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
        std::lock_guard<std::mutex> lock(mutex_);
        latest_=s;
        dirty_=true;
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            latest_= {
            };
            latest_.nextLogicId=c.initialLogicId;
            latest_.running=true;
            latest_.detail="Preparing module data (no wire handshake)";
            dirty_=true;
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
                    Snapshot failure; {
                        std::lock_guard<std::mutex> lock(mutex_);failure=latest_;
                    }
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
        Snapshot s;
        bool dirty=false;
        std::deque<std::string> logs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dirty=dirty_;
            if(dirty)s=latest_;
            dirty_=false;
            logs.swap(logs_);
        }
        for(const auto&l:logs)emit logMessage(QString::fromStdString(l));
        if(dirty)emit snapshotChanged(s);
        if(busy_&&done_.load(std::memory_order_acquire)) {
            if(worker_) {
                worker_->wait();
                worker_.reset();
            }
            busy_=false;
            timer_.stop();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                s=latest_;
            }
            Snapshot summary=s;
            summary.frames.clear();
            history_.push_back(std::move(summary));
            if(history_.size()>20)history_.pop_front();
            emit snapshotChanged(s);
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
