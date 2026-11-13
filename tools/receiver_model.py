"""Bounded diagnostic reassembly/NACK model; NOT the formal receiving platform/renderer."""
from __future__ import annotations
from collections import OrderedDict
from dataclasses import dataclass, field
from rt_protocol import Data, ProtocolError, decode_data, decode_modules, nack_packets, newer

@dataclass
class FrameState:
    first: Data
    last_new: float
    parts: dict[int, bytes] = field(default_factory=dict)
    missing: dict[int, tuple[int, float]] = field(default_factory=dict)
    highest: int = -1

    def mark_tail(self):
        for i in range(self.highest + 1, self.first.count):
            if i not in self.parts:
                self.missing.setdefault(i, (0, 0.0))

class ReceiverModel:
    def __init__(self, on_complete=None):
        self.frames: OrderedDict[int, FrameState] = OrderedDict()
        self.terminal: OrderedDict[int, str] = OrderedDict()
        self.stats = dict(datagrams=0, invalid=0, duplicate=0, completed=0, expired=0,
                          requested_fragments=0, nack_packets=0, recovered=0, retry_flag=0,
                          metadata_failures=0, module_failures=0, total_bytes=0,
                          max_requests_for_fragment=0, active_evictions=0)
        self.on_complete = on_complete

    def retire(self, logic, status):
        self.frames.pop(logic, None)
        self.terminal[logic] = status
        while len(self.terminal) > 256:
            self.terminal.popitem(last=False)

    def ingest(self, packet: bytes, now: float):
        self.stats['datagrams'] += 1
        try:
            d = decode_data(packet)
        except ProtocolError:
            self.stats['invalid'] += 1
            return
        if d.flags:
            self.stats['retry_flag'] += 1
        if d.logic_id in self.terminal:
            self.stats['duplicate'] += 1
            return
        for logic, state in self.frames.items():
            if newer(d.logic_id, logic):
                state.mark_tail()
        if d.logic_id not in self.frames:
            if len(self.frames) >= 32:
                old = next(iter(self.frames))
                self.retire(old, 'resource-limit')
                self.stats['active_evictions'] += 1
            self.frames[d.logic_id] = FrameState(d, now)
        s = self.frames[d.logic_id]
        f = s.first
        if (d.frame_len, d.count, d.ts, d.mask) != (f.frame_len, f.count, f.ts, f.mask):
            self.stats['metadata_failures'] += 1
            self.retire(d.logic_id, 'metadata-mismatch')
            return
        if d.index in s.parts:
            self.stats['duplicate'] += 1
            return
        if d.index > s.highest + 1:
            for i in range(s.highest + 1, d.index):
                if i not in s.parts:
                    s.missing.setdefault(i, (0, 0.0))
        if d.index in s.missing:
            if s.missing[d.index][0] > 0:
                self.stats['recovered'] += 1  # Spec: requested fragment arriving, independent of flags.
            del s.missing[d.index]
        s.parts[d.index] = d.payload
        s.highest = max(s.highest, d.index)
        s.last_new = now
        if len(s.parts) == d.count:
            body = b''.join(s.parts[i] for i in range(d.count))
            try:
                modules = decode_modules(d.mask, body)
            except ProtocolError:
                self.stats['module_failures'] += 1
                self.retire(d.logic_id, 'module-invalid')
                return
            self.stats['completed'] += 1
            self.stats['total_bytes'] += len(body)
            if self.on_complete:
                self.on_complete(d, body, modules)
            self.retire(d.logic_id, 'complete')

    def poll(self, now: float) -> list[bytes]:
        result = []
        for logic, s in list(self.frames.items()):
            if now - s.last_new >= 0.002:
                s.mark_tail()
            if any(attempts == 3 and now - last >= 0.010 - 1e-9
                   for attempts, last in s.missing.values()):
                self.stats['expired'] += 1
                self.retire(logic, 'three-requests-exhausted')
                continue
            due = []
            for index, (attempts, last) in list(s.missing.items()):
                if attempts < 3 and (attempts == 0 or now - last >= 0.010 - 1e-9):
                    due.append(index)
                    s.missing[index] = attempts + 1, now
                    self.stats['max_requests_for_fragment'] = max(self.stats['max_requests_for_fragment'], attempts + 1)
            if due:
                result.extend(nack_packets(logic, due))
                self.stats['requested_fragments'] += len(due)
        self.stats['nack_packets'] += len(result)
        return result
