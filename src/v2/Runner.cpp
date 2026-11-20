#include "v2/Runner.h"
#include "v2/Engine.h"
#include "v2/ProducerWait.h"
#include "sources/AuroraCodec.h"
#include "sources/AuroraSource.h"
#include "core/SamplingClock.h"
#include "protocol/PacketCodec.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace rt::v2 {
namespace {
struct SenderWait {
    const char* kind=nullptr;
    const char* result=nullptr;
    std::uint64_t beginUs=0,returnUs=0;
};
struct TraceClock {
    std::uint64_t processId=0,senderTid=0,frequency=0,beforeTicks=0,afterTicks=0,utcFiletime=0;
};
std::uint64_t nativeThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();
#elif defined(__linux__)
    return static_cast<std::uint64_t>(syscall(SYS_gettid));
#else
    return 0;
#endif
}
TraceClock captureTraceClock() {
    TraceClock c;
    c.senderTid=nativeThreadId();
#ifdef _WIN32
    c.processId=GetCurrentProcessId();
    LARGE_INTEGER freq{},before{},after{};
    FILETIME utc{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&before);
    GetSystemTimePreciseAsFileTime(&utc);
    QueryPerformanceCounter(&after);
    c.frequency=freq.QuadPart;c.beforeTicks=before.QuadPart;c.afterTicks=after.QuadPart;
    c.utcFiletime=(static_cast<std::uint64_t>(utc.dwHighDateTime)<<32)|utc.dwLowDateTime;
#elif defined(__linux__)
    c.processId=static_cast<std::uint64_t>(getpid());
#endif
    return c;
}
// Producers, status submissions and Stop can wake the sender even if the
// notification arrives just before it begins waiting. Only the sender waits.
class SenderWake {
public:
    void notify(std::atomic<std::uint64_t>* issue=nullptr) {
#ifdef _WIN32
        if(issue)issue->store(rt::monotonicUs(),std::memory_order_relaxed);
        SetEvent(work_.get());
#else
        {std::lock_guard<std::mutex> lock(mutex_);pending_=true;}
        if(issue)issue->store(rt::monotonicUs(),std::memory_order_relaxed);
        changed_.notify_one();
#endif
    }
    void until(std::uint64_t deadline,SenderWait* trace) {
        const auto now=rt::monotonicUs();
        if(now>=deadline)return;
        const auto us=deadline-now;
#ifdef _WIN32
        LARGE_INTEGER interval{};
        interval.QuadPart=-static_cast<LONGLONG>(us*10);
        if(timer_ && SetWaitableTimer(timer_.get(),&interval,0,nullptr,nullptr,FALSE)){
            HANDLE handles[]={work_.get(),timer_.get()};
            if(trace){trace->kind="work_event_or_timer";trace->beginUs=rt::monotonicUs();}
            const auto result=WaitForMultipleObjects(2,handles,FALSE,INFINITE);
            if(trace){trace->returnUs=rt::monotonicUs();trace->result=result==WAIT_OBJECT_0?"work_event":result==WAIT_OBJECT_0+1?"timer":"wait_failed";}
        }else{
            if(trace){trace->kind="work_event_or_timeout";trace->beginUs=rt::monotonicUs();}
            const auto result=WaitForSingleObject(work_.get(),static_cast<DWORD>((us+999)/1000));
            if(trace){trace->returnUs=rt::monotonicUs();trace->result=result==WAIT_OBJECT_0?"work_event":result==WAIT_TIMEOUT?"timeout":"wait_failed";}
        }
#else
        std::unique_lock<std::mutex> lock(mutex_);
        if(trace){trace->kind="condition_variable";trace->beginUs=rt::monotonicUs();}
        const bool work=changed_.wait_for(lock,std::chrono::microseconds(us),[this]{return pending_;});
        if(trace){trace->returnUs=rt::monotonicUs();trace->result=work?"work_signal":"timeout";}
        pending_=false;
#endif
    }
#ifdef _WIN32
    SenderWake() {
        if(!work_)throw std::runtime_error("Cannot create sender wake event");
    }
private:
    using Handle=std::unique_ptr<void,decltype(&CloseHandle)>;
    Handle work_{CreateEventW(nullptr,FALSE,FALSE,nullptr),CloseHandle};
    Handle timer_{CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_ALL_ACCESS),CloseHandle};
