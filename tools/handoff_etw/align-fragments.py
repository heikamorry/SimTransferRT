"""Align retained V2 fragment calls and frame stages with same-run ETW data."""

import argparse
from bisect import bisect_right
import importlib.util
import json
from pathlib import Path


SPEC = importlib.util.spec_from_file_location(
    "align_scheduler", Path(__file__).with_name("align-scheduler.py")
)
ALIGN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ALIGN)
STATES = ("running", "ready", "waiting", "unknown")
READY_REASONS = (
    "switch_out_wr_preempted_32",
    "switch_out_wr_quantum_end_30",
    "switch_out_wr_dispatch_int_31",
    "switch_out_other_reason",
    "ready_thread_without_switch_out_reason",
    "unattributed",
)


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def sum_buckets(target, source):
    for key, value in source.items():
        target[key] += value


def ready_reason(segment):
    basis = segment.get("basis", "")
    if basis.startswith("ReadyThread"):
        return "ready_thread_without_switch_out_reason"
    if basis.startswith("CSwitch out, old_state=1, old_wait_reason="):
        code = basis.rsplit("=", 1)[-1]
        return {
            "32": "switch_out_wr_preempted_32",
            "30": "switch_out_wr_quantum_end_30",
            "31": "switch_out_wr_dispatch_int_31",
        }.get(code, "switch_out_other_reason")
    return "unattributed"


class Timeline:
    def __init__(self, aligned):
        self.segments = aligned["segments"]
        self.ends = [item["end_host_us"] for item in self.segments]
        self.ready_reasons = [ready_reason(item) for item in self.segments]
        coverage = aligned["trace_coverage"]
        self.start = coverage["start_host_us"]
        self.end = coverage["end_host_us"]
        self.warnings = aligned["warnings"]
        self.losses = {
            "events": coverage["header_lost_events"],
            "buffers": coverage["header_lost_buffers"],
        }
        self.pid_verified = bool(coverage["trace_thread_metadata_pids"])

    def measure(self, begin, end):
        if end < begin:
            raise ValueError("Window end precedes its start")
        by_state = dict.fromkeys(STATES, 0)
        by_ready_reason = dict.fromkeys(READY_REASONS, 0)
        width = end - begin
        known_coverage = max(0, min(end, self.end) - max(begin, self.start))
        i = bisect_right(self.ends, begin)
        while i < len(self.segments) and self.segments[i]["begin_host_us"] < end:
            seg = self.segments[i]
            span = min(end, seg["end_host_us"]) - max(begin, seg["begin_host_us"])
            by_state[seg["state"]] += max(0, span)
            if seg["state"] == "ready":
                by_ready_reason[self.ready_reasons[i]] += max(0, span)
            i += 1
        by_state["unknown"] += width - sum(by_state.values())
        return {
            "elapsed_us": width,
            "etw_us": by_state,
            "ready_by_recorded_basis_us": by_ready_reason,
            "outside_capture_us": width - known_coverage,
        }

    def conflicts_with_thread_timestamp(self, when):
        # The application reached this clock read on the target thread. Leave
        # a 2 us margin on either side for integer conversion and boundaries.
        around = self.measure(when - 2, when + 2)["etw_us"]
        return around["ready"] + around["waiting"] == 4


def frame_key(frame):
    return int(frame["module_id"]), int(frame["frame_id"]), int(frame["sample_us"])


def retained_frames(trace):
    frames = {}
    choices = {
        f"worst_by_module.{module}": frame
        for module, frame in trace["worst_by_module"].items()
    }
    for name in ("worst_ndi_wait", "worst_ndi_wait_ep", "worst_ndi_ready_to_end"):
        choices[name] = trace.get(name)
    for label, frame in choices.items():
        if frame is None:
            continue
        key = frame_key(frame)
        existing = frames.setdefault(key, {"frame": frame, "roles": []})
        if existing["frame"] != frame:
            raise ValueError(f"Conflicting retained fragment records for frame {key[:2]}")
        existing["roles"].append(label)
    return list(frames.values())


def frame_times(frame):
    times = tuple(int(frame[key]) for key in ("sample_us", "ready_us", "first_attempt_us", "end_us"))
    if tuple(sorted(times)) != times:
        raise ValueError(f"Invalid application timestamps for frame {frame_key(frame)[:2]}")
    return times


