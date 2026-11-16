"""Virtual-clock receiver timing evidence; no sockets or wall-clock guarantees."""
import json
import unittest
from unittest.mock import patch

from receiver_model import ReceiverModel, TimingHistogram
from rt_protocol import STEP, build_modules, decode_nack, encode_data
from udp_feedback_stub import ReceiverRuntime, parser


def packets(logic=1, fragments=5, ts=100):
    mask, body = build_modules({0: b'x' * (fragments * STEP - 5)})
    return [encode_data(logic, ts, mask, body, index) for index in range(fragments)]


def gap(model, missing=2, now=0.0):
    wires = packets()
    for index, wire in enumerate(wires):
        if index != missing:
            model.ingest(wire, now)
    return wires


def count_requests(wires):
    return sum(count for wire in wires for _, ranges in [decode_nack(wire)] for _, count in ranges)


class ReceiverTimingTests(unittest.TestCase):
    def timing(self, model):
        value = model.report_details(model._last_now or 0)['receiver_timing']
        # Reports remain strict JSON even when nothing has been observed.
        json.dumps(value, allow_nan=False)
        return value

    def test_no_observations_are_not_passing_samples(self):
        timing = self.timing(ReceiverModel())
        self.assertEqual(timing['version'], 1)
        self.assertTrue(all(value == 0 for value in timing['evidence'].values()))
        for histogram in timing['histograms'].values():
            self.assertFalse(histogram['observed'])
            self.assertEqual(histogram['count'], 0)
            for field in ('min_us', 'max_us', 'mean_us', 'p50_us', 'p95_us', 'p99_us', 'p999_us'):
                self.assertIsNone(histogram[field])
        self.assertEqual(timing['events']['total'], 0)

    def test_normal_data_records_only_frame_completion(self):
        model = ReceiverModel()
        for index, wire in enumerate(packets()):
            model.ingest(wire, index * .000020)
            self.assertEqual(model.poll(index * .000020), [])
        timing = self.timing(model)
        self.assertEqual(timing['histograms']['frame_first_seen_to_complete']['max_us'], 80)
        self.assertEqual(timing['evidence']['completed_frames'], model.stats['completed'])
        self.assertEqual(timing['evidence']['discovered_fragments'], 0)
        self.assertEqual(timing['events']['total'], 1)
        self.assertEqual(timing['events']['records'][0]['kind'], 'frame_complete')

    def test_delayed_first_poll_and_exact_retry_expiry_boundaries(self):
        model = ReceiverModel()
        gap(model)
        self.assertEqual(count_requests(model.poll(.0005)), 1)
        self.assertEqual(model.frames[1].missing[2], (1, .0005))  # Old tuple API.
        self.assertEqual(model.poll(.010499), [])
        self.assertEqual(count_requests(model.poll(.0105)), 1)
        self.assertEqual(count_requests(model.poll(.0205)), 1)
        self.assertEqual(model.poll(.030499), [])
        model.poll(.0305)
        timing = self.timing(model)
        hist, evidence = timing['histograms'], timing['evidence']
        self.assertEqual(hist['discovery_to_first_request']['max_us'], 500)
        self.assertEqual(hist['request_interval']['count'], 2)
        self.assertEqual(hist['request_interval']['min_us'], 10000)
        self.assertEqual(hist['request_interval']['max_us'], 10000)
        self.assertEqual(hist['request_scheduler_lateness']['max_us'], 500)
        self.assertEqual(hist['third_request_to_expiry']['max_us'], 10000)
        self.assertEqual(hist['missing_discovery_to_frame_expiry']['max_us'], 30500)
        self.assertEqual(evidence['first_request_same_poll_violations'], 0)
        self.assertEqual(evidence['early_retry_intervals'], 0)
        self.assertEqual(evidence['early_expiries'], 0)
        self.assertEqual(evidence['request_budget_exceeded'], 0)
        self.assertEqual(evidence['third_expiry_fragments'], 1)
        self.assertEqual(evidence['expired_frames'], 1)
        self.assertEqual(model.stats['requested_fragments'], hist['request_scheduler_lateness']['count'])
        request = next(event for event in timing['events']['records'] if event['kind'] == 'request_round')
        self.assertEqual(request['fragments'][0], dict(fragment=2, request_ordinal=1,
                         discovered_us=0, request_us=500, due_us=0))

    def test_epsilon_is_preserved_without_false_early_violation(self):
        model = ReceiverModel()
        gap(model)
        model.poll(0)
        second = .010 - model.EPSILON / 2
        self.assertEqual(count_requests(model.poll(second)), 1)
        third = second + .010 - model.EPSILON / 2
        self.assertEqual(count_requests(model.poll(third)), 1)
        model.poll(third + .010 - model.EPSILON / 2)
        evidence = self.timing(model)['evidence']
        self.assertEqual(evidence['early_retry_intervals'], 0)
        self.assertEqual(evidence['early_expiries'], 0)
        self.assertEqual(model.stats['expired'], 1)

    def test_pause_exposes_real_lateness_without_catchup_requests(self):
        model = ReceiverModel()
        gap(model)
        self.assertEqual(count_requests(model.poll(0)), 1)
        self.assertEqual(count_requests(model.poll(1)), 1)
        self.assertEqual(model.poll(1), [])
        self.assertEqual(count_requests(model.poll(1.010)), 1)
        model.poll(1.020)
        timing = self.timing(model)
        self.assertEqual(timing['histograms']['request_interval']['max_us'], 1_000_000)
        self.assertEqual(timing['histograms']['request_scheduler_lateness']['max_us'], 990_000)
        self.assertEqual(timing['histograms']['missing_discovery_to_frame_expiry']['max_us'], 1_020_000)
        self.assertEqual(timing['histograms']['third_request_to_expiry']['max_us'], 10_000)
        self.assertEqual(timing['evidence']['first_request_same_poll_violations'], 0)
        self.assertEqual(model.stats['max_requests_for_fragment'], 3)

    def test_independent_missing_clocks_and_recovery_cancel_old_expiry(self):
        model = ReceiverModel()
        wires = packets(fragments=6)
        for index in (0, 2):
            model.ingest(wires[index], 0)
        model.poll(0)
        for index in (4, 5):
            model.ingest(wires[index], .001)
        model.poll(.001)
        model.poll(.010)
        model.poll(.011)
        model.poll(.020)
        model.ingest(wires[1], .0205)
        model.poll(.021)
        model.poll(.0305)
        self.assertEqual(model.stats['expired'], 0)
        model.poll(.031)
        timing = self.timing(model)
        self.assertEqual(timing['evidence']['first_requests'], 2)
        self.assertEqual(timing['evidence']['retry_intervals'], 4)
        self.assertEqual(timing['evidence']['third_expiry_fragments'], 1)
        self.assertEqual(timing['histograms']['missing_discovery_to_frame_expiry']['max_us'], 30000)
        self.assertEqual(timing['histograms']['frame_first_seen_to_expiry']['max_us'], 31000)
        self.assertEqual(timing['histograms']['first_request_to_recovery']['max_us'], 20500)
        self.assertEqual(timing['histograms']['last_request_to_recovery']['max_us'], 500)

    def test_whole_frame_expiry_excludes_not_yet_due_third_requests(self):
        model = ReceiverModel()
        wires = packets(fragments=6)
        for index in (0, 2):
            model.ingest(wires[index], 0)
        model.poll(0)
        for index in (4, 5):
            model.ingest(wires[index], .001)
        for now in (.001, .010, .011, .020, .021, .030):
            model.poll(now)
        timing = self.timing(model)
        self.assertEqual(timing['evidence']['third_expiry_fragments'], 1)
        self.assertEqual(timing['evidence']['cancelled_missing_fragments'], 1)
        self.assertEqual(timing['histograms']['third_request_to_expiry']['min_us'], 10000)

    def test_flag_zero_recovery_uses_first_and_last_request_times(self):
        model = ReceiverModel()
        wires = gap(model)
        for now in (0, .010, .020):
            model.poll(now)
        model.ingest(wires[2], .029)
        model.poll(1)
        timing = self.timing(model)
        self.assertEqual(timing['evidence']['recovered_without_retry_flag'], 1)
        self.assertEqual(timing['histograms']['first_request_to_recovery']['max_us'], 29000)
        self.assertEqual(timing['histograms']['last_request_to_recovery']['max_us'], 9000)
        self.assertFalse(timing['histograms']['third_request_to_expiry']['observed'])
        self.assertEqual(model.stats['completed'], 1)
        self.assertEqual(model.frames, {})
        recovery = next(event for event in timing['events']['records'] if event['kind'] == 'fragment_recovered')
        self.assertFalse(recovery['retry_flag'])
        self.assertEqual(recovery['request_ordinal'], 3)

    def test_packet_after_deadline_expires_even_before_poll(self):
        model = ReceiverModel()
        wires = gap(model)
        for now in (0, .010, .020):
            model.poll(now)
        model.ingest(wires[2], .030)
        timing = self.timing(model)
        self.assertEqual(timing['evidence']['expired_frames'], 1)
        self.assertEqual(timing['evidence']['recovered_fragments'], 0)
        self.assertEqual(timing['histograms']['third_request_to_expiry']['max_us'], 10000)

    def test_tail_silence_and_newer_frame_have_separate_denominators(self):
        silence = ReceiverModel()
        silence.ingest(packets()[0], 0)
        silence.poll(.003)
        timing = self.timing(silence)
        self.assertEqual(timing['evidence']['tail_silence_discoveries'], 1)
        self.assertEqual(timing['histograms']['silence_tail_lateness']['max_us'], 1000)
        self.assertEqual(timing['histograms']['tail_silence_age']['max_us'], 3000)
        self.assertEqual(timing['evidence']['first_request_same_poll_violations'], 0)
        newer = ReceiverModel()
        newer.ingest(packets()[0], 0)
        newer.ingest(packets(2, fragments=1)[0], .001)
        newer.poll(.001)
        timing = self.timing(newer)
        self.assertEqual(timing['evidence']['tail_newer_frame_discoveries'], 1)
        self.assertEqual(timing['histograms']['tail_newer_frame_age']['max_us'], 1000)
        self.assertEqual(timing['histograms']['newer_frame_to_tail_discovery']['max_us'], 0)
        self.assertFalse(timing['histograms']['silence_tail_lateness']['observed'])

    def test_cancelled_or_unrequested_missing_parts_do_not_invent_recovery(self):
        model = ReceiverModel()
        wires = packets()
        model.ingest(wires[0], 0)
        model.ingest(wires[2], 0)
        model.ingest(wires[1], 0)
        self.assertEqual(self.timing(model)['evidence']['resolved_before_first_request'], 1)
        model.ingest(packets(ts=101)[3], .001)
        model.poll(1)
        timing = self.timing(model)
        self.assertEqual(model.stats['metadata_failures'], 1)
        self.assertEqual(timing['evidence']['expired_frames'], 0)
        self.assertEqual(timing['evidence']['recovered_fragments'], 0)
        self.assertFalse(timing['histograms']['request_interval']['observed'])

    def test_event_history_is_recent_and_fixed_capacity(self):
        model = ReceiverModel()
        for logic in range(1, 146):
            model.ingest(packets(logic, fragments=1)[0], logic * .020)
        events = self.timing(model)['events']
        self.assertEqual((events['total'], events['retained'], events['dropped']), (145, 128, 17))
        self.assertEqual(events['records'][0]['sequence'], 18)
        self.assertEqual(events['records'][-1]['sequence'], 145)
        self.assertEqual(len(model._timing_events), 128)

    def test_large_nack_round_keeps_all_denominators_and_only_eight_details(self):
        model = ReceiverModel()
        wires = packets(fragments=1460)  # <=2MiB; 730 disjoint gaps.
        for index, wire in enumerate(wires):
            if index % 2:
                model.ingest(wire, 0)
        requests = model.poll(0)
        self.assertEqual(len(requests), 3)
        timing = self.timing(model)
        self.assertEqual(timing['evidence']['first_requests'], 730)
        self.assertEqual(timing['histograms']['request_scheduler_lateness']['count'], 730)
        rounds = [event for event in timing['events']['records'] if event['kind'] == 'request_round']
        self.assertEqual(len(rounds), 1)
        self.assertEqual(rounds[0]['fragments_total'], 730)
        self.assertEqual(len(rounds[0]['fragments']), 8)
        self.assertEqual(rounds[0]['fragments_dropped'], 722)

    def test_histogram_storage_and_overflow_are_bounded(self):
        histogram = TimingHistogram()
        for value in (0, .000050, .000101, .150001):
            histogram.add(value)
        report = histogram.summary()
        self.assertEqual(len(histogram.bins), 1002)
        self.assertEqual(report['count'], 4)
        self.assertEqual(report['overflow_count'], 1)
        self.assertEqual(report['p50_us'], 100)
        self.assertEqual(report['max_us'], 150001)

    def test_runtime_exports_timing_without_starting_a_socket(self):
        args = parser().parse_args(['--quiet', '--drain', '--duplicate-nack'])
        with patch('udp_feedback_stub.provenance', return_value={'pid': 1}):
            runtime = ReceiverRuntime(args, clock=lambda: 5.0)
        report = runtime.report()
        self.assertTrue(report['receiver_timing']['runtime']['duplicate_nack_injection'])
        self.assertIn('drain', report['receiver_timing']['runtime']['mode'])
        self.assertFalse(report['receiver_timing']['histograms']['request_interval']['observed'])
        self.assertIn('completed', report)  # Old counters remain top-level.


if __name__ == '__main__':
    unittest.main(verbosity=2)
