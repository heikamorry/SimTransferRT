#pragma once
#include "protocol/ProtocolTypes.h"
#include <deque>
#include <mutex>
#include <optional>
namespace rt {
    struct Command {
        enum class Kind {
            SetMask, NackFeedback, Stop
        }
        kind=Kind::Stop;
        std::uint32_t mask=0;
        Nack nack;
        std::uint64_t queuedUs=0;
    };
    // MPSC bounded queue; Stop is priority and cannot be blocked by feedback overload.
    // No DATA packet is posted as a command. Feedback is fully parsed before being queued.
    class CommandDispatcher {
        public:
        bool post(Command command);
        std::optional<Command> take();
        void clear();
        std::size_t size()const;
        private:mutable std::mutex mutex_;
        std::deque<Command> queue_;
        bool stop_=false;
        std::uint64_t stopQueuedUs_=0;
    };
}
