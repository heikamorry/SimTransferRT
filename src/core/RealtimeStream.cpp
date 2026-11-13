#include "core/RealtimeStream.h"
#include <algorithm>
#include <limits>
namespace rt {
    RealtimeStream::RealtimeStream(StreamConfig c,std::uint64_t start)
    :config_(std::move(c)),source_(config_.modules),cache_(config_.cachePolicy),mask_(source_.initialMask()),nextId_(config_.initialLogicId),startUs_(start),nextSample_(start),lastNow_(start) {
        if(!config_.linkMbps)throw std::invalid_argument("linkMbps must be positive");
        const auto maxSize=source_.sizeForMask(source_.initialMask());
        if(maxSize&&maxSize<=MaxFrameBytes&&conservativeWireBytes(maxSize)*8u>std::uint64_t(config_.linkMbps)*InitialWindowUs&&!config_.allowOversubscribed)
        throw std::invalid_argument("declared link cannot send this frame within 15ms; use a faster link or explicitly enable format-only overload testing");
    }
    void RealtimeStream::setModuleMask(std::uint32_t mask) {
        const auto n=source_.sizeForMask(mask);
        if(n&&n<=MaxFrameBytes&&!config_.allowOversubscribed&&conservativeWireBytes(n)*8u>std::uint64_t(config_.linkMbps)*InitialWindowUs)throw std::invalid_argument("new mask exceeds declared 15ms link budget");
        mask_=mask;
    }
    void RealtimeStream::sample(std::uint64_t now) {
        ++stats_.sampled;
        stats_.maxSampleLatenessUs=std::max(stats_.maxSampleLatenessUs,now-nextSample_);
        if(hasCurrent_&&nextIndex_<currentCount_) {
            stats_.unsentExpired+=currentCount_-nextIndex_;
            ++stats_.deadlineMisses;
        }
        hasCurrent_=false;
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
        currentCount_=frame->fragmentCount();
        nextIndex_=0;
        FrameStatus row;
        row.id=frame->logicId;
        row.bytes=static_cast<std::uint32_t>(frame->bytes.size());
        row.mask=mask_;
        row.timestampUs=now;
        row.count=currentCount_;
        row.fragments.resize(currentCount_);
        for(unsigned i=0;i<currentCount_;++i) {
            row.fragments[i].id=static_cast<std::uint16_t>(i);
            row.fragments[i].bytes=static_cast<std::uint32_t>(std::min(PayloadStep,frame->bytes.size()-i*PayloadStep));
        }
        cache_.put(std::move(*frame));
        rows_[currentId_]=std::move(row);
        for(auto it=rows_.begin();it!=rows_.end();)if(!cache_.find(it->first))it=rows_.erase(it);
        else ++it;
        // Never retain payload for overwritten frames via queued retries.
        for(auto it=retries_.begin();it!=retries_.end();)if(!cache_.find(it->frame)) {
            pending_.erase(key(it->frame,it->index));
            it=retries_.erase(it);
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
            return;
        }
        try {
            validateRanges(n,f->fragmentCount());
        }
        catch(const ProtocolError&) {
            ++stats_.invalidNacks;
            return;
        }
        for(const auto&r:n.ranges)for(std::uint32_t i=r.start;i<std::uint32_t(r.start)+r.count;++i) {
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
                n.logicId,idx
            });
        }
    }
    void RealtimeStream::transmit(const Frame& f,std::uint16_t index,bool retry,std::uint64_t now,const SendFunction& send) {
        auto& row=rows_.at(f.logicId).fragments.at(index);
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
            row.state="Injected drop";
            return;
        }
        auto packet=encodeData(f,index,retry);
        if(!retry&&fault.corruptFirst.count(index)) {
            packet.bytes[HeaderSize]^=1;
            ++stats_.injectedCorrupt;
        }
        const int copies=(!retry&&fault.duplicateFirst.count(index))?2:1;
        for(int copy=0;copy<copies;++copy) {
            if(send(packet.bytes.data(),packet.size)) {
                ++stats_.datagrams;
                stats_.bytesSent+=packet.size;
                if(retry)++stats_.retransmissions;
                row.state=retry?"Retransmitted (unconfirmed)":"Sent locally (unconfirmed)";
            }
            else {
                ++stats_.sendErrors;
                row.state="Local send error";
            }
        }
    }
    void RealtimeStream::tick(std::uint64_t now,const SendFunction& send) {
        if(!running_)return;
        if(now<lastNow_)throw std::invalid_argument("monotonic clock moved backwards");
        lastNow_=now;
        if(!draining_&&now>=nextSample_) {
            const auto missed=(now-nextSample_)/SamplingPeriodUs;
            stats_.skippedOverrun+=missed;
            nextSample_+=missed*SamplingPeriodUs;
            sample(now);
            nextSample_+=SamplingPeriodUs;
            // frameLimit counts sampling triggers, including all-off samples. No infinite test runs.
            if(config_.frameLimit&&stats_.sampled>=config_.frameLimit) {
                draining_=true;
                drainUntil_=now+60000;
                detail_="Draining NACK cache locally; no FIN/SUCCESS on wire";
            }
        }
        if(draining_&&now>=drainUntil_) {
            running_=false;
            detail_="Local run finished; receiver delivery unknown";
            return;
        }
        if(hasCurrent_&&nextIndex_<currentCount_&&now>=pacer_.due(nextIndex_)) {
            const auto due=pacer_.due(nextIndex_);
            stats_.maxSendLatenessUs=std::max(stats_.maxSendLatenessUs,now-due);
            const auto* f=cache_.find(currentId_);
            if(f) {
                auto index=config_.faults.reverseInitial?static_cast<std::uint16_t>(currentCount_-1-nextIndex_):nextIndex_;
                transmit(*f,index,false,now,send);
            }
            pacer_.emitted(now);
            ++nextIndex_;
            if(nextIndex_==currentCount_) {
                stats_.lastInitialDurationUs=now-currentStart_;
                if(stats_.lastInitialDurationUs>InitialWindowUs)++stats_.deadlineMisses;
                rows_[currentId_].initialFinished=true;
            }
            return;
            // One initial packet per scheduling turn, never a whole-frame burst.
        }
        if(!retries_.empty()&&now>=nextRetryUs_) {
            const auto r=retries_.front();
            retries_.pop_front();
            pending_.erase(key(r.frame,r.index));
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
    Snapshot RealtimeStream::snapshot(std::uint64_t now)const {
        Snapshot s;
        s.running=running_;
        s.draining=draining_;
        s.detail=detail_;
        s.stats=stats_;
        s.stats.elapsedUs=now-startUs_;
        s.nextLogicId=nextId_;
        for(const auto&pair:rows_)s.frames.push_back(pair.second);
        // TS is monotonic within a run, including uint32 logic_id wrap.
        std::sort(s.frames.begin(),s.frames.end(),[](const auto&a,const auto&b) {
            return a.timestampUs>b.timestampUs;
        });
        return s;
    }
}
