"""Virtual-clock regressions. No assertions here certify real Windows scheduling."""
import struct
import unittest
from receiver_model import FrameRanges, ReceiverModel
from rt_protocol import (STEP, ProtocolError, build_modules, crc32, decode_data,
                         decode_nack, encode_data, encode_nack, nack_packets)


class Clock:
    def __init__(self):
        self.now = 0.0
    def __call__(self):
        return self.now


def packets(logic=1, size=5 * STEP, ts=100):
    mask, body = build_modules({0: b'x' * (size - 5)})
    return [encode_data(logic, ts, mask, body, i)
            for i in range((size + STEP - 1) // STEP)]


def requested(wires):
    return [(logic, index) for wire in wires for logic, ranges in [decode_nack(wire)]
            for first, count in ranges for index in range(first, first + count)]


class ReceiverRegressionTests(unittest.TestCase):
    def test_complete_frame_no_nack_and_timers_cancelled(self):
        m = ReceiverModel()
        for i, wire in enumerate(packets()):
            now = i * 0.00002
            m.ingest(wire, now)
            self.assertEqual(m.poll(now), [])
        self.assertEqual(m.poll(10), [])
        self.assertEqual(m.stats['completed'], 1)
        self.assertEqual(len(m.frames), 0)
        self.assertEqual(len(m._timers), 0)

    def test_fragment_two_exact_three_requests_then_expiry(self):
        clock = Clock()
        m = ReceiverModel(clock=clock)
        ps = packets()
        for i, wire in enumerate(ps):
            if i != 2:
                m.ingest(wire)
        for now in (0, 0.010, 0.020):
            clock.now = now
            self.assertEqual(requested(m.poll()), [(1, 2)])
            self.assertEqual(m.poll(), [])
            m.ingest(ps[3])  # Re-registering a gap/duplicate must not reset budget.
        clock.now = 0.029999
        self.assertEqual(m.poll(), [])
        clock.now = 0.030
        self.assertEqual(m.poll(), [])
        self.assertEqual(m.stats['expired'], 1)
        m.ingest(ps[2])
        self.assertEqual(m.poll(), [])
        self.assertEqual(m.stats['requested_fragments'], 3)
        self.assertEqual(m.stats['completed'], 0)

    def test_packet_after_final_deadline_without_poll_cannot_revive(self):
        m = ReceiverModel()
        ps = packets()
        for i, wire in enumerate(ps):
            if i != 2:
                m.ingest(wire, 0)
        for now in (0, .010, .020):
            m.poll(now)
        m.ingest(ps[2], .030)
        self.assertEqual(m.stats['expired'], 1)
        self.assertEqual(m.stats['completed'], 0)

    def test_missing_fragments_have_independent_clocks(self):
        m = ReceiverModel()
        ps = packets(size=6 * STEP)
        for index in (0, 2):
            m.ingest(ps[index], 0)
        self.assertEqual(requested(m.poll(0)), [(1, 1)])
        m.ingest(ps[4], .001)
        self.assertEqual(requested(m.poll(.001)), [(1, 3)])
        m.ingest(ps[5], .0015)
        self.assertEqual(m.poll(.009999), [])
        self.assertEqual(requested(m.poll(.010)), [(1, 1)])
        self.assertEqual(requested(m.poll(.011)), [(1, 3)])
        self.assertEqual(requested(m.poll(.020)), [(1, 1)])
        m.ingest(ps[1], .0205)
        self.assertEqual(requested(m.poll(.021)), [(1, 3)])
        m.ingest(ps[3], .029)
        self.assertEqual(m.stats['completed'], 1)
        self.assertEqual(m.poll(.050), [])

    def test_original_without_retry_flag_recovers_and_cancels_expiry(self):
        m = ReceiverModel()
        ps = packets()
        for i, wire in enumerate(ps):
            if i != 2:
                m.ingest(wire, 0)
        for now in (0, .010, .020):
            m.poll(now)
        m.ingest(ps[2], .029)
        self.assertEqual(m.stats['recovered'], 1)
        self.assertEqual(m.stats['retry_flag'], 0)
        self.assertEqual(m.stats['completed'], 1)
        self.assertEqual(m.poll(.100), [])

    def test_lost_nack_and_lost_repair_do_not_create_fourth_request(self):
        m = ReceiverModel()
        for wire in packets()[3:]:
            m.ingest(wire, 0)
        total = []
        for now in (0, .010, .020, .030, .050, 100):
            total.extend(requested(m.poll(now)))  # Simulate dropping every NACK/repair.
        self.assertEqual(len(total), 9)
        self.assertEqual(m.stats['max_requests_for_fragment'], 3)
        self.assertEqual(m.stats['expired'], 1)

    def test_nonzero_first_fragment_reverse_order_is_valid(self):
        m = ReceiverModel()
        for wire in reversed(packets()):
            m.ingest(wire, 0)
        self.assertEqual(m.stats['completed'], 1)
        self.assertEqual(m.stats['metadata_failures'], 0)

    def test_duplicate_does_not_postpone_tail_or_register_again(self):
        m = ReceiverModel()
        ps = packets()
        m.ingest(ps[0], 0)
        for now in (.0005, .001, .0019):
            m.ingest(ps[0], now)
            self.assertEqual(m.poll(now), [])
        self.assertEqual(len(requested(m.poll(.002))), 4)
        for i in range(100):
            m.ingest(ps[0], .003)
            self.assertEqual(m.poll(.003), [])
        self.assertEqual(m.stats['tail_registrations'], 1)
        self.assertEqual(len(requested(m.poll(.012))), 4)

    def test_more_than_363_ranges_is_one_budget_per_fragment(self):
        m = ReceiverModel()
        ps = packets(size=2097152)  # 2 MiB FORMAT geometry, not throughput acceptance.
        for i, wire in enumerate(ps):
            if i % 2 or i == len(ps) - 1:
                m.ingest(wire, 0)
        for now in (0, .010, .020):
            wires = m.poll(now)
            self.assertEqual(len(wires), 3)
            ids = requested(wires)
            self.assertEqual(len(ids), 730)
            self.assertEqual(len(set(ids)), 730)
        self.assertEqual(m.poll(.030), [])
        self.assertEqual(m.stats['requested_fragments'], 2190)

    def test_completed_history_eviction_never_reactivates(self):
        m = ReceiverModel(replay_window=256)
        ps = packets(1)
        for wire in ps:
            m.ingest(wire, 0)
        for logic in range(2, 1000):
            m.ingest(packets(logic, 10)[0], 0)
        self.assertNotIn(1, m.terminal)
        for wire in ps:
            m.ingest(wire, .001)
        self.assertNotIn(1, m.frames)
        self.assertEqual(m.stats['completed'], 999)
        self.assertEqual(m.poll(1), [])
        self.assertLessEqual(len(m._timers), 128)

    def test_terminal_status_eviction_inside_replay_bitmap(self):
        m = ReceiverModel()
        ps = packets(1)
        for wire in ps:
            m.ingest(wire, 0)
        for logic in range(2, 300):
            m.ingest(packets(logic, 10)[0], 0)
        self.assertNotIn(1, m.terminal)
        self.assertLess(299, m.replay_window)
        m.ingest(ps[0], .001)
        self.assertNotIn(1, m.frames)
        self.assertEqual(m.stats['stale_window_rejections'], 0)
        self.assertEqual(m.stats['late'], 1)
        self.assertEqual(m.poll(1), [])

    def test_expired_history_eviction_never_reactivates(self):
        m = ReceiverModel(replay_window=256)
        ps = packets(1)
        m.ingest(ps[3], 0)
        for now in (0, .010, .020, .030):
            m.poll(now)
        for logic in range(2, 1000):
            m.ingest(packets(logic, 10)[0], .030)
        for wire in ps:
            m.ingest(wire, .100)
        self.assertEqual(m.poll(1), [])
        self.assertEqual(m.stats['expired'], 1)
        self.assertEqual(m.stats['completed'], 998)

    def test_older_unseen_frame_and_active_beyond_replay_window_remain_valid(self):
        m = ReceiverModel(replay_window=256)
        ps = packets(1)
        m.ingest(ps[0], 0)
        for logic in range(3, 1000):
            m.ingest(packets(logic, 10)[0], 0)
        # Existing in-flight state remains eligible despite advancing replay fence.
        for wire in ps[1:]:
            m.ingest(wire, .001)
        self.assertEqual(m.stats['completed'], 998)
        # Unseen but within the replay window also remains eligible.
        m.ingest(packets(998, 10)[0], .001)  # Already terminal: duplicate.
        m.ingest(packets(1002, 10)[0], .001)
        m.ingest(packets(1001, 10)[0], .001)
        self.assertEqual(m.stats['completed'], 1000)

    def test_wrap_safe_terminal_and_inflight_comparison(self):
        m = ReceiverModel(replay_window=256)
        ps = packets(0xFFFFFFFE)
        m.ingest(ps[0], 0)
        for logic in (0xFFFFFFFF, 0, 1):
            m.ingest(packets(logic, 10)[0], 0)
        for wire in ps[1:]:
            m.ingest(wire, .001)
        self.assertEqual(m.stats['completed'], 4)
        for wire in ps:
            m.ingest(wire, .002)
        self.assertEqual(m.stats['completed'], 4)
        self.assertEqual(m.poll(1), [])
        self.assertEqual(m.report_details(1)['frame_manifest']['completed'],
                         [[0, 1], [0xFFFFFFFE, 0xFFFFFFFF]])

    def test_resource_eviction_distinct_from_protocol_expiry(self):
        m = ReceiverModel()
        for logic in range(1, 34):
            m.ingest(packets(logic)[0], 0)
        self.assertEqual(len(m.frames), 32)  # Not sender's three-slot cache rule.
        self.assertEqual(m.stats['active_evictions'], 1)
        self.assertEqual(m.stats['expired'], 0)
        m.ingest(packets(1)[1], 0)
        self.assertNotIn(1, m.frames)
        self.assertEqual(m.report_details(0)['frame_manifest']['resource_limit'], [[1, 1]])

    def test_metadata_conflict_terminal_even_after_correct_packet(self):
        m = ReceiverModel()
        ps = packets()
        m.ingest(ps[0], 0)
        m.ingest(packets(ts=101)[1], .001)
        for wire in ps:
            m.ingest(wire, .002)
        self.assertEqual(m.stats['metadata_failures'], 1)
        self.assertEqual(m.stats['completed'], 0)
        self.assertEqual(m.poll(1), [])

    def test_pause_backlog_is_finite_and_does_not_catch_up_retries(self):
        m = ReceiverModel()
        m.ingest(packets()[3], 0)
        self.assertEqual(len(requested(m.poll(0))), 3)
        delayed = requested(m.poll(1))
        self.assertEqual(len(delayed), 4)  # Three retries, one newly discovered tail.
        self.assertEqual(m.poll(1), [])
        m.poll(1.010)
        m.poll(1.020)
        self.assertEqual(len(m.frames), 0)
        self.assertEqual(m.poll(100), [])
        self.assertLessEqual(m.stats['max_requests_for_fragment'], 3)

    def test_ingest_after_final_request_recovery_of_one_fragment_updates_expiry(self):
        m = ReceiverModel()
        ps = packets(size=6 * STEP)
        for i in (0, 2):
            m.ingest(ps[i], 0)
        m.poll(0)
        m.ingest(ps[4], .001)
        m.ingest(ps[5], .001)
        m.poll(.001)
        m.poll(.010)
        m.poll(.011)
        m.poll(.020)
        m.ingest(ps[1], .0205)
        m.poll(.021)
        m.ingest(ps[3], .0305)  # Frag 1 deadline .030 no longer applies; frag 3 .031.
        self.assertEqual(m.stats['completed'], 1)

    def test_clock_injection_and_backwards_clock_rejection(self):
        clock = Clock()
        m = ReceiverModel(clock=clock)
        m.ingest(packets()[0])
        clock.now = .002
        self.assertTrue(m.poll())
        clock.now = .001
        with self.assertRaisesRegex(ValueError, 'backwards'):
            m.poll()

    def test_manifest_compression_and_explicit_limit(self):
        ranges = FrameRanges(limit=2)
        for logic in (2, 4, 1, 3, 4, 6, 8):
            ranges.add(logic)
        self.assertEqual(ranges.ranges, [[1, 4], [6, 6]])
        self.assertEqual(ranges.dropped, 1)

    def test_full_1mib_module_body_geometry_and_retransmission(self):
        mask, body = build_modules({0: b'x' * 1048571})
        self.assertEqual(len(body), 1048576)
        a = encode_data(0xFFFFFFFF, 12345, mask, body, 730)
        b = encode_data(0xFFFFFFFF, 12345, mask, body, 730, True)
        da, db = decode_data(a), decode_data(b)
        self.assertEqual(da.count, 731)
        self.assertEqual(len(da.payload), 296)
        self.assertEqual(a[:7] + a[8:-4], b[:7] + b[8:-4])
        self.assertEqual((da.flags, db.flags), (0, 1))
        self.assertNotEqual(a[-4:], b[-4:])

    def test_module_header_crosses_fragment_and_module_offline_next_frame(self):
        # Module 2's five-byte header starts one byte before the fragment edge.
        mask, body = build_modules({0: b'x' * 1430, 2: b'y' * 2500})
        completions = []
        m = ReceiverModel(lambda data, frame, modules: completions.append(modules))
        for i in reversed(range((len(body) + STEP - 1) // STEP)):
            m.ingest(encode_data(1, 100, mask, body, i), 0)
        mask2, body2 = build_modules({0: b'z'})
        m.ingest(encode_data(2, 200, mask2, body2, 0), .001)
        self.assertEqual(completions, [{0: b'x' * 1430, 2: b'y' * 2500}, {0: b'z'}])
        self.assertEqual(m.stats['module_failures'], 0)

    def test_module_structure_failure_is_terminal_and_classified(self):
        m = ReceiverModel()
        bad = encode_data(1, 100, 1, b'\x00\xff\xff\xff\xff', 0)
        m.ingest(bad, 0)
        m.ingest(packets(1, 10)[0], .001)
        self.assertEqual(m.stats['module_failures'], 1)
        self.assertEqual(m.stats['completed'], 0)
        self.assertEqual(m.report_details(.001)['frame_manifest']['module_invalid'], [[1, 1]])

    def test_wire_boundaries_overlap_and_truncation(self):
        wire = encode_nack(0x12345678, [(2, 2), (3, 2), (2, 2)])
        self.assertEqual(wire[8:12], bytes.fromhex('78563412'))
        self.assertEqual(wire[12:16], bytes.fromhex('03000000'))
        self.assertEqual(len(requested(nack_packets(1, [2, 3, 3, 4, 2]))), 3)
        for cut in (0, 7, 16, len(wire) - 1):
            with self.assertRaises(ProtocolError):
                decode_nack(wire[:cut])
        with self.assertRaises(ProtocolError):
            encode_nack(1, [(65535, 1)])
        with self.assertRaises(ProtocolError):
            decode_data(packets()[0] + b'x')
        damaged = bytearray(packets()[0])
        damaged[-1] ^= 1
        m = ReceiverModel()
        m.ingest(bytes(damaged), 0)
        m.ingest(b'', 0)
        self.assertEqual(m.stats['crc_failures'], 1)
        self.assertEqual(m.stats['geometry_failures'], 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
