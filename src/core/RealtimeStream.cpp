#include "core/RealtimeStream.h"
#include <algorithm>
#include <limits>
namespace rt {
    RealtimeStream::RealtimeStream(StreamConfig c,std::uint64_t start,Clock clock)
    :config_(std::move(c)),source_(config_.modules),cache_(config_.cachePolicy),mask_(source_.initialMask()),nextId_(config_.initialLogicId),startUs_(start),nextSample_(start),lastNow_(start),clock_(std::move(clock)),trace_(config_.timingTraceEnabled) {
        if(!config_.linkMbps)throw std::invalid_argument("linkMbps must be positive");
        // Report the prepared source sizes, including files, without rereading them.
        for(auto& module:config_.modules)module.dataBytes=static_cast<std::uint32_t>(source_.sizeForMask(std::uint32_t(1)<<module.id)-ModuleHeaderSize);
        const auto maxSize=source_.sizeForMask(source_.initialMask());
        if(maxSize&&maxSize<=MaxFrameBytes&&conservativeWireBytes(maxSize)*8u>std::uint64_t(config_.linkMbps)*InitialWindowUs&&!config_.allowOversubscribed)
        throw std::invalid_argument("declared link cannot send this frame within 15ms; use a faster link or explicitly enable format-only overload testing");
    }
    RealtimeStream::CompactFrameStatus& RealtimeStream::rowFor(std::uint32_t id) {
        for(auto& row:rows_)if(row.valid&&row.id==id)return row;
        throw std::logic_error("cached frame has no fragment status");
    }
    const char* RealtimeStream::stateText(FragmentState state)noexcept {
        switch(state) {
        case FragmentState::Queued:return "Queued";
        case FragmentState::InjectedDrop:return "Injected drop";
        case FragmentState::Sent:return "Sent locally (unconfirmed)";
        case FragmentState::Retransmitted:return "Retransmitted (unconfirmed)";
        case FragmentState::SendError:return "Local send error";
        }
        return "Queued";
    }
    void RealtimeStream::setModuleMask(std::uint32_t mask) {
        const auto n=source_.sizeForMask(mask);
        if(n&&n<=MaxFrameBytes&&!config_.allowOversubscribed&&conservativeWireBytes(n)*8u>std::uint64_t(config_.linkMbps)*InitialWindowUs)throw std::invalid_argument("new mask exceeds declared 15ms link budget");
        mask_=mask;
    }
    std::uint64_t RealtimeStream::clockNow(std::uint64_t fallback)const {
        const auto n=clock_?clock_():fallback;
        if(n<fallback)throw std::invalid_argument("monotonic clock moved backwards during work");
        return n;
    }
    void RealtimeStream::abandonInitial(bool stopping,std::uint64_t observedUs)noexcept {
        if(initialPending()) {
            const auto remaining=currentCount_-nextIndex_;
            trace_.record(TraceKind::InitialAbandoned,observedUs,currentId_,remaining);
            trace_.trigger(TraceReason::InitialAbandoned,observedUs,currentId_,remaining);
            if(stopping)stats_.unsentAtStop+=remaining;
            else {stats_.unsentExpired+=remaining;++stats_.deadlineMisses;}
            ++stats_.initialIncompleteFrames;
            hasCurrent_=false;
        }
    }
    void RealtimeStream::stop(std::uint64_t observedUs)noexcept {
        const auto at=observedUs?observedUs:lastNow_;
        abandonInitial(true,at);
        stats_.retriesCancelled+=retries_.size();
        retries_.clear();pending_.clear();
        running_=false;
        trace_.record(TraceKind::Stop,at,currentId_);
        detail_="Stopped locally; receiver delivery unknown";
    }
    void RealtimeStream::sample(std::uint64_t now,std::uint64_t observedUs) {
        if(stats_.sampled) {
            const auto interval=now-stats_.lastSampleUs;
            samplingIntervals_.add(interval);
            const auto deviation=interval>SamplingPeriodUs?interval-SamplingPeriodUs:SamplingPeriodUs-interval;
            // Protocol jitter target is strictly <1000us, including equality as
            // a violation. This counter is not capped by trace-window retention.
            samplingDeviation_.add(deviation,999);
            if(deviation>=1000)++stats_.samplingIntervalViolations;
        }
        else stats_.firstSampleUs=now;
        stats_.lastSampleUs=now;
        stats_.samplingSpanUs=now-stats_.firstSampleUs;
        // Nominal observation window includes the final sampling period, excluding drain.
        stats_.samplingActiveUs=stats_.samplingSpanUs+SamplingPeriodUs;
        ++stats_.sampled;
        stats_.maxSampleLatenessUs=std::max(stats_.maxSampleLatenessUs,now-nextSample_);
        abandonInitial(false,observedUs);
        hasCurrent_=false;
        hasInitialCall_=false; // Never measure an inter-frame idle interval as a packet gap.
        std::optional<Frame> frame;
        try {
            frame=source_.sample(nextId_,now,mask_);
        }
        catch(const ProtocolError&) {
            ++stats_.buildRejected;
            return;
        }
        if(!frame) {
            ++stats_.skippedNoModules;
            return;
        }
        currentId_=nextId_++;
        currentStart_=now;
        firstInitialCall_=0;
        currentFullySubmitted_=true;
        currentCount_=frame->fragmentCount();
        stats_.initialFragmentsPlanned+=currentCount_;
        nextIndex_=0;
        cache_.put(std::move(*frame));
        CompactFrameStatus* available=nullptr;
        for(auto& candidate:rows_) {
            if(candidate.valid&&!cache_.find(candidate.id))candidate.valid=false;
            if(!candidate.valid&&!available)available=&candidate;
        }
        if(!available)throw std::logic_error("three-slot status capacity exhausted");
        auto& row=*available;
        const auto* cached=cache_.find(currentId_);
        row.valid=true;
        row.initialFinished=false;
        row.id=currentId_;
        row.bytes=static_cast<std::uint32_t>(cached->bytes.size());
        row.mask=mask_;
        row.timestampUs=now;
        row.count=currentCount_;
        for(unsigned i=0;i<currentCount_;++i) {
            row.fragments[i]=CompactFragmentStatus{};
        }
        // Never retain payload for overwritten frames via queued retries.
        for(auto it=retries_.begin();it!=retries_.end();)if(!cache_.find(it->frame)) {
            pending_.erase(key(it->frame,it->index));
            it=retries_.erase(it);
            ++stats_.retriesCancelled;
        }
        else ++it;
        pacer_.begin(now,currentCount_);
        hasCurrent_=true;
        ++stats_.built;
    }
    void RealtimeStream::acceptNack(const Nack& n) {
        ++stats_.nacks;
        const Frame* f=cache_.find(n.logicId);
        if(!f) {
            ++stats_.cacheMisses;
            CacheMissEvent event;
            event.requested=n.logicId;event.latest=nextId_-1;event.localUs=clockNow(lastNow_);
            const auto delta=std::uint32_t(event.latest-event.requested);
            event.distance=delta<=0x7FFFFFFFu?std::int64_t(delta):std::int64_t(delta)-0x100000000ll;
            for(const auto* cached:cache_.frames())event.cached.push_back(cached->logicId);
            if(cacheMissEvents_.size()==32){cacheMissEvents_.pop_front();++cacheMissEventsDropped_;}
            cacheMissEvents_.push_back(std::move(event));
            return;
        }
        try {
            validateRanges(n,f->fragmentCount());
        }
        catch(const ProtocolError&) {
            ++stats_.invalidNacks;
            return;
        }
        ++stats_.cacheHits;
        // Normalize intervals first: at most 363 sorts + 1461 unique fragment visits,
        // even when every wire range overlaps. Preserve duplicatePending's old count.
        auto ranges=n.ranges;
        std::sort(ranges.begin(),ranges.end(),[](const auto&a,const auto&b){return a.start<b.start;});
        std::vector<Range> merged;
        std::uint64_t requested=0,unique=0;
        for(const auto&r:ranges) {
            requested+=r.count;
            if(!merged.empty()&&r.start<=std::uint32_t(merged.back().start)+merged.back().count) {
                auto& last=merged.back();
                last.count=static_cast<std::uint16_t>(std::max(std::uint32_t(last.start)+last.count,std::uint32_t(r.start)+r.count)-last.start);
            } else merged.push_back(r);
        }
        for(const auto&r:merged)unique+=r.count;
        stats_.requestedFragments+=requested;
        stats_.duplicatePending+=requested-unique;
        const auto queued=clockNow(lastNow_);
        for(const auto&r:merged)for(std::uint32_t i=r.start;i<std::uint32_t(r.start)+r.count;++i) {
            const auto idx=static_cast<std::uint16_t>(i);
            auto k=key(n.logicId,idx);
            if(pending_.count(k)) {
                ++stats_.duplicatePending;
                continue;
            }
            if(retries_.size()>=3u*1461u) {
                ++stats_.queueDrops;
                continue;
            }
            pending_.insert(k);
            retries_.push_back( {
                n.logicId,idx,queued
            });
            stats_.retryQueueHighWater=std::max(stats_.retryQueueHighWater,std::uint64_t(retries_.size()));
        }
    }
    void RealtimeStream::observeSendCall(std::uint64_t sendStart,bool retry,bool firstCopy,std::size_t datagramBytes)noexcept {
        ++stats_.socketSendCalls;
        const auto bytes=static_cast<std::uint64_t>(datagramBytes);
        if(!stats_.minObservedDatagramBytes||bytes<stats_.minObservedDatagramBytes)stats_.minObservedDatagramBytes=bytes;
        stats_.maxObservedDatagramBytes=std::max(stats_.maxObservedDatagramBytes,bytes);
        if(hasSocketCall_)socketCallGaps_.add(sendStart-previousSocketCallUs_);
        previousSocketCallUs_=sendStart;hasSocketCall_=true;
        if(retry) {
            ++stats_.retrySendCalls;
            // These intervals include idle time and intervening initial traffic;
            // they are not the receiver's per-fragment NACK retry period.
            if(hasRetryCall_)retryPacketGaps_.add(sendStart-previousRetryCallUs_);
            previousRetryCallUs_=sendStart;hasRetryCall_=true;
            return;
        }
        if(!firstCopy) {++stats_.initialDuplicateSendCalls;return;}
        ++stats_.initialFirstCopySendCalls;
        if(hasInitialCall_&&nextIndex_==std::uint32_t(previousInitialOrdinal_)+1u) {
            const auto gap=sendStart-previousInitialCallUs_;
            const auto nominal=pacer_.plannedDue(nextIndex_)-pacer_.plannedDue(previousInitialOrdinal_);
            const auto deviation=gap>nominal?gap-nominal:nominal-gap;
            initialPacketGaps_.add(gap);
            initialPacketGapDeviation_.add(deviation,999);
            if(deviation>=1000)++stats_.initialPacketGapViolations;
            if(!stats_.minInitialNominalGapUs||nominal<stats_.minInitialNominalGapUs)stats_.minInitialNominalGapUs=nominal;
            stats_.maxInitialNominalGapUs=std::max(stats_.maxInitialNominalGapUs,nominal);
        } else ++stats_.initialGapSegments;
        previousInitialCallUs_=sendStart;previousInitialOrdinal_=nextIndex_;hasInitialCall_=true;
    }
    void RealtimeStream::transmit(const Frame& f,std::uint16_t index,bool retry,std::uint64_t now,const SendFunction& send) {
        auto& row=rowFor(f.logicId).fragments.at(index);
        if(retry) {
            ++row.retries;
        }
        else {
            ++stats_.firstAttempts;
        }
        row.lastSendUs=now;
        const auto& fault=config_.faults;
        if(fault.dropAlways.count(index)||(!retry&&fault.dropFirst.count(index))) {
            ++stats_.injectedDrops;
            if(retry)++stats_.retryInjectedDropAttempts;
            else {
                ++stats_.initialInjectedDropAttempts;
                currentFullySubmitted_=false;
                hasInitialCall_=false; // A missing send call splits adjacency, even inside a frame.
            }
            row.state=FragmentState::InjectedDrop;
            return;
        }
        const auto encodeStart=clockNow(now);
        auto packet=encodeData(f,index,retry);
        const auto encodeEnd=clockNow(encodeStart);
        stats_.encodeCrcProfile.add(encodeEnd-encodeStart);
        trace_.recordSpanIfSlow(TraceKind::EncodeBegin,TraceKind::EncodeEnd,encodeStart,encodeEnd,f.logicId,index);
        if(!retry&&fault.corruptFirst.count(index)) {
            packet.bytes[HeaderSize]^=1;
            ++stats_.injectedCorrupt;
        }
        const int copies=(!retry&&fault.duplicateFirst.count(index))?2:1;
        for(int copy=0;copy<copies;++copy) {
            const auto sendStart=clockNow(now);
            if(!retry)stats_.maxPlannedSendLatenessUs=std::max(stats_.maxPlannedSendLatenessUs,sendStart-pacer_.plannedDue(nextIndex_));
            if(!retry&&nextIndex_==0&&copy==0) {
                firstInitialCall_=sendStart;
                trace_.record(TraceKind::InitialBegin,sendStart,f.logicId,currentStart_);
            }
            observeSendCall(sendStart,retry,copy==0,packet.size);
            const bool accepted=send(packet.bytes.data(),packet.size);
            const auto sendEnd=clockNow(sendStart);
            stats_.sendProfile.add(sendEnd-sendStart);
            trace_.recordSpanIfSlow(TraceKind::SendBegin,TraceKind::SendEnd,sendStart,sendEnd,f.logicId,index);
            if(accepted) {
                ++stats_.datagrams;
                stats_.bytesSent+=packet.size;
                if(retry)++stats_.retransmissions;
                row.state=retry?FragmentState::Retransmitted:FragmentState::Sent;
            }
            else {
                ++stats_.sendErrors;
                if(!retry)currentFullySubmitted_=false;
                row.state=FragmentState::SendError;
            }
        }
    }
    void RealtimeStream::tick(std::uint64_t now,const SendFunction& send) {
        if(!running_)return;
        if(now<lastNow_)throw std::invalid_argument("monotonic clock moved backwards");
        lastNow_=now;
        if(!draining_&&now>=nextSample_) {
            trace_.record(TraceKind::Sample,now,nextId_,nextSample_);
            if(stats_.sampled) {
                const auto interval=now-stats_.lastSampleUs;
                const auto deviation=interval>SamplingPeriodUs?interval-SamplingPeriodUs:SamplingPeriodUs-interval;
                if(deviation>=1000)trace_.trigger(TraceReason::SamplingDeviation,now,nextId_,deviation);
            }
            const auto missed=(now-nextSample_)/SamplingPeriodUs;
            stats_.skippedOverrun+=missed;
            nextSample_+=missed*SamplingPeriodUs;
            const auto buildStart=clockNow(now);
            trace_.record(TraceKind::BuildBegin,buildStart,nextId_);
            sample(now,buildStart);
            const auto buildEnd=clockNow(buildStart);
            stats_.buildProfile.add(buildEnd-buildStart);
            trace_.record(TraceKind::BuildEnd,buildEnd,currentId_);
            nextSample_+=SamplingPeriodUs;
            // frameLimit counts sampling triggers, including all-off samples. No infinite test runs.
            if(config_.frameLimit&&stats_.sampled>=config_.frameLimit) {
                draining_=true;
                drainUntil_=now+60000;
                detail_="Draining NACK cache locally; no FIN/SUCCESS on wire";
            }
        }
        const auto tickEntry=now;
        now=clockNow(now); // Sampling/build time must not disappear from send/completion timestamps.
        if(draining_&&now>=drainUntil_) {
            stop(now);
            detail_="Local run finished; receiver delivery unknown";
            return;
        }
        if(hasCurrent_&&nextIndex_<currentCount_&&now>=pacer_.due(nextIndex_)) {
            const auto due=pacer_.due(nextIndex_);
            stats_.maxSendLatenessUs=std::max(stats_.maxSendLatenessUs,now-due);
            stats_.maxPlannedSendLatenessUs=std::max(stats_.maxPlannedSendLatenessUs,now-pacer_.plannedDue(nextIndex_));
            const auto* f=cache_.find(currentId_);
            if(f) {
                auto index=config_.faults.reverseInitial?static_cast<std::uint16_t>(currentCount_-1-nextIndex_):nextIndex_;
                transmit(*f,index,false,now,send);
            }
            // Preserve the legacy dispatch-start pacing anchor; work time never resets TS.
            pacer_.emitted(now);
            const auto finished=clockNow(now);
            ++nextIndex_;
            if(nextIndex_==currentCount_) {
                // Legacy field retains its pre-work tick-entry span for historical compatibility.
                stats_.lastInitialDurationUs=tickEntry-currentStart_;
                if(stats_.lastInitialDurationUs>InitialWindowUs)++stats_.deadlineMisses;
                stats_.lastSampleToInitialEndUs=finished-currentStart_;
                trace_.record(TraceKind::InitialEnd,finished,currentId_,stats_.lastSampleToInitialEndUs);
                if(stats_.lastSampleToInitialEndUs>InitialWindowUs) {
                    ++stats_.actualInitialDeadlineMisses;
                    trace_.trigger(TraceReason::InitialDeadline,finished,currentId_,stats_.lastSampleToInitialEndUs);
                }
                ++stats_.initialFinishedFrames;
                if(currentFullySubmitted_) {
                    ++stats_.initialFullySubmittedFrames;
                    initialDuration_.add(stats_.lastSampleToInitialEndUs,InitialWindowUs);
                    initialCallSpan_.add(finished-firstInitialCall_,InitialWindowUs);
                }
                rowFor(currentId_).initialFinished=true;
            }
            return;
            // One initial packet per scheduling turn, never a whole-frame burst.
        }
        if(!retries_.empty()&&now>=nextRetryUs_) {
            const auto r=retries_.front();
            retries_.pop_front();
            pending_.erase(key(r.frame,r.index));
            stats_.retryMaxWaitUs=std::max(stats_.retryMaxWaitUs,now-r.queuedUs);
            if(const auto*f=cache_.find(r.frame))transmit(*f,r.index,true,now,send);
            nextRetryUs_=now+10;
            // Local fairness cap, not a wire/protocol retry count.
        }
    }
    std::uint64_t RealtimeStream::nextWakeUs(std::uint64_t now)const {
        auto t=now+1000;
        if(!draining_)t=std::min(t,nextSample_);
        if(hasCurrent_&&nextIndex_<currentCount_)t=std::min(t,pacer_.due(nextIndex_));
        if(!retries_.empty())t=std::min(t,nextRetryUs_);
        if(draining_)t=std::min(t,drainUntil_);
        return t;
    }
    Snapshot RealtimeStream::snapshot(std::uint64_t now,bool includeFrames)const {
        Snapshot s;
        s.running=running_;
        s.draining=draining_;
        s.detailsIncluded=includeFrames;
        s.detail=detail_;
        s.stats=stats_;
        s.stats.elapsedUs=now-startUs_;
        s.stats.initialPendingFragments=initialPending()?currentCount_-nextIndex_:0;
        s.runId=config_.runId;s.config=config_;
        s.initialDuration=initialDuration_.summary();s.initialCallSpan=initialCallSpan_.summary();
        s.samplingIntervals=samplingIntervals_.summary();s.samplingDeviation=samplingDeviation_.summary();
        s.initialPacketGaps=initialPacketGaps_.summary();s.initialPacketGapDeviation=initialPacketGapDeviation_.summary();
        s.retryPacketGaps=retryPacketGaps_.summary();s.socketCallGaps=socketCallGaps_.summary();
        if(includeFrames)s.cacheMissEvents.assign(cacheMissEvents_.begin(),cacheMissEvents_.end());
        s.cacheMissEventsDropped=cacheMissEventsDropped_;
        s.nextLogicId=nextId_;
        if(includeFrames)for(const auto& row:rows_)if(row.valid) {
            FrameStatus display;
            display.id=row.id;display.bytes=row.bytes;display.mask=row.mask;
            display.timestampUs=row.timestampUs;display.count=row.count;display.initialFinished=row.initialFinished;
            display.fragments.reserve(row.count);
            for(std::uint16_t i=0;i<row.count;++i) {
                const auto& fragment=row.fragments[i];
                FragmentStatus item;
                item.id=i;item.bytes=static_cast<std::uint32_t>(std::min(PayloadStep,std::size_t(row.bytes)-i*PayloadStep));
                item.retries=fragment.retries;item.lastSendUs=fragment.lastSendUs;item.state=stateText(fragment.state);
                display.fragments.push_back(std::move(item));
            }
            s.frames.push_back(std::move(display));
        }
        // TS is monotonic within a run, including uint32 logic_id wrap.
        std::sort(s.frames.begin(),s.frames.end(),[](const auto&a,const auto&b) {
            return a.timestampUs>b.timestampUs;
        });
        return s;
    }
}
