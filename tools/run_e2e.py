#!/usr/bin/env python3
"""Exercise actual sender process over UDP. Accept SimTransferCli.exe or SimRtProbe.
Exit 0 means functional test matrix passed; it does not certify 50Hz timing on target hardware.
"""
from __future__ import annotations
import argparse
import json
import socket
import subprocess
import sys
import time
from pathlib import Path

def free_port():
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as s:
        s.bind(('127.0.0.1',0))
        return s.getsockname()[1]

def main():
    p=argparse.ArgumentParser();p.add_argument('--sender',required=True);p.add_argument('--out',default='test-results/e2e');args=p.parse_args()
    sender=str(Path(args.sender).resolve());out=Path(args.out).resolve();out.mkdir(parents=True,exist_ok=True)
    tools=Path(__file__).resolve().parent
    cases=[('normal',[],[]),('first_loss',['--drop-first','2'],[]),('crc_damage',['--corrupt-first','4'],[]),
           ('duplicates',['--duplicate-first','1,3'],[]),('reverse',['--reverse'],[]),
           ('duplicate_nack',['--drop-first','2'],['--duplicate-nack']),
           ('permanent_loss',['--drop-always','2'],[]),('wrap',['--start-id','4294967294'],[])]
    results=[]
    for name,extra,stub_extra in cases:
        data_port,cmd_port=free_port(),free_port()
        while data_port==cmd_port:cmd_port=free_port()
        sr=out/(name+'-sender.json');rr=out/(name+'-receiver.json')
        with (out/(name+'-receiver.log')).open('w',encoding='utf-8') as log:
            receiver=subprocess.Popen([sys.executable,str(tools/'udp_feedback_stub.py'),'--port',str(data_port),'--cmd-port',str(cmd_port),'--seconds','1.4','--report',str(rr),'--quiet',*stub_extra],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
            try:
                ready = receiver.stdout.readline()
                log.write(ready);log.flush()
                if not ready.startswith('LISTEN '):
                    raise RuntimeError('receiver failed before readiness: '+ready)
                run=subprocess.run([sender,'--port',str(data_port),'--cmd-port',str(cmd_port),'--frames','15','--frame-bytes','16384','--report',str(sr),*extra],capture_output=True,text=True,timeout=6)
                (out/(name+'-sender.log')).write_text(run.stdout+'\n'+run.stderr,encoding='utf-8')
                remaining, _ = receiver.communicate(timeout=4)
                log.write(remaining)
            finally:
                if receiver.poll() is None:receiver.kill();receiver.wait()
        if not sr.exists() or not rr.exists():
            results.append({'case':name,'pass':False,'reason':'missing report'});continue
        a=json.loads(sr.read_text());b=json.loads(rr.read_text());stats=a['stats']
        ok=run.returncode==0 and stats['built']==15 and stats['datagrams']>0 and a['df_enabled']
        if name=='permanent_loss':ok=ok and b['expired']>0 and b['max_requests_for_fragment']==3
        else:ok=ok and b['completed']>0 and b['module_failures']==0
        if name in ('first_loss','crc_damage','duplicate_nack'):ok=ok and stats['retransmissions']>0 and b['recovered']>0
        results.append({'case':name,'pass':bool(ok),'completed':b['completed'],'expired':b['expired'],'sender_frames':stats['built'],'retransmissions':stats['retransmissions'],'deadline_misses':stats['deadlineMisses'],'note':'functional, not hard-real-time acceptance'})
        print(json.dumps(results[-1]),flush=True)
    (out/'summary.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
    return 0 if all(r['pass'] for r in results) else 1

if __name__=='__main__':raise SystemExit(main())
