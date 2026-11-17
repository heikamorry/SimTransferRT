"""Independent V2 wire codec. V1 is deliberately not auto-detected.

NACK erratum: CRC at 20+2*N, total 24+2*N, minimum 26 bytes.
"""
from dataclasses import dataclass
import struct
import zlib

MAGIC = 0xAA55A55A
MAX_DATAGRAM, STEP, MAX_FRAME = 1472, 1436, 2097152

class ProtocolError(ValueError):
    pass

def newer(a, b):
    return 0 < ((a-b) & 0xffffffff) < 0x80000000

@dataclass(frozen=True)
class Data:
    module_id: int
    frame_id: int
    frame_len: int
    frag_idx: int
    frag_cnt: int
    ts: int
    payload: bytes
    retry: bool = False

@dataclass(frozen=True)
class Nack:
    module_id: int
    frame_id: int
    indices: tuple

@dataclass(frozen=True)
class Status:
    sequence: int
    ts: int
    payload: bytes

def _finish(data):
    return data + struct.pack('<I', zlib.crc32(data))

def _header(kind, size, flags=0):
    return struct.pack('<IHBB', MAGIC, size, kind, flags)

def _check(data, kind, minimum):
    if not minimum <= len(data) <= MAX_DATAGRAM:
        raise ProtocolError('length')
    magic, length, actual, flags = struct.unpack_from('<IHBB', data)
    if magic != MAGIC or length != len(data) or actual != kind:
        raise ProtocolError('header/type/length')
    if flags & ~1 or (kind != 1 and flags):
        raise ProtocolError('flags')
    if data[9:12] != b'\0\0\0':
        raise ProtocolError('reserved')
    if zlib.crc32(data[:-4]) != struct.unpack_from('<I', data, len(data)-4)[0]:
        raise ProtocolError('crc32')
    return flags

def encode_data(module_id, frame_id, ts, frame, frag_idx, retry=False):
    if module_id not in (1, 2) or not 1 <= len(frame) <= MAX_FRAME:
        raise ProtocolError('module/frame length')
    count = (len(frame)+STEP-1)//STEP
    if not 0 <= frag_idx < count:
        raise ProtocolError('fragment index')
    payload = frame[frag_idx*STEP:(frag_idx+1)*STEP]
    body = struct.pack('<B3xIIHHQ', module_id, frame_id, len(frame), frag_idx, count, ts)
    return _finish(_header(1, 36+len(payload), int(retry))+body+payload)

def decode_data(data):
    flags = _check(data, 1, 37)
    module, frame, length, index, count, ts = struct.unpack_from('<B3xIIHHQ', data, 8)
    if module not in (1, 2) or not 1 <= length <= MAX_FRAME:
        raise ProtocolError('module/frame length')
    if count != (length+STEP-1)//STEP or index >= count:
        raise ProtocolError('fragment geometry')
    payload = bytes(data[32:-4])
    if len(payload) != min(STEP, length-index*STEP):
        raise ProtocolError('fragment payload length')
    return Data(module, frame, length, index, count, ts, payload, bool(flags))

def encode_nack(module_id, frame_id, indices):
    indices = tuple(indices)
    if module_id not in (1, 2) or not 1 <= len(indices) <= 724:
        raise ProtocolError('module/indices count')
    if any(not 0 <= i < (MAX_FRAME+STEP-1)//STEP for i in indices):
        raise ProtocolError('index beyond any valid frame')
    return _finish(_header(2, 24+2*len(indices))+struct.pack('<B3xIHH', module_id, frame_id, len(indices), 0)+struct.pack('<'+'H'*len(indices), *indices))

def decode_nack(data):
    _check(data, 2, 26)
    module, frame, count, reserved = struct.unpack_from('<B3xIHH', data, 8)
    if module not in (1, 2) or reserved or not 1 <= count <= 724 or len(data) != 24+2*count:
        raise ProtocolError('nack geometry/reserved')
    indices = struct.unpack_from('<'+'H'*count, data, 20)
    if any(i >= (MAX_FRAME+STEP-1)//STEP for i in indices):
        raise ProtocolError('index beyond any valid frame')
    return Nack(module, frame, indices)

def encode_status(sequence, ts, payload):
    if len(payload) > 1444:
        raise ProtocolError('status payload exceeds MTU')
    return _finish(_header(3, 28+len(payload))+struct.pack('<B3xIQ', 3, sequence, ts)+payload)

def decode_status(data):
    _check(data, 3, 28)
    if data[8] != 3:
        raise ProtocolError('status module')
    sequence, ts = struct.unpack_from('<IQ', data, 12)
    return Status(sequence, ts, bytes(data[24:-4]))

def nack_packets(module_id, frame_id, indices):
    indices = sorted(set(indices))
    return [encode_nack(module_id, frame_id, indices[i:i+724]) for i in range(0,len(indices),724)]
