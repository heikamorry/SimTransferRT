#!/usr/bin/env python3
import struct
import unittest
from rt_protocol import *
from receiver_model import ReceiverModel

class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.mask, self.frame = build_modules({0: b'x' * 5000, 2: b'y' * 2500})
    def packets(self, logic=1, ts=100):
        return [encode_data(logic, ts, self.mask, self.frame, i) for i in range((len(self.frame)+STEP-1)//STEP)]
    def test_crc(self):
        self.assertEqual(crc32(b'123456789'), 0xCBF43926)
    def test_data_offsets(self):
        b = self.packets()[0]
        self.assertEqual(b[:8], bytes.fromhex('5a a5 55 aa c0 05 01 00'))
        self.assertEqual(decode_data(b).payload, self.frame[:STEP])
    def test_data_corruption(self):
        b = bytearray(self.packets()[0]); b[40] ^= 1
        with self.assertRaises(ProtocolError): decode_data(bytes(b))
    def test_boundaries(self):
        for size, count, last in [(1048576,731,296),(2097152,1461,592)]:
            b = encode_data(1,2,1,b'x'*size,count-1)
            self.assertEqual(len(b),last+36)
            self.assertEqual(decode_data(b).count,count)
    def test_nack_ranges(self):
        n = encode_nack(9,[(2,3),(9,2)])
        self.assertEqual(decode_nack(n),(9,[(2,3),(9,2)]))
    def test_multi_nack_same_round(self):
        packets = nack_packets(7, range(0,1461,2))
        self.assertEqual(len(packets),3)
        self.assertEqual(sum(len(decode_nack(x)[1]) for x in packets),731)
        self.assertTrue(all(len(x)<=1472 for x in packets))
    def test_bad_ranges(self):
        for r in ([],[(1,0)],[(65534,2)]):
            with self.assertRaises(ProtocolError): encode_nack(1,r)
    def test_modules(self):
        self.assertEqual(decode_modules(self.mask,self.frame),{0:b'x'*5000,2:b'y'*2500})
    def test_out_of_order(self):
        m=ReceiverModel()
        for b in reversed(self.packets()): m.ingest(b,0.0)
        self.assertEqual(m.stats['completed'],1)
    def test_gap_immediate(self):
        m=ReceiverModel(); ps=self.packets();m.ingest(ps[0],0.0);m.ingest(ps[2],0.0001)
        self.assertEqual(decode_nack(m.poll(0.0001)[0]),(1,[(1,1)]))
    def test_tail_after_2ms(self):
        m=ReceiverModel();m.ingest(self.packets()[0],0.0)
        self.assertEqual(m.poll(0.0019),[])
        self.assertEqual(decode_nack(m.poll(0.002)[0]),(1,[(1,5)]))
    def test_new_frame_marks_tail(self):
        m=ReceiverModel();m.ingest(self.packets()[0],0.0);m.ingest(self.packets(2,200)[0],0.001)
        n=m.poll(0.001);self.assertTrue(any(decode_nack(x)==(1,[(1,5)]) for x in n))
    def test_three_requests_then_expire(self):
        m=ReceiverModel();ps=self.packets()
        for i,b in enumerate(ps):
            if i!=1:m.ingest(b,0.0)
        self.assertEqual(len(m.poll(0.0)),1)
        self.assertEqual(m.poll(0.009),[])
        self.assertEqual(len(m.poll(0.010)),1)
        self.assertEqual(len(m.poll(0.020)),1)
        self.assertEqual(m.poll(0.030),[])
        self.assertEqual(m.stats['expired'],1)
        self.assertEqual(m.stats['max_requests_for_fragment'],3)
    def test_recovered_does_not_require_flag(self):
        m=ReceiverModel();ps=self.packets()
        for i,b in enumerate(ps):
            if i!=1:m.ingest(b,0.0)
        m.poll(0.0);m.ingest(ps[1],0.001)
        self.assertEqual(m.stats['completed'],1);self.assertEqual(m.stats['recovered'],1)
    def test_metadata_mismatch_invalidates(self):
        m=ReceiverModel();ps=self.packets();m.ingest(ps[0],0.0)
        m.ingest(encode_data(1,999,self.mask,self.frame,1),0.001)
        self.assertEqual(m.stats['metadata_failures'],1)
    def test_duplicates(self):
        m=ReceiverModel();p=self.packets()[0];m.ingest(p,0);m.ingest(p,0)
        self.assertEqual(m.stats['duplicate'],1)
    def test_wrap(self):
        self.assertTrue(newer(0,0xFFFFFFFF));self.assertFalse(newer(0xFFFFFFFF,0))
    def test_invalid_module_body(self):
        m=ReceiverModel();m.ingest(encode_data(1,1,1,b'abcdef',0),0)
        self.assertEqual(m.stats['module_failures'],1)

if __name__=='__main__':unittest.main(verbosity=2)
