"""Independent bounded V2 receiver and paired-report evidence collector.

No wire session field exists. A restarted sender must use a new source endpoint,
and receivers must be explicitly restarted/drained before reusing frame IDs.
"""
from __future__ import annotations
from collections import OrderedDict, deque
from dataclasses import dataclass, field
import argparse
import json
import math
import select
import signal
import socket
import struct
import time
import zlib
from pathlib import Path
from v2_protocol import *
from receiver_model import FrameRanges

MASK64 = (1 << 64)-1

def crc16(data):
    value = 0
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0xA001 if value & 1 else 0)
    return value

def decode_bx(data):
    if not 11 <= len(data) <= 16384:
        raise ProtocolError('Aurora length')
    magic, length, head_crc = struct.unpack_from('<HHH', data)
    if magic != 0xA5C4 or len(data) != length+8 or crc16(data[:4]) != head_crc:
        raise ProtocolError('Aurora header/CRC16')
    if crc16(data[6:-2]) != struct.unpack_from('<H', data, len(data)-2)[0]:
        raise ProtocolError('Aurora body CRC16')
    tools, offset, handles = [], 7, set()
    for _ in range(data[6]):
        if offset+2 > len(data)-4:
            raise ProtocolError('Aurora truncated tool')
        handle, status = data[offset:offset+2]
        if handle in handles:raise ProtocolError('Aurora duplicate handle')
        handles.add(handle)
        offset += 2
        tool = {'handle':handle, 'validity':status}
        if status == 1:
            if offset+40 > len(data)-4:
                raise ProtocolError('Aurora valid record length')
            values = struct.unpack_from('<8fII', data, offset)
            if not all(math.isfinite(v) for v in values[:8]):
                raise ProtocolError('Aurora nonfinite transform')
            tool.update(quaternion=values[:4], position_mm=values[4:7], quality=values[7], port_status=values[8], source_frame=values[9])
            offset += 40
        elif status == 2:
            if offset+8 > len(data)-4:
                raise ProtocolError('Aurora missing record length')
            port, frame = struct.unpack_from('<II', data, offset)
            tool.update(port_status=port, source_frame=frame)
            offset += 8
        elif status != 4:
            raise ProtocolError('Aurora validity')
        tools.append(tool)
    if offset != len(data)-4:
        raise ProtocolError('Aurora record count/length')
    return {'tools':tools, 'system_status':struct.unpack_from('<H',data,offset)[0]}

