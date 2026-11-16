"""Mocked OS-error tests are distinct from the Windows Winsock smoke test."""
import contextlib
import ctypes
import io
import json
import socket
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch
from rt_protocol import build_modules, encode_data
from runtime_support import atomic_json, configure_udp_connreset
from udp_feedback_stub import ReceiverRuntime, interrupt_handler, parser


def os_error(code):
    return OSError(code, 'injected OS error')


class Clock:
    now = 0.0
    def __call__(self):
        return self.now


class FakeSocket:
    def __init__(self, events=(), send_error=None, bind_error=None, close_error=None):
        self.events = list(events)
        self.send_error, self.bind_error, self.close_error = send_error, bind_error, close_error
        self.closed = False
    def fileno(self): return 0x123456789
    def setsockopt(self, *args): pass
    def getsockopt(self, *args): return 4194304
    def setblocking(self, value): pass
    def bind(self, address):
        if self.bind_error: raise self.bind_error
    def close(self):
        self.closed = True
        if self.close_error: raise self.close_error
    def recvfrom(self, size):
        if not self.events: raise BlockingIOError(10035, 'empty')
        value = self.events.pop(0)
        if isinstance(value, BaseException): raise value
        return value, ('127.0.0.1', 5500)
    def sendto(self, packet, endpoint):
        if self.send_error: raise self.send_error
        return len(packet)


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name)
        self.mask, self.body = build_modules({0: b'x'*5000})
        self.packets = [encode_data(1, 100, self.mask, self.body, i) for i in range(4)]

    def runtime(self, fake=None, extra=(), interrupt=False, option_status='configured'):
        fake = fake or FakeSocket()
        args = parser().parse_args(['--quiet', '--seconds', '0.05', '--report', str(self.path/'report.json'), *extra])
        clock = Clock()
        def select_fn(*args):
            clock.now += .001
            if interrupt: raise KeyboardInterrupt()
            return [fake], [], []
        with patch('udp_feedback_stub.provenance', return_value={'pid': 12}):
            runtime = ReceiverRuntime(args, clock=clock, socket_factory=lambda *args: fake, select_fn=select_fn)
        stdout, stderr = io.StringIO(), io.StringIO()
        option = dict(status=option_status, error_code=10022 if option_status=='failed' else None)
        with patch('udp_feedback_stub.configure_udp_connreset', return_value=option), \
                contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = runtime.run()
        self.assertEqual(stdout.getvalue(), '')  # Quiet never disables report/statistics.
        report = json.loads((self.path/'report.json').read_text()) if (self.path/'report.json').exists() else None
        return runtime, code, report, stderr.getvalue()

    def test_recv_reset_continues_and_reports_both_option_outcomes(self):
        for setting in ('configured', 'failed'):
            with self.subTest(setting=setting):
                _, code, report, _ = self.runtime(FakeSocket([os_error(10054), *self.packets]), option_status=setting)
                self.assertEqual(code, 0)
                self.assertEqual(report['recv_10054'], 1)
                self.assertEqual(report['completed'], 1)
                self.assertEqual(report['sio_udp_connreset']['status'], setting)

    def test_failed_nacks_consume_only_three_requests(self):
        fake = FakeSocket([p for i,p in enumerate(self.packets) if i != 2], send_error=os_error(10054))
        _, code, report, _ = self.runtime(fake)
        self.assertEqual(code, 0)
        self.assertEqual(report['send_10054'], 3)
        self.assertEqual(report['nack_packets'], 3)
        self.assertEqual(report['nack_send_attempts'], 3)
        self.assertEqual(report['nack_send_accepted'], 0)
        self.assertEqual(report['max_requests_for_fragment'], 3)
        self.assertEqual(report['expired'], 1)

    def test_send_success_separate_from_generated(self):
        _, code, report, _ = self.runtime(FakeSocket(self.packets[:1]), extra=['--duplicate-nack'])
        self.assertEqual(code, 0)
        self.assertEqual(report['nack_send_accepted'], report['nack_packets']*2)
        self.assertEqual(report['max_requests_for_fragment'], 3)

    def test_oversized_and_wouldblock_not_valid_data(self):
        fake = FakeSocket([os_error(10040), b'x'*1473, BlockingIOError(10035, 'empty'), *self.packets])
        _, code, report, _ = self.runtime(fake)
        self.assertEqual(code, 0)
        self.assertEqual(report['recv_10040'], 1)
        self.assertEqual(report['oversized_datagrams'], 1)
        self.assertEqual(report['invalid'], 1)
        self.assertEqual(report['completed'], 1)
        self.assertGreater(report['recv_would_block'], 0)

    def test_send_wouldblock_and_message_size_are_bounded(self):
        for code, counter in ((10035, 'send_would_block'), (10040, 'send_10040')):
            with self.subTest(code=code):
                _, result, report, _ = self.runtime(FakeSocket(self.packets[:1], send_error=os_error(code)))
                self.assertEqual(result, 0)
                self.assertEqual(report[counter], 3)

    def test_unknown_receive_error_saved_nonzero(self):
        _, code, report, stderr = self.runtime(FakeSocket([os_error(10050)]))
        self.assertEqual(code, 1)
        self.assertEqual(report['exit_reason'], 'exception')
        self.assertIn('10050', report['exception'])
        self.assertIn('10050', stderr)

    def test_unknown_send_error_saved_nonzero(self):
        _, code, report, _ = self.runtime(FakeSocket(self.packets[:1], send_error=os_error(10050)))
        self.assertEqual(code, 1)
        self.assertEqual(report['nack_send_failed'], 1)

    def test_bind_failure_and_close_failure_preserve_primary_exception(self):
        fake = FakeSocket(bind_error=os_error(10048), close_error=RuntimeError('close failed'))
        _, code, report, _ = self.runtime(fake)
        self.assertTrue(fake.closed)
        self.assertEqual(code, 1)
        self.assertIn('10048', report['exception'])
        self.assertIn('close failed', report['cleanup_errors'][0])

    def test_keyboard_interrupt_and_duration_save(self):
        for interrupt, reason in ((True, 'keyboard_interrupt'), (False, 'time_limit')):
            _, code, report, _ = self.runtime(interrupt=interrupt)
            self.assertEqual(code, 0)
            self.assertEqual(report['exit_reason'], reason)

    def test_report_directory_unwritable_is_error(self):
        _, code, report, stderr = self.runtime(extra=['--report', str(self.path/'absent'/'report.json')])
        self.assertEqual(code, 1)
        self.assertIn('ERROR saving receiver report', stderr)

    def test_trace_failure_does_not_skip_report(self):
        _, code, report, _ = self.runtime(extra=['--trace-nacks', str(self.path/'missing'/'trace.jsonl')])
        self.assertEqual(code, 1)
        self.assertTrue(report['cleanup_errors'])

    def test_trace_ring_and_ready_checkpoint(self):
        trace = self.path/'trace.jsonl'
        _, code, report, _ = self.runtime(FakeSocket(self.packets[:1]), extra=[
            '--trace-nacks', str(trace), '--trace-limit', '2', '--run-id', 'test-run',
            '--ready-file', str(self.path/'ready.json'), '--status-file', str(self.path/'status.json')])
        self.assertEqual(code, 0)
        self.assertEqual(len(trace.read_text().splitlines()), 2)
        self.assertGreater(report['trace_dropped'], 0)
        self.assertEqual(json.loads((self.path/'ready.json').read_text())['run_id'], 'test-run')
        self.assertTrue((self.path/'status.json').exists())

    def test_drain_never_decodes_or_generates_nacks(self):
        _, code, report, _ = self.runtime(FakeSocket([b'garbage', *self.packets]), extra=['--drain'])
        self.assertEqual(code, 0)
        self.assertEqual(report['drain_datagrams'], 5)
        self.assertEqual(report['datagrams'], 0)
        self.assertEqual(report['nack_send_attempts'], 0)

    def test_atomic_replace_keeps_previous_valid_file_on_failure(self):
        path = self.path/'atomic.json'
        atomic_json(path, {'old': True})
        with patch('runtime_support.os.replace', side_effect=PermissionError('injected')):
            with self.assertRaises(PermissionError): atomic_json(path, {'new': True})
        self.assertEqual(json.loads(path.read_text()), {'old': True})
        self.assertEqual(list(self.path.glob('*.tmp')), [])

    def test_report_generation_failure_preserves_primary_error(self):
        with patch.object(ReceiverRuntime, 'report', side_effect=ValueError('report assembly failed')):
            _, code, report, stderr = self.runtime(FakeSocket([os_error(10050)]))
        self.assertEqual(code, 1)
        self.assertIn('10050', report['exception'])
        self.assertIn('report assembly failed', report['report_generation_error'])
        self.assertIn('runtime_metrics', report)

    def test_ctrl_break_handler_uses_interrupt_cleanup_path(self):
        with self.assertRaises(KeyboardInterrupt):
            interrupt_handler(None, None)

    def test_optional_checkpoint_failure_recovers_on_regular_cadence(self):
        status_path = self.path/'status.json'
        attempts = []
        def write(path, value):
            if Path(path) == status_path:
                attempts.append(value['local_monotonic_s'])
                if len(attempts) == 1:
                    raise PermissionError(5, 'injected replacement denial')
            atomic_json(path, value)
        with patch('udp_feedback_stub.atomic_json', side_effect=write):
            _, code, report, stderr = self.runtime(FakeSocket(self.packets), extra=[
                '--status-file', str(status_path), '--checkpoint-every', '.01'])
        self.assertEqual(code, 0)
        self.assertEqual(report['completed'], 1)
        self.assertEqual(report['checkpoint_failures'], 1)
        self.assertGreater(report['checkpoint_successes'], 0)
        self.assertEqual(report['checkpoint_attempts'], len(attempts))
        self.assertTrue(all(b-a >= .010-1e-9 for a,b in zip(attempts, attempts[1:])))
        self.assertIn('replacement denial', report['last_checkpoint_error']['error'])
        self.assertIsNotNone(report['last_checkpoint_success_monotonic_s'])

    def test_persistent_optional_checkpoint_failure_is_bounded_not_fatal(self):
        status_path = self.path/'status.json'
        def write(path, value):
            if Path(path) == status_path:
                raise PermissionError(5, 'injected replacement denial')
            atomic_json(path, value)
        with patch('udp_feedback_stub.atomic_json', side_effect=write):
            _, code, report, stderr = self.runtime(FakeSocket(self.packets), extra=[
                '--status-file', str(status_path), '--checkpoint-every', '.01'])
        self.assertEqual(code, 0)
        self.assertEqual(report['completed'], 1)
        self.assertGreater(report['checkpoint_failures'], 0)
        self.assertLessEqual(report['checkpoint_attempts'], 6)
        self.assertEqual(report['checkpoint_successes'], 0)
        self.assertEqual(stderr.count('WARNING checkpoint:'), 1)
        self.assertIsNone(report['last_checkpoint_success_monotonic_s'])

    def test_final_report_replacement_failure_stays_fatal(self):
        with patch('udp_feedback_stub.atomic_json', side_effect=PermissionError(5, 'report denied')):
            _, code, report, stderr = self.runtime()
        self.assertEqual(code, 1)
        self.assertIsNone(report)
        self.assertIn('ERROR saving receiver report', stderr)

    @unittest.skipUnless(sys.platform == 'win32', 'real Windows file sharing only')
    def test_real_windows_reader_cannot_abort_optional_checkpoint(self):
        status_path = self.path/'status.json'
        atomic_json(status_path, {'previous': True})
        # Ordinary Python open holds a handle without FILE_SHARE_DELETE on this
        # Windows runtime. Reproduce the observer/read versus atomic replace race.
        with status_path.open('r', encoding='utf-8') as reader:
            _, code, report, stderr = self.runtime(FakeSocket(self.packets), extra=[
                '--status-file', str(status_path), '--checkpoint-every', '.01'])
            self.assertEqual(json.load(reader), {'previous': True})
        self.assertEqual(code, 0)
        self.assertEqual(report['completed'], 1)
        self.assertGreater(report['checkpoint_failures'], 0)
        self.assertEqual(report['checkpoint_successes'], 0)
        atomic_json(status_path, {'replacement_after_reader_close': True})
        self.assertTrue(json.loads(status_path.read_text())['replacement_after_reader_close'])


