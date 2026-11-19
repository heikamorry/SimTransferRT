#!/usr/bin/env python3
"""Bounded Winsock ICMP experiment, separate from the sender/receiver protocol tests."""
import argparse
import select
import socket
import sys
import time
from rt_protocol import encode_nack
from runtime_support import atomic_json
from runtime_support import configure_udp_connreset


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--report', required=True)
    args = p.parse_args()
    if sys.platform != 'win32':
        atomic_json(args.report, dict(platform=sys.platform, status='not_applicable'))
        return 0
    results = []
    packet = encode_nack(1, [(2, 1)])
    for suppress in (False, True):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
            peer.bind(('127.0.0.1', 0))
            closed_port = peer.getsockname()[1]
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(('127.0.0.1', 0))
            sock.setblocking(False)
            configuration = (configure_udp_connreset(sock) if suppress else
                             dict(status='Windows default (not overridden)'))
            start = time.monotonic()
            sock.sendto(packet, ('127.0.0.1', closed_port))
            events = [dict(event='valid_NACK_sendto_closed_port_accepted',
                           local_monotonic_s=time.monotonic(), port=closed_port)]
            while time.monotonic() - start < .2:
                if select.select([sock], [], [], .01)[0]:
                    try:
                        data, _ = sock.recvfrom(1473)
                        events.append(dict(event='recv', local_monotonic_s=time.monotonic(), size=len(data)))
                    except OSError as exc:
                        events.append(dict(event='recv_error', local_monotonic_s=time.monotonic(),
                                           winerror=getattr(exc, 'winerror', None), errno=exc.errno, error=repr(exc)))
                        if getattr(exc, 'winerror', None) not in (10054, 10035):
                            raise
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
                peer.sendto(packet, sock.getsockname())
            readable = select.select([sock], [], [], .1)[0]
            alive = bool(readable and sock.recvfrom(1473)[0] == packet)
            results.append(dict(suppress_reset=suppress, configuration=configuration,
                                events=events, subsequent_datagram_received=alive))
    atomic_json(args.report, dict(platform=sys.platform, results=results,
                note='Actual Windows Winsock local experiment. No packet capture; no claim about original user stop ordering.'))
    return 0 if all(r['subsequent_datagram_received'] for r in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