def result_window(timeline, start, end, base):
    return {
        "begin_since_sample_us": start - base,
        "end_since_sample_us": end - base,
        **timeline.measure(start, end),
    }


def relative_us(value, base):
    return None if value is None else int(value) - base


def detail_window(timeline, label, start, end, base):
    return {"kind": label, **result_window(timeline, start, end, base)}


def fragment_windows(frame, sender, producer, details):
    sample, ready, first, end = frame_times(frame)
    if "span_us" in frame and int(frame["span_us"]) != end - first:
        raise ValueError("Initial span disagrees with application timestamps")
    calls = []
    sender_clock_reads = {first, end}
    not_called = 0
    previous_attempt_end = first
    for packet in frame["fragments"]:
        attempt, attempt_end = (int(packet[key]) for key in ("attempt_us", "end_us"))
        if not previous_attempt_end <= attempt <= attempt_end <= end:
            raise ValueError("Fragment attempts are not in valid dispatch order")
        previous_attempt_end = attempt_end
        sender_clock_reads.update((attempt, attempt_end))
        start, stop = packet["call_start_us"], packet["call_end_us"]
        if not packet["called"]:
            if start is not None or stop is not None:
                raise ValueError("A fragment without a call has a socket call timestamp")
            not_called += 1
            continue
        start, stop = int(start), int(stop)
        if not attempt <= start <= stop <= attempt_end:
            raise ValueError("Socket call timestamps lie outside the fragment's attempt")
        calls.append((start, stop, packet))
        sender_clock_reads.update((start, stop))
    if "call_count" in frame and int(frame["call_count"]) != len(calls):
        raise ValueError("Call count disagrees with recorded fragments")
    if "fragment_count" in frame and int(frame["fragment_count"]) != len(frame["fragments"]):
        raise ValueError("Original attempt count disagrees with recorded fragments")

    all_calls = dict.fromkeys(STATES, 0)
    all_gaps = dict.fromkeys(STATES, 0)
    calls_ready = dict.fromkeys(READY_REASONS, 0)
    gaps_ready = dict.fromkeys(READY_REASONS, 0)
    after_gap_due = dict.fromkeys(STATES, 0)
    call_details, gap_details = [], []
    for i, (start, stop, packet) in enumerate(calls):
        call = detail_window(sender, "socket_call", start, stop, sample)
        call["fragment_index"] = packet["fragment_index"]
        call["success"] = packet["success"]
        sum_buckets(all_calls, call["etw_us"])
        sum_buckets(calls_ready, call["ready_by_recorded_basis_us"])
        call_details.append(call)
        if i == 0:
            continue
        gap_start = calls[i - 1][1]
        gap = detail_window(sender, "between_own_calls", gap_start, start, sample)
        gap["previous_fragment_index"] = calls[i - 1][2]["fragment_index"]
        gap["next_fragment_index"] = packet["fragment_index"]
        gap["next_original_due_since_sample_us"] = relative_us(packet["original_due_us"], sample)
        gap["next_pacing_due_since_sample_us"] = relative_us(packet["pacing_due_us"], sample)
        pacing_due = packet["pacing_due_us"]
        if pacing_due is not None:
            split = max(gap_start, min(start, int(pacing_due)))
            gap["at_or_after_next_pacing_due"] = sender.measure(split, start)
            sum_buckets(after_gap_due, gap["at_or_after_next_pacing_due"]["etw_us"])
        sum_buckets(all_gaps, gap["etw_us"])
        sum_buckets(gaps_ready, gap["ready_by_recorded_basis_us"])
        gap_details.append(gap)

    if calls:
        initial_prefix = sender.measure(first, calls[0][0])
        initial_suffix = sender.measure(calls[-1][1], end)
    else:
        initial_prefix = sender.measure(first, end)
        initial_suffix = sender.measure(end, end)
    initial = sender.measure(first, end)
    accounted = dict.fromkeys(STATES, 0)
    for span in (initial_prefix["etw_us"], all_calls, all_gaps, initial_suffix["etw_us"]):
        sum_buckets(accounted, span)
    if accounted != initial["etw_us"]:
        raise ValueError("Call windows and inter-call gaps do not partition initial dispatch")
    for key, measured in (
        ("before_first_call_us", initial_prefix["elapsed_us"]),
        ("after_last_call_us", initial_suffix["elapsed_us"]),
        ("own_call_us", sum(all_calls.values())),
        ("between_calls_us", sum(all_gaps.values())),
    ):
        if key in frame and int(frame[key]) != measured:
            raise ValueError(f"Application aggregate {key} disagrees with recorded fragments")

    def largest(rows):
        if details == 0:
            return rows
        return sorted(rows, key=lambda row: row["elapsed_us"], reverse=True)[:details]

    sender_conflicts = [when - sample for when in sorted(sender_clock_reads)
                        if sender.conflicts_with_thread_timestamp(when)]
    out = {
        "module_id": frame["module_id"],
        "frame_id": frame["frame_id"],
        "initial_complete": frame["initial_complete"],
        "sample_to_ready_us": ready - sample,
        "sender_while_sample_to_end": sender.measure(sample, end),
        "sender_while_ready_to_end": sender.measure(ready, end),
        "sender_while_ready_to_first": sender.measure(ready, first),
        "sender_while_first_to_end": initial,
        "sender_before_first_actual_call": initial_prefix,
        "sender_after_last_actual_call": initial_suffix,
        "sender_in_own_socket_calls_us": all_calls,
        "sender_in_inter_call_gaps_us": all_gaps,
        "sender_ready_in_own_socket_calls_by_basis_us": calls_ready,
        "sender_ready_in_inter_call_gaps_by_basis_us": gaps_ready,
        "sender_at_or_after_next_pacing_due_in_gaps_us": after_gap_due,
        "socket_calls": len(calls),
        "attempts_without_socket_call": not_called,
        "intervening_local_attempts": frame.get("intervening"),
        "sender_etw_conflict_count": len(sender_conflicts),
        "sender_etw_conflict_examples_since_sample_us": sender_conflicts[:12],
        "longest_socket_calls": largest(call_details),
        "longest_inter_call_gaps": largest(gap_details),
    }
    if producer is not None:
        out["producer_while_sample_to_ready"] = producer.measure(sample, ready)
        out["producer_etw_conflict_count"] = sum(producer.conflicts_with_thread_timestamp(when)
                                                 for when in {sample, ready})
    out["sender_while_first_to_last_actual_call"] = (
        sender.measure(calls[0][0], calls[-1][1]) if calls else None
    )
    return out


