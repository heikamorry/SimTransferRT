#!/usr/bin/env python3
"""Bounded, isolated Qt CLI UDP diagnostics. Local reports never add wire fields."""
from __future__ import annotations
import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time
import uuid
from datetime import datetime, timezone
from acceptance_report import combined_acceptance, markdown as acceptance_markdown

ROOT = Path(__file__).resolve().parents[1]
CASES = {
    'A0-closed': (1048576, [], None),
    'A0-drain': (1048576, [], 'drain'),
    'A1': (1048576, [], 'full'),
    'B64': (65536, ['--drop-first', '2'], 'full'),
    'B1': (1048576, ['--drop-first', '2'], 'full'),
    'C': (1048576, ['--drop-always', '2'], 'full'),
    'CRC': (1048576, ['--corrupt-first', '4'], 'full'),
}

def write_json(path, value):
    path = Path(path)
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(value, indent=2), encoding='utf-8')
    temporary.replace(path)

def free_ports():
    # Reserve both simultaneously during selection. Bind failures after release
    # are surfaced; unrelated processes are never killed to claim a port.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as a, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as b:
        a.bind(('127.0.0.1', 0)); b.bind(('127.0.0.1', 0))
        return a.getsockname()[1], b.getsockname()[1]

def provenance(root):
    def git(*args):
        p = subprocess.run(['git', '-C', str(root), *args], capture_output=True, text=True)
        return p.stdout.strip() if p.returncode == 0 else None
    hashes = {}
    for directory in ('src', 'tools', 'scripts', 'tests'):
        for path in sorted((root / directory).rglob('*')):
            if path.is_file() and '__pycache__' not in path.parts:
                hashes[str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return {'git_head': git('rev-parse', 'HEAD'), 'git_status': git('status', '--porcelain'),
            'source_sha256': hashlib.sha256(json.dumps(hashes, sort_keys=True).encode()).hexdigest(),
            'file_sha256': hashes}

def cpu_seconds(process):
    if os.name != 'nt':
        return None
    class FileTime(ctypes.Structure):
        _fields_ = [('low', ctypes.c_uint32), ('high', ctypes.c_uint32)]
    function = ctypes.WinDLL('kernel32', use_last_error=True).GetProcessTimes
    function.argtypes = [ctypes.c_void_p] + [ctypes.POINTER(FileTime)] * 4
    function.restype = ctypes.c_int32
    creation, end, kernel, user = [FileTime() for _ in range(4)]
    if not function(int(process._handle), ctypes.byref(creation), ctypes.byref(end), ctypes.byref(kernel), ctypes.byref(user)):
        return None
    return ((kernel.high << 32) + kernel.low + (user.high << 32) + user.low) / 1e7

def memory_bytes(process):
    """Windows process counters; private bytes are committed private memory, not RSS."""
    if os.name != 'nt':
        return {'available': False, 'error': 'unsupported platform (Windows counters only)'}
    class ProcessMemoryCountersEx(ctypes.Structure):
        _fields_ = [('cb', ctypes.c_uint32), ('PageFaultCount', ctypes.c_uint32)] + [
            (name, ctypes.c_size_t) for name in (
                'PeakWorkingSetSize', 'WorkingSetSize', 'QuotaPeakPagedPoolUsage',
                'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage',
                'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage', 'PrivateUsage')]
    try:
        function = ctypes.WinDLL('psapi', use_last_error=True).GetProcessMemoryInfo
        function.argtypes = [ctypes.c_void_p, ctypes.POINTER(ProcessMemoryCountersEx), ctypes.c_uint32]
        function.restype = ctypes.c_int32
        counters = ProcessMemoryCountersEx()
        counters.cb = ctypes.sizeof(counters)
        if not function(int(process._handle), ctypes.byref(counters), counters.cb):
            return {'available': False, 'error': 'GetProcessMemoryInfo WinError ' + str(ctypes.get_last_error())}
        return {'available': True, 'working_set_bytes': counters.WorkingSetSize,
                'private_bytes': counters.PrivateUsage,
                'os_peak_working_set_bytes': counters.PeakWorkingSetSize}
    except (OSError, AttributeError, TypeError, ValueError) as exc:
        return {'available': False, 'error': type(exc).__name__ + ': ' + str(exc)}


class BoundedMemorySampler:
    """One-second external observations, constant storage; never a leak verdict."""
    def __init__(self, start, interval=1.0):
        self.start, self.interval = start, interval
        self.processes, self.next_due = {}, {}

    def sample(self, name, process, now, *, force=False):
        if process is None or process.returncode is not None:
            return
        if not force and now < self.next_due.get(name, self.start):
            return
        self.next_due[name] = now + self.interval
        entry = self.processes.setdefault(name, {'pid': process.pid, 'samples': 0,
            'failed_samples': 0, 'first_observed': None, 'last_live_observed': None,
            'sampled_peak_working_set_bytes': None, 'sampled_peak_private_bytes': None,
            'os_peak_working_set_bytes': None, 'last_error': None})
        try:
            observation = memory_bytes(process)
        except Exception as exc:
            # Optional telemetry must never prevent owned-process cleanup.
            observation = {'available': False, 'error': type(exc).__name__ + ': ' + str(exc)}
        if not observation.get('available'):
            entry['failed_samples'] += 1
            entry['last_error'] = observation.get('error', 'counters unavailable')
            return
        point = {'elapsed_seconds': round(now - self.start, 6),
                 'working_set_bytes': observation['working_set_bytes'],
                 'private_bytes': observation['private_bytes']}
        entry['samples'] += 1
        if entry['first_observed'] is None:
            entry['first_observed'] = point
        entry['last_live_observed'] = point
        for target, source in [('sampled_peak_working_set_bytes', 'working_set_bytes'),
                               ('sampled_peak_private_bytes', 'private_bytes'),
                               ('os_peak_working_set_bytes', 'os_peak_working_set_bytes')]:
            entry[target] = max(entry[target] or 0, observation[source])

    def report(self):
        return {'interval_seconds': self.interval, 'storage': 'constant: first/last live/peaks per owned process',
                'source': 'Windows GetProcessMemoryInfo PROCESS_MEMORY_COUNTERS_EX',
                'note': 'First/last are observations, not exact startup/exit values. '
                        'Sampled peaks can miss between-sample spikes; private bytes are private commit. '
                        'No stable-memory or leak-free verdict is inferred from these counters.',
                'processes': self.processes}


def stop_owned(process):
    if process is None or process.poll() is not None:
        return False
    try:
        process.send_signal(signal.CTRL_BREAK_EVENT if os.name == 'nt' else signal.SIGINT)
        process.wait(timeout=3)
        return False
    except (OSError, subprocess.TimeoutExpired):
        process.kill()
        process.wait(timeout=3)
        return True

def read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None

MANIFEST_KEYS = ('seen', 'completed', 'expired', 'resource_limit', 'metadata_mismatch', 'module_invalid')
MAX_INVENTORY_IDS = 1000000
MAX_REPORTED_IDS = 128


def _uint32(value):
    return type(value) is int and 0 <= value <= 0xffffffff


def _manifest_ids(ranges):
    if not isinstance(ranges, list) or len(ranges) > 16384:
        raise ValueError('invalid or excessive manifest ranges')
    count = 0
    result = set()
    for item in ranges:
        if (not isinstance(item, (list, tuple)) or len(item) != 2 or
                not all(_uint32(value) for value in item) or item[0] > item[1]):
            raise ValueError('invalid manifest uint32 range')
        first, last = item
        count += last - first + 1
        if count > MAX_INVENTORY_IDS:
            raise ValueError('manifest range expansion exceeds bounded inventory')
        result.update(range(first, last + 1))
    return result


def reconcile(sender, receiver):
    stats = sender.get('stats', {})
    built = stats.get('built', 0)
    reported_completed = receiver.get('completed', 0)
    valid_count = type(built) is int and 0 <= built <= MAX_INVENTORY_IDS
    result = {'sender_built_denominator': built,
              'completed': reported_completed,
              'completion_fraction': reported_completed / built
                  if valid_count and built and type(reported_completed) is int else None,
              'exact_classification_available': False,
              'note': 'Local user-space observations; stop tail and never-seen frames are separate.'}
    manifest = receiver.get('frame_manifest', {})
    if not isinstance(manifest, dict) or not manifest or receiver.get('manifest_truncated', True) or not valid_count:
        result['exact_classification'] = 'unavailable: absent/truncated manifest or invalid sender count'
        return result
    try:
        next_id = sender['next_logic_id']
        if not _uint32(next_id):
            raise ValueError('invalid sender next_logic_id')
        first_id = (next_id - built) & 0xffffffff
        built_manifest = sender.get('built_manifest')
        if built_manifest is not None and not isinstance(built_manifest, dict):
            raise ValueError('invalid sender built manifest')
        if built_manifest and (built_manifest.get('count') != built or
                               built_manifest.get('first_id') != first_id or
                               built_manifest.get('next_id') != next_id):
            raise ValueError('sender built manifest disagrees with counters')
        expected = {(first_id + index) & 0xffffffff for index in range(built)}
        inventory = {key: _manifest_ids(manifest.get(key, [])) for key in MANIFEST_KEYS}
        active_ids = receiver.get('active_logic_ids', [])
        if (not isinstance(active_ids, list) or len(active_ids) > MAX_INVENTORY_IDS or
                not all(_uint32(value) for value in active_ids)):
            raise ValueError('invalid active frame inventory')
        active = set(active_ids)
    except (KeyError, TypeError, ValueError) as exc:
        result['exact_classification'] = 'unavailable: ' + str(exc)
        return result
    seen, completed = inventory['seen'], inventory['completed']
    outcomes = [inventory[key] for key in MANIFEST_KEYS[1:]] + [active]
    retired = set().union(*(inventory[key] for key in MANIFEST_KEYS[2:]))
    all_outcomes = set().union(*outcomes)
    overlap = set()
    previous = set()
    for outcome in outcomes:
        overlap.update(previous & outcome)
        previous.update(outcome)
    missing = expected - seen
    unclassified = (expected & seen) - all_outcomes
    unexpected = (seen | all_outcomes) - expected
    outside_seen = all_outcomes - seen
    counter_mismatches = []
    for field, key in [('completed', 'completed'), ('expired', 'expired'),
                       ('active_evictions', 'resource_limit'), ('metadata_failures', 'metadata_mismatch'),
                       ('module_failures', 'module_invalid')]:
        if field in receiver and receiver[field] != len(inventory[key]):
            counter_mismatches.append(field)
    if 'active_frames' in receiver and receiver['active_frames'] != len(active):
        counter_mismatches.append('active_frames')
    result.update(exact_classification_available=True,
                  exact_classification='sender contiguous built ID inventory reconciled with receiver manifest',
                  manifest_consistent=not (overlap or outside_seen or counter_mismatches),
                  counter_mismatches=counter_mismatches,
                  seen=len(expected & seen), completed=len(expected & completed),
                  completion_fraction=len(expected & completed) / built if built else None,
                  retired=len(expected & retired), still_in_flight=len(expected & active),
                  retired_by_reason={key: len(expected & inventory[key]) for key in MANIFEST_KEYS[2:]},
                  wholly_missing=len(missing), unclassified_seen=len(unclassified),
                  unexpected_receiver=len(unexpected), overlapping_outcomes=len(overlap),
                  outcomes_without_seen=len(outside_seen),
                  stopping_unsent_fragments=stats.get('unsentAtStop'))
    lists = {'wholly_missing_ids': missing, 'unclassified_seen_ids': unclassified,
             'unexpected_receiver_ids': unexpected, 'overlapping_outcome_ids': overlap,
             'outcomes_without_seen_ids': outside_seen}
    result.update({key: sorted(values)[:MAX_REPORTED_IDS] for key, values in lists.items()})
    result['id_lists_truncated'] = any(len(values) > MAX_REPORTED_IDS for values in lists.values())
    result['id_list_limit'] = MAX_REPORTED_IDS
    return result


def _counter(value):
    return type(value) is int and value >= 0


def realtime_acceptance(name, sender):
    """Add strict evidence gates without changing the historical timing flag."""
    stats = sender.get('stats', {})
    timing = sender.get('timing', {})
    deviation = timing.get('abs_interval_minus_20000us', {}) if isinstance(timing, dict) else {}
    if not isinstance(deviation, dict):
        deviation = {}
    sampled = stats.get('sampled')
    jitter_missing, jitter_failures = [], []
    if not _counter(sampled):
        jitter_missing.append('stats.sampled')
    for key in ('count', 'max_us'):
        if not _counter(deviation.get(key)):
            jitter_missing.append('timing.abs_interval_minus_20000us.' + key)
    if not jitter_missing:
        if sampled < 2:
            jitter_missing.append('at least two actual samples are required')
        elif deviation['count'] != sampled - 1:
            jitter_missing.append('sampling deviation count must cover all sampled - 1 intervals')
        if deviation['max_us'] >= 1000:
            jitter_failures.append('max absolute sampling interval deviation must be < 1000us')
    initial_missing, initial_failures = [], []
    keys = ('built', 'initialFinishedFrames', 'initialIncompleteFrames', 'unsentAtStop', 'initialPendingFragments')
    for key in keys:
        if not _counter(stats.get(key)):
            initial_missing.append('stats.' + key)
    for key in ('initialIncompleteFrames', 'unsentAtStop', 'initialPendingFragments'):
        if _counter(stats.get(key)) and stats[key]:
            initial_failures.append(key + ' must be zero')
    if _counter(stats.get('built')) and _counter(stats.get('initialFinishedFrames')):
        if stats['initialFinishedFrames'] != stats['built']:
            initial_failures.append('initialFinishedFrames must cover every built frame')
    # Deliberate drop scenarios exercise recovery, so fully submitted is not their target.
    if name not in ('B64', 'B1', 'C'):
        if not _counter(stats.get('initialFullySubmittedFrames')):
            initial_missing.append('stats.initialFullySubmittedFrames')
        elif _counter(stats.get('built')) and stats['initialFullySubmittedFrames'] != stats['built']:
            initial_failures.append('initialFullySubmittedFrames must cover every built frame without injected drops')
    result = {'acceptance_schema_version': 3,
              'sampling_jitter_limit_us_exclusive': 1000,
              'sampling_jitter_population': 'all actual samples, including incomplete or expired frames; sampled - 1 intervals',
              'sampling_jitter_max_us': deviation.get('max_us'),
              'sampling_jitter_observations': deviation.get('count')}
    for prefix, missing, failures in [('sampling_jitter', jitter_missing, jitter_failures),
                                      ('initial_dispatch', initial_missing, initial_failures)]:
        verdict = False if failures else None if missing else True
        result.update({prefix + '_target_pass': verdict,
                       prefix + '_status': 'fail' if verdict is False else 'unverified' if verdict is None else 'pass',
                       prefix + '_failures': failures, prefix + '_metrics_unavailable': missing})
    return result


def acceptance(name, sender, receiver, frames_requested, run_id, *, legacy_sender=False, legacy_receiver=False):
    """Keep operational correctness, receiver evidence and timing independently visible."""
    stats = sender.get('stats', {})
    functional_failures = []
    if stats.get('built') != frames_requested or frames_requested <= 0:
        functional_failures.append('sender did not build the requested frame inventory')
    if not sender.get('df_enabled'):
        functional_failures.append('sender did not confirm DF enabled')
    if not legacy_sender and sender.get('run_id') != run_id:
        functional_failures.append('sender run_id mismatch')
    if name != 'A0-closed' and not receiver:
        functional_failures.append('receiver report unavailable')
    if name != 'A0-closed' and not legacy_receiver and receiver.get('run_id') != run_id:
        functional_failures.append('receiver run_id mismatch')
    if name == 'A0-drain' and receiver.get('drain_datagrams', 0) <= 0:
        functional_failures.append('discard receiver did not observe any UDP datagrams')
    timing_keys = ('unsentExpired', 'deadlineMisses', 'sendErrors', 'actualInitialDeadlineMisses')
    missing_timing = [key for key in timing_keys if key not in stats]
    failed_timing = [key for key in timing_keys if key in stats and stats[key] != 0]
    timing_pass = not missing_timing and not failed_timing
    result = {'timing_target_pass': timing_pass,
              'timing_failures': failed_timing,
              'timing_metrics_unavailable': missing_timing}
    result.update(realtime_acceptance(name, sender))
    if name in ('A0-closed', 'A0-drain'):
        # A successful sender-only/drain run says nothing about protocol delivery.
        result.update(functional_target_pass=None,
                      functional_note='not evaluated: this mode has no end-to-end verification',
                      sender_inventory_pass=not functional_failures,
                      functional_failures=functional_failures,
                      reception={'note': 'no end-to-end correctness verification in this mode'})
        return result
    reception = reconcile(sender, receiver)
    if not reception.get('exact_classification_available'):
        functional_failures.append('exact receiver frame inventory unavailable')
    elif (not reception.get('manifest_consistent') or reception['unexpected_receiver'] or
          reception['unclassified_seen'] or reception['wholly_missing'] or reception['still_in_flight']):
        functional_failures.append('receiver inventory has missing, in-flight, unexpected or inconsistent outcomes')
    for key in ('module_failures', 'metadata_failures', 'active_evictions'):
        if receiver.get(key) != 0:
            functional_failures.append('receiver ' + key + ' nonzero or unavailable')
    if receiver.get('max_requests_for_fragment', 4) > 3:
        functional_failures.append('receiver exceeded three requests per fragment')
    if name == 'C':
        if (reception.get('retired_by_reason', {}).get('expired') != frames_requested or
                reception.get('completed') != 0 or receiver.get('max_requests_for_fragment') != 3):
            functional_failures.append('persistent loss did not expire every requested frame after three requests')
    elif reception.get('completed') != frames_requested or reception.get('retired', 0) != 0:
        functional_failures.append('receiver did not complete every sender frame')
    if name in ('B64', 'B1', 'CRC'):
        if stats.get('retransmissions', 0) <= 0 or receiver.get('recovered', 0) <= 0:
            functional_failures.append('fault recovery loop lacks retransmission/recovered-fragment evidence')
    result.update(functional_target_pass=not functional_failures,
                  sender_inventory_pass=stats.get('built') == frames_requested,
                  functional_failures=functional_failures, reception=reception)
    return result


def case_passed(summary):
    return (not summary.get('failure') and summary.get('sender_exit') == 0 and
            (summary.get('receiver_exit') is None if summary.get('case') == 'A0-closed'
             else summary.get('receiver_exit') == 0) and summary.get('timing_target_pass') is True and
            summary.get('sampling_jitter_target_pass') is True and
            summary.get('initial_dispatch_target_pass') is True and
            summary.get('sender_inventory_pass') is True and
            (summary.get('functional_target_pass') is True or
             (summary.get('case') in ('A0-closed', 'A0-drain') and summary.get('functional_target_pass') is None)))


def write_suite_summary(suite_dir, results):
    write_json(suite_dir / 'summary.json', results)
    lines = ['# UDP diagnostic suite', '',
             'Functional outcomes and measured timing targets are evaluated separately. '
             'A0 modes do not verify protocol delivery.', '',
             'The historical Timing flag is retained; Jitter requires max <1000us across all sample intervals, '
             'and Initial requires every built frame to finish initial dispatch. Missing evidence is unverified.', '',
             '| Case | Seconds | Built | Completed | Expired | Functional | Timing | Jitter | Initial | Report |',
             '|---|---:|---:|---:|---:|---|---|---|---|---|']
    def status(value):
        return 'PASS' if value is True else 'FAIL' if value is False else 'not evaluated'
    for result in results:
        directory = Path(result['directory']).name
        lines.append('| ' + ' | '.join(str(value) for value in (
            result['case'], result['seconds'], result.get('built'), result.get('completed'),
            result.get('expired'), status(result.get('functional_target_pass')),
            status(result.get('timing_target_pass')), result.get('sampling_jitter_status', 'unverified'),
            result.get('initial_dispatch_status', 'unverified'), '[details](' + directory + '/summary.md)')) + ' |')
    lines += ['', 'Individual reports retain missing-frame classifications, failures, '
                   'process exit codes, metrics, and reproduction commands.']
    (suite_dir / 'summary.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')

def run_case(args, suite_dir, name, seconds, ordinal):
    size, faults, mode = CASES[name]
    run_id = f'{name}-{ordinal}-{uuid.uuid4().hex[:12]}'
    out = suite_dir / run_id
    out.mkdir()
    data_port, command_port = free_ports()
    env = os.environ.copy()
    env['PATH'] = str(Path(args.qt_prefix) / 'bin') + os.pathsep + env.get('PATH', '')
    frames = max(1, round(seconds * 50))
    sender_command = [str(args.sender), '--port', str(data_port), '--cmd-port', str(command_port),
                      '--nack-port', str(data_port), '--frames', str(frames), '--frame-bytes', str(size),
                      '--report', str(out / 'sender-report.json'), *faults]
    if not args.legacy_sender:
        sender_command += ['--run-id', run_id]
    timing_trace = getattr(args, 'timing_trace', None)
    if timing_trace is not None:
        sender_command += ['--timing-trace', timing_trace]
    managed_receiver = not args.legacy_receiver or mode == 'drain'
    receiver_command = [sys.executable, str(args.receiver), '--port', str(data_port),
                        '--cmd-port', str(command_port), '--seconds', '0' if managed_receiver else str(seconds + 2),
                        '--report', str(out / 'receiver-report.json'), '--quiet']
    if not args.legacy_receiver:
        receiver_command += ['--run-id', run_id, '--ready-file', str(out / 'receiver-ready.json'),
                             '--status-file', str(out / 'receiver-status.json')]
    if mode == 'drain':
        # Original receiver had no discard mode. Use the new discard implementation
        # in both comparisons and record its separate hash in environment.json.
        receiver_command[1] = str(ROOT / 'tools' / 'udp_feedback_stub.py')
        if args.legacy_receiver:
            receiver_command += ['--run-id', run_id, '--ready-file', str(out / 'receiver-ready.json'),
                                 '--status-file', str(out / 'receiver-status.json')]
        receiver_command += ['--drain']
    configuration = {'report_schema_version': 2, 'run_id': run_id, 'case': name,
        'started_utc': datetime.now(timezone.utc).isoformat(), 'python': sys.version,
        'platform': sys.platform, 'network_path': 'IPv4 loopback (not physical 10Gbps)',
        'build_requested': args.build, 'build_verified': args.actual_build,
        'module_mask': 1, 'module_body_bytes': size - 5, 'frame_bytes': size,
        'sampling_period_us': 20000, 'payload_step': 1436, 'frames_requested': frames,
        'fault_arguments': faults, 'priority': 'default', 'affinity': 'default',
        'timing_trace_requested': timing_trace,
        'sender_command': sender_command, 'receiver_command': receiver_command if mode else None,
        'sender_executable_sha256': hashlib.sha256(args.sender.read_bytes()).hexdigest(),
        'receiver_source_sha256': hashlib.sha256(Path(receiver_command[1]).read_bytes()).hexdigest() if mode else None,
        'source': args.source_info, 'checkpoint': '1 second when supported, quiet terminal, no NACK trace',
        'sender_report_legacy': args.legacy_sender, 'receiver_report_legacy': args.legacy_receiver and mode != 'drain',
        'receiver_lifetime_policy': ('sender completion plus 2 second tail, then graceful signal' if managed_receiver
                                     else 'legacy fixed startup-relative limit; baseline comparison may be invalid if sender starts late')}
    write_json(out / 'environment.json', configuration)
    sender = receiver = None
    failure = None
    forced = []
    cleanup_errors = []
    creationflags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == 'nt' else 0
    begin = time.monotonic()
    memory = BoundedMemorySampler(begin)
    with (out / 'sender.log').open('w', encoding='utf-8') as slog, (out / 'receiver.log').open('w', encoding='utf-8') as rlog:
        try:
            if mode:
                receiver = subprocess.Popen(receiver_command, stdout=rlog, stderr=subprocess.STDOUT, env=env, creationflags=creationflags)
                memory.sample('receiver', receiver, time.monotonic())
                ready_deadline = time.monotonic() + 5
                while True:
                    if (out / 'receiver-ready.json').exists():
                        break
                    if args.legacy_receiver and mode != 'drain' and 'LISTEN ' in (out / 'receiver.log').read_text(encoding='utf-8'):
                        break
                    if receiver.poll() is not None or time.monotonic() > ready_deadline:
                        raise RuntimeError('receiver readiness failed; see receiver.log')
                    time.sleep(0.02)
            sender = subprocess.Popen(sender_command, stdout=slog, stderr=subprocess.STDOUT, env=env, creationflags=creationflags)
            memory.sample('sender', sender, time.monotonic())
            configuration.update(sender_pid=sender.pid, receiver_pid=receiver.pid if receiver else None)
            deadline = time.monotonic() + seconds + 10
            last_complete, last_progress, last_nacks = 0, time.monotonic(), 0
            while sender.poll() is None:
                observed_at = time.monotonic()
                memory.sample('sender', sender, observed_at)
                memory.sample('receiver', receiver, observed_at)
                if time.monotonic() > deadline:
                    raise TimeoutError('sender bounded wall-time exceeded')
                if receiver and receiver.poll() is not None:
                    raise RuntimeError('receiver exited before sender')
                status = read_json(out / 'receiver-status.json') or {}
                complete = status.get('completed', status.get('stats', {}).get('completed', 0))
                nacks = status.get('nack_packets', status.get('stats', {}).get('nack_packets', 0))
                if complete > last_complete:
                    last_complete, last_progress, last_nacks = complete, time.monotonic(), nacks
                # Intentional permanent loss expects no completions. Other full modes
                # stop only on sustained stagnation plus growing feedback, not one slow frame.
                if name not in ('C', 'A0-closed', 'A0-drain') and time.monotonic() - last_progress > 5 and nacks - last_nacks > 1000:
                    raise RuntimeError('bounded guard: completions stalled >5s with increasing NACKs')
                time.sleep(0.05)
            configuration['sender_cpu_seconds'] = cpu_seconds(sender)
            configuration['sender_exit_code'] = sender.returncode
            if receiver and not managed_receiver:
                # Original scripts may not preserve reports on SIGBREAK. Keep their
                # finite baseline-only lifetime explicit instead of assuming support.
                receiver.wait(timeout=max(5, seconds + 5 - (time.monotonic() - begin)))
        except (Exception, KeyboardInterrupt) as exc:
            failure = f'{type(exc).__name__}: {exc}'
        finally:
            observed_at = time.monotonic()
            memory.sample('sender', sender, observed_at, force=True)
            memory.sample('receiver', receiver, observed_at, force=True)
            try:
                if stop_owned(sender): forced.append('sender')
            except Exception as exc:
                cleanup_errors.append('sender cleanup: ' + repr(exc))
            # Receiver is always last; preserve its bounded pending feedback when possible.
            try:
                if receiver and receiver.poll() is None:
                    if managed_receiver:
                        # Startup and scheduling delays must not consume the receive
                        # window. The sender has stopped sampling and drained first.
                        time.sleep(2)
                        memory.sample('receiver', receiver, time.monotonic(), force=True)
                        if stop_owned(receiver): forced.append('receiver')
                    else:
                        try:
                            receiver.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            if stop_owned(receiver): forced.append('receiver')
                if receiver:
                    configuration['receiver_cpu_seconds'] = cpu_seconds(receiver)
            except Exception as exc:
                cleanup_errors.append('receiver cleanup: ' + repr(exc))
            if cleanup_errors and failure is None:
                failure = 'process cleanup failed; see environment.json'
            configuration.update(ended_utc=datetime.now(timezone.utc).isoformat(), wall_seconds=time.monotonic() - begin,
                                 process_memory=memory.report(),
                                 failure=failure, forced_kills=forced, cleanup_errors=cleanup_errors,
                                 sender_exit_code=sender.returncode if sender else None,
                                 receiver_exit_code=receiver.returncode if receiver else None)
            write_json(out / 'environment.json', configuration)
    a, b = read_json(out / 'sender-report.json') or {}, read_json(out / 'receiver-report.json') or {}
    stats = a.get('stats', {})
    summary = {'run_id': run_id, 'case': name, 'seconds': seconds, 'directory': str(out),
               'failure': failure, 'sender_exit': configuration.get('sender_exit_code'),
               'receiver_exit': configuration.get('receiver_exit_code'),
               'built': stats.get('built'), 'completed': b.get('completed'), 'expired': b.get('expired'),
               'nacks': stats.get('nacks'), 'cacheMisses': stats.get('cacheMisses'),
               'unsentExpired': stats.get('unsentExpired'), 'deadlineMisses': stats.get('deadlineMisses'),
               'actualInitialDeadlineMisses': stats.get('actualInitialDeadlineMisses'),
               'sendErrors': stats.get('sendErrors'), 'receiver_max_requests': b.get('max_requests_for_fragment'),
               'recovered': b.get('recovered'), 'retransmissions': stats.get('retransmissions'),
               'sender_cpu_seconds': configuration.get('sender_cpu_seconds'),
               'receiver_cpu_seconds': configuration.get('receiver_cpu_seconds')}
    summary['process_memory'] = configuration.get('process_memory')
    summary.update(acceptance(name, a, b, frames, run_id,
                              legacy_sender=args.legacy_sender,
                              legacy_receiver=args.legacy_receiver and mode != 'drain'))
    summary['case_pass'] = case_passed(summary)
    summary['acceptance_table'] = combined_acceptance(a, b, summary)
    write_json(out / 'summary.json', summary)
    write_json(out / 'acceptance.json', summary['acceptance_table'])
    (out / 'acceptance.md').write_text(acceptance_markdown(summary['acceptance_table']), encoding='utf-8')
    (out / 'summary.md').write_text('# ' + run_id + '\n\n' + acceptance_markdown(summary['acceptance_table']) +
        '\n原始收发报告、运行环境及既有功能验收字段保存在本目录JSON中。\n', encoding='utf-8')
    console = {key: value for key, value in summary.items() if key != 'acceptance_table'}
    console['protocol_table_status'] = summary['acceptance_table']['overall_status']
    print(json.dumps(console, ensure_ascii=True), flush=True)
    return summary

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sender', type=Path)
    parser.add_argument('--receiver', type=Path, default=ROOT / 'tools' / 'udp_feedback_stub.py')
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--qt-prefix', default=os.environ.get('QT_PREFIX', r'C:\Qt\6.12.0\msvc2022_64'))
    parser.add_argument('--build', choices=['Debug', 'Release'], default='Release')
    parser.add_argument('--suite', choices=['short', 'long'], default='short')
    parser.add_argument('--cases', default=','.join(CASES))
    parser.add_argument('--seconds', type=float, default=10)
    parser.add_argument('--rounds', type=int, default=1)
    parser.add_argument('--out', type=Path, default=ROOT / 'artifacts' / 'udp-diagnostics')
    parser.add_argument('--legacy-sender', action='store_true')
    parser.add_argument('--legacy-receiver', action='store_true')
    parser.add_argument('--timing-trace', choices=['on', 'off'], default=None,
                        help='forward an explicit trace setting; omitted preserves the sender default')
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or args.seconds <= 0 or args.rounds < 1:
        parser.error('seconds and rounds must be positive')
    if args.legacy_sender and args.timing_trace is not None:
        parser.error('--timing-trace cannot be combined with --legacy-sender')
    args.sender = (args.sender or ROOT / 'build' / ('windows-' + args.build.lower()) / 'SimTransferCli.exe').resolve()
    args.receiver = args.receiver.resolve()
    cache = args.sender.parent / 'CMakeCache.txt'
    cache_text = cache.read_text(encoding='utf-8') if cache.exists() else ''
    build_line = next((line for line in cache_text.splitlines() if line.startswith('CMAKE_BUILD_TYPE:STRING=')), '')
    args.actual_build = build_line.partition('=')[2]
    if args.actual_build != args.build:
        parser.error(f'actual CMake build {args.actual_build!r} does not match requested {args.build!r}')
    if any(name not in CASES for name in args.cases.split(',')):
        parser.error('unknown case')
    args.source_info = provenance(args.source_root.resolve())
    suite_dir = args.out.resolve() / (datetime.now().strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8])
    suite_dir.mkdir(parents=True, exist_ok=False)
    schedule = [(name, args.seconds) for _ in range(args.rounds) for name in args.cases.split(',')]
    if args.suite == 'long':
        schedule = [(name, 60) for _ in range(3) for name in ('A0-drain', 'A1')] + [('A1', 120)]
    results = []
    for index, (name, seconds) in enumerate(schedule):
        results.append(run_case(args, suite_dir, name, seconds, index))
        write_suite_summary(suite_dir, results)
        if (results[-1].get('failure') or '').startswith('KeyboardInterrupt:'):
            break  # User cancellation stops the suite after owned-process cleanup.
    # Nonzero exposes missed acceptance targets; reports are still complete.
    return 0 if all(case_passed(result) for result in results) else 1

if __name__ == '__main__':
    raise SystemExit(main())
