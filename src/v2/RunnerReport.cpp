#include "v2/RunnerReport.h"
#include <iomanip>
#include <sstream>

namespace rt::v2::runner_detail {
namespace {
std::string quote(const std::string &s) {
    std::ostringstream o;
    o << '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            o << "\\\"";
            break;
        case '\\':
            o << "\\\\";
            break;
        case '\n':
            o << "\\n";
            break;
        case '\r':
            o << "\\r";
            break;
        case '\t':
            o << "\\t";
            break;
        default:
            if (c < 32)
                o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c)
                  << std::dec;
            else
                o << c;
        }
    }
    o << '"';
    return o.str();
}

std::string distribution(const Distribution &d) {
    std::ostringstream o;
    o << "{\"count\":" << d.count << ",\"max_us\":" << d.maxUs << ",\"p50_us\":" << d.p50Us
      << ",\"p95_us\":" << d.p95Us << ",\"p99_us\":" << d.p99Us << ",\"p999_us\":" << d.p999Us
      << ",\"violations\":" << d.overLimit << '}';
    return o.str();
}

void writeProducerWait(std::ostream &o, const ProducerWaitStats &w) {
    o << "{\"backend\":" << quote(w.backend) << ",\"timed_wait_slots\":" << w.timedWaitSlots
      << ",\"spin_only_slots\":" << w.spinOnly << ",\"due_on_entry\":" << w.alreadyDue
      << ",\"cancelled\":" << w.cancelled << ",\"fallbacks\":" << w.fallbacks
      << ",\"early_returns\":" << w.earlyReturns << ",\"returns_past_slot\":" << w.returnsPastSlot
      << ",\"lead_us\":" << w.leadUs << ",\"lead_floor_us\":" << w.leadFloorUs
      << ",\"lead_cap_us\":" << w.leadCapUs
      << ",\"priority_raised\":" << (w.priorityRaised ? "true" : "false")
      << ",\"priority_error\":" << w.priorityError << ",\"spin_wall_us\":" << w.spinWallUs
      << ",\"max_wake_overshoot_us\":" << w.overshootMaxUs
      << ",\"wake_overshoot_ge_1000_us\":" << w.overshootGe1ms << ",\"worst_slot_us\":\""
      << w.worstSlotUs << '"' << ",\"worst_wake_target_us\":\"" << w.worstTargetUs
      << "\",\"worst_wake_return_us\":\"" << w.worstReturnUs << "\"}";
}

std::string ledgerJson(const Ledger &l) {
    std::ostringstream o;
    o << "{\"count\":" << l.count << ",\"id_sum\":\"" << l.idSum << "\",\"crc_sum\":\"" << l.crcSum
      << "\",\"stamp_sum\":\"" << l.stampSum << "\"}";
    return o.str();
}

std::string nullableTime(std::uint64_t us) {
    return us ? '"' + std::to_string(us) + '"' : "null";
}

void writeDispatchIdentity(std::ostream &o, const DispatchIdentity &previous) {
    if (!previous.moduleId) {
        o << "null";
        return;
    }
    o << "{\"module_id\":" << unsigned(previous.moduleId) << ",\"frame_id\":" << previous.frameId
      << ",\"fragment_index\":" << previous.index
      << ",\"start_us\":" << nullableTime(previous.startUs)
      << ",\"end_us\":" << nullableTime(previous.endUs)
      << ",\"retry\":" << (previous.retry ? "true" : "false") << '}';
}

