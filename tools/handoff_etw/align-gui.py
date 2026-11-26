"""Align each module's worst widget frame with its GUI thread's same-run scheduler events."""

import argparse
import json
import runpy
from pathlib import Path


# Reuse the same ETW decoder, raw clock validation and interval slicing as fragment analysis.
FRAGMENTS = runpy.run_path(str(Path(__file__).with_name("align-fragments.py")))
STAMPS = ("completed_us", "enqueued_us", "batch_taken_us", "apply_started_us", "widget_updated_us")
STAGES = ("complete_to_enqueue", "enqueue_to_batch", "batch_to_apply", "apply_to_update")


def load_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def gui_frames(report):
    frames = []
    for module in report["receiver"]["ui_observations"]["modules"]:
        frame = module.get("worst_frame")
        if frame is None:
            continue
        times = tuple(int(frame[key]) for key in STAMPS)
        if times[0] <= 0 or times != tuple(sorted(times)):
            raise ValueError("Widget frame timestamps are missing or out of order")
        for stage, begin, end in zip(STAGES, times, times[1:]):
            if int(frame[stage + "_us"]) != end - begin:
                raise ValueError(f"{stage} differs from its application timestamps")
        total = times[-1] - times[0]
        if (total != int(frame["complete_to_update_us"])
                or total != int(module["max_complete_to_widget_update_us"])):
            raise ValueError("Worst frame does not match the corresponding widget metric")
        frames.append((int(module["module_id"]), frame, times))
    if not frames:
        raise ValueError("No module has a retained worst_frame in this GUI report")
    return frames


def longest_queue_segments(timeline, begin, end):
    observed = []
    for segment in timeline.segments:
        first = max(begin, segment["begin_host_us"])
        last = min(end, segment["end_host_us"])
        if last <= first:
            continue
        if (observed and observed[-1]["end_host_us"] == first
                and observed[-1]["state"] == segment["state"]):
            interval = observed[-1]
            interval["end_host_us"] = last
            interval["duration_us"] = last - interval["begin_host_us"]
            if segment["basis"] not in interval["basis"]:
                interval["basis"].append(segment["basis"])
        else:
            observed.append({"begin_host_us": first, "end_host_us": last, "duration_us": last - first,
                             "state": segment["state"], "basis": [segment["basis"]]})
    return sorted(observed, key=lambda interval: interval["duration_us"], reverse=True)[:5]


def callback_partition(trace, window, frame_id, begin, end, timeline):
    if (window is None or window["frame_id"] != frame_id or window["start_us"] != begin
            or window["end_us"] != end or not trace["started_us"] <= begin <= end <= trace["ended_us"]):
        raise ValueError("Qt callback trace does not match this module's retained queue window")
    parts = []
    for key in ("events", "gaps"):
        for entry in window[key]:
            first, last = entry["start_us"], entry["end_us"]
            if key == "events":
                if not trace["started_us"] <= first < last <= trace["ended_us"]:
                    raise ValueError("Qt callback outside the application's event tracing interval")
                first, last = max(begin, first), min(end, last)
                if entry["overlap_us"] != last - first:
                    raise ValueError("Qt callback overlap differs from the retained queue window")
            if not begin <= first < last <= end:
                raise ValueError("Invalid callback or gap interval")
            parts.append((first, last, key, entry))
    totals = {key: dict.fromkeys(FRAGMENTS["STATES"], 0) for key in ("events", "gaps")}
    cursor = begin
    details = []
    for first, last, key, entry in sorted(parts, key=lambda item: item[0]):
        if cursor != first:
            raise ValueError("GUI callback intervals and gaps do not partition the queue window")
        cursor = last
        observed = timeline.measure(first, last)
        FRAGMENTS["sum_buckets"](totals[key], observed["etw_us"])
        details.append({"kind": "callback" if key == "events" else "gap", **entry,
                        "overlap_begin_us": first, "overlap_end_us": last,
                        "etw_us": observed["etw_us"], "outside_capture_us": observed["outside_capture_us"]})
    if (cursor != end or window["covered_us"] != sum(totals["events"].values())
            or window["uncovered_us"] != sum(totals["gaps"].values())):
        raise ValueError("Qt callback totals do not match the retained queue window")
    return {"callback_etw_us": totals["events"], "gap_etw_us": totals["gaps"], "intervals": details}


def wake_relationship(wake, enqueued, batch):
    started, returned, entered = (wake[key] for key in ("post_started_us", "post_returned_us",
                                                         "event_entered_us"))
    if ((returned is not None and (started is None or returned < started))
            or (entered is not None and (started is None or entered < started))):
        raise ValueError("Recorded wake return or entry precedes its post")
    if wake["drained_by_this_event"] and (entered is None or entered > batch):
        raise ValueError("The draining event was not entered by the batch timestamp")
    def difference(left, right):
        return left - right if left is not None and right is not None else None
    # Signed differences are intentional. A frame can inherit an older wake, a timer may
    # drain before post starts, and GUI delivery need not wait for the posting thread's return.
    return {**wake, "enqueue_to_post_start_us": difference(started, enqueued),
            "post_call_elapsed_us": difference(returned, started),
            "post_start_to_event_entry_us": difference(entered, started),
            "post_return_to_batch_us": difference(batch, returned),
            "event_entry_to_batch_us": difference(batch, entered)}