def cross_stream_overlaps(selected, sender):
    overlaps = []
    ndi_frames = [row["frame"] for row in selected if row["frame"]["module_id"] == 1]
    ep_frames = [row["frame"] for row in selected if row["frame"]["module_id"] == 2]
    for ndi in ndi_frames:
        _, ready, ndi_first, _ = frame_times(ndi)
        for ep in ep_frames:
            _, _, ep_first, ep_end = frame_times(ep)
            start, stop = max(ready, ep_first), min(ndi_first, ep_end)
            if start >= stop:
                continue
            by_state = dict.fromkeys(STATES, 0)
            intersected_calls = 0
            for packet in ep["fragments"]:
                if not packet["called"]:
                    continue
                call_begin = max(start, int(packet["call_start_us"]))
                call_end = min(stop, int(packet["call_end_us"]))
                if call_end > call_begin:
                    sum_buckets(by_state, sender.measure(call_begin, call_end)["etw_us"])
                    intersected_calls += 1
            overlaps.append({
                "ndi_frame_id": ndi["frame_id"],
                "ep_frame_id": ep["frame_id"],
                "overlap_since_ndi_ready_us": start - ready,
                "sender_while_ndi_wait_overlaps_ep_initial_span": sender.measure(start, stop),
                "ep_calls_intersecting_ndi_wait": intersected_calls,
                "sender_in_intersecting_ep_calls_us": by_state,
            })
    return overlaps


