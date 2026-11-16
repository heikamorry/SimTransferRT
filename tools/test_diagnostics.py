import copy
import unittest
from run_diagnostics import acceptance, case_passed, reconcile


def reports(count=3):
    sender = {'stats': {'built': count, 'sampled': count, 'unsentExpired': 0, 'deadlineMisses': 0,
                        'sendErrors': 0, 'actualInitialDeadlineMisses': 0, 'unsentAtStop': 0,
                        'initialFinishedFrames': count, 'initialFullySubmittedFrames': count,
                        'initialIncompleteFrames': 0, 'initialPendingFragments': 0,
                        'retransmissions': 0},
              'timing': {'abs_interval_minus_20000us': {'count': max(0, count - 1), 'max_us': 999}},
              'next_logic_id': count + 1, 'df_enabled': True, 'run_id': 'test',
              'built_manifest': {'first_id': 1, 'next_id': count + 1, 'count': count}}
    receiver = {'completed': count, 'expired': 0, 'manifest_truncated': False,
                'active_logic_ids': [], 'active_frames': 0, 'run_id': 'test',
                'module_failures': 0, 'metadata_failures': 0, 'active_evictions': 0,
                'max_requests_for_fragment': 0, 'recovered': 0,
                'frame_manifest': {'seen': [[1, count]], 'completed': [[1, count]],
                                   'expired': [], 'resource_limit': [],
                                   'metadata_mismatch': [], 'module_invalid': []}}
    return sender, receiver


class ReconciliationTests(unittest.TestCase):
    def test_missing_entire_frame_and_wrap_use_sender_denominator(self):
        sender = {'stats': {'built': 4}, 'next_logic_id': 2}
        receiver = {'completed': 1, 'manifest_truncated': False,
                    'active_logic_ids': [1], 'frame_manifest': {
                        'seen': [[0, 1], [4294967294, 4294967294]],
                        'completed': [[4294967294, 4294967294]], 'expired': [[0, 0]]}}
        result = reconcile(sender, receiver)
        self.assertEqual(result['completion_fraction'], .25)
        self.assertEqual(result['wholly_missing_ids'], [4294967295])
        self.assertEqual(result['still_in_flight'], 1)
        self.assertEqual(result['retired'], 1)
        self.assertEqual(result['unclassified_seen_ids'], [])
        self.assertTrue(result['manifest_consistent'])

    def test_legacy_missing_manifest_does_not_claim_no_loss(self):
        result = reconcile({'stats': {'built': 10}}, {'completed': 2})
        self.assertNotIn('wholly_missing', result)
        self.assertEqual(result['completion_fraction'], .2)
        self.assertFalse(result['exact_classification_available'])

    def test_overlap_and_counter_conflicts_are_not_a_success(self):
        sender, receiver = reports()
        receiver['frame_manifest']['expired'] = [[2, 2]]
        result = reconcile(sender, receiver)
        self.assertEqual(result['overlapping_outcomes'], 1)
        self.assertIn('expired', result['counter_mismatches'])
        self.assertFalse(result['manifest_consistent'])

    def test_unrelated_run_completion_does_not_inflate_fraction(self):
        sender, receiver = reports()
        receiver['frame_manifest']['completed'] = [[100, 102]]
        result = reconcile(sender, receiver)
        self.assertEqual(result['completion_fraction'], 0)
        self.assertEqual(result['unexpected_receiver'], 3)
        self.assertEqual(result['outcomes_without_seen'], 3)

    def test_inventory_expansion_is_bounded(self):
        sender, receiver = reports()
        receiver['frame_manifest']['seen'] = [[0, 0xffffffff]]
        result = reconcile(sender, receiver)
        self.assertFalse(result['exact_classification_available'])
        self.assertIn('bounded', result['exact_classification'])

    def test_large_missing_inventory_is_counted_but_list_is_capped(self):
        sender, receiver = reports(500)
        receiver['frame_manifest']['seen'] = []
        receiver['frame_manifest']['completed'] = []
        receiver['completed'] = 0
        result = reconcile(sender, receiver)
        self.assertEqual(result['wholly_missing'], 500)
        self.assertEqual(len(result['wholly_missing_ids']), 128)
        self.assertTrue(result['id_lists_truncated'])

    def test_sender_manifest_must_agree_with_counters(self):
        sender, receiver = reports()
        sender['built_manifest']['first_id'] = 10
        self.assertFalse(reconcile(sender, receiver)['exact_classification_available'])

    def test_malformed_ranges_and_active_ids_are_rejected(self):
        for ranges in ([[5, 3]], [[-1, 3]], [[True, 3]], [[1, 1, 1]], 'invalid'):
            sender, receiver = reports()
            receiver['frame_manifest']['seen'] = ranges
            self.assertFalse(reconcile(sender, receiver)['exact_classification_available'])
        sender, receiver = reports()
        receiver['active_logic_ids'] = [0x100000000]
        self.assertFalse(reconcile(sender, receiver)['exact_classification_available'])