void writeFirstDispatch(std::ostream &o, const FirstDispatchEvent &e) {
    const auto &h = e.handoff;
    const auto notified = h.notifyIssueUs ? h.notifyIssueUs->load(std::memory_order_relaxed) : 0;
    const char *relation = !h.waitKind
                               ? "no_associated_wait"
                               : (h.enqueuedUs < h.waitBeginUs ? "enqueued_before_wait"
                                                               : "enqueued_during_wait_window");
    o << "{\"module_id\":" << unsigned(e.moduleId) << ",\"frame_id\":" << e.frameId
      << ",\"sample_us\":" << nullableTime(e.sampleUs)
      << ",\"ready_us\":" << nullableTime(e.readyUs)
      << ",\"enqueued_us\":" << nullableTime(h.enqueuedUs)
      << ",\"notify_issue_us\":" << nullableTime(notified)
      << ",\"wait_relation\":" << quote(relation)
      << ",\"wait_kind\":" << (h.waitKind ? quote(h.waitKind) : "null")
      << ",\"wait_result\":" << (h.waitResult ? quote(h.waitResult) : "null")
      << ",\"wait_begin_us\":" << nullableTime(h.waitBeginUs)
      << ",\"wait_return_us\":" << nullableTime(h.waitReturnUs)
      << ",\"sender_observed_us\":" << nullableTime(h.senderObservedUs)
      << ",\"dequeued_us\":" << nullableTime(h.dequeuedUs)
      << ",\"submitted_us\":" << nullableTime(h.submittedUs)
      << ",\"selected_us\":" << nullableTime(e.selectedUs)
      << ",\"first_attempt_us\":" << nullableTime(e.firstAttemptUs)
      << ",\"ready_to_first_us\":" << e.readyToFirstUs()
      << ",\"admission_pacing_due_us\":" << nullableTime(e.admissionPacingDueUs)
      << ",\"pacing_due_us\":" << nullableTime(e.pacingDueUs)
      << ",\"intervening\":{\"ndi\":" << e.intervening.ndi << ",\"ep\":" << e.intervening.ep
      << ",\"status\":" << e.intervening.status << ",\"retry\":" << e.intervening.retry
      << "},\"heads_at_submit\":[";
    for (std::size_t i = 0; i < e.headsAtSubmit.size(); ++i) {
        if (i)
            o << ',';
        const auto &head = e.headsAtSubmit[i];
        if (!head.moduleId) {
            o << "null";
            continue;
        }
        o << "{\"module_id\":" << unsigned(head.moduleId) << ",\"frame_id\":" << head.frameId
          << ",\"deadline_us\":" << nullableTime(head.deadlineUs)
          << ",\"next_fragment\":" << head.nextFragment
          << ",\"fragments_remaining\":" << head.fragmentsRemaining
          << ",\"duplicate_pending\":" << (head.duplicatePending ? "true" : "false") << '}';
    }
    o << "],\"previous_at_submit\":";
    writeDispatchIdentity(o, e.previousAtSubmit);
    o << ",\"previous_at_first\":";
    writeDispatchIdentity(o, e.previousAtFirst);
    o << '}';
}

void writeTraceClock(std::ostream &o, const State &s) {
    const auto &c = s.traceClock;
    o << "\"process_id\":" << c.processId << ",\"sender_tid\":" << c.senderTid
      << ",\"producer_tids\":{\"1\":" << s.sources[0].producerTid
      << ",\"2\":" << s.sources[1].producerTid << '}'
      << ",\"qpc_frequency_hz\":" << nullableTime(c.frequency)
      << ",\"qpc_before_ticks\":" << nullableTime(c.beforeTicks)
      << ",\"qpc_after_ticks\":" << nullableTime(c.afterTicks)
      << ",\"utc_filetime_100ns\":" << nullableTime(c.utcFiletime);
}

void writeHandoffTrace(std::ostream &o, const State &s) {
    if (!s.config.handoffTrace) {
        o << "{\"enabled\":false}";
        return;
    }
    o << "{\"enabled\":true,";
    writeTraceClock(o, s);
    o << ",\"notify_scope\":\"Timestamp immediately before SetEvent/notify_one; notification can "
         "follow dequeue, and its issue is not a kernel wake timestamp.\""
      << ",\"wait_scope\":\"Last sender wait before first queue observation, only if enqueue "
         "preceded its observed return; shared signals may coalesce. Brief pacing may spin. Null "
         "means no associated wait.\""
      << ",\"sender_observation\":\"First instrumented loop entry at or after enqueue, or first "
         "locked queue poll if enqueue occurred during that turn. Kernel ReadyThread and first "
         "scheduled instruction require an external trace.\""
      << ",\"worst_first_dispatch\":{";
    for (std::size_t i = 0; i < s.engine.worstFirstDispatch.size(); ++i) {
        if (i)
            o << ',';
        o << '"' << (i + 1) << "\":[";
        const auto &events = s.engine.worstFirstDispatch[i];
        for (std::size_t j = 0; j < events.size(); ++j) {
            if (j)
                o << ',';
            writeFirstDispatch(o, events[j]);
        }
        o << ']';
    }
    o << "}}";
}

