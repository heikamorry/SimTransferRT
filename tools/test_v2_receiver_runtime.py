"""Deterministic socket-scheduling tests; no sockets, sleeps, or processes.

The fake clock represents user-space processing time. These tests verify the
bounded read-before-timeout contract, not kernel arrival timestamps or Windows
scheduling performance.
"""
from collections import deque
import unittest

from v2_protocol import STEP, Nack, decode_nack, encode_data, encode_status
from v2_receiver import Receiver, ReceiverRuntime


SOURCE = ('127.0.0.1', 5500)
OTHER_SOURCE = ('127.0.0.2', 5500)


class Clock:
    def __init__(self, now=100):
        self.now = now

    def __call__(self):
        return self.now


class FakeSocket:
    def __init__(self, clock, packets=(), *, factory=None, receive_us=0,
                 after_receive=None, send_error=None):
        self.clock = clock
        self.packets = deque(packets)
        self.factory = factory
        self.receive_us = receive_us
        self.after_receive = after_receive
        self.send_error = send_error
        self.receive_calls = 0
        self.send_attempts = []
        self.sent = []

    def readable(self):
        return bool(self.packets) or self.factory is not None

    def recvfrom(self, size):
        self.receive_calls += 1
        if self.packets:
            event = self.packets.popleft()
        elif self.factory is not None:
            event = self.factory(self.receive_calls)
        else:
            raise BlockingIOError(10035, 'fake socket queue empty')
        self.clock.now += self.receive_us
        if self.after_receive:
            self.after_receive(self.receive_calls)
        if isinstance(event, BaseException):
            raise event
        if isinstance(event, tuple):
            return event
        return event, SOURCE

    def sendto(self, packet, endpoint):
        event = (bytes(packet), endpoint, self.clock(), self.receive_calls)
        self.send_attempts.append(event)
        if self.send_error is not None:
            raise self.send_error
        self.sent.append(event)
        return len(packet)