class AcceptanceTests(unittest.TestCase):
    def evaluate(self, name='A1', sender=None, receiver=None):
        if sender is None:
            sender, receiver = reports()
        result = acceptance(name, sender, receiver, 3, 'test')
        return dict(result, case=name, failure=None, sender_exit=0,
                    receiver_exit=None if name == 'A0-closed' else 0)

    def test_complete_normal_frame_inventory_passes(self):
        result = self.evaluate()
        self.assertTrue(result['functional_target_pass'])
        self.assertTrue(result['timing_target_pass'])
        self.assertTrue(case_passed(result))

    def test_zero_send_errors_cannot_hide_wholly_missing_frame(self):
        sender, receiver = reports()
        receiver['completed'] = 2
        receiver['frame_manifest']['seen'] = [[1, 2]]
        receiver['frame_manifest']['completed'] = [[1, 2]]
        result = self.evaluate(sender=sender, receiver=receiver)
        self.assertTrue(result['timing_target_pass'])
        self.assertFalse(result['functional_target_pass'])
        self.assertFalse(case_passed(result))

    def test_actual_deadline_miss_blocks_timing_even_if_legacy_counter_zero(self):
        sender, receiver = reports()
        sender['stats']['actualInitialDeadlineMisses'] = 1
        result = self.evaluate(sender=sender, receiver=receiver)
        self.assertTrue(result['functional_target_pass'])
        self.assertFalse(result['timing_target_pass'])
        self.assertIn('actualInitialDeadlineMisses', result['timing_failures'])
        self.assertFalse(case_passed(result))

    def test_max_sampling_deviation_has_strict_boundary_and_cannot_hide_behind_p99(self):
        for maximum, passes in ((999, True), (1000, False), (11614, False)):
            with self.subTest(maximum=maximum):
                sender, receiver = reports()
                sender['timing']['abs_interval_minus_20000us'].update(max_us=maximum, p99_us=100)
                result = self.evaluate(sender=sender, receiver=receiver)
                self.assertTrue(result['timing_target_pass'])  # Historical field is unchanged.
                self.assertTrue(result['functional_target_pass'])
                self.assertEqual(result['sampling_jitter_target_pass'], passes)
                self.assertEqual(case_passed(result), passes)

    def test_jitter_population_covers_all_actual_samples_even_without_a_built_frame(self):
        sender, receiver = reports()
        sender['stats']['sampled'] = 5
        result = self.evaluate(sender=sender, receiver=receiver)
        self.assertEqual(result['sampling_jitter_status'], 'unverified')
        self.assertFalse(case_passed(result))
        sender['timing']['abs_interval_minus_20000us']['count'] = 4
        self.assertTrue(case_passed(self.evaluate(sender=sender, receiver=receiver)))

    def test_missing_invalid_or_partial_sampling_evidence_is_unverified(self):
        for mutation in (
            lambda s: s.pop('timing'),
            lambda s: s['stats'].pop('sampled'),
            lambda s: s['timing']['abs_interval_minus_20000us'].update(count=1),
            lambda s: s['timing']['abs_interval_minus_20000us'].update(max_us=True),
            lambda s: s['timing']['abs_interval_minus_20000us'].update(max_us=-1),
            lambda s: s['timing']['abs_interval_minus_20000us'].update(max_us=float('nan')),
            lambda s: s['timing'].update(abs_interval_minus_20000us=None),
        ):
            sender, receiver = reports()
            mutation(sender)
            result = self.evaluate(sender=sender, receiver=receiver)
            self.assertEqual(result['sampling_jitter_status'], 'unverified')
            self.assertIsNone(result['sampling_jitter_target_pass'])
            self.assertFalse(case_passed(result))
        sender, receiver = reports(1)
        result = acceptance('A1', sender, receiver, 1, 'test')
        self.assertEqual(result['sampling_jitter_status'], 'unverified')

    def test_recovered_or_expired_frames_do_not_exclude_their_sampling_intervals(self):
        for case in ('A1', 'C'):
            sender, receiver = reports()
            if case == 'C':
                receiver.update(completed=0, expired=3, max_requests_for_fragment=3)
                receiver['frame_manifest'].update(completed=[], expired=[[1, 3]])
                sender['stats']['initialFullySubmittedFrames'] = 0  # Intentional injected drops.
            sender['timing']['abs_interval_minus_20000us']['max_us'] = 11614
            result = self.evaluate(case, sender, receiver)
            self.assertTrue(result['functional_target_pass'])
            self.assertFalse(result['sampling_jitter_target_pass'])
            self.assertFalse(case_passed(result))

    def test_complete_receiver_inventory_cannot_hide_unfinished_initial_dispatch(self):
        for changes in ({'initialIncompleteFrames': 1}, {'unsentAtStop': 4},
                        {'initialPendingFragments': 4}, {'initialFinishedFrames': 2},
                        {'initialFullySubmittedFrames': 2}):
            sender, receiver = reports()
            sender['stats'].update(changes)
            result = self.evaluate(sender=sender, receiver=receiver)
            self.assertTrue(result['functional_target_pass'])
            self.assertTrue(result['timing_target_pass'])
            self.assertFalse(result['initial_dispatch_target_pass'])
            self.assertFalse(case_passed(result))

    def test_legacy_initial_dispatch_and_legacy_summary_cannot_pass_new_gates(self):
        sender, receiver = reports()
        del sender['stats']['initialIncompleteFrames']
        result = self.evaluate(sender=sender, receiver=receiver)
        self.assertEqual(result['initial_dispatch_status'], 'unverified')
        self.assertFalse(case_passed(result))
        result = self.evaluate()
        del result['sampling_jitter_target_pass']
        del result['initial_dispatch_target_pass']
        self.assertFalse(case_passed(result))

    def test_missing_new_timing_counter_is_unknown_not_pass(self):
        sender, receiver = reports()
        del sender['stats']['actualInitialDeadlineMisses']
        result = self.evaluate(sender=sender, receiver=receiver)
        self.assertFalse(result['timing_target_pass'])
        self.assertEqual(result['timing_metrics_unavailable'], ['actualInitialDeadlineMisses'])

    def test_recovery_requires_both_sender_and_receiver_evidence(self):
        sender, receiver = reports()
        result = self.evaluate('B1', sender, receiver)
        self.assertFalse(result['functional_target_pass'])
        sender['stats']['retransmissions'] = 3
        receiver['recovered'] = 3
        receiver['max_requests_for_fragment'] = 1
        self.assertTrue(case_passed(self.evaluate('B1', sender, receiver)))

    def test_persistent_loss_requires_all_expected_frames_expired_with_three_requests(self):
        sender, receiver = reports()
        receiver['completed'], receiver['expired'], receiver['max_requests_for_fragment'] = 0, 3, 3
        receiver['frame_manifest']['completed'] = []
        receiver['frame_manifest']['expired'] = [[1, 3]]
        self.assertTrue(case_passed(self.evaluate('C', sender, receiver)))
        receiver['max_requests_for_fragment'] = 2
        self.assertFalse(self.evaluate('C', sender, receiver)['functional_target_pass'])

    def test_wrong_run_and_failed_process_never_pass(self):
        sender, receiver = reports()
        receiver['run_id'] = 'other'
        self.assertFalse(case_passed(self.evaluate(sender=sender, receiver=receiver)))
        result = self.evaluate()
        result['receiver_exit'] = 1
        self.assertFalse(case_passed(result))
        result = self.evaluate()
        result['failure'] = 'forced stop'
        self.assertFalse(case_passed(result))

    def test_truncated_inventory_never_claims_exact_success(self):
        sender, receiver = reports()
        receiver['manifest_truncated'] = True
        self.assertFalse(self.evaluate(sender=sender, receiver=receiver)['functional_target_pass'])

    def test_drain_and_closed_never_claim_protocol_acceptance(self):
        sender, receiver = reports()
        closed = self.evaluate('A0-closed', sender, {})
        self.assertIsNone(closed['functional_target_pass'])
        self.assertTrue(case_passed(closed))
        receiver['drain_datagrams'] = 100
        drain = self.evaluate('A0-drain', sender, receiver)
        self.assertIsNone(drain['functional_target_pass'])
        self.assertTrue(case_passed(drain))
        self.assertFalse(case_passed(self.evaluate('A0-drain', sender, {})))

    def test_requested_frame_shortfall_blocks_closed_baseline_success(self):
        sender, receiver = reports()
        sender['stats']['built'] = 2
        self.assertFalse(case_passed(self.evaluate('A0-closed', sender, {})))


if __name__ == '__main__':
    unittest.main()