void writeFragmentSpan(std::ostream &o, const std::shared_ptr<const FragmentSpan> &frame) {
    if (!frame) {
        o << "null";
        return;
    }
    const auto &f = *frame;
    o << "{\"module_id\":" << unsigned(f.moduleId) << ",\"frame_id\":" << f.frameId
      << ",\"sample_us\":" << nullableTime(f.sampleUs)
      << ",\"ready_us\":" << nullableTime(f.readyUs)
      << ",\"enqueue_us\":" << nullableTime(f.enqueuedUs)
      << ",\"dequeue_us\":" << nullableTime(f.dequeuedUs)
      << ",\"first_attempt_us\":" << nullableTime(f.firstAttemptUs)
      << ",\"end_us\":" << nullableTime(f.endUs) << ",\"span_us\":" << f.spanUs()
      << ",\"ready_to_first_us\":"
      << (f.firstAttemptUs >= f.readyUs ? f.firstAttemptUs - f.readyUs : 0)
      << ",\"ready_to_end_us\":" << (f.endUs >= f.readyUs ? f.endUs - f.readyUs : 0)
      << ",\"initial_complete\":" << (f.complete ? "true" : "false")
      << ",\"call_count\":" << f.callCount << ",\"own_call_us\":" << f.ownCallUs
      << ",\"before_first_call_us\":" << f.beforeFirstCallUs
      << ",\"between_calls_us\":" << f.betweenCallsUs
      << ",\"after_last_call_us\":" << f.afterLastCallUs << ",\"pacing_gap_us\":" << f.pacingGapUs
      << ",\"intervening\":{\"ndi\":" << f.intervening.ndi << ",\"ep\":" << f.intervening.ep
      << ",\"status\":" << f.intervening.status << ",\"retry\":" << f.intervening.retry
      << ",\"duplicate\":" << f.intervening.duplicate
      << "},\"fragment_count\":" << f.fragments.size() << ",\"fragments\":[";
    for (std::size_t i = 0; i < f.fragments.size(); ++i) {
        if (i)
            o << ',';
        const auto &p = f.fragments[i];
        o << "{\"fragment_index\":" << p.index
          << ",\"original_due_us\":" << nullableTime(p.originalDueUs)
          << ",\"pacing_due_us\":" << nullableTime(p.pacingDueUs)
          << ",\"pacing_gap_us\":" << p.pacingGapUs
          << ",\"attempt_us\":" << nullableTime(p.attemptUs)
          << ",\"end_us\":" << nullableTime(p.endUs)
          << ",\"call_start_us\":" << nullableTime(p.called ? p.callStartUs : 0)
          << ",\"call_end_us\":" << nullableTime(p.called ? p.endUs : 0)
          << ",\"submit_return_us\":" << nullableTime(p.submitReturnUs)
          << ",\"completion_observed_us\":" << nullableTime(p.pendingCompletion ? p.endUs : 0)
          << ",\"post_submit_wait_us\":";
        if (p.pendingCompletion)
            o << (p.endUs > p.submitReturnUs ? p.endUs - p.submitReturnUs : 0);
        else
            o << "null";
        o << ",\"called\":" << (p.called ? "true" : "false")
          << ",\"success\":" << (p.success ? "true" : "false") << '}';
    }
    o << "]}";
}

void writeGapPacket(std::ostream &o, const GapPacket &p) {
    o << "{\"module_id\":" << unsigned(p.moduleId) << ",\"id\":" << p.id
      << ",\"kind\":" << quote(p.kind) << ",\"fragment_index\":";
    if (p.moduleId == StatusModule)
        o << "null";
    else
        o << p.index;
    o << ",\"original_due_us\":\"" << p.planUs << "\",\"start_us\":\"" << p.startUs
      << "\",\"end_us\":\"" << p.endUs << "\",\"success\":" << (p.success ? "true" : "false")
      << ",\"ready_us\":" << nullableTime(p.readyUs)
      << ",\"enqueue_us\":" << nullableTime(p.enqueuedUs)
      << ",\"dequeue_us\":" << nullableTime(p.dequeuedUs)
      << ",\"call_start_us\":" << nullableTime(p.callStartUs)
      << ",\"submit_return_us\":" << nullableTime(p.submitReturnUs) << '}';
}

void writeGapPair(std::ostream &o, const WorstGap &worst, bool includeEpFrameIndex = false) {
    o << "{\"actual_gap_us\":" << worst.actualUs << ",\"expected_gap_us\":" << worst.expectedUs
      << ",\"deviation_us\":" << worst.deviationUs << ",\"previous\":";
    writeGapPacket(o, worst.previous);
    o << ",\"current\":";
    writeGapPacket(o, worst.current);
    if (includeEpFrameIndex) {
        o << ",\"previous_ep_frame_index\":";
        if (worst.previousEpFrameIndex)
            o << *worst.previousEpFrameIndex;
        else
            o << "null";
    }
    o << '}';
}

void writeWorstGap(std::ostream &o, const WorstGap &worst, const Distribution &all) {
    o << "{\"count\":" << all.count << ",\"max_us\":" << all.maxUs << ",\"pair\":";
    if (worst.current.kind)
        writeGapPair(o, worst);
    else
        o << "null";
    o << '}';
}