class Distribution:
    def __init__(self):
        self.count = self.total = self.maximum = 0
        self.minimum = None
        self.hist = [0]*2001
    def add(self, value):
        value = max(0, int(value)); self.count += 1; self.total += value
        self.maximum = max(self.maximum, value); self.hist[min(value//100,2000)] += 1
        self.minimum = value if self.minimum is None else min(self.minimum,value)
    def report(self):
        def percentile(f):
            target, total = math.ceil(self.count*f), 0
            for i,n in enumerate(self.hist):
                total += n
                if total >= target:
                    return min(self.maximum,(i+1)*100) if i < 2000 else self.maximum
            return 0
        return {'count':self.count,'min_us':self.minimum,'max_us':self.maximum,'mean_us':self.total/self.count if self.count else None,
                'p99_upper_us':percentile(.99) if self.count else None}

@dataclass
class Assembly:
    packet: Data
    first: int
    last: int
    data: bytearray
    seen: set = field(default_factory=set)
    highest: int = 0
    contiguous: int = 0
    tail: bool = False
    discovered: bool = False
    due: int = 0
    rounds: int = 0
    last_request: int = 0

class Receiver:
    def __init__(self, verify_bx=True, clock=None):
        self.verify_bx, self.clock = verify_bx, clock
        self.active = {1:OrderedDict(),2:OrderedDict()}
        self.retired = {1:deque(maxlen=32),2:deque(maxlen=32)}
        self.highest = {1:None,2:None}
        self.latest = {1:None,2:None}
        self.consumed_times = {1:deque(maxlen=4),2:deque(maxlen=4)}
        self.stats = {m:{k:0 for k in ('datagrams','bytes','completed','consumed','duplicates','late','conflicts','expired','whole_missing','business_errors','retransmissions','nack_rounds','nack_packets','requested_fragments','tail_triggers','gap_triggers','active_high_water')} for m in (1,2)}
        self.ledgers = {m:dict(count=0,id_sum=0,crc_sum=0,stamp_sum=0) for m in (1,2)}
        self.ranges = {m:FrameRanges() for m in (1,2)}
        self.completion = {m:Distribution() for m in (1,2)}
        self.retry_intervals = {m:Distribution() for m in (1,2)}
        self.request_lateness = {m:Distribution() for m in (1,2)}
        self.retry_events = deque(maxlen=128)
        self.retry_event_count = 0
        self.status = None
        self.invalid_packets = self.unexpected_nacks = self.status_applied = self.status_old = 0
        self.on_frame = self.on_status = None

    def receive(self, data, now):
        try:
            if len(data)<8:
                raise ProtocolError('short packet')
            if data[6] == 3:
                status = decode_status(data)
                if self.status is not None and not newer(status.sequence,self.status.sequence):
                    self.status_old += 1; return
                self.status = status; self.status_applied += 1
                if self.on_status: self.on_status(status,now)
                return
            if data[6] == 2:
                decode_nack(data); self.unexpected_nacks += 1; return
            p = decode_data(data)
        except (ProtocolError,struct.error):
            self.invalid_packets += 1; return
        self._receive_data(p,len(data),now)

    def _receive_data(self,p,packet_size,now):
        """Already CRC/geometry-validated DATA; shared with the endpoint filter."""
        m, fid = p.module_id, p.frame_id
        s, active = self.stats[m], self.active[m]
        s['datagrams'] += 1; s['bytes'] += packet_size; s['retransmissions'] += int(p.retry)
        if fid in self.retired[m]:
            s['late'] += 1; return
        if fid not in active:
            hi = self.highest[m]
            if hi is not None and not newer(fid,hi) and ((hi-fid)&0xffffffff)>=4:
                s['late'] += 1; return
            if hi is None or newer(fid,hi):
                if hi is not None: s['whole_missing'] += ((fid-hi)&0xffffffff)-1
                self.highest[m] = fid
                for old in active.values():
                    if newer(fid,old.packet.frame_id):
                        old.tail = True
                        if not old.discovered:
                            old.discovered = True; old.due = now; s['tail_triggers'] += 1
            if len(active)==4:
                old_id,_ = active.popitem(last=False); self.retired[m].append(old_id); s['expired'] += 1
            active[fid] = Assembly(p,now,now,bytearray(p.frame_len))
            s['active_high_water'] = max(s['active_high_water'],len(active))
        a = active[fid]
        if (p.frame_len,p.frag_cnt,p.ts) != (a.packet.frame_len,a.packet.frag_cnt,a.packet.ts):
            s['conflicts'] += 1; self.retired[m].append(fid); del active[fid]; return
        if p.frag_idx in a.seen:
            s['duplicates'] += 1; return
        a.data[p.frag_idx*STEP:p.frag_idx*STEP+len(p.payload)] = p.payload
        a.seen.add(p.frag_idx); a.last=now; a.highest=max(a.highest,p.frag_idx)
        # Each fragment advances this cursor at most once per frame. Scanning
        # the whole received prefix on every packet made a 2 MiB frame O(n²).
        while a.contiguous in a.seen:
            a.contiguous+=1
        if not a.discovered and a.contiguous<a.highest:
            a.discovered=True; a.due=now; s['gap_triggers'] += 1
        if len(a.seen)==p.frag_cnt:
            s['completed'] += 1
            ledger=self.ledgers[m]
            for key,value in [('count',1),('id_sum',fid),('crc_sum',zlib.crc32(a.data)),('stamp_sum',p.ts)]:
                ledger[key]=(ledger[key]+value)&MASK64
            self.ranges[m].add(fid)
            valid=True; business=None
            try:
                if m==1 and self.verify_bx: business=decode_bx(a.data)
            except ProtocolError:
                valid=False; s['business_errors'] += 1
            completed=max(now,self.clock()) if self.clock else now
            self.completion[m].add(completed-a.first)
            latest=self.latest[m]
            if valid and (latest is None or newer(fid,latest['frame_id']) and p.ts>=latest['ts']):
                self.latest[m]={'frame_id':fid,'ts':p.ts,'host_completed_us':completed,'bytes':len(a.data),'business':business}
                self.consumed_times[m].append((fid,p.ts))
                s['consumed'] += 1
                if self.on_frame: self.on_frame(m,fid,p.ts,bytes(a.data),completed)
            elif valid: s['late'] += 1
            self.retired[m].append(fid); del active[fid]

    def poll(self, now):
        packets=[]
        for m,active in self.active.items():
            s=self.stats[m]
            for fid,a in list(active.items()):
                if now-a.last>=2000 and not a.tail:
                    a.tail=True; s['tail_triggers'] += 1
                    if not a.discovered: a.discovered=True; a.due=a.last+2000
                if not a.discovered or now<a.due: continue
                if a.rounds==3:
                    s['expired'] += 1; self.retired[m].append(fid); del active[fid]; continue
                missing=[i for i in range(a.packet.frag_cnt if a.tail else a.highest+1) if i not in a.seen]
                if not missing: continue
                out=nack_packets(m,fid,missing);packets.extend(out)
                self.request_lateness[m].add(now-a.due)
                if a.rounds: self.retry_intervals[m].add(now-a.last_request)
                self.retry_event_count += 1
                self.retry_events.append({'module_id':m,'frame_id':fid,'round':a.rounds+1,'due_us':a.due,'actual_us':now,'missing_count':len(missing),'tail':a.tail})
                s['nack_rounds'] += 1; s['nack_packets'] += len(out); s['requested_fragments'] += len(missing)
                a.rounds+=1;a.last_request=now;a.due=now+10000
        return packets

    def report(self, run_id='', now=0):
        modules={}
        for m in (1,2):
            modules[str(m)]={**self.stats[m],'active_frames':len(self.active[m]),'completion':self.completion[m].report(),
                'retry_intervals':self.retry_intervals[m].report(),'request_lateness':self.request_lateness[m].report(),
                'ledger':{k:str(v) for k,v in self.ledgers[m].items()},'completed_ranges':self.ranges[m].ranges,
                'ranges_dropped':self.ranges[m].dropped,'latest':self.latest[m]}
            latest=self.latest[m]
            modules[str(m)]['display_stale']=latest is None or now-latest['host_completed_us']>(16667 if m==1 else 20000)
        association=None
        if self.latest[1] and self.consumed_times[2]:
            fid,ts=min(self.consumed_times[2],key=lambda item:abs(item[1]-self.latest[1]['ts']))
            delta=abs(ts-self.latest[1]['ts'])
            association={'ndi_frame_id':self.latest[1]['frame_id'],'ep_frame_id':fid,'delta_us':delta,
                         'available':delta<=20000 and not modules['1']['display_stale'] and not modules['2']['display_stale'],
                         'hardware_clock_verified':False}
        return {'kind':'v2_receiver','protocol_version':2,'report_schema_version':1,'run_id':run_id,'modules':modules,'association':association,
                'invalid_packets':self.invalid_packets,'unexpected_nacks':self.unexpected_nacks,
                'status_applied':self.status_applied,'status_old':self.status_old,
                'status_latest_sequence':self.status.sequence if self.status else None,
                'retry_events':list(self.retry_events),'retry_events_dropped':max(0,self.retry_event_count-128),
                'clock':'host monotonic microseconds; not hardware arrival time','hardware_clock_verified':False,
                'real_waveform_validation':'not_tested','display_validation':'not_tested',
                'ledger_evidence':'bounded additive summaries; agreement does not prove bytewise equality of every frame',
                'whole_missing_definition':'forward ID gaps observed before late arrivals, not final loss count'}

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind',default='127.0.0.1');parser.add_argument('--port',type=int,default=5650)
    parser.add_argument('--duration',type=float,default=0);parser.add_argument('--report',required=True)
    parser.add_argument('--run-id',default='');parser.add_argument('--ready-file')
    parser.add_argument('--source-ip',default='127.0.0.1');parser.add_argument('--source-port',type=int,default=0)
    parser.add_argument('--drop-first',default='',help='module:index,... dropped once per frame (first transmissions only)')
    parser.add_argument('--drop-always',default='',help='module:index,...')
    args=parser.parse_args(argv)
    if args.duration<0 or not 1<=args.port<=65535: parser.error('invalid duration/port')
    def drops(text): return {tuple(map(int,item.split(':'))) for item in text.split(',') if item}
    first,always=drops(args.drop_first),drops(args.drop_always)
    clock=lambda:time.monotonic_ns()//1000
    receiver=Receiver(clock=clock)
    sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);sock.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4*1024*1024)
    # Reuse the already-regressed Windows WSAIoctl adapter. Python's socket.ioctl
    # does not accept every Windows UDP control code, even when a constant exists.
    from runtime_support import configure_udp_connreset
    connreset=configure_udp_connreset(sock)
    sock.bind((args.bind,args.port));sock.setblocking(False)
    peer=None;started=clock(); stop=False;drop_count=source_rejected=send_errors=receive_errors=0
    def stopped(*_):
        nonlocal stop
        stop=True
    signal.signal(signal.SIGINT,stopped);signal.signal(signal.SIGTERM,stopped)
    if hasattr(signal,'SIGBREAK'): signal.signal(signal.SIGBREAK,stopped)
    print(json.dumps({'ready':True,'port':sock.getsockname()[1],'protocol_version':2}),flush=True)
    if args.ready_file: Path(args.ready_file).write_text('ready',encoding='utf-8')
    try:
        while not stop and (not args.duration or clock()-started<args.duration*1e6):
            # Poll before/after bounded batches so a busy socket cannot starve timers.
            if peer:
                for nack in receiver.poll(clock()):
                    try: sock.sendto(nack,peer)
                    except OSError: send_errors+=1
            ready,_,_=select.select([sock],[],[],.0005)
            if not ready: continue
            batch_start=clock()
            for _ in range(64):
                try: data,address=sock.recvfrom(65535)
                except BlockingIOError: break
                except (ConnectionResetError,ConnectionRefusedError): receive_errors+=1;break
                if address[0]!=args.source_ip or args.source_port and address[1]!=args.source_port or peer is not None and address!=peer:
                    source_rejected+=1;continue
                # Lock endpoint only after a fully valid packet, not a spoofed header.
                try:
                    if len(data)<8: raise ProtocolError('short')
                    packet=decode_data(data) if data[6]==1 else decode_status(data) if data[6]==3 else None
                    if packet is None: raise ProtocolError('unexpected type')
                except ProtocolError:
                    receiver.receive(data,clock());continue
                peer=address
                if isinstance(packet,Data) and ((packet.module_id,packet.frag_idx) in always or not packet.retry and (packet.module_id,packet.frag_idx) in first):
                    drop_count+=1;continue
                if isinstance(packet,Data):receiver._receive_data(packet,len(data),clock())
                else:receiver.receive(data,clock())
                if clock()-batch_start>=500: break
    finally:
        ended=clock();report=receiver.report(args.run_id,ended)
        report.update(duration_us=ended-started,endpoint=list(peer) if peer else None,source_rejected=source_rejected,
                      injected_drops=drop_count,send_errors=send_errors,receive_errors=receive_errors,transport='IPv4 UDP',
                      loopback=args.bind.startswith('127.'),exit_reason='signal' if stop else 'duration',
                      sio_udp_connreset=connreset)
        dest=Path(args.report);dest.parent.mkdir(parents=True,exist_ok=True)
        temp=dest.with_suffix(dest.suffix+'.tmp');temp.write_text(json.dumps(report,indent=2),encoding='utf-8');temp.replace(dest)
        sock.close()
    return 0

if __name__=='__main__': raise SystemExit(main())
