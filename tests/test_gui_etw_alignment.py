"""Deterministic host-clock, thread identity and coverage checks for GUI ETW alignment."""

import runpy
import unittest
from pathlib import Path


ALIGN = runpy.run_path(str(Path(__file__).resolve().parents[1] / "tools/handoff_etw/align-gui.py"))


def event(us, kind, old_state=-1):
    return {"qpc": str(us * 10), "host_us": str(us), "kind": kind, "target_tid": "42",
            "thread_pid": "13", "cpu": "0", "old_state": str(old_state), "old_wait_reason": "4",
            "ready_flags": "0", "version": "5" if kind.startswith("switch_") else "2"}


def module_frame(module, times):
    row = dict(zip(ALIGN["STAMPS"], times))
    row.update(frame_id=module * 10, drain="frame_event", queue_depth_at_enqueue=module,
               complete_to_update_us=times[-1] - times[0])
    for name, begin, end in zip(ALIGN["STAGES"], times, times[1:]):
        row[name + "_us"] = end - begin
    return {"module_id": module, "max_complete_to_widget_update_us": times[-1] - times[0],
            "worst_frame": row}


class GuiAlignmentTest(unittest.TestCase):
    def setUp(self):
        self.meta = {"clock": "qpc", "frequency": 10_000_000, "coverage_start_host_us": 980,
                     "coverage_end_host_us": 1120, "events_lost": 0, "buffers_lost": 0,
                     "short_records": 0}
        self.events = [event(980, "thread_metadata"), event(990, "switch_in"),
                       event(1015, "switch_out", 5), event(1035, "ready_thread"),
                       event(1040, "ready_thread"), event(1060, "switch_in")]
        self.modules = [module_frame(1, (1007, 1010, 1080, 1082, 1090)),
                        module_frame(2, (1005, 1008, 1080, 1080, 1081))]
        self.report = {"receiver": {"ui_observations": {"modules": self.modules}},
                       "sender": {"run_id": "synthetic", "fragment_trace": {
                           "enabled": True, "qpc_frequency_hz": "10000000",
                           "process_id": 13, "sender_tid": 70, "producer_tids": {"1": 71, "2": 72}}}}

    def aligned(self, pid=13, tid=42):
        return ALIGN["align"](self.report, pid, tid, self.events, self.meta)

    def test_same_batch_keeps_each_window_and_wait_reason(self):
        ndi, ep = self.aligned()["frames"]
        queue = ndi["stages"]["enqueue_to_batch"]
        self.assertTrue(ndi["queue_attribution_usable"])
        self.assertEqual(queue["etw_us"], {"running": 25, "ready": 25, "waiting": 20, "unknown": 0})
        segments = queue["longest_observed_segments"]
        self.assertEqual((segments[0]["state"], segments[0]["begin_host_us"],
                          segments[0]["end_host_us"]), ("ready", 1035, 1060))
        waiting = next(seg for seg in segments if seg["state"] == "waiting")
        self.assertEqual(waiting["basis"], ["CSwitch out, old_state=5, old_wait_reason=4"])
        self.assertEqual(ep["stages"]["batch_to_apply"]["elapsed_us"], 0)
        self.assertEqual(ep["stages"]["enqueue_to_batch"]["etw_us"]["running"], 27)

    def test_partial_window_and_missing_metadata_are_labeled(self):
        self.events.pop(0)
        self.meta["coverage_start_host_us"] = 1020
        result = self.aligned()
        self.assertFalse(result["etw_pid_verified"])
        frame = result["frames"][0]
        self.assertFalse(frame["queue_attribution_usable"])
        self.assertEqual(frame["stages"]["enqueue_to_batch"]["outside_capture_us"], 10)
        self.assertEqual(frame["stages"]["enqueue_to_batch"]["etw_us"]["unknown"], 25)

    def test_wrong_thread_and_report_clock_or_identity_rejected(self):
        for pid, tid in ((9, 42), (13, 70), (13, 71), (13, 43)):
            with self.subTest(pid=pid, tid=tid), self.assertRaises(ValueError):
                self.aligned(pid, tid)
        self.meta["frequency"] = 3_000_000
        with self.assertRaisesRegex(ValueError, "frequencies differ"):
            self.aligned()
        self.meta["frequency"] = 10_000_000
        self.events[0]["thread_pid"] = "14"
        with self.assertRaisesRegex(ValueError, "other PID"):
            self.aligned()

    def test_gui_clock_read_conflict_and_lost_events(self):
        self.events.append(event(1070, "switch_out", 5))
        self.meta["events_lost"] = 1
        frame = self.aligned()["frames"][0]
        self.assertFalse(frame["queue_attribution_usable"])
        self.assertEqual(frame["gui_clock_read_conflicts"], list(ALIGN["STAMPS"][2:]))
        self.assertEqual(len(frame["limitations"]), 2)

    def test_absent_frames_and_mixed_frame_measurements(self):
        self.modules[0]["worst_frame"]["complete_to_enqueue_us"] += 1
        with self.assertRaisesRegex(ValueError, "differs"):
            self.aligned()
        self.modules[0]["worst_frame"] = None
        self.assertEqual(len(self.aligned()["frames"]), 1)
        self.modules[1]["max_complete_to_widget_update_us"] += 1
        with self.assertRaisesRegex(ValueError, "Worst frame"):
            self.aligned()
        self.modules[1]["worst_frame"] = None
        with self.assertRaisesRegex(ValueError, "No module"):
            self.aligned()

    def test_qt_callback_crosses_window_and_scheduler_transitions(self):
        self.modules.pop()
        window = {"frame_id": 10, "start_us": 1010, "end_us": 1080,
                  "covered_us": 45, "uncovered_us": 25,
                  "events": [{"start_us": 1002, "end_us": 1040, "overlap_us": 30,
                              "event_type": 1, "event_name": "Timer", "target_class": "QTimer"},
                             {"start_us": 1065, "end_us": 1099, "overlap_us": 15,
                              "event_type": 12, "event_name": "Paint", "target_class": "QWidget"}],
                  "gaps": [{"start_us": 1040, "end_us": 1065, "reason": "no_long_notify_observed"}]}
        self.report["gui_event_trace"] = {"started_us": 980, "ended_us": 1120,
                                           "modules": [{"module_id": 1, "queue_window": window}]}
        part = self.aligned()["frames"][0]["stages"]["enqueue_to_batch"]["qt_notify"]
        self.assertEqual(part["callback_etw_us"], {"running": 20, "ready": 5, "waiting": 20, "unknown": 0})
        self.assertEqual(part["gap_etw_us"], {"running": 5, "ready": 20, "waiting": 0, "unknown": 0})
        first, gap, last = part["intervals"]
        self.assertEqual(first["start_us"], 1002)
        self.assertEqual(first["overlap_begin_us"], 1010)
        self.assertEqual(gap["reason"], "no_long_notify_observed")
        self.assertEqual(last["overlap_end_us"], 1080)
        window["frame_id"] = 99
        with self.assertRaisesRegex(ValueError, "does not match"):
            self.aligned()
        window["frame_id"] = 10
        window["gaps"][0]["start_us"] -= 1
        with self.assertRaisesRegex(ValueError, "do not partition"):
            self.aligned()

    def test_inherited_wake_and_timer_before_post_returns(self):
        f = self.modules[0]["worst_frame"]
        wake = {"sequence": 2, "origin_module_id": 2, "origin_frame_id": 20, "inherited": True,
                "post_started_us": 1000, "post_returned_us": 1001, "event_entered_us": 1079,
                "drain_event_sequence": 2, "drained_by_this_event": True}
        f["wake"] = wake
        obs = self.aligned()["frames"][0]["wake"]
        self.assertEqual((obs["enqueue_to_post_start_us"], obs["event_entry_to_batch_us"]), (-10, 1))
        f["drain"] = "timer"
        wake.update(post_started_us=1081, post_returned_us=1088, event_entered_us=None,
                    drain_event_sequence=None, drained_by_this_event=False)
        obs = self.aligned()["frames"][0]["wake"]
        self.assertEqual(obs["post_return_to_batch_us"], -8)
        self.assertIsNone(obs["event_entry_to_batch_us"])
        wake["drained_by_this_event"] = True
        with self.assertRaisesRegex(ValueError, "not entered"):
            self.aligned()


if __name__ == "__main__":
    unittest.main()