void writeFragmentTrace(std::ostream &o, const State &s) {
    if (!s.config.fragmentTrace) {
        o << "{\"enabled\":false}";
        return;
    }
    o << "{\"enabled\":true,";
    writeTraceClock(o, s);
    o << ",\"span_scope\":\"First original fragment attempt to last original attempt end. "
         "call_start_us and submit_return_us bound the transport submission. For pending submissions, "
         "call_end_us and completion_observed_us retain the observed completion timestamp. "
         "post_submit_wait_us is zero when completion was already observed by return. Immediate "
         "returns have null completion fields, and injected drops have no call timestamps. "
         "All original attempts of each retained finalized span are in dispatch order.\""
      << ",\"span_distribution_scope\":\"One observation per frame that reached its last original "
         "attempt, including send failures or injected drops. P99.9 uses the 100 us histogram "
         "bucket upper bound capped by the exact maximum. ge_18000_us includes equality.\""
      << ",\"pacing_scope\":\"Pacing deadlines are budgets. pacing_gap_us can overlap other "
         "dispatches; it is not an additional wall-time component or an observed kernel wait.\""
      << ",\"span_distribution_by_module\":{";
    for (std::size_t i = 0; i < s.engine.fragmentSpanDistribution.size(); ++i) {
        if (i)
            o << ',';
        const auto &d = s.engine.fragmentSpanDistribution[i];
        o << '"' << (i + 1) << "\":{\"count\":" << d.count << ",\"p999_us\":" << d.p999Us
          << ",\"max_us\":" << d.maxUs << ",\"ge_18000_us\":" << d.overLimit << '}';
    }
    o << "},\"worst_gap_pairs\":{\"global\":";
    writeWorstGap(o, s.engine.worstPacketGaps[0], s.engine.globalPacketGapDeviation);
    for (std::size_t i = 0; i < s.engine.modules.size(); ++i) {
        o << ",\"" << (i + 1) << "\":";
        writeWorstGap(o, s.engine.worstPacketGaps[i + 1], s.engine.modules[i].packetGapDeviation);
    }
    const auto &violations = s.engine.globalGapViolationPairs;
    o << "},\"global_gap_violations\":{\"threshold_us\":1000,\"capacity\":"
      << MaxGlobalGapViolationPairs << ",\"total\":" << s.engine.globalGapViolations
      << ",\"omitted\":" << (s.engine.globalGapViolations - violations.size()) << ",\"pairs\":[";
    for (std::size_t i = 0; i < violations.size(); ++i) {
        if (i)
            o << ',';
        writeGapPair(o, violations[i], true);
    }
    o << "],\"previous_ep_frames\":[";
    for (std::size_t i = 0; i < s.engine.gapPreviousEpFrames.size(); ++i) {
        if (i)
            o << ',';
        const auto &frame = s.engine.gapPreviousEpFrames[i];
        const char *state = frame.span ? "finalized" : frame.awaitingFinalize ? "pending" : "unavailable";
        o << "{\"frame_id\":" << frame.frameId << ",\"state\":" << quote(state) << ",\"span\":";
        writeFragmentSpan(o, frame.span);
        o << '}';
    }
    o << "]},\"worst_by_module\":{";
    for (std::size_t i = 0; i < s.engine.worstFragmentSpan.size(); ++i) {
        if (i)
            o << ',';
        o << '"' << (i + 1) << "\":";
        writeFragmentSpan(o, s.engine.worstFragmentSpan[i]);
    }
    o << "},\"worst_ndi_wait\":";
    writeFragmentSpan(o, s.engine.worstNdiWait);
    o << ",\"worst_ndi_wait_ep\":";
    writeFragmentSpan(o, s.engine.worstNdiWaitEp);
    o << ",\"worst_ndi_ready_to_end\":";
    writeFragmentSpan(o, s.engine.worstNdiReadyToEnd);
    o << '}';
}

} // namespace