def align(report, pid, tid, events, meta):
    trace = report["sender"]["fragment_trace"]
    if trace.get("enabled") is not True:
        raise ValueError("A same-run joint.json with --fragment-trace enabled is required")
    if pid <= 0 or tid <= 0 or pid != int(trace["process_id"]):
        raise ValueError("Window PID/TID is invalid or its PID differs from the application's PID")
    worker_tids = [trace["sender_tid"], *trace.get("producer_tids", {}).values()]
    if tid in (int(worker) for worker in worker_tids if worker is not None):
        raise ValueError("Window TID matches a sender or producer worker TID")
    frames = gui_frames(report)
    # Include margins around the actual GUI clock reads for switch-boundary rounding.
    start = min(times[0] for _, _, times in frames) - 2
    end = max(times[-1] for _, _, times in frames) + 2
    frequency = int(trace["qpc_frequency_hz"])
    aligned = FRAGMENTS["ALIGN"].analyze(events, meta, start, end, tid, pid, frequency)
    timeline = FRAGMENTS["Timeline"](aligned)
    losses = {key: int(meta[key]) for key in ("events_lost", "buffers_lost", "short_records")}
    output = []
    callback_trace = report.get("gui_event_trace")
    if callback_trace is not None:
        windows = {row["module_id"]: row["queue_window"] for row in callback_trace["modules"]}
        if len(windows) != len(callback_trace["modules"]):
            raise ValueError("Duplicate module in the GUI callback trace")
    for module_id, frame, times in frames:
        stages = {}
        for stage, begin, end in zip(STAGES, times, times[1:]):
            stats = timeline.measure(begin, end)
            stages[stage] = {"begin_host_us": begin, "end_host_us": end, **stats}
        # Completed and enqueued are worker clock reads. Only the last three must
        # be consistent with execution of the target GUI thread.
        conflicts = [key for key, stamp in zip(STAMPS[2:], times[2:])
                     if timeline.conflicts_with_thread_timestamp(stamp)]
        queue = stages["enqueue_to_batch"]
        queue["longest_observed_segments"] = longest_queue_segments(timeline, times[1], times[2])
        if callback_trace is not None:
            queue["qt_notify"] = callback_partition(
                callback_trace, windows.get(module_id), frame["frame_id"], times[1], times[2], timeline)
        problems = []
        if not timeline.pid_verified:
            problems.append("No ETW thread metadata verifies the window PID")
        if any(losses.values()):
            problems.append("Lost, short or incomplete ETW records limit attribution")
        if queue["outside_capture_us"]:
            problems.append("Capture does not cover the full enqueue-to-batch interval")
        if queue["etw_us"]["unknown"]:
            problems.append("Part of the enqueue-to-batch interval has unknown thread state")
        if conflicts:
            problems.append("A GUI clock read conflicts with the apparent off-CPU interval")
        output.append({
            "module_id": module_id, "frame_id": frame["frame_id"], "drain": frame["drain"],
            "queue_depth_at_enqueue": frame["queue_depth_at_enqueue"],
            "complete_to_widget_update_us": times[-1] - times[0], "stages": stages,
            "gui_clock_read_conflicts": conflicts, "queue_attribution_usable": not problems,
            "limitations": problems,
        })
        if frame.get("wake") is not None:
            output[-1]["wake"] = wake_relationship(frame["wake"], times[1], times[2])
    return {
        "run_id": report["sender"]["run_id"], "pid": pid, "gui_tid": tid,
        "qpc_frequency_hz": frequency, "etw_pid_verified": timeline.pid_verified,
        "etw_record_losses": losses, "etw_warnings": timeline.warnings,
        "capture_start_host_us": timeline.start, "capture_end_host_us": timeline.end,
        "frames": output,
        "interpretation": [
            "GUI thread states are measured during the indicated wall-clock intervals. "
            "Frame completion and enqueue timestamps originate on the receiver thread.",
            "Running identifies scheduled time and may include interrupts. Scheduler events "
            "supply no application call stack or Qt callback identity.",
            "Ready means runnable without CPU time. Waiting indicates an observed blocked state. "
            "Neither state alone identifies a specific scheduling or notification cause.",
            "Frames taken in the same GUI batch can have overlapping queue intervals. "
            "Interpret each module separately.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, required=True, help="Traced GUI joint.json")
    parser.add_argument("--csv", type=Path, required=True, help="dump-scheduler records for the HWND TID")
    parser.add_argument("--meta", type=Path, required=True, help="Metadata from the same extraction")
    parser.add_argument("--identity", type=Path, help="Same-run gui-thread.json containing pid and tid")
    parser.add_argument("--pid", type=int, help="PID from GetWindowThreadProcessId")
    parser.add_argument("--tid", type=int, help="GUI TID from GetWindowThreadProcessId")
    parser.add_argument("--output", type=Path, help="Write JSON here (stdout by default)")
    args = parser.parse_args()
    if args.identity and (args.pid is not None or args.tid is not None):
        parser.error("Choose either --identity or the --pid/--tid pair")
    if not args.identity and (args.pid is None or args.tid is None):
        parser.error("--identity or both --pid and --tid are required")
    try:
        identity = load_json(args.identity) if args.identity else {"pid": args.pid, "tid": args.tid}
        result = align(load_json(args.report), int(identity["pid"]), int(identity["tid"]),
                       FRAGMENTS["ALIGN"].read_csv(args.csv), load_json(args.meta))
        data = json.dumps(result, indent=2)
        if args.output:
            args.output.write_text(data + "\n", encoding="utf-8")
        else:
            print(data)
    except (KeyError, TypeError, ValueError, OSError) as exc:
        parser.exit(2, f"Unable to align GUI trace: {exc}\n")


if __name__ == "__main__":
    main()
