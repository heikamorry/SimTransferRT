#!/usr/bin/env python3
"""Exercise the actual Qt CLI (or native probe) over isolated loopback UDP ports.

Exit zero means every requested frame has the expected functional outcome.
Timing misses remain visible and are not part of this small functional matrix.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import subprocess
import time
import sys
import uuid
from run_diagnostics import acceptance, free_ports, read_json, stop_owned, write_json
from acceptance_report import combined_acceptance, markdown as acceptance_markdown

CASES = [
    ('normal', [], [], 'A1'),
    ('first_loss', ['--drop-first', '2'], [], 'B64'),
    ('crc_damage', ['--corrupt-first', '4'], [], 'CRC'),
    ('duplicates', ['--duplicate-first', '1,3'], [], 'A1'),
    ('reverse', ['--reverse'], [], 'A1'),
    ('duplicate_nack', ['--drop-first', '2'], ['--duplicate-nack'], 'B64'),
    ('permanent_loss', ['--drop-always', '2'], [], 'C'),
    ('wrap', ['--start-id', '4294967294'], [], 'A1'),
]


def run_case(sender_path, out, case):
    name, extra, stub_extra, acceptance_case = case
    data_port, command_port = free_ports()
    sender_report = out / (name + '-sender.json')
    receiver_report = out / (name + '-receiver.json')
    ready_path = out / (name + '-ready.json')
    run_id = out.name + '-' + name
    receiver_command = [sys.executable, str(Path(__file__).with_name('udp_feedback_stub.py')),
                        '--port', str(data_port), '--cmd-port', str(command_port),
                        '--seconds', '0', '--report', str(receiver_report), '--quiet',
                        '--run-id', run_id, '--ready-file', str(ready_path), *stub_extra]
    sender_command = [str(sender_path), '--port', str(data_port), '--cmd-port', str(command_port),
                      '--nack-port', str(data_port), '--frames', '15', '--frame-bytes', '16384',
                      '--report', str(sender_report), '--run-id', run_id, *extra]
    creationflags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == 'nt' else 0
    sender = receiver = None
    failure = None
    cleanup_errors, forced = [], []
    with (out / (name + '-sender.log')).open('w', encoding='utf-8') as sender_log, \
         (out / (name + '-receiver.log')).open('w', encoding='utf-8') as receiver_log:
        try:
            receiver = subprocess.Popen(receiver_command, stdout=receiver_log, stderr=subprocess.STDOUT,
                                        creationflags=creationflags)
            ready_deadline = time.monotonic() + 5
            while not ready_path.exists():
                if receiver.poll() is not None or time.monotonic() > ready_deadline:
                    raise RuntimeError('receiver failed before readiness; see receiver log')
                time.sleep(.01)
            sender = subprocess.Popen(sender_command, stdout=sender_log, stderr=subprocess.STDOUT,
                                      creationflags=creationflags)
            sender.wait(timeout=6)
        except (Exception, KeyboardInterrupt) as exc:
            failure = type(exc).__name__ + ': ' + str(exc)
        finally:
            # Only our process objects are touched, sender first. A failed cleanup
            # must not skip receiver cleanup or discard the original error context.
            try:
                if stop_owned(sender):
                    forced.append('sender')
            except Exception as exc:
                cleanup_errors.append('sender: ' + repr(exc))
            try:
                if receiver and receiver.poll() is None:
                    # Lifetime is relative to sender completion, not receiver startup.
                    # A cold Debug startup must not consume the reception window.
                    # The sender already drains its cache; this bounded tail lets the
                    # receiver finish outstanding 2ms/10ms work before graceful exit.
                    time.sleep(.2)
                    if stop_owned(receiver):
                        forced.append('receiver')
            except Exception as exc:
                cleanup_errors.append('receiver: ' + repr(exc))
    sender_data = read_json(sender_report) or {}
    receiver_data = read_json(receiver_report) or {}
    evaluation = acceptance(acceptance_case, sender_data, receiver_data, 15, run_id)
    sender_stats = sender_data.get('stats', {})
    sender_exit = sender.returncode if sender else None
    receiver_exit = receiver.returncode if receiver else None
    passed = (failure is None and not cleanup_errors and not forced and sender_exit == 0 and
              receiver_exit == 0 and evaluation['functional_target_pass'] is True)
    result = {'case': name, 'run_id': run_id, 'pass': passed, 'failure': failure,
              'sender_exit': sender_exit, 'receiver_exit': receiver_exit,
              'cleanup_errors': cleanup_errors, 'forced_kills': forced,
              'completed': receiver_data.get('completed'), 'expired': receiver_data.get('expired'),
              'sender_frames': sender_stats.get('built'), 'retransmissions': sender_stats.get('retransmissions'),
              'deadline_misses': sender_stats.get('deadlineMisses'),
              'actual_deadline_misses': sender_stats.get('actualInitialDeadlineMisses'),
              'functional_failures': evaluation['functional_failures'],
              'reception': evaluation['reception'],
              'note': 'functional 16KiB/15-frame loopback test, not hard-real-time acceptance'}
    joint = dict(result, case=acceptance_case, functional_target_pass=evaluation['functional_target_pass'])
    acceptance_table = combined_acceptance(sender_data, receiver_data, joint)
    result['acceptance_table_status'] = acceptance_table['overall_status']
    write_json(out / (name + '-acceptance.json'), acceptance_table)
    (out / (name + '-acceptance.md')).write_text(acceptance_markdown(acceptance_table), encoding='utf-8')
    write_json(out / (name + '-environment.json'),
               {'run_id': run_id, 'network_path': 'IPv4 loopback',
                'sender_command': sender_command, 'receiver_command': receiver_command,
                'sender_pid': sender.pid if sender else None, 'receiver_pid': receiver.pid if receiver else None,
                'sender_build_reported': sender_data.get('build_type'),
                'sender_exit': sender_exit, 'receiver_exit': receiver_exit,
                'failure': failure, 'cleanup_errors': cleanup_errors, 'forced_kills': forced})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sender', required=True, type=Path)
    parser.add_argument('--out', default='test-results/e2e', type=Path)
    args = parser.parse_args()
    sender_path = args.sender.resolve()
    out = args.out.resolve() / ('e2e-' + uuid.uuid4().hex[:12])
    out.mkdir(parents=True, exist_ok=False)
    results = []
    for case in CASES:
        result = run_case(sender_path, out, case)
        results.append(result)
        write_json(out / 'summary.json', results)  # Keep finished cases even if a later case fails.
        print(json.dumps(result), flush=True)
        if (result['failure'] or '').startswith('KeyboardInterrupt:'):
            break
    print('Reports: ' + str(out), flush=True)
    return 0 if len(results) == len(CASES) and all(result['pass'] for result in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
