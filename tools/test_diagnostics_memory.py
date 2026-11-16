"""No-traffic checks for bounded, non-intrusive process memory observations."""
import ctypes
import json
import os
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from run_diagnostics import BoundedMemorySampler, memory_bytes


def counters(working=100, private=200, peak=300):
    return {'available': True, 'working_set_bytes': working, 'private_bytes': private,
            'os_peak_working_set_bytes': peak}


class MemorySamplingTests(unittest.TestCase):
    def test_sampling_cadence_and_fixed_storage_preserve_first_last_and_full_run_peaks(self):
        sampler = BoundedMemorySampler(10)
        process = SimpleNamespace(pid=7, returncode=None)
        with patch('run_diagnostics.memory_bytes', side_effect=[counters(100, 500, 300),
                    counters(1000, 200, 1200), counters(50, 100, 1200)]) as query:
            sampler.sample('sender', process, 10)
            sampler.sample('sender', process, 10.5)
            sampler.sample('sender', process, 11)
            sampler.sample('sender', process, 12)
            self.assertEqual(query.call_count, 3)
        entry = sampler.report()['processes']['sender']
        self.assertEqual(entry['first_observed']['private_bytes'], 500)
        self.assertEqual(entry['last_live_observed']['working_set_bytes'], 50)
        self.assertEqual(entry['sampled_peak_working_set_bytes'], 1000)
        self.assertEqual(entry['sampled_peak_private_bytes'], 500)
        self.assertEqual(entry['os_peak_working_set_bytes'], 1200)
        with patch('run_diagnostics.memory_bytes', return_value=counters()):
            for second in range(13, 1810):
                sampler.sample('sender', process, second)
        self.assertEqual(entry['samples'], 1800)
        self.assertEqual(len(entry), 9)  # No per-sample list grows with soak duration.
        self.assertLess(len(json.dumps(sampler.report())), 2000)
        self.assertIn('No stable-memory or leak-free verdict', sampler.report()['note'])

    def test_unavailable_probe_is_counted_and_can_recover(self):
        sampler = BoundedMemorySampler(0)
        process = SimpleNamespace(pid=7, returncode=None)
        with patch('run_diagnostics.memory_bytes', side_effect=[
                {'available': False, 'error': 'access denied'}, counters()]):
            sampler.sample('sender', process, 0)
            sampler.sample('sender', process, 1)
        entry = sampler.report()['processes']['sender']
        self.assertEqual(entry['failed_samples'], 1)
        self.assertEqual(entry['samples'], 1)
        self.assertEqual(entry['last_error'], 'access denied')

    def test_exited_and_absent_processes_are_not_probed_or_counted_as_zero_memory(self):
        sampler = BoundedMemorySampler(0)
        with patch('run_diagnostics.memory_bytes') as query:
            sampler.sample('sender', SimpleNamespace(pid=1, returncode=0), 0, force=True)
            sampler.sample('receiver', None, 0)
            query.assert_not_called()
        self.assertEqual(sampler.report()['processes'], {})

    def test_unexpected_optional_probe_failure_is_nonfatal(self):
        sampler = BoundedMemorySampler(0)
        with patch('run_diagnostics.memory_bytes', side_effect=RuntimeError('probe failed')):
            sampler.sample('sender', SimpleNamespace(pid=1, returncode=None), 0)
        entry = sampler.report()['processes']['sender']
        self.assertEqual(entry['samples'], 0)
        self.assertEqual(entry['failed_samples'], 1)
        self.assertIsNone(entry['sampled_peak_private_bytes'])
        self.assertIn('probe failed', entry['last_error'])

    @unittest.skipUnless(os.name == 'nt', 'Windows native counters')
    def test_native_current_process_query_uses_pointer_sized_counter_layout(self):
        get_current = ctypes.WinDLL('kernel32').GetCurrentProcess
        get_current.restype = ctypes.c_void_p
        result = memory_bytes(SimpleNamespace(_handle=get_current()))
        self.assertTrue(result['available'], result)
        self.assertGreater(result['working_set_bytes'], 0)
        self.assertGreater(result['private_bytes'], 0)
        self.assertGreaterEqual(result['os_peak_working_set_bytes'], result['working_set_bytes'])


if __name__ == '__main__':
    unittest.main()