class ConnresetTests(unittest.TestCase):
    def test_native_signature_success_failure_and_pointer_width(self):
        for result, expected in ((0, 'configured'), (-1, 'failed')):
            ws = Mock()
            def ioctl(handle, control, value, size, *args):
                self.assertEqual(handle, 0x123456789)
                self.assertEqual(control, 0x9800000C)
                self.assertEqual(size, 4)
                self.assertEqual(ctypes.cast(value, ctypes.POINTER(ctypes.c_int32)).contents.value, 0)
                return result
            ws.WSAIoctl.side_effect = ioctl
            ws.WSAGetLastError.return_value = 10022
            actual = configure_udp_connreset(FakeSocket(), platform_name='win32', winsock=ws)
            self.assertEqual(actual['status'], expected)
            self.assertIs(ws.WSAIoctl.argtypes[0], ctypes.c_size_t)
            self.assertEqual(actual['error_code'], 10022 if result else None)

    def test_nonwindows_explicit(self):
        self.assertEqual(configure_udp_connreset(FakeSocket(), platform_name='linux')['status'], 'not_applicable')

    @unittest.skipUnless(sys.platform == 'win32', 'real Windows Winsock only')
    def test_real_windows_wsaioctl(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            self.assertEqual(configure_udp_connreset(sock)['status'], 'configured')


if __name__ == '__main__':
    unittest.main(verbosity=2)
