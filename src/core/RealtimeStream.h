#pragma once
#include "core/FrameCache.h"
#include "core/LogicalFrameBuilder.h"
#include "core/PacketPacer.h"
#include "core/TimingTrace.h"
#include "protocol/PacketCodec.h"
#include <deque>
#include <unordered_set>
namespace rt {
    class RealtimeStream {
        public:
        using Clock=std::function<std::uint64_t()>;
        RealtimeStream(StreamConfig config,std::uint64_t startUs,Clock clock={});
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
        // Production callers pass their already-read observation time. Omitting it
        // preserves deterministic callers' last-tick-time behavior.
        void stop(std::uint64_t observedUs=0)noexcept;
        Statistics& diagnostics() noexcept { return stats_; }
        TimingTrace& timingTrace() noexcept { return trace_; }
        std::uint32_t currentLogicId()const noexcept {return currentId_;}
        std::uint64_t nextSamplingDeadlineUs()const noexcept {return draining_?UINT64_MAX:nextSample_;}
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
        Snapshot snapshot(std::uint64_t now,bool includeFrames=true)const;
        const FrameCache& cache()const noexcept {
            return cache_;
        }
        private:
        struct Retry {
            std::uint32_t frame;
            std::uint16_t index;
            std::uint64_t queuedUs;
        };
        enum class FragmentState : std::uint8_t { Queued, InjectedDrop, Sent, Retransmitted, SendError };
        struct CompactFragmentStatus {
            std::uint64_t lastSendUs=0;
            std::uint32_t retries=0;
            FragmentState state=FragmentState::Queued;
        };
        static constexpr std::size_t MaxFragmentCount=(MaxFrameBytes+PayloadStep-1)/PayloadStep;
        struct CompactFrameStatus {
            std::uint32_t id=0,bytes=0,mask=0;
            std::uint64_t timestampUs=0;
            std::uint16_t count=0;
            bool valid=false,initialFinished=false;
            std::array<CompactFragmentStatus,MaxFragmentCount> fragments{};
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
        Clock clock_;
        TimingTrace trace_;
        TimingHistogram initialDuration_,initialCallSpan_,samplingIntervals_,samplingDeviation_;
        TimingHistogram initialPacketGaps_,initialPacketGapDeviation_,retryPacketGaps_,socketCallGaps_;
        bool hasInitialCall_=false,hasRetryCall_=false,hasSocketCall_=false;
        std::uint16_t previousInitialOrdinal_=0,previousInitialIndex_=0;
        std::uint64_t previousInitialEndUs_=0,previousSamplePlannedUs_=0;
        std::uint32_t previousSampleLogicId_=0;
        std::uint64_t previousInitialCallUs_=0,previousRetryCallUs_=0,previousSocketCallUs_=0;
        std::deque<CacheMissEvent> cacheMissEvents_;
        std::uint64_t cacheMissEventsDropped_=0,firstInitialCall_=0;
        bool currentFullySubmitted_=true;
        std::string detail_="Running; receiver delivery unknown";
        std::deque<Retry> retries_;
        std::unordered_set<std::uint64_t> pending_;
        // Three metadata slots mirror cache residency, including modulo wrap collisions.
        // Display strings and vectors are materialized only when publishing details.
        std::array<CompactFrameStatus,3> rows_{};
        CompactFrameStatus& rowFor(std::uint32_t id);
        static const char* stateText(FragmentState state)noexcept;
        void sample(std::uint64_t now,std::uint64_t observedUs);
        std::uint64_t clockNow(std::uint64_t fallback)const;
        void abandonInitial(bool stopping,std::uint64_t observedUs)noexcept;
        void observeSendCall(std::uint64_t sendStart,std::uint64_t sendEnd,std::uint32_t logicId,std::uint16_t index,
                             bool retry,bool firstCopy,bool accepted,std::size_t datagramBytes)noexcept;
        std::uint64_t transmit(const Frame&f,std::uint16_t index,bool retry,std::uint64_t now,const SendFunction& send);
        static std::uint64_t key(std::uint32_t id,std::uint16_t idx) {
            return(std::uint64_t(id)<<16)|idx;
        }
    };
}
