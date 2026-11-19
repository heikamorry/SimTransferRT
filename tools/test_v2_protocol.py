import struct
import unittest
import zlib
from v2_protocol import *

class V2ProtocolTests(unittest.TestCase):
    def test_boundaries(self):
        for module in (1,2):
            for length in (1,1436,1437,2097152):
                frame=bytes(i&255 for i in range(length));count=(length+1435)//1436
                restored=bytearray()
                for index in range(count):
                    packet=encode_data(module,0xffffffff,0x0102030405060708,frame,index)
                    p=decode_data(packet);self.assertLessEqual(len(packet),1472)
                    self.assertEqual((p.frag_cnt,p.frag_idx,p.module_id),(count,index,module));restored.extend(p.payload)
                self.assertEqual(restored,frame)
    def test_nack_erratum_and_split(self):
        packet=encode_nack(2,17,[0]);self.assertEqual(len(packet),26)
        self.assertEqual(decode_nack(packet).indices,(0,))
        packets=nack_packets(1,0,range(1461));self.assertEqual(list(map(len,packets)),[1472,1472,50])
        self.assertEqual(sum(len(decode_nack(p).indices) for p in packets),1461)
    def test_status_boundary(self):
        for size in (0,1444):
            p=encode_status(0xffffffff,4,b'x'*size);self.assertEqual(decode_status(p).payload,b'x'*size)
        with self.assertRaises(ProtocolError):encode_status(0,0,b'x'*1445)
    def test_malformed(self):
        original=encode_data(1,3,9,b'x'*1437,0)
        for offset,value in [(0,0),(6,3),(7,2),(8,3),(9,1),(22,0)]:
            b=bytearray(original);b[offset]=value;b[-4:]=struct.pack('<I',zlib.crc32(b[:-4]))
            with self.assertRaises(ProtocolError):decode_data(b)
        b=bytearray(original);b[-1]^=1
        with self.assertRaises(ProtocolError):decode_data(b)
        with self.assertRaises(ProtocolError):decode_data(original[:-1])
        for size in (0,2097153):
            with self.assertRaises(ProtocolError):encode_data(1,0,0,b'x'*size,0)
    def test_wrap(self):
        self.assertTrue(newer(0,0xffffffff));self.assertFalse(newer(0xffffffff,0));self.assertFalse(newer(0x80000000,0))

if __name__=='__main__':unittest.main()
