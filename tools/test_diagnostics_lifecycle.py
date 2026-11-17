"""No-traffic regression tests for orchestrator-controlled receiver lifetime."""
import json
import io
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch
import run_diagnostics


class DiagnosticsLifecycleTests(unittest.TestCase):
    def exercise(self, timeout=False, legacy=False, mode='A1', timing_trace=None, legacy_sender=False):
        events, children, virtual = [], [], [0.0]
        class Process:
            def __init__(self, name):
                self.name, self.pid, self.returncode = name, 100 + len(children), None
                self.polls = 0
            def poll(self):
                if self.name == 'sender' and self.returncode is None:
                    self.polls += 1
                    if not timeout and self.polls >= 2:
                        self.returncode = 0
                        events.append(('sender-finished', virtual[0]))
                return self.returncode
            def wait(self, timeout):
                events.append(('wait', self.name, timeout))
                self.returncode = 0
                return 0
        def launch(command, **kwargs):
            name = 'receiver' if '--seconds' in command else 'sender'
            if name == 'receiver':
                events.append(('receiver-limit', command[command.index('--seconds') + 1]))
                if '--ready-file' in command:
                    Path(command[command.index('--ready-file') + 1]).write_text('{}')
                else:
                    kwargs['stdout'].write('LISTEN 127.0.0.1\n')
                    kwargs['stdout'].flush()
            else:
                # Simulated cold startup exceeds the old two-second lifetime margin.
                virtual[0] += 3
            child = Process(name)
            children.append(child)
            events.append(('launch', name))
            return child
        def stop(child):
            if child is not None and child.poll() is None:
                events.append(('stop', child.name))
                child.returncode = 0
            return False
        def sleep(seconds):
            events.append(('sleep', seconds))
            virtual[0] += 60 if timeout and seconds == .05 else seconds
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            sender = root / 'sender.exe'
            sender.write_bytes(b'fake executable; never launched')
            args = SimpleNamespace(sender=sender, receiver=Path(run_diagnostics.ROOT) / 'tools/udp_feedback_stub.py',
                                   qt_prefix=root, legacy_sender=legacy_sender, legacy_receiver=legacy,
                                   build='Release', actual_build='Release', source_info={})
            if timing_trace is not None:
                args.timing_trace = timing_trace
            with patch.object(run_diagnostics, 'free_ports', return_value=(30001, 30002)), \
                 patch.object(run_diagnostics.subprocess, 'Popen', side_effect=launch), \
                 patch.object(run_diagnostics, 'stop_owned', side_effect=stop), \
                 patch.object(run_diagnostics, 'cpu_seconds', return_value=.1), \
                 patch.object(run_diagnostics, 'memory_bytes', return_value={
                     'available': True, 'working_set_bytes': 1024,
                     'private_bytes': 2048, 'os_peak_working_set_bytes': 4096}), \
                 patch.object(run_diagnostics.time, 'monotonic', side_effect=lambda: virtual[0]), \
                 patch.object(run_diagnostics.time, 'sleep', side_effect=sleep), \
                 patch.object(run_diagnostics, 'acceptance', return_value={
                     'functional_target_pass': True, 'functional_failures': [], 'reception': {},
                     'timing_target_pass': True, 'sender_inventory_pass': True,
                     'sampling_jitter_target_pass': True, 'initial_dispatch_target_pass': True,
                     'packet_gap_target_pass': True}), patch('builtins.print'):
                result = run_diagnostics.run_case(args, root, mode, 10, 0)
            configuration = json.loads((Path(result['directory']) / 'environment.json').read_text())
        return events, result, configuration

    def test_new_receiver_stays_until_sender_and_two_second_tail(self):
        for legacy, mode in ((False, 'A1'), (True, 'A0-drain')):
            with self.subTest(legacy=legacy, mode=mode):
                events, result, config = self.exercise(legacy=legacy, mode=mode)
                self.assertEqual(events[0], ('receiver-limit', '0'))
                self.assertIsNone(result['failure'])
                self.assertEqual(events[-3:][1:], [('sleep', 2), ('stop', 'receiver')])
                self.assertEqual(events[-3][0], 'sender-finished')
                self.assertIn('sender completion', config['receiver_lifetime_policy'])
                self.assertGreater(config['process_memory']['processes']['sender']['samples'], 0)
                self.assertGreater(config['process_memory']['processes']['receiver']['samples'], 0)
                self.assertEqual(result['process_memory'], config['process_memory'])
        # Legacy original scripts retain their explicitly documented bounded lifetime.
        events, result, config = self.exercise(legacy=True)
        self.assertEqual(events[0], ('receiver-limit', '12'))
        self.assertIn('legacy fixed', config['receiver_lifetime_policy'])
        self.assertNotIn(('stop', 'receiver'), events)

    def test_sender_timeout_is_bounded_and_cleanup_order_remains_sender_first(self):
        events, result, config = self.exercise(timeout=True)
        self.assertFalse(result['case_pass'])
        self.assertTrue(result['failure'].startswith('TimeoutError:'))
        self.assertEqual(events[-3:], [('stop', 'sender'), ('sleep', 2), ('stop', 'receiver')])
        self.assertEqual(config['sender_exit_code'], 0)
        self.assertEqual(config['receiver_exit_code'], 0)

    def test_optional_trace_setting_is_forwarded_only_when_explicit(self):
        for setting in (None, 'on', 'off'):
            with self.subTest(setting=setting):
                _, result, config = self.exercise(timing_trace=setting)
                self.assertIsNone(result['failure'])
                self.assertEqual(config['timing_trace_requested'], setting)
                command = config['sender_command']
                self.assertNotIn('--timing-trace', config['receiver_command'])
                if setting is None:
                    self.assertNotIn('--timing-trace', command)
                else:
                    self.assertEqual(command[command.index('--timing-trace') + 1], setting)
        _, result, config = self.exercise(legacy_sender=True)
        self.assertIsNone(result['failure'])
        self.assertNotIn('--timing-trace', config['sender_command'])
        self.assertNotIn('--run-id', config['sender_command'])

    def test_cli_rejects_explicit_trace_on_legacy_sender_before_starting_work(self):
        for setting in ('on', 'off'):
            with patch.object(run_diagnostics.sys, 'argv', ['run_diagnostics.py', '--legacy-sender',
                              '--timing-trace', setting]), patch('sys.stderr', new_callable=io.StringIO) as stderr, \
                 patch.object(run_diagnostics.subprocess, 'Popen') as launch:
                with self.assertRaises(SystemExit) as error:
                    run_diagnostics.main()
                self.assertEqual(error.exception.code, 2)
                self.assertIn('cannot be combined with --legacy-sender', stderr.getvalue())
                launch.assert_not_called()


if __name__ == '__main__':
    unittest.main()
