#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace rt {
    enum class TraceKind : std::uint8_t {
        Sample, BuildBegin, BuildEnd, InitialBegin, InitialEnd, InitialAbandoned,
        EncodeBegin, EncodeEnd, SendBegin, SendEnd, ReceiveBegin, ReceiveEnd,
        CommandsBegin, CommandsEnd, PublishBegin, PublishEnd, WaitBegin, WaitEnd, Stop
    };
    enum class TraceReason : std::uint8_t {
        SamplingDeviation, InitialDeadline, InitialAbandoned, WaitLateness
    };
    inline const char* traceKindName(TraceKind kind) noexcept {
        switch(kind) {
#define RT_TRACE_KIND(name) case TraceKind::name: return #name;
            RT_TRACE_KIND(Sample) RT_TRACE_KIND(BuildBegin) RT_TRACE_KIND(BuildEnd)
            RT_TRACE_KIND(InitialBegin) RT_TRACE_KIND(InitialEnd) RT_TRACE_KIND(InitialAbandoned)
            RT_TRACE_KIND(EncodeBegin) RT_TRACE_KIND(EncodeEnd) RT_TRACE_KIND(SendBegin) RT_TRACE_KIND(SendEnd)
            RT_TRACE_KIND(ReceiveBegin) RT_TRACE_KIND(ReceiveEnd) RT_TRACE_KIND(CommandsBegin) RT_TRACE_KIND(CommandsEnd)
            RT_TRACE_KIND(PublishBegin) RT_TRACE_KIND(PublishEnd) RT_TRACE_KIND(WaitBegin) RT_TRACE_KIND(WaitEnd) RT_TRACE_KIND(Stop)
#undef RT_TRACE_KIND
        }
        return "Unknown";
    }
    inline const char* traceReasonName(TraceReason reason) noexcept {
        switch(reason) {
        case TraceReason::SamplingDeviation: return "SamplingDeviation";
        case TraceReason::InitialDeadline: return "InitialDeadline";
        case TraceReason::InitialAbandoned: return "InitialAbandoned";
        case TraceReason::WaitLateness: return "WaitLateness";
        }
        return "Unknown";
    }
    struct TimingTraceEvent {
        std::uint64_t sequence=0, atUs=0, value=0;
        std::uint32_t logicId=0;
        TraceKind kind=TraceKind::Sample;
    };
    struct TimingTraceWindow {
        TraceReason reason=TraceReason::SamplingDeviation;
        std::uint64_t triggerUs=0, triggerSequence=0, value=0;
        std::uint32_t logicId=0;
        std::size_t beforeCount=0, afterCount=0;
        bool complete=false;
        std::vector<TimingTraceEvent> events;
    };
    struct TimingTraceReport {
        bool enabled=false;
        std::uint64_t totalEvents=0, totalTriggers=0, triggersNotCaptured=0, eventsOverwritten=0;
        std::array<std::uint64_t,4> triggersByReason{},notCapturedByReason{};
        std::vector<TimingTraceWindow> windows;
    };
    // One allocation before scheduling; record/trigger/finish never allocate or
    // take a lock. Bounded rolling history plus the first non-overlapping incidents.
    // A trigger copies at most 512 small POD events. Concurrent/late incidents are
    // counted, never mistaken for a complete incident history.
    class TimingTrace {
    public:
        static constexpr std::size_t BeforeCapacity=512, AfterCapacity=128, WindowCapacity=8;
        static constexpr std::uint64_t SlowStageThresholdUs=100;
        explicit TimingTrace(bool enabled=true):storage_(enabled?std::make_unique<Storage>():nullptr) {}
        bool enabled()const noexcept {return bool(storage_);}
        void record(TraceKind kind,std::uint64_t atUs,std::uint32_t logicId=0,std::uint64_t value=0)noexcept {
            if(!storage_)return;
            auto& s=*storage_;
            const TimingTraceEvent event{++s.totalEvents,atUs,value,logicId,kind};
            s.history[s.next]=event;
            s.next=(s.next+1)%BeforeCapacity;
            if(s.historyCount<BeforeCapacity)++s.historyCount;
            else ++s.eventsOverwritten;
            if(s.active<WindowCapacity) {
                auto& w=s.windows[s.active];
                w.events[w.beforeCount+w.afterCount++]=event;
                if(w.afterCount==AfterCapacity){w.complete=true;s.active=WindowCapacity;}
            }
        }
        // Reuse the stage's existing clock observations. Normal per-packet work
        // is omitted, while profiles continue to include every operation.
        void recordSpanIfSlow(TraceKind beginKind,TraceKind endKind,std::uint64_t beginUs,
                              std::uint64_t endUs,std::uint32_t logicId=0,std::uint64_t value=0)noexcept {
            if(!storage_||endUs<beginUs||endUs-beginUs<SlowStageThresholdUs)return;
            record(beginKind,beginUs,logicId,value);
            record(endKind,endUs,logicId,value);
        }
        void trigger(TraceReason reason,std::uint64_t atUs,std::uint32_t logicId=0,std::uint64_t value=0)noexcept {
            if(!storage_)return;
            auto& s=*storage_;
            ++s.totalTriggers;
            const auto reasonIndex=static_cast<std::size_t>(reason);
            ++s.triggersByReason[reasonIndex];
            if(s.active<WindowCapacity||s.windowCount==WindowCapacity){++s.triggersNotCaptured;++s.notCapturedByReason[reasonIndex];return;}
            s.active=s.windowCount++;
            auto& w=s.windows[s.active];
            w.reason=reason;w.triggerUs=atUs;w.triggerSequence=s.totalEvents;w.logicId=logicId;w.value=value;
            w.beforeCount=s.historyCount;
            const auto oldest=(s.next+BeforeCapacity-s.historyCount)%BeforeCapacity;
            for(std::size_t i=0;i<s.historyCount;++i)w.events[i]=s.history[(oldest+i)%BeforeCapacity];
        }
        void finish()noexcept {
            if(storage_)storage_->active=WindowCapacity; // Partial final post-window stays explicitly incomplete.
        }
        TimingTraceReport report()const {
            TimingTraceReport report;
            report.enabled=enabled();
            if(!storage_)return report;
            const auto& s=*storage_;
            report.totalEvents=s.totalEvents;report.totalTriggers=s.totalTriggers;
            report.triggersNotCaptured=s.triggersNotCaptured;report.eventsOverwritten=s.eventsOverwritten;
            report.triggersByReason=s.triggersByReason;report.notCapturedByReason=s.notCapturedByReason;
            report.windows.reserve(s.windowCount);
            for(std::size_t i=0;i<s.windowCount;++i) {
                const auto& source=s.windows[i];
                TimingTraceWindow w;
                w.reason=source.reason;w.triggerUs=source.triggerUs;w.triggerSequence=source.triggerSequence;
                w.logicId=source.logicId;w.value=source.value;w.beforeCount=source.beforeCount;
                w.afterCount=source.afterCount;w.complete=source.complete;
                w.events.assign(source.events.begin(),source.events.begin()+source.beforeCount+source.afterCount);
                report.windows.push_back(std::move(w));
            }
            return report;
        }
    private:
        struct StoredWindow {
            TraceReason reason=TraceReason::SamplingDeviation;
            std::uint64_t triggerUs=0,triggerSequence=0,value=0;
            std::uint32_t logicId=0;
            std::size_t beforeCount=0,afterCount=0;
            bool complete=false;
            std::array<TimingTraceEvent,BeforeCapacity+AfterCapacity> events{};
        };
        struct Storage {
            std::array<TimingTraceEvent,BeforeCapacity> history{};
            std::array<StoredWindow,WindowCapacity> windows{};
            std::uint64_t totalEvents=0,totalTriggers=0,triggersNotCaptured=0,eventsOverwritten=0;
            std::array<std::uint64_t,4> triggersByReason{},notCapturedByReason{};
            std::size_t next=0,historyCount=0,windowCount=0,active=WindowCapacity;
        };
        std::unique_ptr<Storage> storage_;
    };
}
