"""Independent Python implementation of the uploaded wire format. No READY/FIN/ACK/SUCCESS."""
from __future__ import annotations
import struct
import zlib
from dataclasses import dataclass
MAGIC = 0xAA55A55A
MAX_DATAGRAM, STEP, MAX_FRAME, MAX_RANGES = 1472, 1436, 2097152, 363
DATA_HEADER = struct.Struct('<IHBBIIHHQI')
NACK_HEADER = struct.Struct('<IHBBIHH')

class ProtocolError(ValueError):
    pass

@dataclass(frozen=True)
class Data:
    logic_id: int
    frame_len: int
    index: int
    count: int
    ts: int
    mask: int
    flags: int
    payload: bytes

def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF

def newer(a: int, b: int) -> bool:
    return 0 < ((a - b) & 0xFFFFFFFF) < 0x80000000

def _check(packet: bytes, expected_type: int, minimum: int) -> None:
    if not minimum <= len(packet) <= MAX_DATAGRAM:
        raise ProtocolError('invalid size')
    magic, size, kind, _ = struct.unpack_from('<IHBB', packet)
    if magic != MAGIC or size != len(packet) or kind != expected_type:
        raise ProtocolError('magic/size/type')
    if crc32(packet[:-4]) != struct.unpack_from('<I', packet, len(packet) - 4)[0]:
        raise ProtocolError('CRC32')

def decode_data(packet: bytes) -> Data:
    _check(packet, 1, 36)
    _, _, _, flags, logic, size, index, count, ts, mask = DATA_HEADER.unpack_from(packet)
    if flags & ~1 or not 1 <= size <= MAX_FRAME or count != (size + STEP - 1) // STEP or index >= count:
        raise ProtocolError('DATA geometry/flags')
    length = min(STEP, size - index * STEP)
    if len(packet) != length + 36:
        raise ProtocolError('DATA length')
    return Data(logic, size, index, count, ts, mask, flags, packet[32:-4])

def encode_data(logic: int, ts: int, mask: int, frame: bytes, index: int, retry: bool = False) -> bytes:
    if not 1 <= len(frame) <= MAX_FRAME:
        raise ProtocolError('frame size')
    count = (len(frame) + STEP - 1) // STEP
    if not 0 <= index < count:
        raise ProtocolError('index')
    body = frame[index * STEP:(index + 1) * STEP]
    packet = DATA_HEADER.pack(MAGIC, len(body) + 36, 1, int(retry), logic, len(frame), index, count, ts, mask) + body
    return packet + struct.pack('<I', crc32(packet))

def encode_nack(logic: int, ranges: list[tuple[int, int]]) -> bytes:
    if not 1 <= len(ranges) <= MAX_RANGES:
        raise ProtocolError('range count')
    if any(not (0 <= start <= 65534 and 1 <= count <= 65535 and start + count <= 65535) for start, count in ranges):
        raise ProtocolError('range')
    packet = NACK_HEADER.pack(MAGIC, 20 + 4 * len(ranges), 2, 0, logic, len(ranges), 0)
    packet += b''.join(struct.pack('<HH', *r) for r in ranges)
    return packet + struct.pack('<I', crc32(packet))

def decode_nack(packet: bytes) -> tuple[int, list[tuple[int, int]]]:
    _check(packet, 2, 24)
    _, _, _, flags, logic, count, reserved = NACK_HEADER.unpack_from(packet)
    if flags or reserved or not 1 <= count <= MAX_RANGES or len(packet) != 20 + 4 * count:
        raise ProtocolError('NACK fields')
    ranges = [struct.unpack_from('<HH', packet, 16 + 4 * i) for i in range(count)]
    if any(n == 0 or start + n > 65535 for start, n in ranges):
        raise ProtocolError('NACK range')
    return logic, ranges

def merge_ranges(ids) -> list[tuple[int, int]]:
    result: list[tuple[int, int]] = []
    for index in sorted(set(ids)):
        if result and result[-1][0] + result[-1][1] == index:
            first, count = result[-1]
            result[-1] = first, count + 1
        else:
            result.append((index, 1))
    return result

def nack_packets(logic: int, missing) -> list[bytes]:
    ranges = merge_ranges(missing)
    return [encode_nack(logic, ranges[i:i + MAX_RANGES]) for i in range(0, len(ranges), MAX_RANGES)]

def build_modules(modules: dict[int, bytes]) -> tuple[int, bytes]:
    if not modules or any(not 0 <= m < 32 for m in modules):
        raise ProtocolError('modules')
    mask = sum(1 << m for m in modules)
    data = b''.join(struct.pack('<BI', m, len(modules[m])) + modules[m] for m in sorted(modules))
    if len(data) > MAX_FRAME:
        raise ProtocolError('frame too large')
    return mask, data

def decode_modules(mask: int, data: bytes) -> dict[int, bytes]:
    pos = 0
    result: dict[int, bytes] = {}
    for m in range(32):
        if not (mask & (1 << m)):
            continue
        if len(data) - pos < 5:
            raise ProtocolError('short module header')
        module_id, length = struct.unpack_from('<BI', data, pos)
        pos += 5
        if module_id != m or length > len(data) - pos:
            raise ProtocolError('module order/length')
        result[m] = data[pos:pos + length]
        pos += length
    if pos != len(data) or not result:
        raise ProtocolError('mask/total mismatch')
    return result
