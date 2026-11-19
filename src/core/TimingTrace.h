#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace rt {
    enum class TraceKind : std::uint8_t {
        Sample, BuildBegin, BuildEnd, InitialBegin, InitialEnd, InitialAbandoned,
        EncodeBegin, EncodeEnd, SendBegin, SendEnd, ReceiveBegin, ReceiveEnd,
        CommandsBegin, CommandsEnd, PublishBegin, PublishEnd, WaitBegin, WaitEnd, Stop, InitialPacketGap
    };
    enum class TraceReason : std::uint8_t {
        SamplingDeviation, InitialDeadline, InitialAbandoned, WaitLateness, InitialPacketGap, SlowSend
    };
    inline const char* traceKindName(TraceKind kind) noexcept {
        switch(kind) {
#define RT_TRACE_KIND(name) case TraceKind::name: return #name;
            RT_TRACE_KIND(Sample) RT_TRACE_KIND(BuildBegin) RT_TRACE_KIND(BuildEnd)
            RT_TRACE_KIND(InitialBegin) RT_TRACE_KIND(InitialEnd) RT_TRACE_KIND(InitialAbandoned)
            RT_TRACE_KIND(EncodeBegin) RT_TRACE_KIND(EncodeEnd) RT_TRACE_KIND(SendBegin) RT_TRACE_KIND(SendEnd)
            RT_TRACE_KIND(ReceiveBegin) RT_TRACE_KIND(ReceiveEnd) RT_TRACE_KIND(CommandsBegin) RT_TRACE_KIND(CommandsEnd)
            RT_TRACE_KIND(PublishBegin) RT_TRACE_KIND(PublishEnd) RT_TRACE_KIND(WaitBegin) RT_TRACE_KIND(WaitEnd) RT_TRACE_KIND(Stop) RT_TRACE_KIND(InitialPacketGap)
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
        case TraceReason::InitialPacketGap: return "InitialPacketGap";
        case TraceReason::SlowSend: return "SlowSend";
        }
        return "Unknown";
    }
    enum class TraceTraffic : std::uint8_t { None, InitialFirst, InitialDuplicate, Retry };
    inline const char* traceTrafficName(TraceTraffic traffic) noexcept {
        switch(traffic) {
        case TraceTraffic::None:return "None";
        case TraceTraffic::InitialFirst:return "InitialFirst";
        case TraceTraffic::InitialDuplicate:return "InitialDuplicate";
        case TraceTraffic::Retry:return "Retry";
        }
        return "Unknown";
    }
    inline constexpr std::size_t TraceReasonCount=6;
    // Exact adjacent observations survive even when the rolling history no longer
    // contains the previous sample or omitted fast packet stages. Zero is a valid
    // timestamp; present/hasPrevious distinguish missing values from time zero.
    struct TimingTraceContext {
        bool present=false,hasPrevious=false,hasPlanned=false,sendResultKnown=false,accepted=false;
        std::uint64_t previousStartUs=0,previousEndUs=0,currentStartUs=0,currentEndUs=0;
        std::uint64_t previousPlannedUs=0,currentPlannedUs=0;
        std::uint32_t previousLogicId=0;
        std::uint16_t previousFragmentIndex=UINT16_MAX,fragmentIndex=UINT16_MAX;
        std::uint16_t previousOrdinal=UINT16_MAX,ordinal=UINT16_MAX;
        TraceTraffic traffic=TraceTraffic::None;
    };
    struct TimingTraceEvent {
        std::uint64_t sequence=0, atUs=0, value=0;
        std::uint32_t logicId=0;
        TraceKind kind=TraceKind::Sample;
        TraceTraffic traffic=TraceTraffic::None;
        std::uint16_t ordinal=UINT16_MAX;
    };
    struct TimingTraceWindow {
        TraceReason reason=TraceReason::SamplingDeviation;
        std::uint64_t triggerUs=0, triggerSequence=0, value=0;
        std::uint32_t logicId=0;
        std::size_t beforeCount=0, afterCount=0;
        bool complete=false;
        TimingTraceContext context;
        std::vector<TimingTraceEvent> events;
    };
    struct TimingTraceReport {
        bool enabled=false;
        // totalTriggers = triggersNotCaptured + windowsEvicted + windows.size().
        // NotCaptured means never admitted; eviction means captured then replaced.
        std::uint64_t totalEvents=0,totalTriggers=0,triggersNotCaptured=0,windowsEvicted=0,eventsOverwritten=0;
        std::array<std::uint64_t,TraceReasonCount> triggersByReason{},notCapturedByReason{},evictedByReason{};
        std::vector<TimingTraceWindow> windows;
    };
    // One allocation before scheduling. All recording and retention operations
    // use fixed POD arrays without allocation or locking. Each reason owns two
    // slots: retain its greatest values (earlier trigger wins ties), irrespective
    // of other reasons or overlapping post-windows. History is bounded by event
    // count, not by duration; exact trigger context is retained separately.
    class TimingTrace {
    public:
        static constexpr std::size_t BeforeCapacity=512,AfterCapacity=128,WindowsPerReason=2;
        static constexpr std::size_t WindowCapacity=TraceReasonCount*WindowsPerReason;
        static constexpr std::uint64_t SlowStageThresholdUs=100,SlowSendThresholdUs=1000;
        explicit TimingTrace(bool enabled=true):storage_(enabled?std::make_unique<Storage>():nullptr) {}
        bool enabled()const noexcept {return bool(storage_);}
        void record(TraceKind kind,std::uint64_t atUs,std::uint32_t logicId=0,std::uint64_t value=0,
                    TraceTraffic traffic=TraceTraffic::None,std::uint16_t ordinal=UINT16_MAX)noexcept {
            if(!storage_)return;
            auto& s=*storage_;
            const TimingTraceEvent event{++s.totalEvents,atUs,value,logicId,kind,traffic,ordinal};
            s.history[s.next]=event;
            s.next=(s.next+1)%BeforeCapacity;
            if(s.historyCount<BeforeCapacity)++s.historyCount;
            else ++s.eventsOverwritten;
            if(s.activeMask)for(std::size_t i=0;i<WindowCapacity;++i)if(s.activeMask&(1u<<i)) {
                auto& w=s.windows[i];
                w.events[w.beforeCount+w.afterCount++]=event;
                if(w.afterCount==AfterCapacity){w.complete=true;s.activeMask&=~(1u<<i);}
            }
        }
        void recordSpanIfSlow(TraceKind beginKind,TraceKind endKind,std::uint64_t beginUs,
                              std::uint64_t endUs,std::uint32_t logicId=0,std::uint64_t value=0,
                              TraceTraffic traffic=TraceTraffic::None,std::uint16_t ordinal=UINT16_MAX)noexcept {
            if(!storage_||endUs<beginUs||endUs-beginUs<SlowStageThresholdUs)return;
            record(beginKind,beginUs,logicId,value,traffic,ordinal);
            record(endKind,endUs,logicId,value,traffic,ordinal);
        }
        void trigger(TraceReason reason,std::uint64_t atUs,std::uint32_t logicId=0,std::uint64_t value=0,
                     const TimingTraceContext& context={})noexcept {
            if(!storage_)return;
            auto& s=*storage_;
            const auto reasonIndex=static_cast<std::size_t>(reason);
            if(reasonIndex>=TraceReasonCount)return;
            ++s.totalTriggers;++s.triggersByReason[reasonIndex];
            const auto first=reasonIndex*WindowsPerReason;
            auto selected=first;
            for(std::size_t i=first;i<first+WindowsPerReason;++i) {
                if(!s.windows[i].occupied){selected=i;break;}
                if(s.windows[i].value<s.windows[selected].value ||
                   (s.windows[i].value==s.windows[selected].value&&s.windows[i].admissionOrder>s.windows[selected].admissionOrder))selected=i;
            }
            auto& w=s.windows[selected];
            if(w.occupied) {
                if(value<=w.value){++s.triggersNotCaptured;++s.notCapturedByReason[reasonIndex];return;}
                ++s.windowsEvicted;++s.evictedByReason[reasonIndex];
            } else {w.occupied=true;++s.windowCount;}
            w.reason=reason;w.triggerUs=atUs;w.triggerSequence=s.totalEvents;w.logicId=logicId;w.value=value;
            w.admissionOrder=s.totalTriggers;w.context=context;w.beforeCount=s.historyCount;w.afterCount=0;w.complete=false;
            const auto oldest=(s.next+BeforeCapacity-s.historyCount)%BeforeCapacity;
            for(std::size_t i=0;i<s.historyCount;++i)w.events[i]=s.history[(oldest+i)%BeforeCapacity];
            s.activeMask|=1u<<selected;
        }
        void finish()noexcept {
            if(storage_)storage_->activeMask=0; // Partial final windows remain explicitly incomplete.
        }
        TimingTraceReport report()const {
            TimingTraceReport report;
            report.enabled=enabled();
            if(!storage_)return report;
            const auto& s=*storage_;
            report.totalEvents=s.totalEvents;report.totalTriggers=s.totalTriggers;
            report.triggersNotCaptured=s.triggersNotCaptured;report.windowsEvicted=s.windowsEvicted;
            report.eventsOverwritten=s.eventsOverwritten;
            report.triggersByReason=s.triggersByReason;report.notCapturedByReason=s.notCapturedByReason;
            report.evictedByReason=s.evictedByReason;
            report.windows.reserve(s.windowCount);
            for(const auto& source:s.windows)if(source.occupied) {
                TimingTraceWindow w;
                w.reason=source.reason;w.triggerUs=source.triggerUs;w.triggerSequence=source.triggerSequence;
                w.logicId=source.logicId;w.value=source.value;w.beforeCount=source.beforeCount;
                w.afterCount=source.afterCount;w.complete=source.complete;w.context=source.context;
                w.events.assign(source.events.begin(),source.events.begin()+source.beforeCount+source.afterCount);
                report.windows.push_back(std::move(w));
            }
            std::stable_sort(report.windows.begin(),report.windows.end(),[](const auto& a,const auto& b){return a.triggerSequence<b.triggerSequence;});
            return report;
        }
    private:
        struct StoredWindow {
            TraceReason reason=TraceReason::SamplingDeviation;
            std::uint64_t triggerUs=0,triggerSequence=0,value=0,admissionOrder=0;
            std::uint32_t logicId=0;
            std::size_t beforeCount=0,afterCount=0;
            bool occupied=false,complete=false;
            TimingTraceContext context;
            std::array<TimingTraceEvent,BeforeCapacity+AfterCapacity> events{};
        };
        struct Storage {
            std::array<TimingTraceEvent,BeforeCapacity> history{};
            std::array<StoredWindow,WindowCapacity> windows{};
            std::uint64_t totalEvents=0,totalTriggers=0,triggersNotCaptured=0,windowsEvicted=0,eventsOverwritten=0;
            std::array<std::uint64_t,TraceReasonCount> triggersByReason{},notCapturedByReason{},evictedByReason{};
            std::size_t next=0,historyCount=0,windowCount=0;
            std::uint32_t activeMask=0;
        };
        std::unique_ptr<Storage> storage_;
    };
}
