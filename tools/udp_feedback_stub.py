#!/usr/bin/env python3
"""Diagnostic receiver for the uploaded DATA/NACK protocol. It never sends SUCCESS.
Usage: python tools/udp_feedback_stub.py --port 5650 --cmd-port 5500
"""
from __future__ import annotations
import argparse
import json
import select
import socket
import time
from pathlib import Path
from receiver_model import ReceiverModel

def main() -> int:
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
    args = p.parse_args()
    for port in (args.port, args.cmd_port):
        if not 1 <= port <= 65535:
            p.error('ports must be 1..65535')
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        s.bind((args.bind, args.port))
        s.setblocking(False)
        last_frame = [None]
        def complete(d, body, modules):
            last_frame[0] = body
            if not args.quiet and model.stats['completed'] % 50 == 1:
                print(f'LOCAL COMPLETE logic_id={d.logic_id} modules={list(modules)} bytes={len(body)} (no SUCCESS sent)', flush=True)
        model = ReceiverModel(complete)
        start = time.monotonic()
        print(f'LISTEN {args.bind}:{args.port}; NACK -> {args.cmd_ip}:{args.cmd_port}; DATA/NACK only', flush=True)
        try:
            while not args.seconds or time.monotonic() - start < args.seconds:
                readable, _, _ = select.select([s], [], [], 0.001)
                if readable:
                    for _ in range(256):
                        try:
                            packet, _ = s.recvfrom(1473)
                        except BlockingIOError:
                            break
                        model.ingest(packet, time.monotonic())
                        for nack in model.poll(time.monotonic()):
                            s.sendto(nack, (args.cmd_ip, args.cmd_port))
                            if args.duplicate_nack:
                                s.sendto(nack, (args.cmd_ip, args.cmd_port))
                for nack in model.poll(time.monotonic()):
                    s.sendto(nack, (args.cmd_ip, args.cmd_port))
                    if args.duplicate_nack:
                        s.sendto(nack, (args.cmd_ip, args.cmd_port))
        except KeyboardInterrupt:
            pass
        report = dict(model.stats)
        report.update(active_frames=len(model.frames), diagnostic_only=True,
                      receiver_rendering_implemented=False,
                      timing_note='Python select loop is a functional test tool, NOT a microsecond timing oracle')
        Path(args.report).write_text(json.dumps(report, indent=2), encoding='utf-8')
        if args.output and last_frame[0] is not None:
            Path(args.output).write_bytes(last_frame[0])
        print(json.dumps(report, indent=2), flush=True)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
