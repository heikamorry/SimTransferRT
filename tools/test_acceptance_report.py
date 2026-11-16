"""Evidence gates for exported tables, independent of network timing."""
import copy
import unittest
from acceptance_report import (row, table, sender_rows, receiver_acceptance,
                               combined_acceptance, markdown)


def by_id(value):
    return {item['id']: item for item in value['rows']}


def receiver_fixture():
    def hist(n, low, high):
        return dict(count=n, min_us=low, max_us=high, p99_us=high, p999_us=high, observed=True)
    return dict(run_id='paired', completed=2, expired=1,
                requested_fragments=3, max_requests_for_fragment=3,
                receiver_rendering_implemented=False,
                receiver_timing=dict(version=1, histograms={
                    'discovery_to_first_request': hist(1, 0, 20),
                    'request_interval': hist(2, 10000, 10500),
                    'third_request_to_expiry': hist(1, 10000, 10000),
                    'frame_first_seen_to_complete': hist(2, 1000, 1100),
                }, evidence=dict(first_requests=1, first_request_same_poll_violations=0,
                    retry_intervals=2, early_retry_intervals=0, third_expiry_fragments=1,
                    early_expiries=0, expired_frames=1, request_budget_exceeded=0, completed_frames=2)))


class AcceptanceReportTests(unittest.TestCase):
    def pair(self):
        sender = dict(run_id='paired', stats=dict(built=2), next_logic_id=3,
            built_manifest=dict(first_id=1, count=2, next_id=3), acceptance_table=table([
            row('sampling_jitter', '采样偏差', '实际采样间隔减20ms', '<1000us', '500us', 'pass', '全量观测')], 'sender-local'))
        receiver = receiver_fixture()
        receiver.update(expired=0, manifest_truncated=False, active_frames=0, active_logic_ids=[],
                        metadata_failures=0, module_failures=0, active_evictions=0,
                        frame_manifest=dict(seen=[[1, 2]], completed=[[1, 2]], expired=[],
                                            resource_limit=[], metadata_mismatch=[], module_invalid=[]))
        summary = dict(run_id='paired', sender_exit=0, receiver_exit=0, failure=None,
                       case='A1', functional_target_pass=True, functional_failures=[],
                       reception=dict(exact_classification_available=True, manifest_consistent=True,
                                      sender_built_denominator=2, completed=2, retired=0,
                                      wholly_missing=0, still_in_flight=0))
        return sender, receiver, summary

    def test_missing_and_legacy_never_mean_zero_or_pass(self):
        for malformed in ({}, {'acceptance_table': None}, {'acceptance_table': []},
                          {'acceptance_table': {'schema_version': 1, 'rows': [{'status': 'pass'}]}}):
            self.assertTrue(all(r['status'] == 'not_measured' for r in sender_rows(malformed)))
        self.assertTrue(all(r['status'] == 'not_measured' for r in receiver_acceptance({})['rows']))

    def test_same_run_join_retains_sender_metrics_and_unknown_hardware(self):
        result = combined_acceptance(*self.pair())
        rows = by_id(result)
        self.assertEqual(rows['receiver_delivery']['status'], 'pass')
        self.assertEqual(rows['sampling_jitter']['status'], 'pass')
        self.assertEqual(rows['hardware_timestamp']['status'], 'not_measured')
        self.assertEqual(result['overall_status'], 'not_measured')

    def test_wrong_run_or_failed_process_never_claims_delivery(self):
        for change in ('run', 'exit', 'failure'):
            sender, receiver, summary = self.pair()
            if change == 'run': receiver['run_id'] = 'different'
            elif change == 'exit': summary['receiver_exit'] = 1
            else: summary['failure'] = 'timeout'
            result = combined_acceptance(sender, receiver, summary)
            self.assertEqual(by_id(result)['receiver_delivery']['status'], 'not_measured')
            self.assertEqual(by_id(result)['report_pairing']['status'], 'fail')

    def test_truncated_incomplete_or_missing_inventory_is_not_measured(self):
        for key in ('exact_classification_available', 'manifest_consistent', 'completed'):
            sender, receiver, summary = self.pair()
            summary['reception'].pop(key)
            self.assertEqual(by_id(combined_acceptance(sender, receiver, summary))['receiver_delivery']['status'], 'not_measured')

    def test_drain_never_has_joint_acceptance(self):
        sender, receiver, summary = self.pair()
        summary['case'] = 'A0-drain'
        self.assertEqual(by_id(combined_acceptance(sender, receiver, summary))['receiver_delivery']['status'], 'not_measured')

    def test_expected_persistent_drop_not_mislabeled_complete_delivery(self):
        sender, receiver, summary = self.pair()
        summary['case'] = 'C'
        summary['reception'].update(completed=0, retired=2)
        receiver.update(completed=0, expired=2)
        receiver['frame_manifest'].update(completed=[], expired=[[1, 2]])
        result = by_id(combined_acceptance(sender, receiver, summary))['receiver_delivery']
        self.assertEqual(result['status'], 'pass')
        self.assertIn('不代表完整交付', result['reason'])

    def test_detected_delivery_failure_propagates(self):
        sender, receiver, summary = self.pair()
        summary.update(functional_target_pass=False, functional_failures=['missing frame'])
        result = combined_acceptance(sender, receiver, summary)
        self.assertEqual(result['overall_status'], 'fail')
        self.assertEqual(by_id(result)['receiver_delivery']['status'], 'fail')

    def test_retry_count_samples_and_early_violation_are_checked(self):
        report = receiver_fixture()
        self.assertEqual(by_id(receiver_acceptance(report))['nack_retry_interval']['status'], 'pass')
        report['receiver_timing']['evidence']['early_retry_intervals'] = 1
        self.assertEqual(by_id(receiver_acceptance(report))['nack_retry_interval']['status'], 'fail')
        report['receiver_timing']['evidence']['retry_intervals'] = 3
        self.assertEqual(by_id(receiver_acceptance(report))['nack_retry_interval']['status'], 'not_measured')

    def test_empty_nan_or_negative_histograms_cannot_pass(self):
        for count, minimum in [(0, 0), (1, float('nan')), (1, -1)]:
            report = receiver_fixture()
            report['receiver_timing']['histograms']['request_interval'].update(count=count, min_us=minimum)
            self.assertEqual(by_id(receiver_acceptance(report))['nack_retry_interval']['status'], 'not_measured')

    def test_lateness_and_approximate_targets_do_not_invent_tolerance(self):
        report = receiver_fixture()
        report['receiver_timing']['histograms']['missing_discovery_to_frame_expiry'] = {
            'count': 1, 'min_us': 30001, 'max_us': 30001}
        item = by_id(receiver_acceptance(report))['nack_total_wait']
        self.assertEqual(item['status'], 'not_measured')
        self.assertIn('30001', item['actual'])

    def test_duplicate_ids_in_sender_table_are_rejected(self):
        sender, _, _ = self.pair()
        sender['acceptance_table']['rows'] *= 2
        self.assertTrue(all(r['status'] == 'not_measured' for r in sender_rows(sender)))

    def test_markdown_columns_and_untrusted_text_are_preserved_as_text(self):
        value = table([row('test', 'name | <script>', 'a\nb', '1', '2', 'fail', 'reason')], 'test')
        text = markdown(value)
        self.assertIn('| 要求 | 测量口径 | 阈值 | 实测值 | 结果 | 原因 |', text)
        self.assertNotIn('<script>', text)
        self.assertIn('&#124;', text)
        self.assertIn('a<br>b', text)
        self.assertIn('失败', text)

    def test_scope_and_each_required_column_always_present(self):
        result = combined_acceptance(*self.pair())
        for r in result['rows']:
            for column in ('id', 'requirement', 'measurement', 'threshold', 'actual', 'status', 'reason', 'evidence'):
                self.assertIn(column, r)
        self.assertEqual(sum(result['status_counts'].values()), len(result['rows']))

    def test_forged_summary_cannot_replace_raw_manifests(self):
        sender, receiver, summary = self.pair()
        sender.pop('built_manifest')
        receiver.pop('frame_manifest')
        self.assertEqual(by_id(combined_acceptance(sender, receiver, summary))['receiver_delivery']['status'], 'not_measured')

    def test_histogram_early_wait_cannot_be_hidden_by_zero_counter(self):
        for metric, identifier in [('request_interval', 'nack_retry_interval'), ('third_request_to_expiry', 'nack_expiry_wait')]:
            receiver = receiver_fixture()
            receiver['receiver_timing']['histograms'][metric].update(min_us=5000, max_us=5000)
            self.assertEqual(by_id(receiver_acceptance(receiver))[identifier]['status'], 'fail')

    def test_invalid_zero_budget_and_boolean_exit_are_not_success(self):
        receiver = receiver_fixture()
        receiver['max_requests_for_fragment'] = 0
        self.assertEqual(by_id(receiver_acceptance(receiver))['nack_request_budget']['status'], 'not_measured')
        sender, receiver, summary = self.pair()
        summary['sender_exit'] = False
        self.assertEqual(by_id(combined_acceptance(sender, receiver, summary))['report_pairing']['status'], 'fail')

    def test_extreme_integer_and_false_label_are_handled(self):
        receiver = receiver_fixture()
        receiver['receiver_timing']['histograms']['request_interval']['min_us'] = 10**309
        self.assertEqual(by_id(receiver_acceptance(receiver))['nack_retry_interval']['status'], 'not_measured')
        sender, _, _ = self.pair()
        sender['acceptance_table']['rows'][0].update(status='fail', status_label='通过')
        self.assertEqual(sender_rows(sender)[0]['status_label'], '失败')

    def test_early_expiry_uses_frame_denominator_and_survives_empty_fragment_histogram(self):
        report = receiver_fixture()
        self.assertEqual(by_id(receiver_acceptance(report))['nack_expiry_wait']['status'], 'pass')
        report['receiver_timing']['evidence'].pop('expired_frames')
        self.assertEqual(by_id(receiver_acceptance(report))['nack_expiry_wait']['status'], 'not_measured')
        report['receiver_timing']['evidence'].update(expired_frames=1, early_expiries=1, third_expiry_fragments=0)
        report['receiver_timing']['histograms']['third_request_to_expiry'].update(count=0, min_us=None, max_us=None)
        self.assertEqual(by_id(receiver_acceptance(report))['nack_expiry_wait']['status'], 'fail')


if __name__ == '__main__':
    unittest.main()