#else
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool pending_=false;
#endif
};
struct SourceStats {
    ProducerWaitStats wait;
    std::uint64_t producerTid=0;
    std::uint64_t sampled=0,built=0,skipped=0,queueDrops=0,errors=0,queueHighWater=0;
    std::uint64_t firstSampleUs=0,lastSampleUs=0,duplicateSource=0,prepareMaxUs=0;
    std::uint64_t newSourceFrames=0,sourceCounterGaps=0,sourceCounterResets=0,mixedCounterFrames=0;
    std::uint64_t semanticChanges=0;
    std::uint64_t unmappedSourceFrames=0,clockErrorBoundUs=0,cancelledAttempts=0;
    double actualDeviceHz=0,counterHz=0;
    bool commonClockMapped=false;
    std::uint32_t nextId=1;
    std::size_t payloadBytes=0;
    Ledger ledger;
    Distribution intervals,deviation,lateness;
    std::string error,kind="simulation";
};
// The sender can dequeue before the producer reaches SetEvent. The shared
// diagnostic timestamp outlives that race without extending the mailbox lock.
struct TraceTicket {
    FrameHandoff handoff;
    std::atomic<std::uint64_t> notifyIssueUs{0};
};
struct QueuedFrame {
    PreparedPtr frame;
    std::shared_ptr<TraceTicket> ticket;
};
struct SourceMailbox {
    std::mutex mutex;std::deque<QueuedFrame> queue;SourceStats stats;
    // Only used by the opt-in untraced path. write is published after the
    // immutable frame, read only after consumer releases that cell. At most
    // four resident frames, exactly the same drop-new policy as the deque.
    std::array<PreparedPtr,4> ring{};
    alignas(64) std::atomic<unsigned> write{0};
    alignas(64) std::atomic<unsigned> read{0};
    unsigned push(PreparedPtr frame){
        const auto w=write.load(std::memory_order_relaxed);
        const auto used=w-read.load(std::memory_order_acquire);
        if(used==ring.size())return 0;
        ring[w%ring.size()]=std::move(frame);
        write.store(w+1,std::memory_order_release);
        return used+1;
    }
    PreparedPtr pop(){
        const auto r=read.load(std::memory_order_relaxed);
        if(r==write.load(std::memory_order_acquire))return {};
        auto frame=std::move(ring[r%ring.size()]);
        read.store(r+1,std::memory_order_release);
        return frame;
    }
    std::atomic<std::uint64_t> nextSample{0};std::atomic<bool> done{false};
};
struct State {
    Config config;Preflight capacity;LinkObservation link;
    TraceClock traceClock;
    std::array<SourceStats,2> sources;EngineSnapshot engine;
    std::uint64_t startedUs=0,endedUs=0,elapsedUs=0;
    std::uint16_t localPort=0;bool df=false,finished=false;
    std::string transport,sendApi,boostVersion,error,endedReason;
    int sendBuffer=0,receiveBuffer=0,resetError=0;
    bool asynchronous=false;
    std::uint64_t feedbackPollCalls=0,feedbackEmptyPolls=0,feedbackReadErrors=0;
    std::string lastFeedbackReadError;
    std::uint64_t statusIngressAccepted=0,statusIngressRejected=0,statusIngressCancelled=0,statusIngressPending=0;
};
std::string quote(const std::string& s){std::ostringstream o;o<<'"';for(unsigned char c:s){switch(c){case '"':o<<"\\\"";break;case '\\':o<<"\\\\";break;case '\n':o<<"\\n";break;case '\r':o<<"\\r";break;case '\t':o<<"\\t";break;default:if(c<32)o<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<unsigned(c)<<std::dec;else o<<c;}}o<<'"';return o.str();}
std::string distribution(const Distribution& d){std::ostringstream o;o<<"{\"count\":"<<d.count<<",\"max_us\":"<<d.maxUs<<",\"p50_us\":"<<d.p50Us<<",\"p95_us\":"<<d.p95Us<<",\"p99_us\":"<<d.p99Us<<",\"p999_us\":"<<d.p999Us<<",\"violations\":"<<d.overLimit<<'}';return o.str();}
void writeProducerWait(std::ostream& o,const ProducerWaitStats& w){
    o<<"{\"backend\":"<<quote(w.backend)<<",\"timed_wait_slots\":"<<w.timedWaitSlots
     <<",\"spin_only_slots\":"<<w.spinOnly<<",\"due_on_entry\":"<<w.alreadyDue
     <<",\"cancelled\":"<<w.cancelled<<",\"fallbacks\":"<<w.fallbacks
     <<",\"early_returns\":"<<w.earlyReturns<<",\"returns_past_slot\":"<<w.returnsPastSlot
     <<",\"lead_us\":"<<w.leadUs<<",\"lead_cap_us\":"<<w.leadCapUs
     <<",\"spin_wall_us\":"<<w.spinWallUs<<",\"max_wake_overshoot_us\":"<<w.overshootMaxUs
     <<",\"wake_overshoot_ge_1000_us\":"<<w.overshootGe1ms<<",\"worst_slot_us\":\""<<w.worstSlotUs<<'"'
     <<",\"worst_wake_target_us\":\""<<w.worstTargetUs<<"\",\"worst_wake_return_us\":\""<<w.worstReturnUs<<"\"}";
}
std::string ledgerJson(const Ledger& l){std::ostringstream o;o<<"{\"count\":"<<l.count<<",\"id_sum\":\""<<l.idSum<<"\",\"crc_sum\":\""<<l.crcSum<<"\",\"stamp_sum\":\""<<l.stampSum<<"\"}";return o.str();}
std::string nullableTime(std::uint64_t us) {
    return us?'"'+std::to_string(us)+'"':"null";
}
void writeDispatchIdentity(std::ostream& o,const DispatchIdentity& previous) {
    if(!previous.moduleId){o<<"null";return;}
    o<<"{\"module_id\":"<<unsigned(previous.moduleId)<<",\"frame_id\":"<<previous.frameId
     <<",\"fragment_index\":"<<previous.index<<",\"start_us\":"<<nullableTime(previous.startUs)
     <<",\"end_us\":"<<nullableTime(previous.endUs)<<",\"retry\":"<<(previous.retry?"true":"false")<<'}';
}
void writeFirstDispatch(std::ostream& o,const FirstDispatchEvent& e) {
    const auto& h=e.handoff;
    const auto notified=h.notifyIssueUs?h.notifyIssueUs->load(std::memory_order_relaxed):0;
    const char* relation=!h.waitKind?"no_associated_wait":
        (h.enqueuedUs<h.waitBeginUs?"enqueued_before_wait":"enqueued_during_wait_window");
    o<<"{\"module_id\":"<<unsigned(e.moduleId)<<",\"frame_id\":"<<e.frameId
     <<",\"sample_us\":"<<nullableTime(e.sampleUs)<<",\"ready_us\":"<<nullableTime(e.readyUs)
     <<",\"enqueued_us\":"<<nullableTime(h.enqueuedUs)<<",\"notify_issue_us\":"<<nullableTime(notified)
     <<",\"wait_relation\":"<<quote(relation)
     <<",\"wait_kind\":"<<(h.waitKind?quote(h.waitKind):"null")<<",\"wait_result\":"<<(h.waitResult?quote(h.waitResult):"null")
     <<",\"wait_begin_us\":"<<nullableTime(h.waitBeginUs)<<",\"wait_return_us\":"<<nullableTime(h.waitReturnUs)
     <<",\"sender_observed_us\":"<<nullableTime(h.senderObservedUs)<<",\"dequeued_us\":"<<nullableTime(h.dequeuedUs)
     <<",\"submitted_us\":"<<nullableTime(h.submittedUs)<<",\"selected_us\":"<<nullableTime(e.selectedUs)
     <<",\"first_attempt_us\":"<<nullableTime(e.firstAttemptUs)<<",\"ready_to_first_us\":"<<e.readyToFirstUs()
     <<",\"admission_pacing_due_us\":"<<nullableTime(e.admissionPacingDueUs)<<",\"pacing_due_us\":"<<nullableTime(e.pacingDueUs)
     <<",\"intervening\":{\"ndi\":"<<e.intervening.ndi<<",\"ep\":"<<e.intervening.ep
     <<",\"status\":"<<e.intervening.status<<",\"retry\":"<<e.intervening.retry<<"},\"heads_at_submit\":[";
    for(std::size_t i=0;i<e.headsAtSubmit.size();++i){
        if(i)o<<',';
        const auto& head=e.headsAtSubmit[i];
        if(!head.moduleId){o<<"null";continue;}
        o<<"{\"module_id\":"<<unsigned(head.moduleId)<<",\"frame_id\":"<<head.frameId
         <<",\"deadline_us\":"<<nullableTime(head.deadlineUs)<<",\"next_fragment\":"<<head.nextFragment
         <<",\"fragments_remaining\":"<<head.fragmentsRemaining<<",\"duplicate_pending\":"<<(head.duplicatePending?"true":"false")<<'}';
    }
    o<<"],\"previous_at_submit\":";writeDispatchIdentity(o,e.previousAtSubmit);
    o<<",\"previous_at_first\":";writeDispatchIdentity(o,e.previousAtFirst);
    o<<'}';
}
void writeTraceClock(std::ostream& o,const State& s) {
    const auto& c=s.traceClock;
    o<<"\"process_id\":"<<c.processId<<",\"sender_tid\":"<<c.senderTid
     <<",\"producer_tids\":{\"1\":"<<s.sources[0].producerTid<<",\"2\":"<<s.sources[1].producerTid<<'}'
     <<",\"qpc_frequency_hz\":"<<nullableTime(c.frequency)<<",\"qpc_before_ticks\":"<<nullableTime(c.beforeTicks)
     <<",\"qpc_after_ticks\":"<<nullableTime(c.afterTicks)<<",\"utc_filetime_100ns\":"<<nullableTime(c.utcFiletime);
}
void writeHandoffTrace(std::ostream& o,const State& s) {
    if(!s.config.handoffTrace){o<<"{\"enabled\":false}";return;}
    o<<"{\"enabled\":true,";
    writeTraceClock(o,s);
    o<<",\"notify_scope\":\"Timestamp immediately before SetEvent/notify_one; notification can follow dequeue, and its issue is not a kernel wake timestamp.\""
     <<",\"wait_scope\":\"Last sender wait before first queue observation, only if enqueue preceded its observed return; shared signals may coalesce. Brief pacing may spin. Null means no associated wait.\""
     <<",\"sender_observation\":\"First instrumented loop entry at or after enqueue, or first locked queue poll if enqueue occurred during that turn. Kernel ReadyThread and first scheduled instruction require an external trace.\""
     <<",\"worst_first_dispatch\":{";
    for(std::size_t i=0;i<s.engine.worstFirstDispatch.size();++i){
        if(i)o<<',';
        o<<'"'<<(i+1)<<"\":[";
        const auto& events=s.engine.worstFirstDispatch[i];
        for(std::size_t j=0;j<events.size();++j){
            if(j)o<<',';
            writeFirstDispatch(o,events[j]);
        }
        o<<']';
    }
    o<<"}}";
}
void writeFragmentSpan(std::ostream& o,const std::shared_ptr<const FragmentSpan>& frame) {
    if(!frame){o<<"null";return;}
    const auto& f=*frame;
    o<<"{\"module_id\":"<<unsigned(f.moduleId)<<",\"frame_id\":"<<f.frameId
     <<",\"sample_us\":"<<nullableTime(f.sampleUs)<<",\"ready_us\":"<<nullableTime(f.readyUs)
     <<",\"first_attempt_us\":"<<nullableTime(f.firstAttemptUs)<<",\"end_us\":"<<nullableTime(f.endUs)
     <<",\"span_us\":"<<f.spanUs()<<",\"ready_to_first_us\":"<<(f.firstAttemptUs>=f.readyUs?f.firstAttemptUs-f.readyUs:0)
     <<",\"ready_to_end_us\":"<<(f.endUs>=f.readyUs?f.endUs-f.readyUs:0)
     <<",\"initial_complete\":"<<(f.complete?"true":"false")<<",\"call_count\":"<<f.callCount<<",\"own_call_us\":"<<f.ownCallUs
     <<",\"before_first_call_us\":"<<f.beforeFirstCallUs<<",\"between_calls_us\":"<<f.betweenCallsUs
     <<",\"after_last_call_us\":"<<f.afterLastCallUs<<",\"pacing_gap_us\":"<<f.pacingGapUs
     <<",\"intervening\":{\"ndi\":"<<f.intervening.ndi<<",\"ep\":"<<f.intervening.ep
     <<",\"status\":"<<f.intervening.status<<",\"retry\":"<<f.intervening.retry<<",\"duplicate\":"<<f.intervening.duplicate
     <<"},\"fragment_count\":"<<f.fragments.size()<<",\"fragments\":[";
    for(std::size_t i=0;i<f.fragments.size();++i){
        if(i)o<<',';
        const auto& p=f.fragments[i];
        o<<"{\"fragment_index\":"<<p.index<<",\"original_due_us\":"<<nullableTime(p.originalDueUs)
         <<",\"pacing_due_us\":"<<nullableTime(p.pacingDueUs)<<",\"pacing_gap_us\":"<<p.pacingGapUs<<",\"attempt_us\":"<<nullableTime(p.attemptUs)
         <<",\"end_us\":"<<nullableTime(p.endUs)<<",\"call_start_us\":"<<nullableTime(p.called?p.callStartUs:0)
         <<",\"call_end_us\":"<<nullableTime(p.called?p.endUs:0)<<",\"called\":"<<(p.called?"true":"false")
         <<",\"success\":"<<(p.success?"true":"false")<<'}';
    }
    o<<"]}";
}
void writeGapPacket(std::ostream& o,const GapPacket& p) {
    o<<"{\"module_id\":"<<unsigned(p.moduleId)<<",\"id\":"<<p.id<<",\"kind\":"<<quote(p.kind)
     <<",\"fragment_index\":";
    if(p.moduleId==StatusModule)o<<"null";else o<<p.index;
    o<<",\"original_due_us\":\""<<p.planUs<<"\",\"start_us\":\""<<p.startUs
     <<"\",\"end_us\":\""<<p.endUs<<"\",\"success\":"<<(p.success?"true":"false")<<'}';
}
void writeWorstGap(std::ostream& o,const WorstGap& worst,const Distribution& all) {
    o<<"{\"count\":"<<all.count<<",\"max_us\":"<<all.maxUs<<",\"pair\":";
    if(!worst.current.kind){o<<"null}";return;}
    o<<"{\"actual_gap_us\":"<<worst.actualUs<<",\"expected_gap_us\":"<<worst.expectedUs
     <<",\"deviation_us\":"<<worst.deviationUs<<",\"previous\":";
    writeGapPacket(o,worst.previous);
    o<<",\"current\":";
    writeGapPacket(o,worst.current);
    o<<"}}";
}
void writeFragmentTrace(std::ostream& o,const State& s) {
    if(!s.config.fragmentTrace){o<<"{\"enabled\":false}";return;}
    o<<"{\"enabled\":true,";
    writeTraceClock(o,s);
    o<<",\"span_scope\":\"First original fragment attempt to last original attempt end. Actual socket-call bounds are null for an injected drop. All original attempts of each retained finalized span are in dispatch order.\""
     <<",\"span_distribution_scope\":\"One observation per frame that reached its last original attempt, including send failures or injected drops. P99.9 uses the 100 us histogram bucket upper bound capped by the exact maximum. ge_18000_us includes equality.\""
     <<",\"pacing_scope\":\"Pacing deadlines are budgets. pacing_gap_us can overlap other dispatches; it is not an additional wall-time component or an observed kernel wait.\""
     <<",\"span_distribution_by_module\":{";
    for(std::size_t i=0;i<s.engine.fragmentSpanDistribution.size();++i){
        if(i)o<<',';
        const auto& d=s.engine.fragmentSpanDistribution[i];
        o<<'"'<<(i+1)<<"\":{\"count\":"<<d.count<<",\"p999_us\":"<<d.p999Us
         <<",\"max_us\":"<<d.maxUs<<",\"ge_18000_us\":"<<d.overLimit<<'}';
    }
    o<<"},\"worst_gap_pairs\":{\"global\":";
    writeWorstGap(o,s.engine.worstPacketGaps[0],s.engine.globalPacketGapDeviation);
    for(std::size_t i=0;i<s.engine.modules.size();++i){
        o<<",\""<<(i+1)<<"\":";
        writeWorstGap(o,s.engine.worstPacketGaps[i+1],s.engine.modules[i].packetGapDeviation);
    }
    o<<"},\"worst_by_module\":{";
    for(std::size_t i=0;i<s.engine.worstFragmentSpan.size();++i){
        if(i)o<<',';
        o<<'"'<<(i+1)<<"\":";
        writeFragmentSpan(o,s.engine.worstFragmentSpan[i]);
    }
    o<<"},\"worst_ndi_wait\":";writeFragmentSpan(o,s.engine.worstNdiWait);
    o<<",\"worst_ndi_wait_ep\":";writeFragmentSpan(o,s.engine.worstNdiWaitEp);
    o<<",\"worst_ndi_ready_to_end\":";writeFragmentSpan(o,s.engine.worstNdiReadyToEnd);
    o<<'}';
}
std::string report(const State& s){
    std::ostringstream o;o<<std::setprecision(12);
    o<<"{\"kind\":\"v2_sender\",\"protocol_version\":2,\"report_schema_version\":2,\"architecture\":\"B: independent preparation workers and one UDP sender\",\"run_id\":"<<quote(s.config.runId)
     <<",\"finished\":"<<(s.finished?"true":"false")<<",\"error\":"<<quote(s.error)<<",\"ended_reason\":"<<quote(s.endedReason)
     <<",\"elapsed_us\":"<<s.elapsedUs<<",\"started_host_us\":\""<<s.startedUs<<"\",\"ended_host_us\":\""<<s.endedUs<<"\",\"transport\":"<<quote(s.transport)
     <<",\"send_api\":"<<(s.sendApi.empty()?"null":quote(s.sendApi))<<",\"local_port\":"<<s.localPort<<",\"df_enabled\":"<<(s.df?"true":"false")
     <<",\"boost_version\":"<<(s.boostVersion.empty()?"null":quote(s.boostVersion))
     <<",\"build_type\":"<<quote(SIM_BUILD_TYPE)<<",\"source_revision_at_configure\":"<<quote(SIM_SOURCE_REV)<<",\"tracked_dirty_at_configure\":"<<quote(SIM_SOURCE_DIRTY)
     <<",\"socket_send_buffer_bytes\":"<<s.sendBuffer<<",\"socket_receive_buffer_bytes\":"<<s.receiveBuffer<<",\"udp_reset_setup_error\":"<<s.resetError
     <<",\"first_send_endpoint\":"<<quote(s.asynchronous?"completion_handler_observed":"local_synchronous_return")
     <<",\"config\":{\"mailbox_fast_path\":"<<(s.config.mailboxFastPath?"true":"false")<<",\"network_profile\":"<<quote(networkProfileName(s.config.linkMbps))<<",\"selected_link_mbps\":"<<s.config.linkMbps<<",\"effective_link_mbps\":"<<s.engine.effectiveMbps
     <<",\"target_ip\":"<<quote(s.config.targetIp)<<",\"bind_ip\":"<<quote(s.config.bindIp)<<",\"target_port\":"<<s.config.targetPort<<",\"command_port\":"<<s.config.commandPort
     <<",\"configured_observed_limit_mbps\":"<<s.config.observedLinkMbps<<",\"path_limit_mbps\":"<<s.config.pathLimitMbps<<",\"ndi_tools\":"<<s.config.ndiTools<<",\"ep_bytes\":"<<s.config.epBytes
     <<",\"ndi_hz\":"<<s.config.ndiHz<<",\"ep_hz\":"<<s.config.epHz<<",\"ndi_enabled\":"<<(s.config.ndiEnabled?"true":"false")<<",\"ep_enabled\":"<<(s.config.epEnabled?"true":"false")
     <<",\"duration_us\":"<<s.config.durationUs<<",\"frame_limit\":"<<s.config.frameLimit<<",\"status_interval_us\":"<<s.config.statusIntervalUs<<",\"fault_injection\":"<<(s.config.faults.any()?"true":"false")<<",\"allow_oversubscribed\":"<<(s.config.allowOversubscribed?"true":"false")<<'}'
     <<",\"link_observation\":{\"local_link_mbps\":"<<s.link.mbps<<",\"loopback\":"<<(s.link.loopback?"true":"false")<<",\"physical_ethernet\":"<<(s.link.physical?"true":"false")<<",\"interface\":"<<quote(s.link.interfaceName)<<",\"reason\":"<<quote(s.link.reason)<<",\"observed_host_us\":\""<<s.link.observedAtUs<<"\"}"
     <<",\"preflight\":{\"feasible\":"<<(s.capacity.feasible?"true":"false")<<",\"formula\":\"IPv4 Ethernet: UDP bytes + 66, DATA frame bytes + 102 * fragments; no VLAN/tunnel\",\"utilization\":"<<s.capacity.utilization<<",\"ndi_wire_us\":"<<s.capacity.ndiWireUs<<",\"ep_wire_us\":"<<s.capacity.epWireUs<<",\"simultaneous_wire_us\":"<<s.capacity.synchronousWireUs<<",\"reason\":"<<quote(s.capacity.reason)<<'}'
     <<",\"handoff_trace\":";
    writeHandoffTrace(o,s);
    o<<",\"fragment_trace\":";
    writeFragmentTrace(o,s);
    o<<",\"clock\":{\"domain\":\"host_monotonic_simulation_or_playback\",\"hardware_verified\":false,\"timestamp_note\":\"Wire timestamp is explicitly simulated/playback time. Aurora device frame counter is retained inside BX; not an independently synchronized hardware clock.\"},\"modules\":{";
    for(std::size_t i=0;i<2;++i){if(i)o<<',';const auto& a=s.sources[i];const auto& m=s.engine.modules[i];
        o<<'"'<<(i+1)<<"\":{\"source_kind\":"<<quote(a.kind)<<",\"sampled\":"<<a.sampled<<",\"built\":"<<a.built<<",\"skipped_slots\":"<<a.skipped<<",\"queue_drops\":"<<a.queueDrops<<",\"source_errors\":"<<a.errors<<",\"source_error\":"<<quote(a.error)
         <<",\"queue_high_water\":"<<a.queueHighWater<<",\"payload_bytes\":"<<a.payloadBytes<<",\"next_frame_id\":"<<a.nextId<<",\"duplicate_source_frames\":"<<a.duplicateSource<<",\"prepare_max_us\":"<<a.prepareMaxUs
         <<",\"new_source_frames\":"<<a.newSourceFrames<<",\"source_counter_gaps\":"<<a.sourceCounterGaps<<",\"source_counter_resets\":"<<a.sourceCounterResets<<",\"mixed_counter_frames\":"<<a.mixedCounterFrames<<",\"semantic_changes\":"<<a.semanticChanges
         <<",\"unmapped_source_frames\":"<<a.unmappedSourceFrames<<",\"common_clock_mapped\":"<<(a.commonClockMapped?"true":"false")<<",\"clock_error_bound_us\":"<<a.clockErrorBoundUs<<",\"actual_device_hz\":"<<a.actualDeviceHz<<",\"counter_hz\":"<<a.counterHz
         <<",\"cancelled_attempts\":"<<a.cancelledAttempts<<",\"sampling_measurement\":"<<quote(a.kind=="device"?"host query start timing, not hardware sampling":"simulated/playback frame production timing")
         <<",\"sampling_span_us\":"<<(a.lastSampleUs>=a.firstSampleUs?a.lastSampleUs-a.firstSampleUs:0)<<",\"sampling_intervals\":"<<distribution(a.intervals)<<",\"sampling_deviation\":"<<distribution(a.deviation)<<",\"sampling_lateness\":"<<distribution(a.lateness)
         <<",\"producer_wait\":";
        writeProducerWait(o,a.wait);
        o<<",\"accepted\":"<<m.accepted<<",\"initial_complete\":"<<m.initialComplete<<",\"initial_incomplete\":"<<m.initialIncomplete<<",\"deadline_misses\":"<<m.deadlineMisses<<",\"send_errors\":"<<m.sendErrors
         <<",\"datagrams\":"<<m.datagrams<<",\"bytes\":"<<m.bytes<<",\"retransmissions\":"<<m.retransmissions<<",\"unsent_at_stop\":"<<m.unsentAtStop<<",\"unsent_evicted\":"<<m.unsentEvicted<<",\"retry_cancelled\":"<<m.retryCancelled
         <<",\"nacks\":"<<m.nacks<<",\"invalid_nacks\":"<<m.invalidNacks<<",\"cache_misses\":"<<m.cacheMisses<<",\"requested_fragments\":"<<m.requestedFragments<<",\"duplicate_pending\":"<<m.duplicatePending<<",\"retry_queue_drops\":"<<m.retryQueueDrops<<",\"cache_high_water\":"<<m.cacheHighWater<<",\"retry_high_water\":"<<m.retryHighWater<<",\"injected_drops\":"<<m.injectedDrops
         <<",\"initial_duration\":"<<distribution(m.initialDuration)<<",\"packet_gaps\":"<<distribution(m.packetGaps)<<",\"packet_gap_deviation\":"<<distribution(m.packetGapDeviation)<<",\"original_plan_lateness\":"<<distribution(m.originalPlanLateness)<<",\"retry_wait\":"<<distribution(m.retryWait)
         <<",\"ledger\":"<<ledgerJson(a.ledger)<<",\"initial_submission_ledger\":"<<ledgerJson(m.submitted);
        if(s.config.handoffTrace)o<<",\"ready_to_first\":"<<distribution(m.readyToFirst);
        o<<'}';
    }
    o<<"},\"status\":{\"accepted\":"<<s.engine.statusAccepted<<",\"sent\":"<<s.engine.statusSent<<",\"rejected\":"<<s.engine.statusDropped<<",\"send_errors\":"<<s.engine.statusSendErrors<<",\"cancelled\":"<<s.engine.statusCancelled<<",\"queue_high_water\":"<<s.engine.statusQueueHighWater<<",\"maximum_submit_latency_us\":"<<s.engine.statusMaximumLatencyUs<<",\"next_sequence\":"<<s.engine.nextStatusSequence<<'}'
     <<",\"status_ingress\":{\"accepted\":"<<s.statusIngressAccepted<<",\"rejected\":"<<s.statusIngressRejected<<",\"cancelled\":"<<s.statusIngressCancelled<<",\"pending\":"<<s.statusIngressPending<<'}'
     <<",\"invalid_feedback\":"<<s.engine.invalidFeedback<<",\"feedback_source_rejected\":"<<s.engine.sourceRejected<<",\"rate_changes\":"<<s.engine.rateChanges<<",\"global_packet_gaps\":"<<distribution(s.engine.globalPacketGaps)<<",\"global_packet_gap_deviation\":"<<distribution(s.engine.globalPacketGapDeviation)
     <<",\"feedback_poll_calls\":"<<s.feedbackPollCalls<<",\"feedback_empty_polls\":"<<s.feedbackEmptyPolls
     <<",\"feedback_read_errors\":"<<s.feedbackReadErrors<<",\"last_feedback_read_error\":"<<quote(s.lastFeedbackReadError)
     <<",\"worst_packet_events\":[";
    for(std::size_t i=0;i<s.engine.worstEvents.size();++i){if(i)o<<',';const auto&e=s.engine.worstEvents[i];o<<"{\"module_id\":"<<unsigned(e.moduleId)<<",\"frame_id\":"<<e.frameId<<",\"fragment_index\":"<<e.index<<",\"planned_sample_us\":\""<<e.plannedSampleUs<<"\",\"actual_sample_us\":\""<<e.sampleUs<<"\",\"ready_us\":\""<<e.readyUs<<"\",\"original_due_us\":\""<<e.originalDueUs<<"\",\"adjusted_due_us\":\""<<e.adjustedDueUs<<"\",\"send_start_us\":\""<<e.sendStartUs<<"\",\"send_end_us\":\""<<e.sendEndUs<<"\",\"deadline_us\":\""<<e.deadlineUs<<"\",\"retry\":"<<(e.retry?"true":"false")<<",\"success\":"<<(e.success?"true":"false")<<'}';}
    o<<"],\"initial_deadline_events\":[";
    for(std::size_t i=0;i<s.engine.initialDeadlineEvents.size();++i){
        if(i)o<<',';
        const auto& e=s.engine.initialDeadlineEvents[i];
        o<<"{\"module_id\":"<<unsigned(e.moduleId)<<",\"frame_id\":"<<e.frameId
         <<",\"planned_sample_us\":\""<<e.plannedSampleUs<<"\",\"actual_sample_us\":\""<<e.sampleUs
         <<"\",\"ready_us\":\""<<e.readyUs<<"\",\"first_attempt_us\":\""<<e.firstAttemptUs
         <<"\",\"last_attempt_end_us\":\""<<e.endUs<<"\",\"deadline_us\":\""<<e.deadlineUs
         <<"\",\"overrun_us\":"<<e.overrunUs()<<",\"initial_complete\":"<<(e.complete?"true":"false");
        if(s.config.handoffTrace){
            o<<",\"first_dispatch\":";
            if(e.firstDispatch)writeFirstDispatch(o,*e.firstDispatch);else o<<"null";
        }
        o<<'}';
    }
    o<<"],\"acceptance\":[";bool first=true;
    auto row=[&](const std::string& id,unsigned module,const std::string& req,const std::string& measurement,const std::string& threshold,const std::string& measured,const std::string& status,const std::string& reason){if(!first)o<<',';first=false;o<<"{\"id\":"<<quote(id)<<",\"module_id\":"<<module<<",\"requirement\":"<<quote(req)<<",\"measurement\":"<<quote(measurement)<<",\"threshold\":"<<quote(threshold)<<",\"measured\":"<<quote(measured)<<",\"status\":"<<quote(status)<<",\"reason\":"<<quote(reason)<<'}';};
    for(unsigned i=0;i<2;++i){const auto&a=s.sources[i];const auto&m=s.engine.modules[i];const bool enabled=i==0?s.config.ndiEnabled:s.config.epEnabled;const auto noData=!enabled||!a.sampled;const std::string name=i==0?"NDI":"EP";
        row("sampling_integrity",i+1,name+" source continuity","Original independent sample/query slots; no interpolation","0 skipped slots / queue loss / source error / unmapped device frame",std::to_string(a.skipped)+" skipped, "+std::to_string(a.queueDrops)+" queue drops, "+std::to_string(a.errors)+" errors, "+std::to_string(a.unmappedSourceFrames)+" unmapped",a.skipped||a.queueDrops||a.errors||a.unmappedSourceFrames?"fail":(noData?"not_tested":"pass"),"A source failure remains failed even before its first sample. Hardware sampling is separate.");
        row("sampling_jitter",i+1,name+" sampling deviation (retained engineering target)","Absolute actual production/query interval minus configured nominal period; all intervals","strictly < 1000 us",std::to_string(a.deviation.maxUs)+" us; "+std::to_string(a.deviation.overLimit)+" / "+std::to_string(a.deviation.count)+" violations",!a.deviation.count||a.kind=="device"?"not_tested":(a.deviation.maxUs>=1000?"fail":"pass"),"Device host query intervals cannot establish hardware sample jitter; the <1 ms engineering threshold is unchanged");
        row("initial_submission",i+1,name+" complete initial transmission",s.asynchronous?"Sample to observation of all successful local completion handlers (not NIC egress)":"Simulation/playback sample to last local send return; device input retains host-request proxy separately",i==0?"<= 15000 us":"<= 18000 us",std::to_string(m.initialDuration.maxUs)+" us; "+std::to_string(m.deadlineMisses)+" late; "+std::to_string(m.initialIncomplete)+" incomplete",!a.built||a.kind=="device"?"not_tested":(m.deadlineMisses||m.initialIncomplete||m.initialComplete!=a.built?"fail":"pass"),"Local submission only; device common timestamp does not independently establish source-to-host clock offset or physical egress");
        row("packet_gap_jitter",i+1,name+" packet interval deviation (engineering target)","Actual first-copy interval minus original global scheduling plan interval","strictly < 1000 us",std::to_string(m.packetGapDeviation.maxUs)+" us; "+std::to_string(m.packetGapDeviation.overLimit)+" / "+std::to_string(m.packetGapDeviation.count)+" violations",!m.packetGapDeviation.count?"not_tested":(m.packetGapDeviation.maxUs>=1000?"fail":"pass"),"Full-population counters; original due time is never replaced by catch-up due time");
        row("hardware_sampling",i+1,name+" new hardware sampling and common timestamp","Device frame counter, calibrated clock mapping and measured source rate",i==0?"V2 ~60 Hz and verified common microsecond clock":"V2 50 Hz and verified common microsecond clock","No physical device timing evidence","not_tested","Simulation/playback cannot verify real Aurora 40/66 Hz mode or EP sampling clock");
        row("receiver_delivery",i+1,name+" complete receive and consume","Compare paired receiver report and content/frame ledgers","All built frames accounted for","Requires paired receiver report","not_tested","UDP send success alone is not delivery evidence");
        row("nack_timing",i+1,name+" receiver retry sequence","Receiver gap/tail detection and per-module request timestamps","2 ms tail; immediate first; 10 ms spacing; 3 rounds",std::to_string(m.nacks)+" NACK packets received","not_tested","Evaluate exact receiver timing and deterministic tests separately; sender cannot infer receiver's detection clock");
    }
    const auto& global=s.engine.globalPacketGapDeviation;
    row("global_packet_gap_deviation",0,"All-stream packet interval deviation (engineering target)",
        "Local send-start interval versus scheduled interval across DATA, retries and status","strictly < 1000 us",
        std::to_string(global.maxUs)+" us; "+std::to_string(global.overLimit)+" / "+std::to_string(global.count)+" violations",
        !global.count?"not_tested":global.overLimit?"fail":"pass",
        "Includes retry scheduling anchors; this deviation is distinct from a measured wire gap");
    row("capacity",0,"Declared load capacity","Ethernet serialization feasibility screening; not performance measurement","Demand within effective rate and 15/18 ms windows",std::to_string(s.capacity.utilization*100)+" percent",s.capacity.feasible?"pass":"fail",s.capacity.reason);
    row("status_immediate",3,"Immediate complete status snapshot","Status confirmation to local socket submission","Numerical immediate bound unspecified",std::to_string(s.engine.statusMaximumLatencyUs)+" us; "+std::to_string(s.engine.statusSent)+" sent","not_tested","No invented timing threshold; status uses one packet and no NACK");
    row("physical_egress",0,"Actual network egress deadlines","Hardware NIC or external capture timestamps","NDI <=15 ms; EP <=18 ms","Unavailable","not_tested",s.link.reason);
    row("display",0,"Independent display and timestamp association","Receive-consume-render trace with common hardware time","No wait beyond a module period; real waveform contract required","Sender report has no render timestamps","not_tested","Use receiver GUI consumption evidence; no real EP layout or verified clock was supplied");
    o<<"]}";return o.str();
}
Bytes readFile(const std::string& path,std::size_t max){
    std::ifstream f(std::filesystem::u8path(path),std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("Cannot open replay file: "+path);
    const auto n=f.tellg();if(n<=0||static_cast<std::uint64_t>(n)>max)throw std::runtime_error("Replay payload size outside allowed range");
    Bytes b(static_cast<std::size_t>(n));f.seekg(0);if(!f.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(b.size())))throw std::runtime_error("Cannot read complete replay payload");return b;
}
}
struct Session::Impl {
    TransportFactory factory;rt::aurora::NdiSourceFactory ndiFactory;LinkObserver observe;
    SenderWake wake;
    std::thread worker;
    ProducerStop producerStop;
    const std::atomic<bool>& stopRequested=producerStop.flag();
    std::atomic<bool> active{false};
    mutable std::mutex mutex;
    std::shared_ptr<const State> state=std::make_shared<const State>();
    std::mutex statusMutex;std::deque<std::pair<Bytes,std::uint64_t>> statuses;
    std::atomic<bool> hasStatus{false}; // hint only; payload ownership stays under statusMutex
    std::uint64_t acceptedStatuses=0,rejectedStatuses=0,cancelledStatuses=0;
    Impl(TransportFactory f,rt::aurora::NdiSourceFactory n,LinkObserver linkObserver)
        :factory(std::move(f)),ndiFactory(std::move(n)),observe(std::move(linkObserver)){}
    void publishState(std::shared_ptr<const State> next) {
        std::lock_guard<std::mutex> lock(mutex);
        state.swap(next); // The previous state is released after the guard.
    }
    void execute(Config c);
    void source(Config c,unsigned id,std::uint64_t origin,SourceMailbox& mail,std::size_t ndiBytes,
                std::size_t epBytes,const std::atomic<unsigned>& observedRate);
};
void Session::Impl::source(Config c,unsigned id,std::uint64_t origin,SourceMailbox& mail,std::size_t ndiBytes,
                           std::size_t epBytes,const std::atomic<unsigned>& observedRate){
    SourceStats stats;stats.nextId=id==1?c.initialNdiId:c.initialEpId;TimingHistogram interval,deviation,late;
    if(c.handoffTrace || c.fragmentTrace)stats.producerTid=nativeThreadId();
    std::unique_ptr<rt::aurora::INdiSource> ndi;
    auto publish=[&]{stats.intervals=interval.summary();stats.deviation=deviation.summary();stats.lateness=late.summary();std::lock_guard<std::mutex> lock(mail.mutex);mail.stats=stats;};
    try{
        const bool enabled=id==1?c.ndiEnabled:c.epEnabled;if(!enabled){publish();mail.done=true;wake.notify();return;}
        const auto file=id==1?c.ndiReplayFile:c.epReplayFile;Bytes replay,ep;
        if(!file.empty()){stats.kind="replay";replay=readFile(file,id==1?16384:rt::MaxFrameBytes);if(id==1)rt::aurora::decodeBx(replay);}
        if(id==1){
            auto sourceFactory=ndiFactory?ndiFactory:(!replay.empty()?rt::aurora::replaySourceFactory(replay,c.ndiHz):rt::aurora::simulationSourceFactory(c.ndiTools,c.ndiHz));
            ndi=sourceFactory();if(!ndi)throw std::runtime_error("NDI factory returned no data source");
            stats.kind=ndi->kind();ndi->start(stopRequested);stats.actualDeviceHz=stats.kind=="device"?ndi->actualHz():0;stats.counterHz=ndi->counterHz();
        }
        if(id==2&&replay.empty()){ep.resize(c.epBytes);for(std::size_t j=0;j<ep.size();++j)ep[j]=static_cast<std::uint8_t>(j*17+3);}
        const std::size_t payload=file.empty()?(id==1?11+42*c.ndiTools:ep.size()):replay.size();stats.payloadBytes=payload;
        const auto check=preflight(c,id==1?payload:ndiBytes,id==2?payload:epBytes,observedRate.load());
        if(!check.feasible&&!c.allowOversubscribed)throw std::runtime_error(check.reason);
        std::optional<StaticEpPayload> epTemplate;
        if(id==EpModule)epTemplate.emplace(replay.empty()?std::move(ep):std::move(replay));
        const auto hz=id==1?c.ndiHz:c.epHz;const auto phase=id==1?c.ndiPhaseUs:c.epPhaseUs;
        ProducerWait sampleWait(producerStop,stats.wait,hz);
        const bool ringPath=c.mailboxFastPath && !c.handoffTrace;
        std::uint64_t slot=0,previous=0;
        auto advance=[&](std::uint64_t now,PreparedPtr p={}){
            auto next=slot+1;const auto latest=(now-origin-phase)*hz/1000000ULL+1;
            if(latest>next){stats.skipped+=latest-next;next=latest;}slot=next;
            if(ringPath){
                mail.nextSample.store(origin+phase+(slot*1000000ULL)/hz,std::memory_order_release);
                if(p){
                    const auto depth=mail.push(std::move(p));
                    if(!depth)++stats.queueDrops;
                    stats.queueHighWater=std::max<std::uint64_t>(stats.queueHighWater,depth);
                }
                wake.notify();
                // Snapshot data still have a mutex: no concurrent unsynchronised
                // copy of strings or histogram storage. Final publish is unconditional.
                publish();
                return;
            }
            stats.intervals=interval.summary();stats.deviation=deviation.summary();stats.lateness=late.summary();
            auto ticket=c.handoffTrace&&p?std::make_shared<TraceTicket>():nullptr;
            {
                std::lock_guard<std::mutex> lock(mail.mutex);
                if(p){
                    if(mail.queue.size()==4){++stats.queueDrops;ticket.reset();}
                    else{
                        mail.queue.push_back({std::move(p),ticket});
                        if(ticket)ticket->handoff.enqueuedUs=rt::monotonicUs();
                    }
                }
                stats.queueHighWater=std::max<std::uint64_t>(stats.queueHighWater,mail.queue.size());mail.stats=stats;mail.nextSample=origin+phase+(slot*1000000ULL)/hz;
            }
            wake.notify(ticket?&ticket->notifyIssueUs:nullptr);
        };
        while(!stopRequested.load(std::memory_order_acquire)){
            const auto planned=origin+phase+(slot*1000000ULL)/hz;
            mail.nextSample=planned;
            if(c.durationUs&&planned>=origin+c.durationUs)break;
            if(c.frameLimit&&stats.sampled>=c.frameLimit)break;
            std::uint64_t now=0;
            if(!sampleWait.until(planned,now))break;
            if(c.durationUs&&now>=origin+c.durationUs){const auto total=(c.durationUs>phase?((c.durationUs-phase)*hz+999999)/1000000:0);if(total>slot)stats.skipped+=total-slot;break;}
            const auto sampled=now;++stats.sampled;if(!stats.firstSampleUs)stats.firstSampleUs=sampled;stats.lastSampleUs=sampled;
            late.add(sampled>planned?sampled-planned:0,999);
            if(previous){const auto gap=sampled-previous;interval.add(gap);const auto nominal=static_cast<std::uint64_t>(std::llround(1000000.0/hz));deviation.add(gap>nominal?gap-nominal:nominal-gap,999);}previous=sampled;
            Frame frame;frame.moduleId=static_cast<std::uint8_t>(id);frame.timestampUs=sampled;
            std::uint64_t observedCounter=0;
            if(id==1){auto observation=ndi->read(rt::monotonicUs,stopRequested);frame.bytes=std::move(observation.bytes);
                if(observation.newSourceFrame)++stats.newSourceFrames;else ++stats.duplicateSource;
                if(observation.semanticChange)++stats.semanticChanges;
                bool firstCounter=true,mixed=false;
                for(const auto&tool:observation.toolTimes){stats.sourceCounterGaps+=tool.counter.missingSamples;stats.sourceCounterResets+=tool.counter.reset?1:0;if(firstCounter){observedCounter=tool.counter.unwrapped;firstCounter=false;}else mixed=mixed||observedCounter!=tool.counter.unwrapped;}
                if(mixed)++stats.mixedCounterFrames;
                stats.commonClockMapped=observation.sourceTime.has_value();
                if(observation.sourceTime)stats.clockErrorBoundUs=std::max(stats.clockErrorBoundUs,observation.sourceTime->errorBoundUs);
                if(stats.kind=="device"){
                    if(!observation.sourceTime){++stats.unmappedSourceFrames;advance(rt::monotonicUs());continue;}
                    frame.timestampUs=observation.sourceTime->timestampUs;
                    if(!observation.newSourceFrame&&!observation.semanticChange){advance(rt::monotonicUs());continue;}
                }
                // Reject an oversized device reply here, before it can reach the
                // shared sender and interrupt the independent EP/status streams.
                stats.payloadBytes=frame.bytes.size();
                const auto currentBudget=preflight(c,frame.bytes.size(),epBytes,observedRate.load());
                if(!currentBudget.feasible&&!c.allowOversubscribed)throw std::runtime_error("NDI observed length fails capacity screening: "+currentBudget.reason);
            }
            frame.frameId=stats.nextId++;
            auto p=epTemplate?epTemplate->sample(frame.frameId,planned,sampled):
                prepareFrame(std::move(frame),planned,sampled,0,observedCounter);
            now=rt::monotonicUs();stats.prepareMaxUs=std::max(stats.prepareMaxUs,now-sampled);++stats.built;stats.ledger.add(*p);
            advance(now,std::move(p));
        }
    }catch(const std::exception&e){if(stopRequested)++stats.cancelledAttempts;else{++stats.errors;stats.error=e.what();}}
    if(ndi){try{ndi->shutdown();}catch(const std::exception&e){++stats.errors;if(stats.error.empty())stats.error=e.what();}}
    try{publish();}catch(...){}
    mail.nextSample=0;mail.done.store(true,std::memory_order_release);wake.notify();
}
void Session::Impl::execute(Config c){
    State local;local.config=c;std::array<SourceMailbox,2> sources;std::array<std::thread,2> producers;
    if(c.handoffTrace || c.fragmentTrace)local.traceClock=captureTraceClock();
    sources[0].stats.nextId=c.initialNdiId;sources[1].stats.nextId=c.initialEpId;local.engine.nextStatusSequence=c.initialStatusSequence;
    std::unique_ptr<Engine> engineOwner;
    std::unique_ptr<IUdpTransport> udp; // lives through cancellation and delivery of terminal completions
    std::thread observer;std::atomic<bool> observerStop{false};std::atomic<unsigned> observedRate{0};std::mutex observationMutex;LinkObservation latestObservation;
    auto publish=[&](const Engine* engine){
        {std::lock_guard<std::mutex> lock(observationMutex);if(latestObservation.observedAtUs)local.link=latestObservation;}
        // local keeps session metadata; event vectors belong to each published snapshot.
        auto next=std::make_shared<State>(local);
        if(engine)next->engine=engine->snapshot();
        for(unsigned i=0;i<2;++i){
            std::lock_guard<std::mutex> lock(sources[i].mutex);
            next->sources[i]=sources[i].stats;
        }
        {
            std::lock_guard<std::mutex> lock(statusMutex);
            next->statusIngressAccepted=acceptedStatuses;
            next->statusIngressRejected=rejectedStatuses;
            next->statusIngressCancelled=cancelledStatuses;
            next->statusIngressPending=statuses.size();
        }
        const auto now=rt::monotonicUs();
        next->elapsedUs=local.startedUs&&now>local.startedUs?now-local.startedUs:0;
        publishState(std::move(next));
    };
    try{
        local.link=observe(c);latestObservation=local.link;observedRate=local.link.mbps;
        local.engine.effectiveMbps=effectiveMbps(c,local.link.mbps);
        auto fileBytes=[](const std::string& path){std::error_code error;if(path.empty())return std::size_t(0);const auto size=std::filesystem::file_size(std::filesystem::u8path(path),error);return error?std::size_t(0):static_cast<std::size_t>(size);};
        std::size_t knownNdiBytes=fileBytes(c.ndiReplayFile),knownEpBytes=fileBytes(c.epReplayFile);
        local.capacity=preflight(c,knownNdiBytes,knownEpBytes,local.link.mbps);if(!local.capacity.feasible&&!c.allowOversubscribed)throw std::runtime_error(local.capacity.reason);
        udp=factory();if(!udp)throw std::runtime_error("Transport factory returned no socket");
        StreamConfig transportConfig;transportConfig.targetIp=c.targetIp;transportConfig.bindIp=c.bindIp;transportConfig.targetPort=c.targetPort;transportConfig.commandPort=c.commandPort;
        udp->open(transportConfig);local.localPort=udp->localPort();local.df=udp->dfEnabled();local.transport=udp->name();local.sendApi=udp->sendApi();
        local.boostVersion=udp->libraryVersion();local.sendBuffer=udp->sendBufferBytes();local.receiveBuffer=udp->receiveBufferBytes();local.resetError=udp->connResetError();
        local.asynchronous=local.sendApi.find("async_send_to")!=std::string::npos;
        engineOwner=std::make_unique<Engine>(c,local.link.mbps);auto& engine=*engineOwner;local.startedUs=rt::monotonicUs()+50000;
        for(unsigned i=0;i<2;++i)producers[i]=std::thread([&,i,initialNdi=knownNdiBytes,initialEp=knownEpBytes]{
            source(c,i+1,local.startedUs,sources[i],initialNdi,initialEp,observedRate);
        });
        observer=std::thread([&]{while(!observerStop){for(unsigned n=0;n<10&&!observerStop;++n)std::this_thread::sleep_for(std::chrono::milliseconds(100));if(observerStop)break;
            try{auto observation=observe(c);if(observation.mbps)observedRate=observation.mbps;std::lock_guard<std::mutex> lock(observationMutex);latestObservation=std::move(observation);}
            catch(const std::exception&e){std::lock_guard<std::mutex> lock(observationMutex);latestObservation.reason=std::string("Link observer failed; retaining last known rate: ")+e.what();}
            catch(...){std::lock_guard<std::mutex> lock(observationMutex);latestObservation.reason="Link observer failed; retaining last known rate";}
        }});
        std::uint64_t nextPublish=0,nextStatus=c.statusIntervalUs?local.startedUs:UINT64_MAX,drainStart=0;
        const auto stopAt=c.durationUs?local.startedUs+c.durationUs+100000:UINT64_MAX;
        constexpr unsigned maxDispatchesPerPoll=8;
        std::uint64_t lastServiceUs=0;
        unsigned dispatchesSinceService=0;
        auto nextService=[&](std::uint64_t packetDue){
            return lastServiceUs+(packetDue==UINT64_MAX?500u:100u);
        };
        const bool ringPath=c.mailboxFastPath && !c.handoffTrace;
        SenderWait lastWait;
        while(!stopRequested.load(std::memory_order_acquire)){
            auto now=rt::monotonicUs();
            bool sizeChanged=false;
            for(unsigned i=0;i<2;++i){
                PreparedPtr p;
                std::shared_ptr<TraceTicket> ticket;
                std::uint64_t nextSample=0;
                if(ringPath){
                    p=sources[i].pop();
                    nextSample=sources[i].nextSample.load(std::memory_order_acquire);
                }else{
                    auto& sourceBox=sources[i];
                    std::lock_guard<std::mutex> lock(sourceBox.mutex);
                    if(!sourceBox.queue.empty()){
                        if(c.handoffTrace){
                            // Mark every newly visible frame, including ones that
                            // remain behind the head until a later sender turn.
                            const auto poll=rt::monotonicUs();
                            for(auto& queued:sourceBox.queue){
                                auto& h=queued.ticket->handoff;
                                if(h.senderObservedUs)continue;
                                h.senderObservedUs=now>=h.enqueuedUs?now:poll;
                                if(lastWait.returnUs && h.enqueuedUs<=lastWait.returnUs){
                                    h.waitKind=lastWait.kind;h.waitResult=lastWait.result;
                                    h.waitBeginUs=lastWait.beginUs;h.waitReturnUs=lastWait.returnUs;
                                }
                            }
                        }
                        p=std::move(sourceBox.queue.front().frame);
                        ticket=std::move(sourceBox.queue.front().ticket);
                        sourceBox.queue.pop_front();
                        if(ticket)ticket->handoff.dequeuedUs=rt::monotonicUs();
                    }
                    nextSample=sourceBox.nextSample;
                }
                if(p){
                    auto& known=i==0?knownNdiBytes:knownEpBytes;
                    if(known!=p->payload->byteSize){known=p->payload->byteSize;sizeChanged=true;}
                    if(ticket){
                        auto h=std::move(ticket->handoff);
                        h.notifyIssueUs=std::shared_ptr<const std::atomic<std::uint64_t>>(ticket,&ticket->notifyIssueUs);
                        h.submittedUs=rt::monotonicUs();
                        engine.submit(std::move(p),std::move(h));
                    }else engine.submit(std::move(p));
                }engine.setNextSample(static_cast<std::uint8_t>(i+1),nextSample);
            }
            if(c.handoffTrace)lastWait={};
            if(!c.mailboxFastPath || hasStatus.load(std::memory_order_acquire)){
                std::lock_guard<std::mutex> lock(statusMutex);
                if(!statuses.empty()){
                    auto item=std::move(statuses.front());statuses.pop_front();
                    engine.submitStatus(std::move(item.first),item.second);
                }
                hasStatus.store(!statuses.empty(),std::memory_order_release);
            }
            if(c.durationUs&&now>=local.startedUs+c.durationUs)nextStatus=UINT64_MAX;
            if(now>=nextStatus){Bytes b(16);rt::writeLe32(b.data(),1);rt::writeLe32(b.data()+4,c.ndiEnabled?1:0);rt::writeLe32(b.data()+8,c.epEnabled?1:0);rt::writeLe32(b.data()+12,c.linkMbps);engine.submitStatus(std::move(b),now);nextStatus+=c.statusIntervalUs;if(nextStatus<=now)nextStatus=now+c.statusIntervalUs;}
            const auto currentObserved=observedRate.load();
            const auto budget=effectiveMbps(c,currentObserved);
            if(sizeChanged || budget!=local.capacity.effectiveMbps){
                local.capacity=preflight(c,knownNdiBytes,knownEpBytes,currentObserved);
                if(!local.capacity.feasible&&!c.allowOversubscribed)
                    throw std::runtime_error("Current payload sizes exceed the effective link budget");
            }
            engine.setEffectiveMbps(budget,now);
            // Exactly one send per turn. Stop and producer queues are revisited at every packet boundary.
            const auto stepUs=now;
            const bool sent=engine.stepTransport(stepUs,*udp,rt::monotonicUs);
            if(sent)++dispatchesSinceService;
            now=rt::monotonicUs();
            auto packetDue=engine.nextDueUs();
            // A short pacing gap does not end a burst. Bound feedback latency by
            // both dispatch count and elapsed time, then give the socket an idle poll.
            if(dispatchesSinceService>=maxDispatchesPerPoll || now>=nextService(packetDue) ||
               now>=nextPublish || now>=stopAt || (packetDue==UINT64_MAX && dispatchesSinceService)){
                lastServiceUs=rt::monotonicUs();
                dispatchesSinceService=0;
                IncomingDatagram incoming;
                for(unsigned n=0;n<4&&!stopRequested&&(!n||rt::monotonicUs()-lastServiceUs<40);++n){
                    ++local.feedbackPollCalls;
                    const auto outcome=udp->receiveFeedback(incoming);
                    if(outcome==ReceiveOutcome::Error){
                        ++local.feedbackReadErrors;
                        local.lastFeedbackReadError=udp->lastError();
                        break;
                    }
                    if(outcome==ReceiveOutcome::Empty){++local.feedbackEmptyPolls;break;}
                    if(incoming.sourceIp!=c.allowedNackIp||(c.allowedNackPort&&incoming.sourcePort!=c.allowedNackPort))engine.rejectFeedbackSource();
                    else engine.feedback(incoming.bytes.data(),incoming.bytes.size(),rt::monotonicUs());
                    if(engine.nextDueUs()<=rt::monotonicUs())break;
                }
                now=rt::monotonicUs();
                if(now>=nextPublish){publish(&engine);nextPublish=now+100000;}
                const bool done=sources[0].done&&sources[1].done&&(c.ndiEnabled||c.epEnabled||(c.durationUs&&now>=local.startedUs+c.durationUs));
                if(done&&!engine.pendingInitial() && !udp->inFlight()){
                    bool empty=true;
                    for(auto& box:sources){
                        if(ringPath)empty=empty && box.read.load()==box.write.load();
                        else {std::lock_guard<std::mutex> lock(box.mutex);empty=empty && box.queue.empty();}
                    }
                    if(empty){
                        if(!drainStart)drainStart=now;
                        if(now-drainStart>=100000){local.endedReason="sources_finished_and_retry_grace_elapsed";break;}
                    }
                }
                if(now>=stopAt){local.endedReason="duration_expired";break;}
                packetDue=engine.nextDueUs(); // Feedback may have scheduled a retry.
            }
            // A pacing slot may have matured since the step's clock sample. Only
            // a step declined at an already-due slot needs the guarded retry delay.
            if(packetDue<=now&&(sent||packetDue>stepUs))continue;
            // A guarded retry revisits the producer queues at the next packet
            // opportunity. Pace short on-wire gaps as before. With no pending
            // packets, wait on the wake event until the next idle feedback poll.
            auto due=packetDue<=now ? now+20 : packetDue;
            due=std::min({due,nextStatus,nextPublish,nextService(packetDue),stopAt});
            if(packetDue!=UINT64_MAX && due<=now+180){
                if(c.handoffTrace){lastWait.kind="brief_pacing";lastWait.beginUs=rt::monotonicUs();}
                rt::waitBrieflyUntil(due);
                if(c.handoffTrace){lastWait.returnUs=rt::monotonicUs();lastWait.result="pacing_return";}
            }else wake.until(due,c.handoffTrace?&lastWait:nullptr);
        }
        if(local.endedReason.empty())local.endedReason="user_stop";
    }catch(const std::exception&e){local.error=e.what();local.endedReason="local_failure";}
    catch(...){local.error="Unknown worker error";local.endedReason="local_failure";}
    producerStop.request();for(auto& t:producers)if(t.joinable())t.join();observerStop=true;if(observer.joinable())observer.join();
    for(auto& sourceBox:sources){std::lock_guard<std::mutex> lock(sourceBox.mutex);sourceBox.stats.queueDrops+=sourceBox.queue.size();sourceBox.queue.clear();while(sourceBox.pop())++sourceBox.stats.queueDrops;}
    {std::lock_guard<std::mutex> lock(statusMutex);cancelledStatuses+=statuses.size();statuses.clear();}
    if(udp)udp->cancelOutstanding();
    if(engineOwner)engineOwner->stop();
    if(local.feedbackReadErrors && local.error.empty())
        local.error="UDP feedback read error: "+local.lastFeedbackReadError;
    local.finished=true;local.endedUs=rt::monotonicUs();
    // A throw here would escape the worker thread and terminate the process while
    // busy() still reports true. Keep the flag transition unconditional.
    try{publish(engineOwner.get());}catch(const std::exception&e){if(local.error.empty())local.error=std::string("publish failed: ")+e.what();}catch(...){if(local.error.empty())local.error="publish failed";}
    active.store(false,std::memory_order_release);
}
Session::Session(TransportFactory f,rt::aurora::NdiSourceFactory n,LinkObserver observer)
    :p_(std::make_unique<Impl>(std::move(f),std::move(n),std::move(observer))){}
