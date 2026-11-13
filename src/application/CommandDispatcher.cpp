#include "application/CommandDispatcher.h"
namespace rt {
    bool CommandDispatcher::post(Command c) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(c.kind==Command::Kind::Stop) {
            stop_=true;
            return true;
        }
        if(c.kind==Command::Kind::SetMask) {
            for(auto it=queue_.rbegin();it!=queue_.rend();++it)if(it->kind==Command::Kind::SetMask) {
                *it=std::move(c);
                return true;
            }
        }
        if(queue_.size()>=512)return false;
        queue_.push_back(std::move(c));
        return true;
    }
    std::optional<Command> CommandDispatcher::take() {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stop_) {
            stop_=false;
            Command c;
            c.kind=Command::Kind::Stop;
            return c;
        }
        if(queue_.empty())return std::nullopt;
        auto c=std::move(queue_.front());
        queue_.pop_front();
        return c;
    }
    void CommandDispatcher::clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        stop_=false;
    }
    std::size_t CommandDispatcher::size()const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size()+(stop_?1u:0u);
    }
}
