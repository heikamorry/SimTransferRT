"""Bounded diagnostic reassembly/NACK model, not a production receiver.

Times are user-space monotonic processing times, not kernel/NIC arrival times.
Request budgets are consumed when NACKs are generated, even if sendto fails.
Serial arithmetic assumes less than 2**31 frames of ambiguity; restart/full-cycle
ID reuse needs coordinated reset because the protocol has no session identifier.
"""
from __future__ import annotations
from bisect import bisect_left
from collections import OrderedDict, deque
from dataclasses import dataclass, field
import heapq
import time
from rt_protocol import Data, ProtocolError, decode_data, decode_modules, nack_packets, newer


class FrameRanges:
    """Sorted inclusive uint32 ranges, bounded diagnostic inventory, not state."""
    def __init__(self, limit=16384):
        self.ranges, self.limit, self.dropped = [], limit, 0

    def add(self, logic):
        ranges = self.ranges
        pos = bisect_left(ranges, [logic, logic])
        if pos and ranges[pos - 1][1] >= logic:
            return
        if pos < len(ranges) and ranges[pos][0] == logic:
            return
        left = pos > 0 and ranges[pos - 1][1] + 1 == logic
        right = pos < len(ranges) and ranges[pos][0] == logic + 1
        if left and right:
            ranges[pos - 1][1] = ranges[pos][1]
            del ranges[pos]
        elif left:
            ranges[pos - 1][1] = logic
        elif right:
            ranges[pos][0] = logic
        elif len(ranges) < self.limit:
            ranges.insert(pos, [logic, logic])
        else:
            self.dropped += 1


