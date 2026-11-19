#include "application/StreamRunner.h"
#include "application/AcceptanceReport.h"
#include "core/RealtimeStream.h"
#include "core/SamplingClock.h"
#include <sstream>
#include <iomanip>
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif
namespace rt {
    Snapshot runStream(const StreamConfig& config,IUdpTransport& udp,CommandDispatcher& commands,const SnapshotCallback& publish,const LogCallback& log,const RunnerHooks& hooks) {
        const std::function<std::uint64_t()> clock=hooks.clock?hooks.clock:monotonicUs;
        const std::function<void(std::uint64_t)> waitUntil=hooks.waitUntil?hooks.waitUntil:waitBrieflyUntil;
        udp.open(config);
        // Module files/templates are prepared by the constructor BEFORE the first sampling deadline.
        // Reset origin after bounded preparation through a fresh initial timestamp in first tick below.
        RealtimeStream stream(config,clock(),clock);
        auto& metrics=stream.diagnostics();
        auto& trace=stream.timingTrace();
#ifdef _WIN32
        const auto threadId=static_cast<std::uint64_t>(GetCurrentThreadId());
#else
        const std::uint64_t threadId=0;
#endif
        bool publishDeferred=false;
        enum class FeedbackPhase { Receive, ControlPoll, Commands };
        auto feedbackPhase=FeedbackPhase::Receive;
        constexpr std::uint64_t PublishPeriodUs=100000,MaxPublishDeferralUs=100000,PublishSafetyMarginUs=1000;
        auto nextFeedback=clock(),nextPublish=nextFeedback;
        std::uint64_t lastSendErrors=0;
        log("UDP bound to "+config.bindIp+":"+std::to_string(udp.localPort())+"; DF enabled; no READY/FIN/SUCCESS messages");
        log("TS source: SIMULATED host monotonic microsecond counter; module bodies: opaque test bytes");
        if(config.cachePolicy==CachePolicy::FifoErratum)log("EXPLICIT ERRATUM MODE: FIFO cache replaces documented logic_id%3 indexing");
        if(config.allowOversubscribed)log("FORMAT-ONLY overload testing allowed; 15ms deadline is NOT assured");
        // Discover an already-requested local Stop before starting the sampling
        // clock. Later control polls are ordinary bounded work, never a reason to
        // postpone a sampling deadline that has already arrived.
        if(hooks.pollControl)hooks.pollControl();
        stream.resetSchedule(clock());
        const auto stopRequested=[&] {
            if(hooks.cancelRequested&&hooks.cancelRequested()) {
                stream.stop(clock());
                return true;
            }
            if(auto c=commands.takeStop()) {
                const auto at=clock();
                if(at>=c->queuedUs)metrics.commandMaxWaitUs=std::max(metrics.commandMaxWaitUs,at-c->queuedUs);
                stream.stop(at);
                return true;
            }
            return false;
        };
        while(stream.running()) {
            // Also runs immediately after waitUntil returns. Only cancellation/Stop
            // precedes a due sample; receive, control polling and ordinary
            // commands wait until the next scheduling turn.
            if(stopRequested())break;
            auto now=clock();
            if(now>=nextFeedback&&now<stream.nextSamplingDeadlineUs()) {
                if(feedbackPhase==FeedbackPhase::Receive) {
                    IncomingDatagram d;
                    const auto feedbackStart=clock();
                    for(int i=0;i<32;++i) {
                        if(stopRequested())break;
                        const auto itemStart=clock();
                        if(itemStart>=stream.nextSamplingDeadlineUs()||itemStart-feedbackStart>=50)break;
                        const auto receiveStart=clock();
                        trace.record(TraceKind::ReceiveBegin,receiveStart,stream.currentLogicId());
                        const bool received=udp.receive(d);
                        const auto receiveEnd=clock();
                        metrics.receiveProfile.add(receiveEnd-receiveStart);
                        trace.record(TraceKind::ReceiveEnd,receiveEnd,stream.currentLogicId(),received?d.bytes.size():0);
                        if(!received)break;
                        ++metrics.nackDatagramsReceived;
                        if((!config.allowedNackIp.empty()&&config.allowedNackIp!="*"&&d.sourceIp!=config.allowedNackIp)||(config.allowedNackPort&&d.sourcePort!=config.allowedNackPort)) {
                            stream.rejectedSource();
                            continue;
                        }
                        const auto decodeStart=clock();
                        try {
                            Command c;
                            c.kind=Command::Kind::NackFeedback;
                            c.nack=decodeNack(d.bytes.data(),d.bytes.size());
                            ++metrics.nackParsed;
                            if(!commands.post(std::move(c)))stream.queueRejected();
                        }
                        catch(const ProtocolError&) {
                            stream.badNack();
                        }
                        metrics.decodeProfile.add(clock()-decodeStart);
                    }
                    // Count the final item too, including a slow empty receive.
                    // A budget bounds dispatch, not the duration of a syscall.
                    if(clock()-feedbackStart>=50)++metrics.feedbackBudgetHits;
                    feedbackPhase=FeedbackPhase::ControlPoll;
                }
                // Bounded work prevents a feedback flood from starving initial DATA transmission.
                if(stream.running()&&!stopRequested()&&clock()<stream.nextSamplingDeadlineUs()) {
                    const auto commandStart=clock();
                    trace.record(TraceKind::CommandsBegin,commandStart,stream.currentLogicId());
                    if(feedbackPhase==FeedbackPhase::ControlPoll&&hooks.pollControl)hooks.pollControl();
                    feedbackPhase=FeedbackPhase::Commands;
                    metrics.commandQueueHighWater=std::max(metrics.commandQueueHighWater,std::uint64_t(commands.size()));
                    bool commandsVisited=false;
                    for(int i=0;i<32;++i) {
                        if(stopRequested())break;
                        const auto itemStart=clock();
                        if(itemStart>=stream.nextSamplingDeadlineUs()||itemStart-commandStart>=50)break;
                        commandsVisited=true;
                        auto c=commands.take();
                        if(!c)break;
                        if(itemStart>=c->queuedUs)metrics.commandMaxWaitUs=std::max(metrics.commandMaxWaitUs,itemStart-c->queuedUs);
                        if(c->kind==Command::Kind::Stop) {
                            stream.stop(clock());
                            break;
                        }
                        if(c->kind==Command::Kind::NackFeedback)stream.acceptNack(c->nack);
                        else if(c->kind==Command::Kind::SetMask) {
                            try {
                                stream.setModuleMask(c->mask);
                                log("Module mask applied at next sample: "+std::to_string(c->mask));
                            }
                            catch(const std::exception&e) {
                                log(std::string("Rejected mask: ")+e.what());
                            }
                        }
                    }
                    const auto commandEnd=clock();
                    if(commandEnd-commandStart>=50)++metrics.commandBudgetHits;
                    metrics.commandProfile.add(commandEnd-commandStart);
                    trace.record(TraceKind::CommandsEnd,commandEnd,stream.currentLogicId(),commandEnd-commandStart);
                    // Resume after an interrupted receive/poll instead of
                    // repeating it forever while ordinary commands starve.
                    if(commandsVisited&&commandEnd<stream.nextSamplingDeadlineUs())feedbackPhase=FeedbackPhase::Receive;
                }
                // When sampling interrupts either phase, resume control work on
                // the following turn instead of deferring it another full poll
                // interval. A mask still queued at the deadline affects the
                // first sample AFTER its command is applied, never retroactively.
                now=clock();
                if(now<stream.nextSamplingDeadlineUs())nextFeedback=now+(stream.initialPending()?100u:1000u);
            }
            if(!stream.running()||stopRequested())break;
            now=clock();
            stream.tick(now,[&](const auto*p,std::size_t n) {
                return udp.send(p,n);
            });
            now=clock();
            if(now>=nextPublish&&stream.running()&&now<stream.nextSamplingDeadlineUs()) {
                const auto samplingDeadline=stream.nextSamplingDeadlineUs();
                const bool detailed=!stream.initialPending()&&samplingDeadline>=now&&
                    samplingDeadline-now>=PublishSafetyMarginUs;
                if(!detailed&&!publishDeferred) {++metrics.deferredSnapshots;publishDeferred=true;}
                if(detailed||now-nextPublish>=MaxPublishDeferralUs) {
                    metrics.maxSnapshotDeferralUs=std::max(metrics.maxSnapshotDeferralUs,now-nextPublish);
                    if(detailed)++metrics.detailedSnapshots;else ++metrics.lightSnapshots;
                    const auto publishStart=now;
                    trace.record(TraceKind::PublishBegin,publishStart,stream.currentLogicId(),detailed?1:0);
                    auto snap=stream.snapshot(now,detailed);
                    snap.workerThreadId=threadId;
                    snap.localPort=udp.localPort();
                    snap.dfEnabled=udp.dfEnabled();
                    snap.transport=udp.name();snap.sendBufferBytes=udp.sendBufferBytes();snap.receiveBufferBytes=udp.receiveBufferBytes();snap.connResetError=udp.connResetError();
                    publish(snap);
                    if(snap.stats.sendErrors>lastSendErrors) {
                        log("Local send errors="+std::to_string(snap.stats.sendErrors)+" latest="+udp.lastError());
                        lastSendErrors=snap.stats.sendErrors;
                    }
                    const auto publishEnd=clock();
                    metrics.publishProfile.add(publishEnd-publishStart);
                    trace.record(TraceKind::PublishEnd,publishEnd,stream.currentLogicId(),publishEnd-publishStart);
                    nextPublish=publishEnd+PublishPeriodUs;
                    publishDeferred=false;
                }
            }
            // Fixed-size event writes only: serialization happens after the send loop.
            if(stream.running()) {
                const auto waitStart=clock();
                const auto wake=std::min(stream.nextWakeUs(waitStart),nextFeedback);
                waitUntil(wake);
                const auto waitEnd=clock();
                // An already-due retry may use deadline 0: no wait was requested.
                // Do not turn that sentinel (or work already late before wait) into
                // an apparent epoch-sized scheduler stall.
                if(wake>waitStart&&waitEnd>wake&&waitEnd-wake>=TimingTrace::SlowStageThresholdUs) {
                    trace.record(TraceKind::WaitBegin,waitStart,stream.currentLogicId(),wake);
                    trace.record(TraceKind::WaitEnd,waitEnd,stream.currentLogicId(),wake);
                    if(waitEnd-wake>=1000)
                        trace.trigger(TraceReason::WaitLateness,waitEnd,stream.currentLogicId(),waitEnd-wake);
                }
            }
        }
        trace.finish();
        auto result=stream.snapshot(clock());
        result.workerThreadId=threadId;
        result.timingTrace=trace.report();
        result.localPort=udp.localPort();
        result.dfEnabled=udp.dfEnabled();
        result.transport=udp.name();result.sendBufferBytes=udp.sendBufferBytes();result.receiveBufferBytes=udp.receiveBufferBytes();result.connResetError=udp.connResetError();
        publish(result);
        log(result.detail);
        return result;
    }
    namespace {
        void writeTimingTrace(std::ostream& o,const Snapshot& snapshot) {
            const auto& trace=snapshot.timingTrace;
            o<<"  \"timing_trace\": {\"version\":3,\"enabled\":"<<(snapshot.config.timingTraceEnabled?"true":"false")
             <<",\"exported\":"<<(trace.enabled?"true":"false")
             <<",\"packet_stage_min_us\":"<<TimingTrace::SlowStageThresholdUs
             <<",\"wait_lateness_event_min_us\":"<<TimingTrace::SlowStageThresholdUs
             <<",\"slow_send_trigger_min_us\":"<<TimingTrace::SlowSendThresholdUs
             <<",\"wait_lateness_requires_future_deadline\":true"
             <<",\"event_policy\":\"all frame/build/initial and receive/command/publish boundaries; packet stages and future-wait lateness >=100us only; profiles still cover every operation\""
             <<",\"clock\":\"host monotonic microseconds (QPC-derived on Windows), not NIC timestamps\""
             <<",\"capture_policy\":\"greatest two values per reason; earlier trigger wins ties; overlapping post-windows allowed; fixed event counts, not fixed time; not a representative sample\""
             <<",\"sequence_policy\":\"recording order; deferred span start timestamps may precede earlier recorded events\""
             <<",\"accounting\":\"total_triggers = triggers_not_captured + windows_evicted + retained windows; not_captured means never admitted, evicted means admitted then replaced\""
             <<",\"before_capacity\":"<<TimingTrace::BeforeCapacity<<",\"after_capacity\":"<<TimingTrace::AfterCapacity
             <<",\"windows_per_reason\":"<<TimingTrace::WindowsPerReason
             <<",\"window_capacity\":"<<TimingTrace::WindowCapacity<<",\"total_events\":"<<trace.totalEvents
             <<",\"total_triggers\":"<<trace.totalTriggers<<",\"triggers_not_captured\":"<<trace.triggersNotCaptured
             <<",\"windows_evicted\":"<<trace.windowsEvicted
             <<",\"rolling_events_overwritten\":"<<trace.eventsOverwritten
             <<",\"event_values\":\"Sample=planned sampling deadline before skipped-period adjustment; WaitBegin/WaitEnd=planned wake deadline; InitialBegin=original sample timestamp; InitialEnd=sample-to-last-attempt us; InitialAbandoned=unsent fragment count; InitialPacketGap=absolute gap deviation us; EncodeBegin/EncodeEnd/SendBegin/SendEnd=fragment index; PublishBegin=1 detailed/0 light; ReceiveEnd=received bytes; CommandsEnd/PublishEnd=duration us; other events=0\""
             <<",\"triggers_by_reason\": [";
            for(std::size_t i=0;i<trace.triggersByReason.size();++i) {
                if(i)o<<',';
                o<<"{\"reason\":\""<<traceReasonName(static_cast<TraceReason>(i))<<"\",\"count\":"<<trace.triggersByReason[i]
                 <<",\"not_captured\":"<<trace.notCapturedByReason[i]<<",\"evicted\":"<<trace.evictedByReason[i]<<'}';
            }
            o<<"],\"windows\":[";
            for(std::size_t i=0;i<trace.windows.size();++i) {
                if(i)o<<',';
                const auto& w=trace.windows[i];
                o<<"{\"reason\":\""<<traceReasonName(w.reason)<<"\",\"trigger_us\":"<<w.triggerUs
                 <<",\"trigger_sequence\":"<<w.triggerSequence<<",\"logic_id\":"<<w.logicId<<",\"value\":"<<w.value
                 <<",\"before_count\":"<<w.beforeCount<<",\"after_count\":"<<w.afterCount
                 <<",\"post_window_complete\":"<<(w.complete?"true":"false")<<",\"context\":";
                const auto& c=w.context;
                if(!c.present)o<<"null";
                else {
                    o<<"{\"has_previous\":"<<(c.hasPrevious?"true":"false")
                     <<",\"has_planned\":"<<(c.hasPlanned?"true":"false")
                     <<",\"traffic\":\""<<traceTrafficName(c.traffic)<<"\""
                     <<",\"previous_logic_id\":"<<c.previousLogicId
                     <<",\"previous_start_us\":"<<c.previousStartUs<<",\"previous_end_us\":"<<c.previousEndUs
                     <<",\"current_start_us\":"<<c.currentStartUs<<",\"current_end_us\":"<<c.currentEndUs
                     <<",\"previous_planned_us\":"<<(c.hasPrevious&&c.hasPlanned?std::to_string(c.previousPlannedUs):"null")
                     <<",\"current_planned_us\":"<<(c.hasPlanned?std::to_string(c.currentPlannedUs):"null")
                     <<",\"previous_fragment_index\":"<<(c.previousFragmentIndex==UINT16_MAX?"null":std::to_string(c.previousFragmentIndex))
                     <<",\"fragment_index\":"<<(c.fragmentIndex==UINT16_MAX?"null":std::to_string(c.fragmentIndex))
                     <<",\"previous_ordinal\":"<<(c.previousOrdinal==UINT16_MAX?"null":std::to_string(c.previousOrdinal))
                     <<",\"ordinal\":"<<(c.ordinal==UINT16_MAX?"null":std::to_string(c.ordinal))
                     <<",\"send_result_known\":"<<(c.sendResultKnown?"true":"false")
                     <<",\"accepted\":"<<(c.sendResultKnown?(c.accepted?"true":"false"):"null")<<'}';
                }
                o<<",\"events\":[";
                for(std::size_t j=0;j<w.events.size();++j) {
                    if(j)o<<',';
                    const auto& e=w.events[j];
                    o<<"{\"sequence\":"<<e.sequence<<",\"at_us\":"<<e.atUs<<",\"kind\":\""<<traceKindName(e.kind)
                     <<"\",\"logic_id\":"<<e.logicId<<",\"value\":"<<e.value
                     <<",\"traffic\":\""<<traceTrafficName(e.traffic)<<"\",\"ordinal\":"
                     <<(e.ordinal==UINT16_MAX?"null":std::to_string(e.ordinal))<<'}';
                }
                o<<"]}";
            }
            o<<"]}";
        }
        std::string escape(const std::string&s) {
            std::string o;
            for(unsigned char c:s) {
                if(c=='"'||c=='\\') {
                    o+='\\';
                    o+=char(c);
                }
                else if(c=='\n')o+="\\n";
                else if(c>=32)o+=char(c);
            }
            return o;
        }
    }
    std::string snapshotJson(const Snapshot& s) {
        std::ostringstream o;
        o<<"{\n  \"report_schema_version\": 2,\n  \"run_id\": \""<<escape(s.runId)<<"\",\n";
        o<<"  \"details_included\": "<<(s.detailsIncluded?"true":"false")<<", \"worker_thread_id\": "<<s.workerThreadId<<",\n";
        o<<"  \"snapshot_policy\": {\"period_us\":100000,\"max_deferral_us\":100000,\"sampling_guard_us\":1000,\"detail_requires_initial_complete\":true,\"final_snapshot_always_detailed\":true},\n";
        o<<"  \"build_type\": \""<<SIM_BUILD_TYPE<<"\",\n";
        o<<"  \"source_revision_at_configure\": \""<<SIM_SOURCE_REV<<"\", \"tracked_dirty_at_configure\": \""<<SIM_SOURCE_DIRTY<<"\",\n";
#ifdef _WIN32
        o<<"  \"pid\": "<<_getpid()<<",\n";
#else
        o<<"  \"pid\": "<<getpid()<<",\n";
#endif
        o<<"  \"transport\": \""<<escape(s.transport)<<"\",\n";
        o<<"  \"send_buffer_bytes\": "<<s.sendBufferBytes<<", \"receive_buffer_bytes\": "<<s.receiveBufferBytes<<", \"sio_udp_connreset_error\": "<<s.connResetError<<",\n";
        o<<"  \"bind_ip\": \""<<escape(s.config.bindIp)<<"\", \"target_ip\": \""<<escape(s.config.targetIp)<<"\", \"target_port\": "<<s.config.targetPort<<",\n";
        o<<"  \"allowed_nack_ip\": \""<<escape(s.config.allowedNackIp)<<"\", \"allowed_nack_port\": "<<s.config.allowedNackPort<<",\n";
        o<<"  \"cache_policy\": \""<<(s.config.cachePolicy==CachePolicy::SpecModulo?"modulo":"fifo-explicit")<<"\", \"link_mbps_capacity_input\": "<<s.config.linkMbps<<",\n";
        o<<"  \"feedback_budget_us\": 50, \"command_budget_us\": 50, \"per_phase_count_limit\": 32,\n";
        o<<"  \"initial_modules\": [";
        for(std::size_t i=0;i<s.config.modules.size();++i) {
            const auto& module=s.config.modules[i];if(i)o<<",";
            o<<"{\"id\":"<<unsigned(module.id)<<",\"initial_online\":"<<(module.online?"true":"false")<<",\"prepared_body_bytes\":"<<module.dataBytes<<",\"file\":\""<<escape(module.file)<<"\"}";
        }
        o<<"],\n  \"latest_frame_geometry\": [";
        for(std::size_t i=0;i<s.frames.size();++i) {
            const auto& f=s.frames[i];if(i)o<<",";
            o<<"{\"logic_id\":"<<f.id<<",\"frame_bytes\":"<<f.bytes<<",\"fragment_count\":"<<f.count<<",\"module_mask\":"<<f.mask<<",\"sample_monotonic_us\":"<<f.timestampUs<<",\"initial_finished\":"<<(f.initialFinished?"true":"false")<<"}";
        }
        o<<"],\n  \"faults\": {";
        auto ids=[&](const char* name,const auto& values) {
            o<<"\""<<name<<"\":[";bool first=true;
            for(auto value:values){if(!first)o<<",";first=false;o<<value;}o<<"],";
        };
        ids("drop_first",s.config.faults.dropFirst);ids("drop_always",s.config.faults.dropAlways);
        ids("corrupt_first",s.config.faults.corruptFirst);ids("duplicate_first",s.config.faults.duplicateFirst);
        o<<"\"reverse_initial\":"<<(s.config.faults.reverseInitial?"true":"false")<<"},\n";
        o<<"  \"built_manifest\": {\"first_id\": "<<s.config.initialLogicId<<", \"count\": "<<s.stats.built<<", \"next_id\": "<<s.nextLogicId<<", \"encoding\": \"consecutive uint32 modulo IDs; only successful builds consume IDs\"},\n";
        o<<"  \"protocol\": \"UDP multi-module document, LE AA55A55A\",\n  \"receiver_delivery\": \"unknown: no ACK/SUCCESS in this protocol\",\n  \"timestamp_source\": \"SIMULATED host monotonic microseconds\",\n";
        o<<"  \"detail\": \""<<escape(s.detail)<<"\",\n  \"running\": "<<(s.running?"true":"false")<<",\n  \"local_port\": "<<s.localPort<<",\n  \"df_enabled\": "<<(s.dfEnabled?"true":"false")<<",\n  \"next_logic_id\": "<<s.nextLogicId<<",\n  \"stats\": {\n";
#define STAT(name) o<<"    \"" #name "\": "<<s.stats.name<<",\n";
        STAT(sampled) STAT(built) STAT(skippedNoModules) STAT(skippedOverrun) STAT(buildRejected)
        STAT(firstAttempts) STAT(datagrams) STAT(bytesSent) STAT(retransmissions) STAT(injectedDrops) STAT(injectedCorrupt)
        STAT(nacks) STAT(invalidNacks) STAT(cacheMisses) STAT(duplicatePending) STAT(queueDrops) STAT(sourceRejected)
        STAT(unsentExpired) STAT(sendErrors) STAT(deadlineMisses) STAT(maxSampleLatenessUs) STAT(maxSendLatenessUs) STAT(lastInitialDurationUs)
        STAT(initialFinishedFrames) STAT(initialFullySubmittedFrames) STAT(initialIncompleteFrames) STAT(unsentAtStop) STAT(initialPendingFragments) STAT(initialFragmentsPlanned)
        STAT(lastSampleToInitialEndUs) STAT(actualInitialDeadlineMisses) STAT(samplingSpanUs) STAT(samplingActiveUs) STAT(firstSampleUs) STAT(lastSampleUs)
        STAT(maxPlannedSendLatenessUs) STAT(nackDatagramsReceived) STAT(nackParsed) STAT(cacheHits) STAT(requestedFragments)
        STAT(retryQueueHighWater) STAT(retryMaxWaitUs) STAT(retriesCancelled) STAT(commandQueueHighWater) STAT(commandMaxWaitUs) STAT(feedbackBudgetHits) STAT(commandBudgetHits)
        STAT(detailedSnapshots) STAT(lightSnapshots) STAT(deferredSnapshots) STAT(maxSnapshotDeferralUs)
        STAT(samplingIntervalViolations) STAT(initialPacketGapViolations)
        STAT(socketSendCalls) STAT(initialFirstCopySendCalls) STAT(initialDuplicateSendCalls) STAT(retrySendCalls)
        STAT(initialGapSegments) STAT(initialInjectedDropAttempts) STAT(retryInjectedDropAttempts)
        STAT(minInitialNominalGapUs) STAT(maxInitialNominalGapUs)
        STAT(minObservedDatagramBytes) STAT(maxObservedDatagramBytes)
#undef STAT
        o<<"    \"elapsedUs\": "<<s.stats.elapsedUs<<"\n  },\n";
        auto distribution=[&](const char* name,const Distribution& d) {
            o<<"    \""<<name<<"\": {\"count\": "<<d.count<<", \"max_us\": "<<d.maxUs<<", \"p50_us\": "<<d.p50Us<<", \"p95_us\": "<<d.p95Us<<", \"p99_us\": "<<d.p99Us<<", \"p999_us\": "<<d.p999Us<<", \"over_limit\": "<<d.overLimit<<"}";
        };
        o<<"  \"timing\": {\n    \"quantiles\": \"all observations, 100us bucket upper bounds; >100ms overflow uses exact maximum\",\n";
        o<<"    \"initial_population\": \"fully locally submitted initial frames only; excludes incomplete, injected-drop and local-send-error frames; no delivery claim\",\n";
        o<<"    \"legacy\": \"lastInitialDurationUs and deadlineMisses retain tick-entry scheduling-span semantics; actualInitialDeadlineMisses uses post-send time; unsentExpired counts replacement at next sample only\",\n";
        o<<"    \"sampling_window\": \"samplingSpanUs=last-first actual sample; samplingActiveUs adds one nominal 20ms period; elapsedUs includes drain\",\n";
        distribution("sample_to_last_initial_submit",s.initialDuration);o<<",\n";
        distribution("first_send_call_to_last_completion",s.initialCallSpan);o<<",\n";
        distribution("sampling_interval",s.samplingIntervals);o<<",\n";
        distribution("abs_interval_minus_20000us",s.samplingDeviation);o<<",\n";
        o<<"    \"sampling_violation_rule\": \"absolute deviation >=1000us; over_limit and samplingIntervalViolations use this strict <1ms requirement\",\n";
        o<<"    \"packet_gap_measurement\": {\"version\":1,\"clock\":\"user-space send-call start, not NIC timestamps\",\"initial_population\":\"adjacent first-copy send ordinals within one frame; injected drops break segments; local errors are still observed calls; duplicate copies excluded\",\"initial_expected_count\":\"initialFirstCopySendCalls - initialGapSegments\",\"deviation\":\"absolute gap minus pacer nominal slot gap (may vary with frame size); >=1000us violates\",\"retry_and_socket_gaps\":\"include idle and interleaved traffic, no fixed 10ms or 20us acceptance threshold\"},\n";
        distribution("initial_packet_gap",s.initialPacketGaps);o<<",\n";
        distribution("initial_packet_gap_abs_deviation",s.initialPacketGapDeviation);o<<",\n";
        distribution("retry_packet_gap",s.retryPacketGaps);o<<",\n";
        distribution("socket_call_gap",s.socketCallGaps);o<<"\n  },\n  \"profiles\": {\n";
        auto profile=[&](const char* name,const StageProfile& p) {
            o<<"    \""<<name<<"\": {\"calls\": "<<p.calls<<", \"total_us\": "<<p.totalUs<<", \"max_us\": "<<p.maxUs<<"}";
        };
        profile("receive",s.stats.receiveProfile);o<<",\n";profile("decode_and_enqueue",s.stats.decodeProfile);o<<",\n";
        profile("commands",s.stats.commandProfile);o<<",\n";profile("build_and_cache",s.stats.buildProfile);o<<",\n";
        profile("encode_and_crc",s.stats.encodeCrcProfile);o<<",\n";profile("send",s.stats.sendProfile);o<<",\n";
        profile("snapshot_and_publish",s.stats.publishProfile);o<<"\n  },\n";
        o<<"  \"cache_miss_events_dropped\": "<<s.cacheMissEventsDropped<<", \"cache_miss_events\": [";
        bool first=true;
        for(const auto& e:s.cacheMissEvents) {
            if(!first)o<<",";first=false;
            o<<"{\"requested\":"<<e.requested<<",\"latest\":"<<e.latest<<",\"signed_latest_minus_requested\":"<<e.distance<<",\"local_monotonic_us\":"<<e.localUs<<",\"cache_ids\":[";
            for(std::size_t i=0;i<e.cached.size();++i){if(i)o<<",";o<<e.cached[i];}
            o<<"]}";
        }
        o<<"],\n";
        writeTimingTrace(o,s);
        o<<",\n  \"acceptance_table\": "<<acceptanceTableJson(s)<<"\n}\n";
        return o.str();
    }
}