class V2ReceiverRuntimeTests(unittest.TestCase):
    def make_runtime(self, packets=(), *, now=100, factory=None, receive_us=0,
                     wait_us=0, stop_requested=lambda: False,
                     after_receive=None, send_error=None, pinned=True,
                     drop_first=(), drop_always=()):
        clock = Clock(now)
        receiver = Receiver(verify_bx=False, clock=clock)
        sock = FakeSocket(clock, packets, factory=factory,
                          receive_us=receive_us, after_receive=after_receive,
                          send_error=send_error)
        waits = []

        def wait_readable(timeout):
            waits.append(timeout)
            if timeout > 0:
                clock.now += wait_us
            return sock.readable()

        runtime = ReceiverRuntime(sock, receiver, clock=clock,
                                  wait_readable=wait_readable,
                                  stop_requested=stop_requested,
                                  source_ip=SOURCE[0], source_port=SOURCE[1],
                                  drop_first=drop_first,
                                  drop_always=drop_always)
        if pinned:
            runtime.peer = SOURCE
        return runtime, receiver, sock, clock, waits

    @staticmethod
    def fragments(module=2, frame_id=1, count=3):
        payload = b'x' * (STEP * (count - 1) + 1)
        return [encode_data(module, frame_id, 10, payload, i)
                for i in range(count)]

    @staticmethod
    def requests(sock):
        return [decode_nack(event[0]) for event in sock.sent]

    def test_queued_remainder_completes_before_overdue_tail_poll(self):
        packets = self.fragments()
        runtime, receiver, sock, clock, _ = self.make_runtime(packets[1:])
        receiver.receive(packets[0], 100)
        clock.now = 4000
        runtime.service_once()
        self.assertEqual(receiver.stats[2]['completed'], 1)
        self.assertEqual(receiver.stats[2]['consumed'], 1)
        self.assertEqual(receiver.stats[2]['nack_rounds'], 0)
        self.assertEqual(receiver.stats[2]['tail_triggers'], 0)
        self.assertFalse(sock.sent)

    def test_queued_duplicate_does_not_change_completion_or_make_nack(self):
        packets = self.fragments()
        runtime, receiver, sock, clock, _ = self.make_runtime(
            [packets[1], packets[1], packets[2]])
        receiver.receive(packets[0], 100)
        clock.now = 4000
        runtime.service_once()
        self.assertEqual(receiver.stats[2]['duplicates'], 1)
        self.assertEqual(receiver.stats[2]['completed'], 1)
        self.assertEqual(receiver.stats[2]['consumed'], 1)
        self.assertFalse(sock.sent)

    def test_empty_queue_keeps_two_ms_ten_ms_and_three_round_limits(self):
        runtime, receiver, sock, clock, _ = self.make_runtime()
        receiver.receive(self.fragments(count=2)[0], 100)
        for now, expected_rounds, expected_expired in (
                (2099, 0, 0), (2100, 1, 0),
                (12099, 1, 0), (12100, 2, 0),
                (22099, 2, 0), (22100, 3, 0),
                (32099, 3, 0), (32100, 3, 1)):
            with self.subTest(now=now):
                clock.now = now
                runtime.service_once()
                self.assertEqual(receiver.stats[2]['nack_rounds'], expected_rounds)
                self.assertEqual(receiver.stats[2]['expired'], expected_expired)
        self.assertEqual(self.requests(sock), [Nack(2, 1, (1,))] * 3)
        self.assertEqual(receiver.retry_intervals[2].minimum, 10000)
        self.assertEqual(receiver.retry_intervals[2].maximum, 10000)

    def test_idle_wait_lateness_is_reported_without_moving_original_due(self):
        runtime, receiver, sock, clock, waits = self.make_runtime(wait_us=500)
        receiver.receive(self.fragments(count=2)[0], 100)
        clock.now = 1900
        runtime.service_once()
        self.assertEqual(sum(timeout > 0 for timeout in waits), 1)
        self.assertEqual(self.requests(sock), [Nack(2, 1, (1,))])
        event = receiver.retry_events[-1]
        self.assertEqual((event['due_us'], event['actual_us']), (2100, 2400))
        self.assertEqual(receiver.request_lateness[2].maximum, 300)

    def test_queued_final_fragment_beats_third_round_expiry(self):
        packets = self.fragments(count=2)
        runtime, receiver, sock, clock, _ = self.make_runtime()
        receiver.receive(packets[0], 100)
        for now in (2100, 12100, 22100):
            clock.now = now
            runtime.service_once()
        sock.packets.append(packets[1])
        clock.now = 32100
        runtime.service_once()
        self.assertEqual(receiver.stats[2]['nack_rounds'], 3)
        self.assertEqual(receiver.stats[2]['expired'], 0)
        self.assertEqual(receiver.stats[2]['completed'], 1)
        self.assertEqual(receiver.stats[2]['consumed'], 1)
        self.assertFalse(receiver.active[2])
        self.assertEqual(len(sock.sent), 3)

    def test_partial_recovery_does_not_postpone_existing_retry_due(self):
        packets = self.fragments()
        runtime, receiver, sock, clock, _ = self.make_runtime()
        receiver.receive(packets[0], 100)
        clock.now = 2100
        runtime.service_once()
        sock.packets.append(packets[1])
        clock.now = 12150
        runtime.service_once()
        self.assertEqual(self.requests(sock),
                         [Nack(2, 1, (1, 2)), Nack(2, 1, (2,))])
        event = receiver.retry_events[-1]
        self.assertEqual((event['round'], event['due_us'], event['actual_us']),
                         (2, 12100, 12150))
        self.assertEqual(receiver.request_lateness[2].maximum, 50)
        self.assertEqual(receiver.retry_intervals[2].maximum, 10050)

    def test_immediate_gap_request_precedes_next_queued_packet(self):
        packets = self.fragments()
        runtime, receiver, sock, _, _ = self.make_runtime(
            [packets[1], packets[0], packets[2]])
        runtime.service_once()
        self.assertEqual(self.requests(sock), [Nack(2, 1, (0,))])
        self.assertEqual(sock.sent[0][3], 1)
        self.assertEqual(receiver.stats[2]['gap_triggers'], 1)
        self.assertEqual(receiver.stats[2]['completed'], 1)
        self.assertEqual(receiver.stats[2]['nack_rounds'], 1)

    def test_immediate_mode_never_creates_other_module_tail(self):
        receiver = Receiver(verify_bx=False)
        receiver.receive(self.fragments(2, count=2)[0], 100)
        receiver.receive(self.fragments(1, count=2)[1], 3000)
        immediate = receiver.poll(3000, immediate_module=1)
        self.assertEqual([decode_nack(p) for p in immediate],
                         [Nack(1, 1, (0,))])
        self.assertEqual(receiver.stats[2]['tail_triggers'], 0)
        self.assertEqual(receiver.stats[2]['nack_rounds'], 0)
        self.assertEqual([decode_nack(p) for p in receiver.poll(3000)],
                         [Nack(2, 1, (1,))])

    def test_immediate_mode_does_not_run_retry_or_expiry(self):
        receiver = Receiver(verify_bx=False)
        receiver.receive(self.fragments(count=2)[0], 100)
        receiver.poll(2100)
        for now in (12100, 22100):
            self.assertFalse(receiver.poll(now, immediate_module=2))
            self.assertEqual(len(receiver.poll(now)), 1)
        self.assertFalse(receiver.poll(32100, immediate_module=2))
        self.assertEqual(receiver.stats[2]['expired'], 0)
        receiver.poll(32100)
        self.assertEqual(receiver.stats[2]['expired'], 1)

    def test_other_module_or_status_does_not_reset_discovered_gap_due(self):
        for packet in (encode_data(1, 1, 10, b'ndi', 0),
                       encode_status(1, 10, b'state')):
            with self.subTest(kind=packet[6]):
                runtime, receiver, sock, clock, _ = self.make_runtime([packet])
                receiver.receive(self.fragments()[2], 100)
                clock.now = 400
                runtime.service_once()
                self.assertEqual(self.requests(sock), [Nack(2, 1, (0, 1))])
                self.assertEqual(receiver.retry_events[-1]['due_us'], 100)
                self.assertEqual(receiver.request_lateness[2].maximum, 300)

    def test_all_receive_branches_obey_packet_and_time_budgets(self):
        def valid(number):
            return encode_data(1, number, 10, b'ndi', 0)

        def corrupt(number):
            packet = bytearray(valid(number))
            packet[-1] ^= 1
            return bytes(packet)

        streams = (
            ('valid', valid, {}),
            ('malformed', lambda _: b'bad', {}),
            ('crc_invalid', corrupt, {}),
            ('source_rejected', lambda n: (valid(n), OTHER_SOURCE), {}),
            ('drop_first', valid, {'drop_first': {(1, 0)}}),
            ('drop_always', valid, {'drop_always': {(1, 0)}}),
        )
        for name, factory, options in streams:
            for receive_us, maximum_packets in ((0, 64), (125, 4)):
                with self.subTest(stream=name, receive_us=receive_us):
                    runtime, receiver, sock, clock, _ = self.make_runtime(
                        factory=factory, receive_us=receive_us, **options)
                    receiver.receive(self.fragments(count=2)[0], 100)
                    clock.now = 2100
                    runtime.service_once()
                    self.assertEqual(sock.receive_calls, maximum_packets)
                    self.assertEqual(self.requests(sock), [Nack(2, 1, (1,))])
                    self.assertEqual(sock.sent[0][3], maximum_packets)
                    self.assertEqual(receiver.stats[2]['nack_rounds'], 1)
                    self.assertEqual(receiver.retry_events[-1]['due_us'], 2100)
                    self.assertEqual(receiver.request_lateness[2].maximum,
                                     receive_us * maximum_packets)
                    if name in ('malformed', 'crc_invalid'):
                        self.assertEqual(receiver.invalid_packets, maximum_packets)
                    elif name == 'source_rejected':
                        self.assertEqual(runtime.source_rejected, maximum_packets)
                    elif name.startswith('drop_'):
                        self.assertEqual(runtime.drop_count, maximum_packets)

    def test_invalid_or_foreign_packets_do_not_pin_endpoint(self):
        valid = encode_data(2, 1, 10, b'ep', 0)
        runtime, receiver, sock, _, _ = self.make_runtime(
            [b'bad', (valid, OTHER_SOURCE)], pinned=False)
        runtime.service_once()
        self.assertIsNone(runtime.peer)
        self.assertEqual(runtime.source_rejected, 1)
        self.assertEqual(receiver.invalid_packets, 1)
        self.assertFalse(sock.sent)
        sock.packets.append(valid)
        runtime.service_once()
        self.assertEqual(runtime.peer, SOURCE)
        self.assertEqual(receiver.stats[2]['consumed'], 1)

    def test_ready_then_would_block_still_services_due_timer(self):
        runtime, receiver, sock, clock, _ = self.make_runtime(
            [BlockingIOError(10035, 'readiness changed')])
        receiver.receive(self.fragments(count=2)[0], 100)
        clock.now = 2100
        runtime.service_once()
        self.assertEqual(self.requests(sock), [Nack(2, 1, (1,))])
        self.assertEqual(runtime.receive_errors, 0)

    def test_windows_reset_or_refused_does_not_starve_due_timer(self):
        for error in (ConnectionResetError(10054, 'fake reset'),
                      ConnectionRefusedError(10061, 'fake refused')):
            with self.subTest(error=type(error).__name__):
                runtime, receiver, sock, clock, _ = self.make_runtime([error])
                receiver.receive(self.fragments(count=2)[0], 100)
                clock.now = 2100
                runtime.service_once()
                self.assertEqual(runtime.receive_errors, 1)
                self.assertEqual(self.requests(sock), [Nack(2, 1, (1,))])

    def test_failed_feedback_consumes_round_without_local_retry_loop(self):
        runtime, receiver, sock, clock, _ = self.make_runtime(
            send_error=OSError(10055, 'fake send buffer full'))
        receiver.receive(self.fragments(count=2)[0], 100)
        for now, expected_attempts in ((2100, 1), (2101, 1), (12100, 2)):
            clock.now = now
            runtime.service_once()
            self.assertEqual(len(sock.send_attempts), expected_attempts)
            self.assertEqual(runtime.send_errors, expected_attempts)
            self.assertEqual(receiver.stats[2]['nack_rounds'], expected_attempts)
        self.assertFalse(sock.sent)

    def test_stop_before_cycle_does_not_wait_read_or_send(self):
        runtime, receiver, sock, clock, waits = self.make_runtime(
            factory=lambda _: b'bad', stop_requested=lambda: True)
        receiver.receive(self.fragments(count=2)[0], 100)
        clock.now = 2100
        runtime.service_once()
        self.assertFalse(waits)
        self.assertEqual(sock.receive_calls, 0)
        self.assertFalse(sock.send_attempts)
        self.assertEqual(receiver.stats[2]['nack_rounds'], 0)

    def test_stop_during_continuous_invalid_stream_is_bounded(self):
        stopped = [False]

        def after_receive(count):
            if count == 3:
                stopped[0] = True

        runtime, receiver, sock, clock, _ = self.make_runtime(
            factory=lambda _: b'bad', stop_requested=lambda: stopped[0],
            after_receive=after_receive)
        receiver.receive(self.fragments(count=2)[0], 100)
        clock.now = 2100
        runtime.service_once()
        self.assertEqual(sock.receive_calls, 3)
        self.assertFalse(sock.send_attempts)
        self.assertEqual(receiver.stats[2]['nack_rounds'], 0)

    def test_stop_during_split_feedback_cancels_remainder_and_accounts_all(self):
        stopped = [False]
        runtime, receiver, sock, clock, _ = self.make_runtime(
            stop_requested=lambda: stopped[0])
        # 1,450 fragments, with 1,449 missing: 724 + 724 + 1 in one round.
        payload = b'x' * (STEP * 1449 + 1)
        receiver.receive(encode_data(2, 1, 10, payload, 0), 100)
        original_send = sock.sendto

        def stop_after_first_send(packet, endpoint):
            result = original_send(packet, endpoint)
            stopped[0] = True
            return result

        sock.sendto = stop_after_first_send
        clock.now = 2100
        runtime.service_once()
        metrics = runtime.metrics
        self.assertEqual(metrics['feedback_generated_packets'], 3)
        self.assertEqual(metrics['feedback_send_attempts'], 1)
        self.assertEqual(metrics['feedback_sent_packets'], 1)
        self.assertEqual(metrics['feedback_cancelled_packets'], 2)
        self.assertEqual(runtime.send_errors, 0)
        self.assertEqual(metrics['feedback_generated_packets'],
                         metrics['feedback_send_attempts'] +
                         metrics['feedback_cancelled_packets'])
        self.assertEqual(metrics['feedback_send_attempts'],
                         metrics['feedback_sent_packets'] + runtime.send_errors)
        self.assertEqual(receiver.stats[2]['nack_rounds'], 1)
        self.assertEqual(receiver.stats[2]['nack_packets'], 3)
        self.assertEqual(receiver.stats[2]['requested_fragments'], 1449)
        self.assertEqual(len(self.requests(sock)[0].indices), 724)
        runtime.service_once()
        self.assertEqual(len(sock.send_attempts), 1)
        self.assertEqual(metrics['feedback_cancelled_packets'], 2)

    def test_batch_limit_services_timer_even_when_target_tail_remains_queued(self):
        packets = self.fragments(count=2)
        runtime, receiver, sock, clock, _ = self.make_runtime(
            [b'bad'] * 64 + [packets[1]])
        receiver.receive(packets[0], 100)
        clock.now = 4000
        runtime.service_once()
        self.assertEqual(sock.receive_calls, 64)
        self.assertEqual(receiver.invalid_packets, 64)
        self.assertEqual(len(sock.packets), 1)
        self.assertEqual(receiver.stats[2]['completed'], 0)
        self.assertEqual(self.requests(sock), [Nack(2, 1, (1,))])
        self.assertEqual(runtime.metrics['packet_limit_hits'], 1)
        self.assertEqual(runtime.metrics['timer_polls'], 1)
        self.assertEqual(runtime.metrics['feedback_polls_with_readable_socket'], 1)
        self.assertEqual(receiver.retry_events[-1]['due_us'], 2100)
        self.assertEqual(receiver.request_lateness[2].maximum, 1900)

        # A bounded drain cannot prove that all missing packets are absent from
        # the kernel queue. The next batch completes this frame without waiting
        # for the already-requested retransmission or hiding the prior NACK.
        runtime.service_once()
        self.assertEqual(receiver.stats[2]['completed'], 1)
        self.assertEqual(receiver.stats[2]['consumed'], 1)
        self.assertEqual(receiver.stats[2]['expired'], 0)
        self.assertEqual(receiver.stats[2]['nack_rounds'], 1)
        self.assertEqual(runtime.metrics['feedback_generated_packets'], 1)
        self.assertEqual(len(sock.sent), 1)


if __name__ == '__main__':
    unittest.main()