def controls(report, timeline, limit):
    # These rows contain handoff timings, while fragment call windows require
    # an independently retained fragment_trace frame record.
    trace = report.get("handoff_trace", {})
    candidates = trace.get("worst_first_dispatch", {}).get("1", [])
    hits = []
    rejected = []
    for event in sorted(candidates, key=lambda row: row["ready_to_first_us"], reverse=True):
        sample, ready, first = (int(event[key]) for key in ("sample_us", "ready_us", "first_attempt_us"))
        if not timeline.start <= sample <= ready < first <= timeline.end:
            continue
        sender_clock_reads = {first}
        for key in ("dequeued_us", "submitted_us", "selected_us"):
            if event.get(key) is not None:
                sender_clock_reads.add(int(event[key]))
        if any(timeline.conflicts_with_thread_timestamp(when) for when in sender_clock_reads):
            rejected.append(event["frame_id"])
            continue
        if len(hits) >= limit:
            continue
        hits.append({
            "module_id": 1,
            "frame_id": event["frame_id"],
            "sample_to_ready_us": ready-sample,
            "sender_while_ready_to_first": timeline.measure(ready, first),
            "intervening_local_attempts": event["intervening"],
            "udp_call_windows_available": False,
        })
    return hits, rejected


def make_timeline(csv_path, metadata_path, start, end, tid, pid, frequency):
    aligned = ALIGN.analyze(
        ALIGN.read_csv(csv_path), read_json(metadata_path), start, end, tid, pid, frequency
    )
    return Timeline(aligned)


def main(args):
    report = read_json(args.report)
    trace = report.get("fragment_trace")
    if not trace or not trace.get("enabled", True):
        raise ValueError("The report lacks an enabled V2 fragment_trace snapshot")
    selected = retained_frames(trace)
    if args.frame:
        module, frame_id = (int(part) for part in args.frame.split(":"))
        selected = [row for row in selected if frame_key(row["frame"])[:2] == (module, frame_id)]
    if not selected:
        raise ValueError("No retained diagnostic frame matches the selection")
    times = [frame_times(row["frame"]) for row in selected]
    start, end = min(values[0] for values in times), max(values[-1] for values in times)
    sender_metadata = read_json(args.sender_meta)
    # A later control within the same retained ETL is useful even when the
    # worst NDI application trace came from an overwritten part of the run.
    capture_start = int(sender_metadata["coverage_start_host_us"])
    capture_end = int(sender_metadata["coverage_end_host_us"])
    sender = make_timeline(
        args.sender_csv, args.sender_meta, min(start, capture_start), max(end, capture_end),
        int(trace["sender_tid"]), int(trace["process_id"]), int(trace["qpc_frequency_hz"])
    )
    producers = {}
    for module, paths in ((1, args.ndi), (2, args.ep)):
        if paths:
            producers[module] = make_timeline(
                *paths, start, end, int(trace["producer_tids"][str(module)]),
                int(trace["process_id"]), int(trace["qpc_frequency_hz"])
            )
    frames = []
    for row in selected:
        frame = row["frame"]
        windows = fragment_windows(frame, sender, producers.get(frame["module_id"]), args.top)
        windows["retention_roles"] = row["roles"]
        frames.append(windows)
    observed_ndi = any(
        "worst_ndi_wait" in frame["retention_roles"]
        and frame["sender_while_sample_to_end"]["outside_capture_us"] == 0
        and frame["sender_etw_conflict_count"] == 0
        for frame in frames
    )
    ndi_controls, rejected_controls = ([], []) if observed_ndi else controls(report, sender, 3)
    return {
        "run_id": report["run_id"],
        "qpc_frequency_hz": int(trace["qpc_frequency_hz"]),
        "sender_capture_duration_us": sender.end - sender.start,
        "sender_process_identity_verified": sender.pid_verified,
        "sender_trace_losses": sender.losses,
        "sender_trace_warnings": sender.warnings,
        "producer_trace_warnings": {str(module): item.warnings for module, item in producers.items()},
        "frames": frames,
        "retained_ndi_wait_and_ep_call_overlaps": cross_stream_overlaps(selected, sender),
        "covered_ndi_handoff_controls": ndi_controls,
        "ndi_controls_excluded_by_scheduler_conflicts": rejected_controls,
        "measurement_notes": [
            "Running means the thread is scheduled; DPC/ISR can run in these intervals.",
            "Ready means runnable without being scheduled. It does not by itself prove involuntary preemption.",
            "Ready reasons use explicit switch-out labels where retained, leaving standalone ReadyThread intervals without a switch-out reason.",
            "Between-own-call gaps include pacing, engine work, and any other stream's work or socket calls.",
            "Any missing ETL coverage is included in unknown. A loss warning limits attribution.",
            "Positive conflict counts show an application clock read inside an apparent off-CPU interval. Do not use those partitions for attribution. A circular trace can be locally incomplete even with no header losses.",
            "ETL and application timestamps use integer QPC-derived microseconds; sub-microsecond differences can round to zero.",
        ],
    }


