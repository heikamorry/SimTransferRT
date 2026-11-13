#pragma once
#include "core/FrameCache.h"
#include "core/LogicalFrameBuilder.h"
#include "core/PacketPacer.h"
#include "protocol/PacketCodec.h"
#include <deque>
#include <map>
#include <unordered_set>
namespace rt {
    class RealtimeStream {
        public:
        RealtimeStream(StreamConfig config,std::uint64_t startUs);
        // nowUs is a monotonic microsecond sampling timestamp. Hardware clock integration is external.
        void tick(std::uint64_t nowUs,const SendFunction& send);
        void acceptNack(const Nack& nack);
        void badNack()noexcept {
            ++stats_.invalidNacks;
        }
        void queueRejected()noexcept {
            ++stats_.queueDrops;
        }
        void rejectedSource()noexcept {
            ++stats_.sourceRejected;
        }
        void setModuleMask(std::uint32_t mask);
        void stop()noexcept {
            running_=false;
            detail_="Stopped locally; receiver delivery unknown";
        }
        void resetSchedule(std::uint64_t start) {
            startUs_=nextSample_=lastNow_=start;
        }
        bool initialPending()const noexcept {
            return hasCurrent_&&nextIndex_<currentCount_;
        }
        bool running()const noexcept {
            return running_;
        }
        std::uint64_t nextWakeUs(std::uint64_t now)const;
        Snapshot snapshot(std::uint64_t now)const;
        const FrameCache& cache()const noexcept {
            return cache_;
        }
        private:
        struct Retry {
            std::uint32_t frame;
            std::uint16_t index;
        };
        StreamConfig config_;
        ModuleDataSource source_;
        FrameCache cache_;
        PacketPacer pacer_;
        std::uint32_t mask_=0,nextId_=0,currentId_=0;
        std::uint16_t nextIndex_=0,currentCount_=0;
        bool hasCurrent_=false,running_=true,draining_=false;
        std::uint64_t startUs_=0,nextSample_=0,currentStart_=0,nextRetryUs_=0,drainUntil_=0;
        std::uint64_t lastNow_=0;
        Statistics stats_;
        std::string detail_="Running; receiver delivery unknown";
        std::deque<Retry> retries_;
        std::unordered_set<std::uint64_t> pending_;
        std::map<std::uint32_t,FrameStatus> rows_;
        void sample(std::uint64_t now);
        void transmit(const Frame&f,std::uint16_t index,bool retry,std::uint64_t now,const SendFunction& send);
        static std::uint64_t key(std::uint32_t id,std::uint16_t idx) {
            return(std::uint64_t(id)<<16)|idx;
        }
    };
}
