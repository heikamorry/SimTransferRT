"""Partition one app QPC window using target-thread ETW records."""
import argparse
import csv
import json
from pathlib import Path

# Documented classic Thread_V2 layout and the newer CSwitch/thread metadata
# versions decoded in the captured Windows trace.
SUPPORTED_VERSIONS = {
    "switch_in": {2, 5},
    "switch_out": {2, 5},
    "ready_thread": {2},
    "thread_metadata": {2, 3},
}

def read_csv(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return sorted(csv.DictReader(stream), key=lambda e: int(e["qpc"]))

def analyze(events, meta, begin, end, expected_tid, expected_pid, app_frequency=None):
    if begin >= end:
        raise ValueError("App ready timestamp must precede the first-attempt timestamp")
    if meta["clock"] != "qpc" or int(meta["frequency"]) <= 0:
        raise ValueError("The file must be extracted using the original QPC clock")
    if {int(event["target_tid"]) for event in events} != {expected_tid}:
        raise ValueError("Wrong or missing target TID in extracted records")
    freq = int(meta["frequency"])
    if app_frequency is not None and app_frequency != freq:
        raise ValueError("Application and ETL QPC frequencies differ")
    if "selected_events" in meta and len(events) != int(meta["selected_events"]):
        raise ValueError("Extracted row count differs from the ETL summary")
    for event in events:
        kind = event["kind"]
        version = int(event["version"])
        if version not in SUPPORTED_VERSIONS.get(kind, set()):
            raise ValueError(f"Unsupported kernel scheduler record: {kind} v{version}")
        if int(event["qpc"]) * 1_000_000 // freq != int(event["host_us"]):
            raise ValueError("Raw and host_us timestamps do not agree")

    reported_pids = {int(e["thread_pid"]) for e in events if e["kind"] == "thread_metadata"}
    if len(reported_pids) > 1:
        raise ValueError("Multiple process lifetimes share this TID. Use a trace for one lifetime")
    if expected_pid and reported_pids and expected_pid not in reported_pids:
        raise ValueError(f"Target TID metadata identifies other PID(s): {sorted(reported_pids)}")
    coverage_start = int(meta["coverage_start_host_us"])
    coverage_end = int(meta["coverage_end_host_us"])
    lower, upper = max(begin, coverage_start), min(end, coverage_end)
    segments = []
    warnings = []
    if lower >= upper:
        warnings.append("Trace does not contain this host_us interval")
    if expected_pid and not reported_pids:
        warnings.append("No Thread_V2 PID metadata retained. TID is from the application's marker")
    if meta["events_lost"] or meta["buffers_lost"] or meta["short_records"]:
        warnings.append("Trace has lost or unsupported records. Missing switches can invalidate durations")
    state, cause = "unknown", "no prior target scheduler event"
    slice_start = lower

    def append(a, b, value, details):
        if b > a:
            segments.append(dict(begin_host_us=a, end_host_us=b, duration_us=b-a, state=value, basis=details))

    if lower < upper:
        for event in events:
            when, kind = int(event["host_us"]), event["kind"]
            if when < coverage_start or kind == "thread_metadata":
                continue
            if when > upper:
                break
            if kind == "switch_in":
                next_state = "running"
                basis = "CSwitch in on CPU " + event["cpu"]
            elif kind == "switch_out":
                old = int(event["old_state"])
                next_state = {1:"ready",5:"waiting"}.get(old,"unknown")
                basis = "CSwitch out, old_state=" + event["old_state"] +                         ", old_wait_reason=" + event["old_wait_reason"]
            elif kind == "ready_thread":
                flags = int(event["ready_flags"])
                if state == "running" or flags & 0x6:
                    continue
                next_state = "ready"
                basis = "ReadyThread" + (" from DPC" if flags & 1 else "")
            else:
                continue
            if when >= lower:
                append(slice_start, when, state, cause)
                slice_start = when
            state, cause = next_state, basis
        append(slice_start, upper, state, cause)

    totals = {name:sum(s["duration_us"] for s in segments if s["state"] == name)
              for name in ("running","ready","waiting","unknown")}
    uncovered = end - begin - sum(totals.values())
    totals["unknown"] += uncovered
    return dict(app_window=dict(begin_host_us=begin, end_host_us=end, duration_us=end-begin,
                                target_tid=expected_tid, expected_pid=expected_pid),
                trace_coverage=dict(start_host_us=coverage_start,end_host_us=coverage_end,
                                    contains_full_window=uncovered==0,uncovered_us=uncovered,
                                    header_lost_events=meta["events_lost"],header_lost_buffers=meta["buffers_lost"],
                                    trace_thread_metadata_pids=sorted(reported_pids)),
                elapsed_us_by_scheduler_state=totals, segments=segments, warnings=warnings)

def self_test():
    def e(ts, kind, old=-1, flags=-1):
        version = 5 if kind.startswith("switch_") else 2
        return dict(qpc=str(ts*10),host_us=str(ts),kind=kind,target_tid="123",thread_pid="0",
                    cpu="3",old_state=str(old),old_wait_reason="4",ready_flags=str(flags),version=str(version))
    cfg=dict(clock="qpc",frequency=10_000_000,coverage_start_host_us=90,coverage_end_host_us=150,
             events_lost=0,buffers_lost=0,short_records=0)
    events=[e(91,"switch_in"),e(101,"switch_out",5),e(111,"ready_thread",flags=1),
            e(121,"switch_in"),e(129,"switch_out",1),e(134,"switch_in")]
    result=analyze(events,cfg,100,136,123,0)
    assert result["elapsed_us_by_scheduler_state"]==dict(running=11,ready=15,waiting=10,unknown=0),result
    cfg["coverage_end_host_us"]=134
    result=analyze(events,cfg,100,136,123,0)
    assert result["trace_coverage"]["uncovered_us"]==2
    assert result["elapsed_us_by_scheduler_state"]["unknown"]==2
    events[0]["version"] = "6"
    try:
        analyze(events,cfg,100,136,123,0)
    except ValueError as exc:
        assert "Unsupported kernel scheduler record" in str(exc)
    else:
        raise AssertionError("Unsupported event layout was accepted")
    print("Synthetic scheduler states, clock alignment, coverage and version checks: PASS")

if __name__ == "__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--self-test",action="store_true")
    p.add_argument("--csv",type=Path)
    p.add_argument("--meta",type=Path)
    p.add_argument("--begin-us",type=int)
    p.add_argument("--end-us",type=int)
    p.add_argument("--tid",type=int)
    p.add_argument("--pid",type=int,default=0)
    p.add_argument("--qpc-frequency-hz",type=int)
    args=p.parse_args()
    if args.self_test:
        self_test()
    else:
        if None in (args.csv,args.meta,args.begin_us,args.end_us,args.tid):
            p.error("--csv, --meta, --begin-us, --end-us, and --tid are required")
        meta=json.loads(args.meta.read_text(encoding="utf-8-sig"))
        result=analyze(read_csv(args.csv),meta,args.begin_us,args.end_us,args.tid,args.pid,args.qpc_frequency_hz)
        print(json.dumps(result,indent=2))
