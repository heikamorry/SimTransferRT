#!/usr/bin/env python3
"""Bounded diagnostic DATA/NACK receiver; no SUCCESS or hardware timing claims."""
from __future__ import annotations
import argparse
from collections import deque
from datetime import datetime, timezone
import errno
import json
import math
import select
import signal
import socket
import struct
import sys
import time
import traceback
from acceptance_report import receiver_acceptance
from pathlib import Path
from receiver_model import ReceiverModel
from runtime_support import atomic_json, configure_udp_connreset, provenance


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bind', default='127.0.0.1')
    p.add_argument('--port', type=int, default=5650)
    p.add_argument('--cmd-ip', default='127.0.0.1')
    p.add_argument('--cmd-port', type=int, default=5500)
    p.add_argument('--seconds', type=float, default=0.0, help='0 = until Ctrl+C')
    p.add_argument('--report', default='receiver-report.json')
    p.add_argument('--output', default='', help='write ONLY most recent complete logical frame')
    p.add_argument('--duplicate-nack', action='store_true')
    p.add_argument('--quiet', action='store_true')
    p.add_argument('--summary-every', type=float, default=1.0)
    p.add_argument('--trace-nacks', default='', metavar='PATH', help='bounded recent NACK/event JSONL ring saved at exit')
    p.add_argument('--trace-limit', type=int, default=2048)
    p.add_argument('--run-id', default='uncoordinated')
    p.add_argument('--ready-file', default='', help='readiness JSON written after successful bind')
    p.add_argument('--status-file', default='', help='optional low-frequency atomic progress checkpoint')
    p.add_argument('--checkpoint-every', type=float, default=1.0)
    p.add_argument('--drain', action='store_true', help='discard UDP without CRC/reassembly/NACK; NOT protocol acceptance')
    return p


def error_code(exc):
    return getattr(exc, 'winerror', None) or getattr(exc, 'errno', None)