std::string report(const State &s) {
    std::ostringstream o;
    o << std::setprecision(12);
    o << "{\"kind\":\"v2_sender\",\"protocol_version\":2,\"report_schema_version\":2,"
         "\"architecture\":\"B: independent preparation workers and one UDP sender\",\"run_id\":"
      << quote(s.config.runId) << ",\"finished\":" << (s.finished ? "true" : "false")
      << ",\"error\":" << quote(s.error) << ",\"ended_reason\":" << quote(s.endedReason)
      << ",\"elapsed_us\":" << s.elapsedUs << ",\"started_host_us\":\"" << s.startedUs
      << "\",\"ended_host_us\":\"" << s.endedUs << "\",\"transport\":" << quote(s.transport)
      << ",\"send_api\":" << (s.sendApi.empty() ? "null" : quote(s.sendApi))
      << ",\"local_port\":" << s.localPort << ",\"df_enabled\":" << (s.df ? "true" : "false")
      << ",\"boost_version\":" << (s.boostVersion.empty() ? "null" : quote(s.boostVersion))
      << ",\"product_version\":" << quote(SIM_PRODUCT_VERSION)
      << ",\"build_type\":" << quote(SIM_BUILD_TYPE)
      << ",\"source_revision_at_configure\":" << quote(SIM_SOURCE_REV)
      << ",\"tracked_dirty_at_configure\":" << quote(SIM_SOURCE_DIRTY)
      << ",\"socket_send_buffer_bytes\":" << s.sendBuffer
      << ",\"socket_receive_buffer_bytes\":" << s.receiveBuffer
      << ",\"udp_reset_setup_error\":" << s.resetError << ",\"first_send_endpoint\":"
      << quote(s.asynchronous ? "completion_handler_observed" : "local_synchronous_return")
      << ",\"sender_priority_raised\":" << (s.senderPriorityRaised ? "true" : "false")
      << ",\"sender_priority_error\":" << s.senderPriorityError
      << ",\"operation_storage\":{\"enabled\":" << (s.operationStorage.enabled ? "true" : "false")
      << ",\"hits\":" << s.operationStorage.hits << ",\"fallbacks\":" << s.operationStorage.fallbacks
      << '}'
      << ",\"ndi_handoff\":{\"guard_windows\":" << s.ndiGuardWindows
      << ",\"guard_window_dequeues\":" << s.ndiGuardHits
      << ",\"brief_wait_wall_us\":" << s.ndiGuardWaitWallUs
      << ",\"max_ready_to_dequeue_us\":" << s.ndiReadyToDequeueMaxUs << '}'
      << ",\"config\":{\"mailbox_fast_path\":" << (s.config.mailboxFastPath ? "true" : "false")
      << ",\"ndi_handoff_guard_us\":" << s.config.ndiHandoffGuardUs
      << ",\"asio_pooled_handler\":" << (s.config.asioPooledHandler ? "true" : "false")
      << ",\"network_profile\":" << quote(networkProfileName(s.config.linkMbps))
      << ",\"selected_link_mbps\":" << s.config.linkMbps
      << ",\"effective_link_mbps\":" << s.engine.effectiveMbps
      << ",\"target_ip\":" << quote(s.config.targetIp) << ",\"bind_ip\":" << quote(s.config.bindIp)
      << ",\"target_port\":" << s.config.targetPort << ",\"command_port\":" << s.config.commandPort
      << ",\"configured_observed_limit_mbps\":" << s.config.observedLinkMbps
      << ",\"path_limit_mbps\":" << s.config.pathLimitMbps << ",\"ndi_tools\":" << s.config.ndiTools
      << ",\"ep_bytes\":" << s.config.epBytes << ",\"ndi_hz\":" << s.config.ndiHz
      << ",\"ep_hz\":" << s.config.epHz
      << ",\"ndi_enabled\":" << (s.config.ndiEnabled ? "true" : "false")
      << ",\"ep_enabled\":" << (s.config.epEnabled ? "true" : "false")
      << ",\"duration_us\":" << s.config.durationUs << ",\"frame_limit\":" << s.config.frameLimit
      << ",\"status_interval_us\":" << s.config.statusIntervalUs
      << ",\"fault_injection\":" << (s.config.faults.any() ? "true" : "false")
      << ",\"allow_oversubscribed\":" << (s.config.allowOversubscribed ? "true" : "false") << '}'
      << ",\"link_observation\":{\"local_link_mbps\":" << s.link.mbps
      << ",\"loopback\":" << (s.link.loopback ? "true" : "false")
      << ",\"physical_ethernet\":" << (s.link.physical ? "true" : "false")
      << ",\"interface\":" << quote(s.link.interfaceName) << ",\"reason\":" << quote(s.link.reason)
      << ",\"observed_host_us\":\"" << s.link.observedAtUs << "\"}"
      << ",\"preflight\":{\"feasible\":" << (s.capacity.feasible ? "true" : "false")
      << ",\"formula\":\"IPv4 Ethernet: UDP bytes + 66, DATA frame bytes + 102 * fragments; no "
         "VLAN/tunnel\",\"utilization\":"
      << s.capacity.utilization << ",\"ndi_wire_us\":" << s.capacity.ndiWireUs
      << ",\"ep_wire_us\":" << s.capacity.epWireUs
      << ",\"simultaneous_wire_us\":" << s.capacity.synchronousWireUs
      << ",\"reason\":" << quote(s.capacity.reason) << '}' << ",\"handoff_trace\":";
    writeHandoffTrace(o, s);
    o << ",\"fragment_trace\":";
    writeFragmentTrace(o, s);
    o << ",\"clock\":{\"domain\":\"host_monotonic_simulation_or_playback\",\"hardware_verified\":"
         "false,\"timestamp_note\":\"Wire timestamp is explicitly simulated/playback time. Aurora "
         "device frame counter is retained inside BX; not an independently synchronized hardware "
         "clock.\"},\"modules\":{";
    for (std::size_t i = 0; i < 2; ++i) {
        if (i)
            o << ',';
        const auto &a = s.sources[i];
        const auto &m = s.engine.modules[i];
        o << '"' << (i + 1) << "\":{\"source_kind\":" << quote(a.kind)
          << ",\"sampled\":" << a.sampled << ",\"built\":" << a.built
          << ",\"skipped_slots\":" << a.skipped << ",\"queue_drops\":" << a.queueDrops
          << ",\"source_errors\":" << a.errors << ",\"source_error\":" << quote(a.error)
          << ",\"queue_high_water\":" << a.queueHighWater << ",\"payload_bytes\":" << a.payloadBytes
          << ",\"next_frame_id\":" << a.nextId
          << ",\"duplicate_source_frames\":" << a.duplicateSource
          << ",\"prepare_max_us\":" << a.prepareMaxUs
          << ",\"new_source_frames\":" << a.newSourceFrames
          << ",\"source_counter_gaps\":" << a.sourceCounterGaps
          << ",\"source_counter_resets\":" << a.sourceCounterResets
          << ",\"mixed_counter_frames\":" << a.mixedCounterFrames
          << ",\"semantic_changes\":" << a.semanticChanges
          << ",\"unmapped_source_frames\":" << a.unmappedSourceFrames
          << ",\"common_clock_mapped\":" << (a.commonClockMapped ? "true" : "false")
          << ",\"clock_error_bound_us\":" << a.clockErrorBoundUs
          << ",\"actual_device_hz\":" << a.actualDeviceHz << ",\"counter_hz\":" << a.counterHz
          << ",\"cancelled_attempts\":" << a.cancelledAttempts << ",\"sampling_measurement\":"
          << quote(a.kind == "device" ? "host query start timing, not hardware sampling"
                                      : "simulated/playback frame production timing")
          << ",\"sampling_span_us\":"
          << (a.lastSampleUs >= a.firstSampleUs ? a.lastSampleUs - a.firstSampleUs : 0)
          << ",\"sampling_intervals\":" << distribution(a.intervals)
          << ",\"sampling_deviation\":" << distribution(a.deviation)
          << ",\"sampling_lateness\":" << distribution(a.lateness) << ",\"producer_wait\":";
        writeProducerWait(o, a.wait);
        o << ",\"accepted\":" << m.accepted << ",\"initial_complete\":" << m.initialComplete
          << ",\"initial_incomplete\":" << m.initialIncomplete
          << ",\"deadline_misses\":" << m.deadlineMisses << ",\"send_errors\":" << m.sendErrors
          << ",\"datagrams\":" << m.datagrams << ",\"bytes\":" << m.bytes
          << ",\"retransmissions\":" << m.retransmissions
          << ",\"unsent_at_stop\":" << m.unsentAtStop << ",\"unsent_evicted\":" << m.unsentEvicted
          << ",\"retry_cancelled\":" << m.retryCancelled << ",\"nacks\":" << m.nacks
          << ",\"invalid_nacks\":" << m.invalidNacks << ",\"cache_misses\":" << m.cacheMisses
          << ",\"requested_fragments\":" << m.requestedFragments
          << ",\"duplicate_pending\":" << m.duplicatePending
          << ",\"retry_queue_drops\":" << m.retryQueueDrops
          << ",\"cache_high_water\":" << m.cacheHighWater
          << ",\"retry_high_water\":" << m.retryHighWater
          << ",\"injected_drops\":" << m.injectedDrops
          << ",\"initial_duration\":" << distribution(m.initialDuration)
          << ",\"packet_gaps\":" << distribution(m.packetGaps)
          << ",\"packet_gap_deviation\":" << distribution(m.packetGapDeviation)
          << ",\"original_plan_lateness\":" << distribution(m.originalPlanLateness)
          << ",\"retry_wait\":" << distribution(m.retryWait)
          << ",\"ledger\":" << ledgerJson(a.ledger)
          << ",\"initial_submission_ledger\":" << ledgerJson(m.submitted);
        if (s.config.handoffTrace)
            o << ",\"ready_to_first\":" << distribution(m.readyToFirst);
        o << '}';
    }
    o << "},\"status\":{\"accepted\":" << s.engine.statusAccepted
      << ",\"sent\":" << s.engine.statusSent << ",\"rejected\":" << s.engine.statusDropped
      << ",\"send_errors\":" << s.engine.statusSendErrors
      << ",\"cancelled\":" << s.engine.statusCancelled
      << ",\"queue_high_water\":" << s.engine.statusQueueHighWater
      << ",\"maximum_submit_latency_us\":" << s.engine.statusMaximumLatencyUs
      << ",\"next_sequence\":" << s.engine.nextStatusSequence << '}'
      << ",\"status_ingress\":{\"accepted\":" << s.statusIngressAccepted
      << ",\"rejected\":" << s.statusIngressRejected
      << ",\"cancelled\":" << s.statusIngressCancelled << ",\"pending\":" << s.statusIngressPending
      << '}' << ",\"invalid_feedback\":" << s.engine.invalidFeedback
      << ",\"feedback_source_rejected\":" << s.engine.sourceRejected
      << ",\"rate_changes\":" << s.engine.rateChanges
      << ",\"global_packet_gaps\":" << distribution(s.engine.globalPacketGaps)
      << ",\"global_packet_gap_deviation\":" << distribution(s.engine.globalPacketGapDeviation)
      << ",\"feedback_poll_calls\":" << s.feedbackPollCalls
      << ",\"feedback_empty_polls\":" << s.feedbackEmptyPolls
      << ",\"feedback_read_errors\":" << s.feedbackReadErrors
      << ",\"last_feedback_read_error\":" << quote(s.lastFeedbackReadError)
      << ",\"worst_packet_events\":[";
    for (std::size_t i = 0; i < s.engine.worstEvents.size(); ++i) {
        if (i)
            o << ',';
        const auto &e = s.engine.worstEvents[i];
        o << "{\"module_id\":" << unsigned(e.moduleId) << ",\"frame_id\":" << e.frameId
          << ",\"fragment_index\":" << e.index << ",\"planned_sample_us\":\"" << e.plannedSampleUs
          << "\",\"actual_sample_us\":\"" << e.sampleUs << "\",\"ready_us\":\"" << e.readyUs
          << "\",\"original_due_us\":\"" << e.originalDueUs << "\",\"adjusted_due_us\":\""
          << e.adjustedDueUs << "\",\"send_start_us\":\"" << e.sendStartUs
          << "\",\"send_end_us\":\"" << e.sendEndUs << "\",\"deadline_us\":\"" << e.deadlineUs
          << "\",\"retry\":" << (e.retry ? "true" : "false")
          << ",\"success\":" << (e.success ? "true" : "false") << '}';
    }
    o << "],\"initial_deadline_events\":[";
    for (std::size_t i = 0; i < s.engine.initialDeadlineEvents.size(); ++i) {
        if (i)
            o << ',';
        const auto &e = s.engine.initialDeadlineEvents[i];
        o << "{\"module_id\":" << unsigned(e.moduleId) << ",\"frame_id\":" << e.frameId
          << ",\"planned_sample_us\":\"" << e.plannedSampleUs << "\",\"actual_sample_us\":\""
          << e.sampleUs << "\",\"ready_us\":\"" << e.readyUs << "\",\"first_attempt_us\":\""
          << e.firstAttemptUs << "\",\"last_attempt_end_us\":\"" << e.endUs
          << "\",\"deadline_us\":\"" << e.deadlineUs << "\",\"overrun_us\":" << e.overrunUs()
          << ",\"initial_complete\":" << (e.complete ? "true" : "false");
        if (s.config.handoffTrace) {
            o << ",\"first_dispatch\":";
            if (e.firstDispatch)
                writeFirstDispatch(o, *e.firstDispatch);
            else
                o << "null";
        }
        o << '}';
    }
    o << "],\"acceptance\":[";
    bool first = true;
    auto row = [&](const std::string &id, unsigned module, const std::string &req,
                   const std::string &measurement, const std::string &threshold,
                   const std::string &measured, const std::string &status,
                   const std::string &reason) {
        if (!first)
            o << ',';
        first = false;
        o << "{\"id\":" << quote(id) << ",\"module_id\":" << module
          << ",\"requirement\":" << quote(req) << ",\"measurement\":" << quote(measurement)
          << ",\"threshold\":" << quote(threshold) << ",\"measured\":" << quote(measured)
          << ",\"status\":" << quote(status) << ",\"reason\":" << quote(reason) << '}';
    };
    for (unsigned i = 0; i < 2; ++i) {
        const auto &a = s.sources[i];
        const auto &m = s.engine.modules[i];
        const bool enabled = i == 0 ? s.config.ndiEnabled : s.config.epEnabled;
        const auto noData = !enabled || !a.sampled;
        const std::string name = i == 0 ? "NDI" : "EP";
        row("sampling_integrity", i + 1, name + " source continuity",
            "Original independent sample/query slots; no interpolation",
            "0 skipped slots / queue loss / source error / unmapped device frame",
            std::to_string(a.skipped) + " skipped, " + std::to_string(a.queueDrops) +
                " queue drops, " + std::to_string(a.errors) + " errors, " +
                std::to_string(a.unmappedSourceFrames) + " unmapped",
            a.skipped || a.queueDrops || a.errors || a.unmappedSourceFrames
                ? "fail"
                : (noData ? "not_tested" : "pass"),
            "A source failure remains failed even before its first sample. Hardware sampling is "
            "separate.");
        row("sampling_jitter", i + 1, name + " sampling deviation (retained engineering target)",
            "Absolute actual production/query interval minus configured nominal period; all "
            "intervals",
            "strictly < 1000 us",
            std::to_string(a.deviation.maxUs) + " us; " + std::to_string(a.deviation.overLimit) +
                " / " + std::to_string(a.deviation.count) + " violations",
            !a.deviation.count || a.kind == "device"
                ? "not_tested"
                : (a.deviation.maxUs >= 1000 ? "fail" : "pass"),
            "Device host query intervals cannot establish hardware sample jitter; the <1 ms "
            "engineering threshold is unchanged");
        row("initial_submission", i + 1, name + " complete initial transmission",
            s.asynchronous ? "Sample to observation of all successful local completion handlers "
                             "(not NIC egress)"
                           : "Simulation/playback sample to last local send return; device input "
                             "retains host-request proxy separately",
            i == 0 ? "<= 15000 us" : "<= 18000 us",
            std::to_string(m.initialDuration.maxUs) + " us; " + std::to_string(m.deadlineMisses) +
                " late; " + std::to_string(m.initialIncomplete) + " incomplete; " +
                std::to_string(m.initialComplete) + "/" + std::to_string(a.built) +
                " complete/built",
            !a.built || a.kind == "device"
                ? "not_tested"
                : (m.deadlineMisses || m.initialIncomplete
                       ? "fail"
                       : (!s.finished ? "running"
                                      : (m.initialComplete == a.built ? "pass" : "fail"))),
            a.kind == "device"
                ? "Device common timestamp does not independently establish source-to-host clock "
                  "offset or physical egress"
                : "The maximum covers completed frames. During a run, in-flight frames and "
                  "independently published counters are pending evaluation. After draining, all "
                  "built frames must have completed successfully by their deadlines.");
        row("packet_gap_jitter", i + 1, name + " packet interval deviation (engineering target)",
            "Actual first-copy interval minus original global scheduling plan interval",
            "strictly < 1000 us",
            std::to_string(m.packetGapDeviation.maxUs) + " us; " +
                std::to_string(m.packetGapDeviation.overLimit) + " / " +
                std::to_string(m.packetGapDeviation.count) + " violations",
            !m.packetGapDeviation.count ? "not_tested"
                                        : (m.packetGapDeviation.maxUs >= 1000 ? "fail" : "pass"),
            "Full-population counters; original due time is never replaced by catch-up due time");
        row("hardware_sampling", i + 1, name + " new hardware sampling and common timestamp",
            "Device frame counter, calibrated clock mapping and measured source rate",
            i == 0 ? "V2 ~60 Hz and verified common microsecond clock"
                   : "V2 50 Hz and verified common microsecond clock",
            "No physical device timing evidence", "not_tested",
            "Simulation/playback cannot verify real Aurora 40/66 Hz mode or EP sampling clock");
        row("receiver_delivery", i + 1, name + " complete receive and consume",
            "Compare paired receiver report and content/frame ledgers",
            "All built frames accounted for", "Requires paired receiver report", "not_tested",
            "UDP send success alone is not delivery evidence");
        row("nack_timing", i + 1, name + " receiver retry sequence",
            "Receiver gap/tail detection and per-module request timestamps",
            "2 ms tail; immediate first; 10 ms spacing; 3 rounds",
            std::to_string(m.nacks) + " NACK packets received", "not_tested",
            "Evaluate exact receiver timing and deterministic tests separately; sender cannot "
            "infer receiver's detection clock");
    }
    const auto &global = s.engine.globalPacketGapDeviation;
    row("global_packet_gap_deviation", 0,
        "All-stream packet interval deviation (engineering target)",
        "Local send-start interval versus scheduled interval across DATA, retries and status",
        "strictly < 1000 us",
        std::to_string(global.maxUs) + " us; " + std::to_string(global.overLimit) + " / " +
            std::to_string(global.count) + " violations",
        !global.count      ? "not_tested"
        : global.overLimit ? "fail"
                           : "pass",
        "Includes retry scheduling anchors; this deviation is distinct from a measured wire gap");
    row("capacity", 0, "Declared load capacity",
        "Ethernet serialization feasibility screening; not performance measurement",
        "Demand within effective rate and 15/18 ms windows",
        std::to_string(s.capacity.utilization * 100) + " percent",
        s.capacity.feasible ? "pass" : "fail", s.capacity.reason);
    row("status_immediate", 3, "Immediate complete status snapshot",
        "Status confirmation to local socket submission", "Numerical immediate bound unspecified",
        std::to_string(s.engine.statusMaximumLatencyUs) + " us; " +
            std::to_string(s.engine.statusSent) + " sent",
        "not_tested", "No invented timing threshold; status uses one packet and no NACK");
    row("physical_egress", 0, "Actual network egress deadlines",
        "Hardware NIC or external capture timestamps", "NDI <=15 ms; EP <=18 ms", "Unavailable",
        "not_tested", s.link.reason);
    row("display", 0, "Independent display and timestamp association",
        "Receive-consume-render trace with common hardware time",
        "No wait beyond a module period; real waveform contract required",
        "Sender report has no render timestamps", "not_tested",
        "Use receiver GUI consumption evidence; no real EP layout or verified clock was supplied");
    o << "]}";
    return o.str();
}

} // namespace rt::v2::runner_detail
