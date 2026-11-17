from copy import deepcopy
import unittest
from v2_acceptance import build_acceptance

def reports():
    ledger={'count':'2','id_sum':'3','crc_sum':'7','stamp_sum':'9'}
    sender={'protocol_version':2,'run_id':'paired','local_port':5500,'config':{'bind_ip':'127.0.0.1'},'modules':{'1':{'built':2,'ledger':ledger}}}
    receiver={'protocol_version':2,'run_id':'paired','endpoint':['127.0.0.1',5500],'modules':{'1':{'completed':2,'consumed':2,'business_errors':0,'ledger':deepcopy(ledger),
        'nack_rounds':3,'retry_intervals':{'count':2,'min_us':10000,'max_us':10000},'request_lateness':{'count':3,'max_us':0}}}}
    return sender,receiver

def verdict(sender,receiver,identifier):
    return next(row['status'] for row in build_acceptance(sender,receiver)['acceptance'] if row['id']==identifier and row['module_id']==1)

class V2AcceptanceEvidenceTests(unittest.TestCase):
    def test_complete_evidence(self):
        s,r=reports()
        for identifier in ('joint_delivery','consumer_delivery','receiver_retry_timing'):self.assertEqual(verdict(s,r,identifier),'pass')
    def test_ledger_count_cannot_hide_independent_count_mismatch(self):
        s,r=reports();r['modules']['1']['completed']=1
        self.assertEqual(verdict(s,r,'joint_delivery'),'fail')
        s,r=reports();s['modules']['1']['built']=3
        self.assertEqual(verdict(s,r,'joint_delivery'),'fail')
    def test_missing_or_invalid_measurement_never_passes(self):
        for bad in (-1,True,'NaN',str(2**64),1.5):
            s,r=reports();s['modules']['1']['ledger']['count']=bad;r['modules']['1']['ledger']['count']=bad
            self.assertEqual(verdict(s,r,'joint_delivery'),'not_tested')
        s,r=reports();del r['modules']['1']['business_errors']
        self.assertEqual(verdict(s,r,'consumer_delivery'),'not_tested')
    def test_endpoint_and_run_identity(self):
        s,r=reports();r['endpoint'][1]=5501
        self.assertEqual(verdict(s,r,'joint_delivery'),'fail')
        s,r=reports();r['endpoint'][0]='192.0.2.1'
        self.assertEqual(verdict(s,r,'joint_delivery'),'fail')
        s,r=reports();del r['endpoint']
        self.assertEqual(verdict(s,r,'joint_delivery'),'not_tested')
        s,r=reports();r['run_id']='other'
        self.assertEqual(verdict(s,r,'joint_delivery'),'not_tested')
    def test_early_retry_cannot_hide_behind_maximum(self):
        s,r=reports();r['modules']['1']['retry_intervals']['min_us']=9000
        self.assertEqual(verdict(s,r,'receiver_retry_timing'),'fail')
        s,r=reports();del r['modules']['1']['retry_intervals']['min_us']
        self.assertEqual(verdict(s,r,'receiver_retry_timing'),'not_tested')
    def test_request_statistics_must_cover_all_rounds(self):
        s,r=reports();r['modules']['1']['request_lateness']['count']=2
        self.assertEqual(verdict(s,r,'receiver_retry_timing'),'fail')
    def test_protocol_delivery_does_not_hide_business_failure(self):
        s,r=reports();r['modules']['1'].update(consumed=1,business_errors=1)
        self.assertEqual(verdict(s,r,'joint_delivery'),'pass')
        self.assertEqual(verdict(s,r,'consumer_delivery'),'fail')
    def test_no_hardware_or_display_success_inferred(self):
        s,r=reports();result=build_acceptance(s,r)
        for row in result['acceptance']:
            if row['id'] in ('hardware_time','wire_departure','display','status_immediate'):self.assertEqual(row['status'],'not_tested')

if __name__=='__main__':unittest.main()