class ReceiverRuntime:
    def __init__(self, args, *, clock=time.monotonic, socket_factory=socket.socket,
                 select_fn=select.select):
        self.args, self.clock = args, clock
        self.socket_factory, self.select_fn = socket_factory, select_fn
        self.sock = None
        self.last_frame = None
        self.last_warning = {}
        self.trace = deque(maxlen=args.trace_limit)
        self.trace_records = 0
        self.model = ReceiverModel(self.complete)
        self.started = self.clock()
        self.started_utc = datetime.now(timezone.utc).isoformat()
        self.info = provenance()
        self.metrics = dict(recv_10054=0, send_10054=0, recv_10040=0, send_10040=0,
                            recv_would_block=0, send_would_block=0, nack_send_attempts=0,
                            nack_send_accepted=0, nack_send_failed=0, oversized_datagrams=0,
                            socket_datagrams=0, socket_bytes=0, drain_datagrams=0,
                            ingest_calls=0, ingest_total_us=0, ingest_max_us=0,
                            poll_calls=0, poll_total_us=0, poll_max_us=0,
                            loop_gap_max_us=0, batch_limit_hits=0,
                            receive_total_us=0, receive_max_us=0,
                            select_total_us=0, select_max_us=0,
                            nack_send_total_us=0, nack_send_max_us=0,
                            checkpoint_attempts=0, checkpoint_successes=0, checkpoint_failures=0)
        self.last_checkpoint_error = None
        self.last_checkpoint_success = None
        self.connreset = dict(status='not_attempted', platform=sys.platform, implementation=None)
        self.cleanup_errors = []
        self.exception = None
        self.exit_reason = 'startup'
        self.socket_buffers = {}

    def event(self, kind, **details):
        if self.args.trace_nacks:
            self.trace_records += 1
            self.trace.append(dict(kind=kind, local_monotonic_s=self.clock(), **details))

    def warn(self, kind, exc):
        now = self.clock()
        if now - self.last_warning.get(kind, float('-inf')) >= 1.0:
            print(f'WARNING {kind}: {exc!r}; continuing (see counters)', file=sys.stderr, flush=True)
            self.last_warning[kind] = now
        self.event(kind, error=repr(exc))

    def recoverable(self, direction, exc):
        code = error_code(exc)
        if code == 10054:
            self.metrics[direction + '_10054'] += 1
            self.warn(direction + '_10054', exc)
            return True
        if code in (10040, errno.EMSGSIZE):
            self.metrics[direction + '_10040'] += 1
            self.warn(direction + '_10040', exc)
            return True
        if isinstance(exc, BlockingIOError) or code in (10035, errno.EAGAIN, errno.EWOULDBLOCK):
            self.metrics[direction + '_would_block'] += 1
            return True
        return False

    def complete(self, d, body, modules):
        if self.args.output:
            self.last_frame = body

    def feedback(self):
        begin = self.clock()
        packets = self.model.poll(begin)
        elapsed = (self.clock() - begin) * 1e6
        self.metrics['poll_calls'] += 1
        self.metrics['poll_total_us'] += elapsed
        self.metrics['poll_max_us'] = max(self.metrics['poll_max_us'], elapsed)
        for packet in packets:
            # Budget is consumed at generation even when sendto fails; never restart
            # or implicitly retry requests outside the model's per-fragment timers.
            self.event('nack_generated', logic_id=struct.unpack_from('<I', packet, 8)[0],
                       ranges=[list(struct.unpack_from('<HH', packet, i)) for i in range(16, len(packet)-4, 4)])
            for _ in range(2 if self.args.duplicate_nack else 1):
                self.metrics['nack_send_attempts'] += 1
                send_begin = self.clock()
                try:
                    sent = self.sock.sendto(packet, (self.args.cmd_ip, self.args.cmd_port))
                    if sent != len(packet):
                        raise OSError('partial UDP send')
                except OSError as exc:
                    self.metrics['nack_send_failed'] += 1
                    if not self.recoverable('send', exc):
                        raise
                else:
                    self.metrics['nack_send_accepted'] += 1
                    self.event('nack_send_accepted', bytes=sent)
                finally:
                    elapsed = (self.clock() - send_begin) * 1e6
                    self.metrics['nack_send_total_us'] += elapsed
                    self.metrics['nack_send_max_us'] = max(self.metrics['nack_send_max_us'], elapsed)

    def progress(self):
        return dict(run_id=self.args.run_id, local_monotonic_s=self.clock(),
                    completed=self.model.stats['completed'], expired=self.model.stats['expired'],
                    active_frames=len(self.model.frames), datagrams=self.metrics['socket_datagrams'],
                    nack_packets=self.model.stats['nack_packets'])

    def loop(self):
        self.sock = self.socket_factory(socket.AF_INET, socket.SOCK_DGRAM)
        self.connreset = configure_udp_connreset(self.sock)
        if self.connreset['status'] == 'failed':
            print('WARNING SIO_UDP_CONNRESET: ' + json.dumps(self.connreset), file=sys.stderr, flush=True)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        self.sock.bind((self.args.bind, self.args.port))
        self.sock.setblocking(False)
        self.socket_buffers = dict(receive=self.sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF),
                                   send=self.sock.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF))
        if self.args.ready_file:
            atomic_json(self.args.ready_file, dict(run_id=self.args.run_id, pid=self.info['pid'],
                                                  endpoint=[self.args.bind, self.args.port]))
        if not self.args.quiet:
            print(f'LISTEN {self.args.bind}:{self.args.port}; NACK -> {self.args.cmd_ip}:{self.args.cmd_port}; DATA/NACK only', flush=True)
        start = previous = self.clock()
        next_summary = start + self.args.summary_every
        next_checkpoint = start
        while not self.args.seconds or self.clock() - start < self.args.seconds:
            now = self.clock()
            self.metrics['loop_gap_max_us'] = max(self.metrics['loop_gap_max_us'], (now - previous) * 1e6)
            previous = now
            select_begin = self.clock()
            readable, _, _ = self.select_fn([self.sock], [], [], 0.001)
            elapsed = (self.clock() - select_begin) * 1e6
            self.metrics['select_total_us'] += elapsed
            self.metrics['select_max_us'] = max(self.metrics['select_max_us'], elapsed)
            if readable:
                batch_begin = self.clock()
                # Bound both packet count and elapsed work. A busy socket cannot
                # indefinitely starve 2ms/10ms timers; gaps are requested this round.
                for index in range(64):
                    receive_begin = self.clock()
                    try:
                        packet, _ = self.sock.recvfrom(1473)
                    except OSError as exc:
                        if not self.recoverable('recv', exc):
                            raise
                        break  # Yield to timers; persistent errors cannot busy-loop.
                    finally:
                        elapsed = (self.clock() - receive_begin) * 1e6
                        self.metrics['receive_total_us'] += elapsed
                        self.metrics['receive_max_us'] = max(self.metrics['receive_max_us'], elapsed)
                    self.metrics['socket_datagrams'] += 1
                    self.metrics['socket_bytes'] += len(packet)
                    if len(packet) > 1472:
                        self.metrics['oversized_datagrams'] += 1
                    if self.args.drain:
                        self.metrics['drain_datagrams'] += 1
                    else:
                        begin = self.clock()
                        self.model.ingest(packet, begin)
                        elapsed = (self.clock() - begin) * 1e6
                        self.metrics['ingest_calls'] += 1
                        self.metrics['ingest_total_us'] += elapsed
                        self.metrics['ingest_max_us'] = max(self.metrics['ingest_max_us'], elapsed)
                    if self.clock() - batch_begin >= .000250:
                        self.metrics['batch_limit_hits'] += 1
                        break
                else:
                    self.metrics['batch_limit_hits'] += 1
            if not self.args.drain:
                self.feedback()
            now = self.clock()
            if self.args.status_file and now >= next_checkpoint:
                self.metrics['checkpoint_attempts'] += 1
                try:
                    atomic_json(self.args.status_file, self.progress())
                except OSError as exc:
                    # An optional observer checkpoint must never terminate DATA
                    # processing. Windows readers may temporarily deny replacement.
                    # No immediate retry: preserve the ordinary checkpoint cadence.
                    self.metrics['checkpoint_failures'] += 1
                    self.last_checkpoint_error = dict(local_monotonic_s=self.clock(),
                        path=self.args.status_file, code=error_code(exc), error=repr(exc))
                    self.warn('checkpoint', exc)
                else:
                    self.metrics['checkpoint_successes'] += 1
                    self.last_checkpoint_success = self.clock()
                next_checkpoint = now + self.args.checkpoint_every
            if not self.args.quiet and now >= next_summary:
                print('SUMMARY ' + json.dumps(self.progress()), flush=True)
                next_summary = now + self.args.summary_every
        self.exit_reason = 'time_limit'

    def report(self):
        now = self.clock()
        report = dict(self.model.stats)
        if hasattr(self.model, 'report_details'):
            report.update(self.model.report_details(now))
        if 'receiver_timing' in report:
            report['receiver_timing']['runtime'] = dict(
                mode='drain: no protocol timing observations' if self.args.drain else 'diagnostic reassembly/NACK',
                duplicate_nack_injection=self.args.duplicate_nack,
                observation_points='ingest/poll processing timestamps; request generation precedes socket sendto',
                poll_cadence='after at most 64 datagrams or a checked 250us batch; select timeout 1ms; OS scheduling can exceed these budgets')
        report.update(self.metrics)
        report.update(report_schema_version=2, run_id=self.args.run_id, environment=self.info,
                      active_frames=len(self.model.frames), diagnostic_only=True,
                      receiver_rendering_implemented=False, configuration=vars(self.args),
                      endpoint=dict(bind=[self.args.bind, self.args.port], nack=[self.args.cmd_ip, self.args.cmd_port]),
                      network_path='loopback' if self.args.bind.startswith('127.') and self.args.cmd_ip.startswith('127.') else 'unspecified physical route',
                      socket_buffers=self.socket_buffers, sio_udp_connreset=self.connreset,
                      started_utc=self.started_utc, ended_utc=datetime.now(timezone.utc).isoformat(),
                      elapsed_s=now-self.started, exit_reason=self.exit_reason,
                      exception=self.exception, cleanup_errors=self.cleanup_errors,
                      last_checkpoint_error=self.last_checkpoint_error,
                      last_checkpoint_success_monotonic_s=self.last_checkpoint_success,
                      checkpoint_policy='optional OSError is counted and skipped until next regular cadence; final report failure remains fatal',
                      trace_records=self.trace_records, trace_retained=len(self.trace),
                      trace_dropped=max(0, self.trace_records-len(self.trace)),
                      nack_budget_policy='consumed at generation; failed sendto consumes request; no unbounded local retries',
                      batch_limits=dict(packets=64, processing_budget_us=250,
                                        note='budget checked after each datagram; individual decode/complete may exceed it'),
                      mode='drain: NOT protocol acceptance' if self.args.drain else 'full protocol diagnostic receiver',
                      timing_note='User-space monotonic processing times, not kernel/NIC arrivals. Python is not hard real-time.')
        report['acceptance_table'] = receiver_acceptance(report)
        return report

    def run(self):
        status = 0
        try:
            self.loop()
        except KeyboardInterrupt:
            self.exit_reason = 'keyboard_interrupt'
        except Exception:
            status = 1
            self.exit_reason = 'exception'
            self.exception = traceback.format_exc()
            print(self.exception, file=sys.stderr, flush=True)
        finally:
            if self.sock is not None:
                try:
                    self.sock.close()
                except Exception:
                    self.cleanup_errors.append('socket.close: ' + traceback.format_exc())
                    status = 1
            # Each cleanup is isolated: even output/trace/close failure must try JSON.
            try:
                if self.args.output and self.last_frame is not None:
                    Path(self.args.output).write_bytes(self.last_frame)
            except Exception:
                self.cleanup_errors.append('frame output: ' + traceback.format_exc())
                status = 1
            try:
                if self.args.trace_nacks:
                    with Path(self.args.trace_nacks).open('w', encoding='utf-8') as trace:
                        for event in self.trace:
                            trace.write(json.dumps(event) + '\n')
            except Exception:
                self.cleanup_errors.append('trace: ' + traceback.format_exc())
                status = 1
            if self.cleanup_errors:
                print('\n'.join(self.cleanup_errors), file=sys.stderr, flush=True)
            try:
                report = self.report()
            except Exception:
                # If diagnostic assembly itself fails, preserve a minimal report
                # instead of losing the original runtime failure and all counters.
                status = 1
                generation_error = traceback.format_exc()
                print('ERROR assembling receiver report:\n' + generation_error, file=sys.stderr, flush=True)
                report = dict(report_schema_version=2, run_id=self.args.run_id,
                              exit_reason=self.exit_reason, exception=self.exception,
                              report_generation_error=generation_error,
                              cleanup_errors=self.cleanup_errors, environment=self.info,
                              sio_udp_connreset=self.connreset, runtime_metrics=dict(self.metrics))
            try:
                report['exit_code'] = status
                atomic_json(self.args.report, report)
            except Exception:
                print('ERROR saving receiver report to ' + self.args.report + ':\n' + traceback.format_exc(), file=sys.stderr, flush=True)
                status = 1
        return status


def interrupt_handler(signum, frame):
    # Windows process-group CTRL_BREAK_EVENT maps to SIGBREAK, not SIGINT.
    # Reuse the bounded cleanup/report path used by Ctrl+C.
    raise KeyboardInterrupt()


def main(argv=None):
    p = parser()
    args = p.parse_args(argv)
    if any(not 1 <= port <= 65535 for port in (args.port, args.cmd_port)):
        p.error('ports must be 1..65535')
    if (not all(math.isfinite(v) for v in (args.seconds, args.summary_every, args.checkpoint_every)) or
            args.seconds < 0 or args.summary_every <= 0 or args.checkpoint_every <= 0 or args.trace_limit < 0):
        p.error('seconds/trace-limit must be nonnegative; summary/checkpoint intervals must be positive')
    runtime = ReceiverRuntime(args)
    previous = None
    if sys.platform == 'win32':
        previous = signal.signal(signal.SIGBREAK, interrupt_handler)
    try:
        return runtime.run()
    finally:
        if previous is not None:
            signal.signal(signal.SIGBREAK, previous)


if __name__ == '__main__':
    raise SystemExit(main())