class TimingHistogram:
    """All observations, fixed 100us buckets through 100ms plus overflow."""
    BUCKET_US, LAST_REGULAR_BUCKET = 100, 1000

    def __init__(self):
        self.bins = [0] * (self.LAST_REGULAR_BUCKET + 2)
        self.count = self.total_us = self.maximum_us = 0
        self.minimum_us = None

    def add(self, seconds):
        value = max(0, int(round(seconds * 1_000_000)))
        self.count += 1
        self.total_us += value
        self.maximum_us = max(self.maximum_us, value)
        self.minimum_us = value if self.minimum_us is None else min(self.minimum_us, value)
        bucket = min(len(self.bins) - 1, (value + self.BUCKET_US - 1) // self.BUCKET_US)
        self.bins[bucket] += 1

    def summary(self):
        result = dict(observed=bool(self.count), count=self.count,
                      min_us=self.minimum_us, max_us=self.maximum_us if self.count else None,
                      mean_us=self.total_us / self.count if self.count else None,
                      sum_us=self.total_us, overflow_count=self.bins[-1])
        for name, numerator, denominator in (('p50_us', 50, 100), ('p95_us', 95, 100),
                                              ('p99_us', 99, 100), ('p999_us', 999, 1000)):
            value, accumulated = None, 0
            if self.count:
                target = (self.count * numerator + denominator - 1) // denominator
                for index, count in enumerate(self.bins):
                    accumulated += count
                    if accumulated >= target:
                        value = self.maximum_us if index == len(self.bins) - 1 else min(self.maximum_us, index * self.BUCKET_US)
                        break
            result[name] = value
        return result


@dataclass
class MissingTiming:
    discovered: float
    first_eligible_poll: int
    first_request: float | None = None


@dataclass
class FrameState:
    first: Data
    last_new: float
    parts: dict[int, bytes] = field(default_factory=dict)
    missing: dict[int, tuple[int, float]] = field(default_factory=dict)
    highest: int = -1
    tail_marked: bool = False
    timer_due: float | None = None
    timer_generation: int = 0
    expiry_due: float | None = None
    first_seen: float = 0.0
    missing_timing: dict[int, MissingTiming] = field(default_factory=dict)


class ReceiverModel:
    TAIL_SECONDS, RETRY_SECONDS, EPSILON = 0.002, 0.010, 1e-9
    TIMING_EVENT_CAPACITY, TIMING_EVENT_FRAGMENT_LIMIT = 128, 8

    def __init__(self, on_complete=None, *, clock=time.monotonic,
                 replay_window=4096, max_active=32):
        if replay_window < 256 or max_active < 1:
            raise ValueError('replay_window >= 256 and max_active >= 1 required')
        self.frames: OrderedDict[int, FrameState] = OrderedDict()
        self.terminal: OrderedDict[int, str] = OrderedDict()
        self.stats = dict(datagrams=0, invalid=0, duplicate=0, completed=0, expired=0,
                          requested_fragments=0, nack_packets=0, recovered=0, retry_flag=0,
                          metadata_failures=0, module_failures=0, total_bytes=0,
                          max_requests_for_fragment=0, active_evictions=0,
                          valid=0, crc_failures=0, geometry_failures=0, late=0,
                          stale_window_rejections=0, active_high_water=0,
                          tail_registrations=0, timer_high_water=0,
                          timer_compactions=0, max_timer_lateness_us=0)
        self.on_complete, self.clock = on_complete, clock
        self.max_active, self.replay_window = max_active, replay_window
        self._latest, self._retired_bits = None, 0
        self._replay_mask = (1 << replay_window) - 1
        self._timers, self._timer_serial = [], 0
        self._ready = OrderedDict()
        self._last_now = None
        self._events = dict(first_received=None, last_received=None,
                            first_completed=None, last_completed=None)
        self._manifest = {key: FrameRanges() for key in
                          ('seen', 'completed', 'expired', 'resource_limit',
                           'metadata_mismatch', 'module_invalid')}
        self._poll_sequence = 0
        self._timing_histograms = {name: TimingHistogram() for name in (
            'discovery_to_first_request', 'request_interval', 'request_scheduler_lateness',
            'silence_tail_lateness', 'newer_frame_to_tail_discovery',
            'tail_silence_age', 'tail_newer_frame_age', 'third_request_to_expiry',
            'missing_discovery_to_frame_expiry', 'first_request_to_recovery',
            'last_request_to_recovery', 'frame_first_seen_to_complete',
            'frame_first_seen_to_expiry')}
        self._timing_evidence = dict(discovered_fragments=0, first_requests=0,
            retry_intervals=0, early_retry_intervals=0, third_expiry_fragments=0,
            early_expiries=0, first_request_same_poll_violations=0,
            request_budget_exceeded=0, expired_frames=0, completed_frames=0,
            recovered_fragments=0, recovered_without_retry_flag=0,
            resolved_before_first_request=0, tail_silence_discoveries=0,
            tail_newer_frame_discoveries=0, late_request_deadlines=0,
            cancelled_missing_fragments=0)
        self._timing_events = deque(maxlen=self.TIMING_EVENT_CAPACITY)
        self._timing_event_total = 0

    @staticmethod
    def _timestamp_us(now):
        return int(round(now * 1_000_000))

    def _timing_event(self, kind, logic, now, **details):
        self._timing_event_total += 1
        self._timing_events.append(dict(sequence=self._timing_event_total, kind=kind,
            logic_id=logic, at_us=self._timestamp_us(now), **details))

    def _discover(self, logic, state, indices, now, reason, eligible_poll):
        details, added = [], 0
        for index in indices:
            if index in state.missing:
                continue
            state.missing[index] = (0, 0.0)
            state.missing_timing[index] = MissingTiming(now, eligible_poll)
            added += 1
            if len(details) < self.TIMING_EVENT_FRAGMENT_LIMIT:
                details.append(dict(fragment=index, discovered_us=self._timestamp_us(now),
                                    first_eligible_poll=eligible_poll))
        self._timing_evidence['discovered_fragments'] += added
        if added:
            self._timing_event('missing_discovered', logic, now, reason=reason,
                fragments_total=added, fragments=details, fragments_dropped=added-len(details))
        return added

    def _request_timing(self, state, index, previous_attempts, previous_last, now):
        metadata = state.missing_timing[index]
        ordinal = previous_attempts + 1
        due = metadata.discovered if ordinal == 1 else previous_last + self.RETRY_SECONDS
        evidence, histograms = self._timing_evidence, self._timing_histograms
        if ordinal == 1:
            metadata.first_request = now
            evidence['first_requests'] += 1
            evidence['first_request_same_poll_violations'] += self._poll_sequence != metadata.first_eligible_poll
            histograms['discovery_to_first_request'].add(now - metadata.discovered)
        else:
            evidence['retry_intervals'] += 1
            evidence['early_retry_intervals'] += now - previous_last < self.RETRY_SECONDS - self.EPSILON
            histograms['request_interval'].add(now - previous_last)
        evidence['request_budget_exceeded'] += ordinal > 3
        evidence['late_request_deadlines'] += now > due + self.EPSILON
        histograms['request_scheduler_lateness'].add(now - due)
        return metadata, ordinal, due

    def _retirement_timing(self, logic, state, status, now):
        histograms, evidence = self._timing_histograms, self._timing_evidence
        if status == 'complete':
            evidence['completed_frames'] += 1
            histograms['frame_first_seen_to_complete'].add(now - state.first_seen)
            self._timing_event('frame_complete', logic, now,
                               first_seen_us=self._timestamp_us(state.first_seen), ts=state.first.ts)
            return
        if status != 'three-requests-exhausted':
            evidence['cancelled_missing_fragments'] += len(state.missing)
            self._timing_event('frame_retired', logic, now, reason=status,
                               missing_fragments=len(state.missing))
            return
        evidence['expired_frames'] += 1
        histograms['frame_first_seen_to_expiry'].add(now - state.first_seen)
        earliest = min((item.discovered for item in state.missing_timing.values()), default=now)
        histograms['missing_discovery_to_frame_expiry'].add(now - earliest)
        expired_count, details = 0, []
        for index, (attempts, last) in state.missing.items():
            if attempts != 3 or now - last < self.RETRY_SECONDS - self.EPSILON:
                continue
            expired_count += 1
            histograms['third_request_to_expiry'].add(now - last)
            if len(details) < self.TIMING_EVENT_FRAGMENT_LIMIT:
                metadata = state.missing_timing[index]
                details.append(dict(fragment=index, request_ordinal=attempts,
                    discovered_us=self._timestamp_us(metadata.discovered),
                    third_request_us=self._timestamp_us(last),
                    due_us=self._timestamp_us(last + self.RETRY_SECONDS)))
        evidence['third_expiry_fragments'] += expired_count
        evidence['early_expiries'] += expired_count == 0
        evidence['cancelled_missing_fragments'] += len(state.missing) - expired_count
        self._timing_event('frame_expired', logic, now,
            first_seen_us=self._timestamp_us(state.first_seen),
            earliest_still_missing_discovered_us=self._timestamp_us(earliest),
            missing_fragments=len(state.missing), fragments_total=expired_count,
            fragments=details, fragments_dropped=expired_count-len(details))

    def _now(self, now):
        now = self.clock() if now is None else now
        if self._last_now is not None and now < self._last_now:
            raise ValueError('receiver clock moved backwards')
        self._last_now = now
        return now

    def _event(self, kind, logic, now):
        event = dict(logic_id=logic, monotonic_s=now)
        if self._events['first_' + kind] is None:
            self._events['first_' + kind] = event
        self._events['last_' + kind] = event

    def _advance(self, logic):
        if self._latest is None:
            self._latest = logic
            return True
        if newer(logic, self._latest):
            delta = (logic - self._latest) & 0xFFFFFFFF
            self._retired_bits = (0 if delta >= self.replay_window else
                                  (self._retired_bits << delta) & self._replay_mask)
            self._latest = logic
            return True
        return False

    def _is_retired(self, logic):
        if self._latest is None or newer(logic, self._latest):
            return False
        distance = (self._latest - logic) & 0xFFFFFFFF
        # Active frames bypass this fence. Unseen older frames inside the window
        # remain eligible; merely being older than the latest is not rejection.
        if distance >= self.replay_window:
            self.stats['stale_window_rejections'] += 1
            return True
        return bool(self._retired_bits & (1 << distance))

    def _compact_timers(self):
        # At most one live event per frame; stale heap entries have a fixed bound.
        if len(self._timers) > max(64, 4 * self.max_active):
            self._timers = [(s.timer_due, s.timer_generation, logic)
                            for logic, s in self.frames.items() if s.timer_due is not None]
            heapq.heapify(self._timers)
            self.stats['timer_compactions'] += 1

    def _schedule(self, logic, state, due):
        if state.timer_due is not None and state.timer_due <= due:
            return
        self._timer_serial += 1
        state.timer_generation = self._timer_serial
        state.timer_due = due
        heapq.heappush(self._timers, (due, state.timer_generation, logic))
        self._compact_timers()
        self.stats['timer_high_water'] = max(self.stats['timer_high_water'], len(self._timers))

    def _mark_tail(self, logic, state, now, reason='silence', eligible_poll=None):
        if state.tail_marked:
            return
        state.tail_marked = True
        self.stats['tail_registrations'] += 1
        eligible_poll = self._poll_sequence + 1 if eligible_poll is None else eligible_poll
        added = self._discover(logic, state, range(state.highest + 1, state.first.count),
                               now, 'tail_' + reason, eligible_poll)
        if added:
            self._timing_evidence['tail_' + reason + '_discoveries'] += 1
            self._timing_histograms['tail_' + reason + '_age'].add(now - state.last_new)
            if reason == 'silence':
                self._timing_histograms['silence_tail_lateness'].add(now - state.last_new - self.TAIL_SECONDS)
            else:
                # The newer-frame predicate is handled in the same ingest call.
                # This is a logical processing observation, not wire arrival latency.
                self._timing_histograms['newer_frame_to_tail_discovery'].add(0)
        if state.missing:
            self._ready[logic] = None

    def retire(self, logic, status):
        state = self.frames.pop(logic, None)
        if state is not None:
            self._retirement_timing(logic, state, status, self._last_now)
        self._ready.pop(logic, None)
        self.terminal[logic] = status
        while len(self.terminal) > 256:
            self.terminal.popitem(last=False)
        if self._latest is not None:
            distance = (self._latest - logic) & 0xFFFFFFFF
            if distance < self.replay_window:
                self._retired_bits |= 1 << distance
        key = {'complete': 'completed', 'three-requests-exhausted': 'expired',
               'resource-limit': 'resource_limit', 'metadata-mismatch': 'metadata_mismatch',
               'module-invalid': 'module_invalid'}[status]
        self._manifest[key].add(logic)
        # Removal of the owning FrameState invalidates all of its timer tasks.
        self._compact_timers()

    def ingest(self, packet: bytes, now: float | None = None):
        now = self._now(now)
        self.stats['datagrams'] += 1
        try:
            d = decode_data(packet)
        except ProtocolError as error:
            self.stats['invalid'] += 1
            self.stats['crc_failures' if str(error) == 'CRC32' else 'geometry_failures'] += 1
            return
        self.stats['valid'] += 1
        self._event('received', d.logic_id, now)
        if d.flags:
            self.stats['retry_flag'] += 1
        pending = self.frames.get(d.logic_id)
        if pending is not None and pending.expiry_due is not None and now >= pending.expiry_due - self.EPSILON:
            # A packet dequeued after the final deadline cannot rescue a frame
            # merely because the runtime has not called poll yet this turn.
            self.stats['expired'] += 1
            self.retire(d.logic_id, 'three-requests-exhausted')
        if d.logic_id not in self.frames and self._is_retired(d.logic_id):
            self.stats['duplicate'] += 1
            self.stats['late'] += 1
            return
        advanced = self._advance(d.logic_id)
        if advanced:
            # Only a genuinely newer frame can newly expose tails; not every DATA.
            for logic, state in self.frames.items():
                if newer(d.logic_id, logic):
                    self._mark_tail(logic, state, now, 'newer_frame')
        if d.logic_id not in self.frames:
            if len(self.frames) >= self.max_active:
                self.retire(next(iter(self.frames)), 'resource-limit')
                self.stats['active_evictions'] += 1
            self.frames[d.logic_id] = FrameState(d, now, first_seen=now)
            self._manifest['seen'].add(d.logic_id)
            self.stats['active_high_water'] = max(self.stats['active_high_water'], len(self.frames))
            self._schedule(d.logic_id, self.frames[d.logic_id], now + self.TAIL_SECONDS)
        s = self.frames[d.logic_id]
        f = s.first
        if (d.frame_len, d.count, d.ts, d.mask) != (f.frame_len, f.count, f.ts, f.mask):
            self.stats['metadata_failures'] += 1
            self.retire(d.logic_id, 'metadata-mismatch')
            return
        if d.index in s.parts:
            self.stats['duplicate'] += 1
            return  # Duplicates never renew the tail inactivity clock.
        if not s.tail_marked and d.index > s.highest + 1:
            self._discover(d.logic_id, s, range(s.highest + 1, d.index), now,
                           'gap', self._poll_sequence + 1)
            self._ready[d.logic_id] = None
        if d.index in s.missing:
            previous_attempts, previous_last = s.missing[d.index]
            metadata = s.missing_timing.pop(d.index)
            if previous_attempts > 0:
                self.stats['recovered'] += 1  # Requested arrival, including flags=0.
                self._timing_evidence['recovered_fragments'] += 1
                self._timing_evidence['recovered_without_retry_flag'] += not bool(d.flags)
                self._timing_histograms['first_request_to_recovery'].add(now - metadata.first_request)
                self._timing_histograms['last_request_to_recovery'].add(now - previous_last)
                self._timing_event('fragment_recovered', d.logic_id, now, fragment=d.index,
                    discovered_us=self._timestamp_us(metadata.discovered),
                    first_request_us=self._timestamp_us(metadata.first_request),
                    last_request_us=self._timestamp_us(previous_last),
                    request_ordinal=previous_attempts, retry_flag=bool(d.flags))
            else:
                self._timing_evidence['resolved_before_first_request'] += 1
            del s.missing[d.index]
            if previous_attempts == 3 and previous_last + self.RETRY_SECONDS == s.expiry_due:
                s.expiry_due = min((last + self.RETRY_SECONDS
                                    for attempts, last in s.missing.values() if attempts == 3),
                                   default=None)
        s.parts[d.index] = d.payload
        s.highest = max(s.highest, d.index)
        s.last_new = now
        if newer(self._latest, d.logic_id):
            self._mark_tail(d.logic_id, s, now, 'newer_frame')
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
            self._event('completed', d.logic_id, now)
            self.retire(d.logic_id, 'complete')
            if self.on_complete:
                self.on_complete(d, body, modules)

    def poll(self, now: float | None = None) -> list[bytes]:
        now = self._now(now)
        self._poll_sequence += 1
        due_frames = self._ready
        self._ready = OrderedDict()
        while self._timers and self._timers[0][0] <= now + self.EPSILON:
            deadline, generation, logic = heapq.heappop(self._timers)
            s = self.frames.get(logic)
            if s is None or s.timer_generation != generation or s.timer_due != deadline:
                continue
            s.timer_due = None
            self.stats['max_timer_lateness_us'] = max(
                self.stats['max_timer_lateness_us'], max(0, int((now - deadline) * 1e6)))
            due_frames[logic] = None
        result = []
        for logic in due_frames:
            s = self.frames.get(logic)
            if s is None:
                continue
            if not s.tail_marked and now - s.last_new >= self.TAIL_SECONDS - self.EPSILON:
                self._mark_tail(logic, s, now, eligible_poll=self._poll_sequence)
                self._ready.pop(logic, None)  # Serviced in this processing round.
            if any(attempts == 3 and now - last >= self.RETRY_SECONDS - self.EPSILON
                   for attempts, last in s.missing.values()):
                self.stats['expired'] += 1
                self.retire(logic, 'three-requests-exhausted')
                continue
            due, request_details, ordinals = [], [], [0, 0, 0]
            next_due = None if s.tail_marked else s.last_new + self.TAIL_SECONDS
            for index, (attempts, last) in s.missing.items():
                if attempts < 3 and (attempts == 0 or now - last >= self.RETRY_SECONDS - self.EPSILON):
                    due.append(index)
                    metadata, ordinal, request_due = self._request_timing(s, index, attempts, last, now)
                    ordinals[ordinal - 1] += 1
                    if len(request_details) < self.TIMING_EVENT_FRAGMENT_LIMIT:
                        request_details.append(dict(fragment=index, request_ordinal=ordinal,
                            discovered_us=self._timestamp_us(metadata.discovered),
                            request_us=self._timestamp_us(now), due_us=self._timestamp_us(request_due)))
                    attempts += 1
                    last = now
                    s.missing[index] = attempts, last
                    self.stats['max_requests_for_fragment'] = max(self.stats['max_requests_for_fragment'], attempts)
                deadline = last + self.RETRY_SECONDS
                next_due = deadline if next_due is None else min(deadline, next_due)
                if attempts == 3:
                    s.expiry_due = deadline if s.expiry_due is None else min(s.expiry_due, deadline)
            if due:
                result.extend(nack_packets(logic, due))
                self.stats['requested_fragments'] += len(due)
                self._timing_event('request_round', logic, now, poll_sequence=self._poll_sequence,
                    fragments_total=len(due), requests_by_ordinal=ordinals,
                    fragments=request_details, fragments_dropped=len(due)-len(request_details))
            if next_due is not None:
                self._schedule(logic, s, next_due)
        self.stats['nack_packets'] += len(result)
        return result

    def timing_report(self):
        return dict(version=1, units='us',
            clock='user-space monotonic processing timestamps; not NIC/kernel arrival or cross-host latency',
            policy=dict(retry_interval_us=10000, tail_silence_us=2000, maximum_requests=3,
                third_request_wait_us=10000, nominal_discovery_to_expiry_us=30000,
                scheduler_epsilon_us=self.EPSILON * 1_000_000,
                first_request_round='first available poll after ingest discovery, or current poll for silence discovery',
                no_observations='observed=false and null extrema/quantiles: not measured, never a passing timing observation',
                duration_resolution='nearest microsecond; nonnegative durations clamp sub-epsilon negative lateness to zero; early counters compare original seconds with scheduler epsilon',
                quantiles='all observations; fixed 100us bucket upper bounds through 100ms; overflow uses observed maximum',
                request_budget='generation consumes a request even if local sendto fails; duplicate-nack injection may transmit extra copies',
                newer_frame_timing='predicate and registration share the ingest processing timestamp, so delay is zero by construction; tail_newer_frame_age measures time since last unique fragment',
                frame_expiry='earliest still-missing discovery to frame retirement; third_request_to_expiry includes only actually due third-request fragments',
                strict_deadline_claim=False),
            denominators=dict(discovery_to_first_request='first_requests', request_interval='retry_intervals',
                request_scheduler_lateness='first_requests + retry_intervals',
                silence_tail_lateness='tail_silence_discoveries (frames with newly discovered tail fragments)',
                newer_frame_to_tail_discovery='tail_newer_frame_discoveries (frames with newly discovered tail fragments)',
                tail_silence_age='tail_silence_discoveries', tail_newer_frame_age='tail_newer_frame_discoveries',
                third_request_to_expiry='third_expiry_fragments; later incomplete fragments cancelled by the same frame expiry excluded',
                missing_discovery_to_frame_expiry='expired_frames', first_request_to_recovery='recovered_fragments',
                last_request_to_recovery='recovered_fragments', frame_first_seen_to_complete='completed_frames',
                frame_first_seen_to_expiry='expired_frames'),
            evidence_denominators=dict(first_request_same_poll_violations='first_requests',
                early_retry_intervals='retry_intervals', early_expiries='expired_frames (retired without any third-request fragment actually due)',
                request_budget_exceeded='first_requests + retry_intervals',
                recovered_without_retry_flag='recovered_fragments',
                late_request_deadlines='first_requests + retry_intervals'),
            evidence=dict(self._timing_evidence),
            histograms={name: histogram.summary() for name, histogram in self._timing_histograms.items()},
            events=dict(policy='most recent diagnostic events; one discovery batch/request round per frame, not full per-packet logs',
                capacity=self.TIMING_EVENT_CAPACITY, fragment_detail_limit=self.TIMING_EVENT_FRAGMENT_LIMIT,
                total=self._timing_event_total, retained=len(self._timing_events),
                dropped=self._timing_event_total-len(self._timing_events), records=list(self._timing_events)))

    def report_details(self, now=None):
        now = self.clock() if now is None else now
        last = self._events['last_completed']
        return dict(self._events, active_logic_ids=list(self.frames),
                    last_complete_age_s=None if last is None else max(0, now - last['monotonic_s']),
                    frame_manifest={key: value.ranges for key, value in self._manifest.items()},
                    manifest_range_semantics='inclusive uint32 [first,last]; wrap uses separate ranges',
                    manifest_truncated=any(value.dropped for value in self._manifest.values()),
                    manifest_dropped_events={key: value.dropped for key, value in self._manifest.items()},
                    replay_window_frames=self.replay_window, max_active_frames=self.max_active,
                    timer_entries=len(self._timers),
                    receiver_timing=self.timing_report(),
                    request_budget_policy='consumed at generation even if local sendto fails; no local retry',
                    receive_time_source='user-space monotonic processing; not NIC/kernel arrival')
