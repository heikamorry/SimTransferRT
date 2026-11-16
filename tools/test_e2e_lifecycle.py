"""Mock lifecycle tests; no socket traffic or real timing assertions."""
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import run_e2e


class LifecycleTests(unittest.TestCase):
    def exercise(self, timeout):
        events = []
        children = []
        class Process:
            def __init__(self, name):
                self.name, self.pid, self.returncode = name, 100 + len(children), None
            def poll(self):
                return self.returncode
            def wait(self, timeout):
                events.append(('wait', self.name, timeout))
                if self.name == 'sender' and fail_wait:
                    raise subprocess.TimeoutExpired('sender', timeout)
                self.returncode = 0
                return 0
        fail_wait = timeout
        def launch(command, **kwargs):
            name = 'receiver' if '--ready-file' in command else 'sender'
            if name == 'receiver':
                self.assertEqual(command[command.index('--seconds') + 1], '0')
                Path(command[command.index('--ready-file') + 1]).write_text('{}')
            child = Process(name)
            children.append(child)
            events.append(('launch', name))
            return child
        def stop(child):
            if child is not None and child.poll() is None:
                events.append(('stop', child.name))
                child.returncode = 0
            return False
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            with patch.object(run_e2e, 'free_ports', return_value=(30001, 30002)), \
                 patch.object(run_e2e.subprocess, 'Popen', side_effect=launch), \
                 patch.object(run_e2e, 'stop_owned', side_effect=stop), \
                 patch.object(run_e2e.time, 'sleep', side_effect=lambda seconds: events.append(('sleep', seconds))), \
                 patch.object(run_e2e, 'acceptance', return_value={
                     'functional_target_pass': True, 'functional_failures': [], 'reception': {}}):
                result = run_e2e.run_case(Path('sender.exe'), out, run_e2e.CASES[0])
        return events, result

    def test_receiver_lifetime_follows_bounded_sender_and_tail(self):
        events, result = self.exercise(False)
        self.assertTrue(result['pass'])
        self.assertEqual(events, [('launch', 'receiver'), ('launch', 'sender'),
                                  ('wait', 'sender', 6), ('sleep', .2), ('stop', 'receiver')])

    def test_timeout_preserves_failure_and_stops_sender_before_receiver(self):
        events, result = self.exercise(True)
        self.assertFalse(result['pass'])
        self.assertTrue(result['failure'].startswith('TimeoutExpired:'))
        self.assertEqual(events[-3:], [('stop', 'sender'), ('sleep', .2), ('stop', 'receiver')])


if __name__ == '__main__':
    unittest.main()