Session::~Session(){stop();if(p_->worker.joinable())p_->worker.join();}
void Session::setTransportFactory(TransportFactory factory){
    if(busy())throw std::logic_error("Cannot switch transport during a V2 session");
    if(!factory)throw std::invalid_argument("Missing V2 transport factory");
    if(p_->worker.joinable())p_->worker.join();
    p_->factory=std::move(factory);
}
void Session::start(const Config& config){
    if(busy())throw std::runtime_error("V2 session is already active or stopping");
    if(p_->worker.joinable())p_->worker.join();validateConfig(config);
    auto c=config;if(c.runId.empty())c.runId="v2-"+std::to_string(rt::monotonicUs());
    auto initial=std::make_shared<State>();
    initial->config=c;
    initial->capacity=preflight(c);
    initial->sources[0].nextId=c.initialNdiId;
    initial->sources[1].nextId=c.initialEpId;
    initial->engine.nextStatusSequence=c.initialStatusSequence;
    initial->engine.effectiveMbps=effectiveMbps(c);
    p_->publishState(std::move(initial));
    {std::lock_guard<std::mutex> lock(p_->statusMutex);p_->statuses.clear();p_->hasStatus=false;p_->acceptedStatuses=0;p_->rejectedStatuses=0;p_->cancelledStatuses=0;}
    p_->producerStop.reset();p_->active=true;
    try{p_->worker=std::thread([this,c]{p_->execute(c);});}catch(...){p_->active=false;throw;}
}
void Session::stop()noexcept{p_->producerStop.request();p_->wake.notify();}
bool Session::busy()const noexcept{return p_->active.load(std::memory_order_acquire);}
bool Session::epHandoffExceeded(std::uint64_t thresholdUs)const{
    std::lock_guard<std::mutex> lock(p_->mutex);
    const auto& state=*p_->state;
    const auto& ep=state.engine.modules[EpModule-1].readyToFirst;
    return state.config.handoffTrace && ep.count && ep.maxUs>thresholdUs;
}
bool Session::epFragmentSpanReached(std::uint64_t thresholdUs)const{
    std::lock_guard<std::mutex> lock(p_->mutex);
    const auto& state=*p_->state;
    const auto& ep=state.engine.worstFragmentSpan[EpModule-1];
    return state.config.fragmentTrace && ep && ep->spanUs()>=thresholdUs;
}
RunResult Session::snapshot()const{
    std::shared_ptr<const State> latest;
    {std::lock_guard<std::mutex> lock(p_->mutex);latest=p_->state;}
    const State& s=*latest;
    RunResult r;r.reportJson=report(s);r.error=s.error;r.nextNdiId=s.sources[0].nextId;r.nextEpId=s.sources[1].nextId;r.nextStatusSequence=s.engine.nextStatusSequence;
    r.success=s.finished&&s.error.empty()&&s.engine.statusSendErrors==0;
    for(unsigned i=0;i<2;++i)r.success=r.success&&!s.sources[i].errors&&!s.engine.modules[i].sendErrors;
    return r;
}
RunResult Session::wait(){if(p_->worker.joinable())p_->worker.join();return snapshot();}
bool Session::postStatus(const Bytes& b,std::uint64_t when){
    if(!busy()||p_->stopRequested||b.size()>1444)return false;if(!when)when=rt::monotonicUs();
    {
        std::lock_guard<std::mutex> lock(p_->statusMutex);
        if(p_->stopRequested||p_->statuses.size()>=64){++p_->rejectedStatuses;return false;}
        p_->statuses.emplace_back(b,when);++p_->acceptedStatuses;p_->hasStatus.store(true,std::memory_order_release);
    }
    p_->wake.notify();
    return true;
}
namespace {
void publishTrigger(const std::filesystem::path& destination,const std::string& json) {
    // The watcher treats existence as completion. Stage the complete document
    // on the same volume, with an exclusive staging directory per destination.
    auto staging=destination;
    staging+=".writing";
    if(!std::filesystem::create_directory(staging))
        throw std::runtime_error("trigger report staging directory already exists: "+staging.u8string());
    const auto temp=staging/"report.json";
    std::error_code ec;
    try {
        std::ofstream out(temp,std::ios::binary);
        out.exceptions(std::ios::failbit|std::ios::badbit);
        out<<json<<'\n';
        out.close();
        if(std::filesystem::exists(destination))throw std::runtime_error("trigger report already exists: "+destination.u8string());
        std::filesystem::rename(temp,destination);
    }catch(...){
        std::filesystem::remove(temp,ec);
        std::filesystem::remove(staging,ec);
        throw;
    }
    std::filesystem::remove(staging,ec);
}
}
RunResult run(const Config& c,const Session::TransportFactory& factory,const std::function<bool()>& cancelled,
              const std::string& handoffTriggerReport,const std::string& fragmentTriggerReport){
    const auto handoffPath=std::filesystem::u8path(handoffTriggerReport);
    const auto fragmentPath=std::filesystem::u8path(fragmentTriggerReport);
    bool handoffArmed=!handoffPath.empty(),fragmentArmed=!fragmentPath.empty();
    if(handoffArmed && (!c.handoffTrace || std::filesystem::exists(handoffPath)))
        throw std::invalid_argument("handoff trigger requires tracing and a new report path");
    if(fragmentArmed && (!c.fragmentTrace || std::filesystem::exists(fragmentPath)))
        throw std::invalid_argument("fragment trigger requires tracing and a new report path");
    if(handoffArmed && fragmentArmed){
        const auto first=std::filesystem::weakly_canonical(std::filesystem::absolute(handoffPath));
        const auto second=std::filesystem::weakly_canonical(std::filesystem::absolute(fragmentPath));
#ifdef _WIN32
        const bool same=CompareStringOrdinal(first.c_str(),-1,second.c_str(),-1,TRUE)==CSTR_EQUAL;
#else
        const bool same=first==second;
#endif
        if(same)throw std::invalid_argument("handoff and fragment triggers require distinct report paths");
    }
    Session session(factory);
    session.start(c);
    unsigned polls=0;
    auto captureIfDue=[&]{
        const bool handoff=handoffArmed && session.epHandoffExceeded(6999);
        const bool fragments=fragmentArmed && session.epFragmentSpanReached(18000);
        if(!handoff && !fragments)return;
        const auto json=session.snapshot().reportJson;
        if(handoff){publishTrigger(handoffPath,json);handoffArmed=false;}
        if(fragments){publishTrigger(fragmentPath,json);fragmentArmed=false;}
    };
    while(session.busy()){
        if(cancelled && cancelled())session.stop();
        if((handoffArmed || fragmentArmed) && ++polls==10){polls=0;captureIfDue();}
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    captureIfDue();
    return session.wait();
}
}
