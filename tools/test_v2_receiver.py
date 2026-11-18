import unittest
import struct
from v2_protocol import *
from v2_receiver import Receiver,crc16,decode_bx
from v2_acceptance import build_acceptance

def sample_bx():
    body=b'\x03'+bytes([1,1])+struct.pack('<8fII',1,0,0,0,10,20,30,.2,0,8)+bytes([2,2])+struct.pack('<II',0,8)+bytes([3,4])+b'\0\0'
    h=struct.pack('<HH',0xa5c4,len(body));return h+struct.pack('<H',crc16(h))+body+struct.pack('<H',crc16(body))

class V2ReceiverTests(unittest.TestCase):
    def receiver(self):return Receiver(verify_bx=False)
    def test_crc16_independent_vectors(self):
        self.assertEqual(crc16(b''), 0)
        self.assertEqual(crc16(b'123456789'), 0xbb3d)
        self.assertEqual(crc16(b'OKAY'), 0xa896)

    def test_reorder_evicts_oldest_frame_number_including_wrap(self):
        for newest in (6, 1):
            with self.subTest(newest=newest):
                r = self.receiver()
                payload = b'x' * 1437
                ids = [(newest-distance) & 0xffffffff for distance in (1, 2, 3, 4, 0)]
                for frame_id in ids:
                    r.receive(encode_data(2, frame_id, 10, payload, 0), 100)
                self.assertEqual(set(r.active[2]), set(ids) - {ids[3]})
                # All remaining partial frames can still complete.
                for frame_id in reversed(ids[:3]):
                    r.receive(encode_data(2, frame_id, 10, payload, 1), 101)
                r.receive(encode_data(2, newest, 10, payload, 1), 102)
                self.assertEqual(r.stats[2]['completed'], 4)
                self.assertEqual(r.stats[2]['consumed'], 4)
                self.assertEqual(r.stats[2]['expired'], 1)
    def test_module_isolation_and_consumption(self):
        r=self.receiver();data=b'x'*1437
        r.receive(encode_data(1,1,100,data,0),100)
        r.receive(encode_data(2,1,100,b'ep',0),100)
        self.assertEqual(r.stats[2]['consumed'],1);self.assertEqual(r.stats[1]['completed'],0)
        self.assertFalse(r.poll(2099))
        n=r.poll(2100);self.assertEqual(decode_nack(n[0]),Nack(1,1,(1,)))
        r.receive(encode_data(1,1,100,data,1,True),2200)
        self.assertEqual(r.stats[1]['consumed'],1);self.assertFalse(r.poll(12100))
    def test_gap_immediate_and_three_rounds(self):
        r=self.receiver();r.receive(encode_data(1,7,1,b'x'*3000,1),100)
        n=r.poll(100);self.assertEqual(decode_nack(n[0]).indices,(0,))
        self.assertFalse(r.poll(10099));self.assertEqual(len(r.poll(10100)),1)
        self.assertFalse(r.poll(20099));self.assertEqual(len(r.poll(20100)),1)
        self.assertFalse(r.poll(30099));self.assertEqual(len(r.active[1]),1)
        self.assertFalse(r.poll(30100));self.assertEqual(r.stats[1]['expired'],1)
        self.assertEqual(r.retry_intervals[1].report()['max_us'],10000)
    def test_new_frame_tail_only_same_module(self):
        r=self.receiver();r.receive(encode_data(1,1,0,b'x'*1437,0),100)
        r.receive(encode_data(2,2,0,b'x',0),101);self.assertFalse(r.poll(101))
        r.receive(encode_data(1,2,1,b'x',0),102)
        self.assertEqual(decode_nack(r.poll(102)[0]).indices,(1,))
    def test_duplicate_conflict_and_late_no_rollback(self):
        r=self.receiver();p=encode_data(2,1,10,b'x'*1437,0);r.receive(p,0);r.receive(p,1)
        self.assertEqual(r.stats[2]['duplicates'],1)
        r.receive(encode_data(2,1,11,b'x'*1437,1),2);self.assertEqual(r.stats[2]['conflicts'],1)
        r.receive(encode_data(2,3,30,b'a',0),3)
        r.receive(encode_data(2,2,20,b'b',0),4)
        self.assertEqual(r.latest[2]['frame_id'],3);self.assertEqual(r.stats[2]['completed'],2)
    def test_wrap_status_and_buffer_bound(self):
        r=self.receiver()
        for fid in [0xfffffffe,0xffffffff,0,1]:r.receive(encode_data(2,fid,10,b'x',0),0)
        self.assertEqual(r.stats[2]['completed'],4);self.assertEqual(r.stats[2]['whole_missing'],0)
        for seq in [0xffffffff,0,0xffffffff,0]:r.receive(encode_status(seq,0,b''),0)
        self.assertEqual((r.status_applied,r.status_old),(2,2))
        for fid in range(2,20):r.receive(encode_data(2,fid,10,b'x'*1437,0),0)
        self.assertLessEqual(len(r.active[2]),4);self.assertLessEqual(len(r.retired[2]),32)
    def test_large_nack_split(self):
        r=self.receiver();r.receive(encode_data(2,1,0,b'x'*2097152,1460),0)
        packets=r.poll(0);self.assertEqual(len(packets),3)
        self.assertEqual(sum(len(decode_nack(p).indices) for p in packets),1460)
        self.assertEqual(r.stats[2]['nack_rounds'],1)
    def test_aurora_and_true_completion_clock(self):
        b=sample_bx();self.assertEqual([t['validity'] for t in decode_bx(b)['tools']],[1,2,4])
        r=Receiver(clock=lambda:150);r.receive(encode_data(1,1,90,b,0),100)
        self.assertEqual(r.latest[1]['host_completed_us'],150)
        self.assertEqual(r.completion[1].maximum,50)
        broken=bytearray(b);broken[-1]^=1;r.receive(encode_data(1,2,90,broken,0),200)
        self.assertEqual(r.stats[1]['business_errors'],1)
        self.assertEqual(r.stats[1]['completed'],2);self.assertEqual(r.stats[1]['consumed'],1)
    def test_report_and_joint_missing_evidence(self):
        r=self.receiver();r.receive(encode_data(2,1,3,b'x',0),0)
        report=r.report('test');report['endpoint']=['127.0.0.1',5500]
        sender={'protocol_version':2,'run_id':'test','local_port':5500,'modules':{'2':{'built':1,'ledger':report['modules']['2']['ledger']}}}
        result=build_acceptance(sender,report)
        rows=[x for x in result['acceptance'] if x['id']=='joint_delivery']
        self.assertEqual([x['status'] for x in rows],['not_tested','pass'])
        sender['run_id']='other';self.assertFalse(build_acceptance(sender,report)['paired'])
    def test_association_uses_nearest_history(self):
        r=self.receiver()
        for m,fid,ts in [(2,1,80000),(1,1,90000),(2,2,105000)]:
            r.receive(encode_data(m,fid,ts,b'x',0),100)
        a=r.report(now=100)['association']
        self.assertEqual((a['ep_frame_id'],a['delta_us']),(1,10000))
        self.assertTrue(a['available']);self.assertFalse(a['hardware_clock_verified'])
    def test_whole_missing_is_observed_gap_not_final_loss(self):
        r=self.receiver()
        for fid in (1,3,2):r.receive(encode_data(2,fid,100,b'x',0),fid)
        report=r.report()
        self.assertEqual(report['modules']['2']['whole_missing'],1)
        self.assertEqual(report['modules']['2']['completed'],3)
        self.assertEqual(report['modules']['2']['completed_ranges'],[[1,3]])
        self.assertIn('not final loss',report['whole_missing_definition'])

if __name__=='__main__':unittest.main()