def self_test():
    aligned = {
        "segments": [
            {"begin_host_us": 10, "end_host_us": 20, "state": "running"},
            {"begin_host_us": 20, "end_host_us": 26, "state": "ready",
             "basis": "CSwitch out, old_state=1, old_wait_reason=32"},
            {"begin_host_us": 26, "end_host_us": 30, "state": "ready", "basis": "ReadyThread"},
            {"begin_host_us": 30, "end_host_us": 32, "state": "waiting"},
            {"begin_host_us": 32, "end_host_us": 50, "state": "running"},
        ],
        "trace_coverage": {
            "start_host_us": 10, "end_host_us": 50, "header_lost_events": 0,
            "header_lost_buffers": 0, "trace_thread_metadata_pids": [1],
        },
        "warnings": [],
    }
    timeline = Timeline(aligned)

    def attempt(index, begin, end, called, due=None):
        return {
            "fragment_index": index, "attempt_us": str(begin), "end_us": str(end),
            "call_start_us": str(begin) if called else None,
            "call_end_us": str(end) if called else None,
            "original_due_us": due, "pacing_due_us": due,
            "called": called, "success": called,
        }

    frame = {
        "module_id": 2, "frame_id": 218, "sample_us": "7", "ready_us": "9",
        "first_attempt_us": "10", "end_us": "44", "span_us": 34, "initial_complete": False,
        "fragments": [
            attempt(0, 10, 11, False),
            attempt(1, 12, 21, True),
            attempt(2, 35, 43, True, "27"),
            attempt(3, 43, 44, False),
        ],
    }
    out = fragment_windows(frame, timeline, timeline, 0)
    assert out["sender_while_sample_to_end"]["outside_capture_us"] == 3
    assert out["sender_before_first_actual_call"]["elapsed_us"] == 2
    assert out["sender_after_last_actual_call"]["elapsed_us"] == 1
    assert out["sender_in_own_socket_calls_us"] == dict(running=16, ready=1, waiting=0, unknown=0)
    assert out["sender_in_inter_call_gaps_us"] == dict(running=3, ready=9, waiting=2, unknown=0)
    assert out["sender_ready_in_inter_call_gaps_by_basis_us"]["switch_out_wr_preempted_32"] == 5
    assert out["sender_ready_in_inter_call_gaps_by_basis_us"]["ready_thread_without_switch_out_reason"] == 4
    assert out["sender_at_or_after_next_pacing_due_in_gaps_us"] == dict(running=3, ready=3, waiting=2, unknown=0)
    assert out["attempts_without_socket_call"] == 2
    assert timeline.conflicts_with_thread_timestamp(24)
    ndi = {"module_id": 1, "frame_id": 7, "sample_us": "15", "ready_us": "19",
           "first_attempt_us": "40", "end_us": "42"}
    overlap = cross_stream_overlaps([{"frame": frame}, {"frame": ndi}], timeline)[0]
    assert overlap["sender_while_ndi_wait_overlaps_ep_initial_span"]["elapsed_us"] == 21
    assert overlap["ep_calls_intersecting_ndi_wait"] == 2
    assert overlap["sender_in_intersecting_ep_calls_us"] == dict(running=6, ready=1, waiting=0, unknown=0)
    print("Synthetic fragment calls, gaps, pacing, cross-stream overlap and coverage: PASS")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--sender-csv", type=Path)
    parser.add_argument("--sender-meta", type=Path)
    parser.add_argument("--ndi", type=Path, nargs=2, metavar=("CSV", "META"))
    parser.add_argument("--ep", type=Path, nargs=2, metavar=("CSV", "META"))
    parser.add_argument("--frame", help="Select a retained frame by stream ID and frame ID, e.g. 2:218")
    parser.add_argument("--top", type=int, default=12, help="Detail rows per group. 0 keeps all rows.")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    else:
        if not args.report or not args.sender_csv or not args.sender_meta or args.top < 0:
            parser.error("--report, --sender-csv and --sender-meta are required, and --top cannot be negative")
        print(json.dumps(main(args), indent=2))
