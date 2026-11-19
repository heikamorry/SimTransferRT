#!/usr/bin/env python3
"""Same-input receiver CPU comparison; not UDP, latency, or throughput acceptance.

--before points at an archived receiver_model.py from the pre-change tree.
Virtual input timestamps are identical in both implementations. Packet construction
and model import are outside the timed region; ingest + poll are measured together.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import platform
import statistics
import sys
import time
from receiver_model import ReceiverModel
from rt_protocol import STEP, build_modules, encode_data


def load_before(path):
    spec = importlib.util.spec_from_file_location('receiver_model_before', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module.ReceiverModel


def inputs(stalled):
    mask, body = build_modules({0: b'x' * 1048571})
    events = []
    if stalled:
        # Frozen user processing time models a batch during a backlog. Sixteen
        # incomplete frames have tail holes and retry timers not yet due.
        for logic in range(1, 17):
            events.append((encode_data(logic, 100, mask, body, 0), 0.0))
        for index in range(731):
            events.append((encode_data(17, 100, mask, body, index), 0.001))
    else:
        for logic in range(1, 21):
            for index in range(731):
                events.append((encode_data(logic, logic * 20000, mask, body, index),
                               logic * .020 + index * .000020))
    return events


def measure(cls, events):
    m = cls()
    start = time.perf_counter_ns()
    for wire, now in events:
        m.ingest(wire, now)
        m.poll(now)
    elapsed = (time.perf_counter_ns() - start) / 1e6
    return dict(elapsed_ms=elapsed, datagrams=m.stats['datagrams'],
                completed=m.stats['completed'], requested_fragments=m.stats['requested_fragments'],
                nack_packets=m.stats['nack_packets'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', required=True)
    parser.add_argument('--report', required=True)
    parser.add_argument('--rounds', type=int, default=3)
    args = parser.parse_args()
    before = load_before(args.before)
    result = dict(python=sys.version, platform=platform.platform(),
                  timing_note='CPU microbenchmark only; same virtual inputs; no real UDP',
                  before_sha256=hashlib.sha256(Path(args.before).read_bytes()).hexdigest(),
                  after_sha256=hashlib.sha256(Path(__file__).with_name('receiver_model.py').read_bytes()).hexdigest(),
                  scenarios={})
    for name, stalled in [('normal_20x1MiB', False), ('sixteen_stalled_plus_one_1MiB', True)]:
        events = inputs(stalled)
        samples = {'before': [], 'after': []}
        # Alternate implementation order so warmup/order is visible, not hidden.
        for round_index in range(args.rounds):
            order = [('before', before), ('after', ReceiverModel)]
            if round_index % 2:
                order.reverse()
            for label, cls in order:
                sample = measure(cls, events)
                samples[label].append(sample)
                print(name, round_index, label, json.dumps(sample), flush=True)
        medians = {label: statistics.median(s['elapsed_ms'] for s in rows)
                   for label, rows in samples.items()}
        result['scenarios'][name] = dict(samples=samples, median_ms=medians,
                                         ratio_before_over_after=medians['before'] / medians['after'])
    Path(args.report).write_text(json.dumps(result, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
