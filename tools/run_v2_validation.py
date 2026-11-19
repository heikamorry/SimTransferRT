"""V2 functional matrix and explicit-duration soaks; V1 tooling is unchanged.

Example: --sender build/windows-v2-release/src/SimTransferCli.exe --soak-seconds 600
         --long-seconds 7200 --output artifacts/udp-stability/v2-validation-unique
Functional delivery and strict timing acceptance are reported separately.
"""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import time
import uuid
from run_diagnostics import free_ports,stop_owned,read_json,write_json,cpu_seconds,BoundedMemorySampler,provenance
from v2_acceptance import build_acceptance,ledger_values,unsigned
from v2_receiver import crc16,decode_bx

ROOT=Path(__file__).resolve().parents[1]

def validation_provenance(root):
    """Hash the working files actually built, including untracked source files."""
    root=Path(root)
    existing=provenance(root)
    hashes=dict(existing['file_sha256'])
    for directory in ('docs','resources'):
        for path in sorted((root/directory).rglob('*')):
            if path.is_file() and '__pycache__' not in path.parts and path.suffix not in ('.pyc','.tmp'):
                hashes[path.relative_to(root).as_posix()]=hashlib.sha256(path.read_bytes()).hexdigest()
    for name in ('CMakeLists.txt','CMakePresets.json','README.md','.gitignore'):
        path=root/name
        if path.is_file():hashes[name]=hashlib.sha256(path.read_bytes()).hexdigest()
    # Normalize Windows separators before deriving one reproducible tree digest.
    hashes={name.replace('\\','/'):value for name,value in hashes.items()}
    existing.update(file_sha256=hashes,source_sha256=hashlib.sha256(json.dumps(hashes,sort_keys=True).encode()).hexdigest(),
        scope='All working files under src/tests/tools/scripts/docs/resources, root CMake files/README/.gitignore; includes untracked source. Excludes artifacts/build/dist and Python caches.')
    return existing

def binary_sha256(path):
    try:return hashlib.sha256(Path(path).read_bytes()).hexdigest()
    except OSError:return None

def _ledger_equal(s,r,module):
    left=s.get('modules',{}).get(str(module),{}).get('ledger',{})
    right=r.get('modules',{}).get(str(module),{}).get('ledger',{})
    lv,rv=ledger_values(left),ledger_values(right)
    return bool(lv and rv and lv==rv and lv['count']>0 and lv['count']==s.get('modules',{}).get(str(module),{}).get('built') and rv['count']==r.get('modules',{}).get(str(module),{}).get('completed'))

def functional_check(name,s,r):
    reasons=[]
    if not s or not r:return False,['missing sender or receiver report']
    if s.get('error'):reasons.append('sender error: '+s['error'])
    if s.get('run_id')!=r.get('run_id'):reasons.append('run IDs mismatch')
    if r.get('send_errors',0):reasons.append('receiver NACK send errors')
    enabled=[m for m,key in [(1,'ndi_enabled'),(2,'ep_enabled')] if s.get('config',{}).get(key,True)]
    if name.startswith('producer_error'):
        ndi=s.get('modules',{}).get('1',{});ep=s.get('modules',{}).get('2',{})
        if not ndi.get('source_errors',0) or not ndi.get('source_error'):reasons.append('NDI source error not reported')
        if ndi.get('built',0) or r.get('modules',{}).get('1',{}).get('completed',0):reasons.append('invalid BX was transmitted or accepted')
        if not _ledger_equal(s,r,2) or ep.get('source_errors',0):reasons.append('healthy EP source did not continue independently')
        if not r.get('status_applied',0):reasons.append('status stopped after NDI source error')
    elif name.startswith('permanent_loss'):
        if not _ledger_equal(s,r,1):reasons.append('NDI affected by EP permanent loss')
        if not r.get('modules',{}).get('2',{}).get('expired',0):reasons.append('permanently missing EP did not expire')
        if _ledger_equal(s,r,2):reasons.append('fault injection did not cause expected EP loss')
    elif name.startswith('stop'):
        if s.get('ended_reason')!='user_stop':reasons.append('stop did not reach user_stop lifecycle')
    else:
        for m in enabled:
            if not _ledger_equal(s,r,m):reasons.append(f'module {m} delivery summary mismatch')
            if r.get('modules',{}).get(str(m),{}).get('business_errors',0):reasons.append(f'module {m} business decode errors')
    if name.startswith(('first_loss','ndi_loss','crc_damage')):
        if not sum(s.get('modules',{}).get(str(m),{}).get('retransmissions',0) for m in enabled):reasons.append('expected retransmission not observed')
    if name.startswith('crc_damage') and not r.get('invalid_packets',0):reasons.append('expected CRC rejection not observed')
    if name.startswith('duplicates') and not sum(r.get('modules',{}).get(str(m),{}).get('duplicates',0) for m in enabled):reasons.append('expected duplicate not observed')
    if name.startswith('status') and not r.get('status_applied',0):reasons.append('status was not independently applied')
    if name.startswith('status_only'):
        if enabled or any(m.get('built',0) for m in s.get('modules',{}).values()):reasons.append('status-only unexpectedly built data frames')
        sent=s.get('status',{}).get('sent',0)
        if sent<=0 or sent!=r.get('status_applied'):reasons.append('status-only sent/applied count mismatch')
        if s.get('elapsed_us',0)<s.get('config',{}).get('duration_us',0):reasons.append('status-only ended before requested duration')
    if name.startswith('replay_'):
        ndi=s.get('modules',{}).get('1',{});latest=r.get('modules',{}).get('1',{}).get('latest') or {}
        validity=[tool.get('validity') for tool in (latest.get('business') or {}).get('tools',[])]
        expected=[1,2,4] if name.startswith('replay_mixed') else [2,4]
        if validity!=expected:reasons.append(f'BX validity semantics mismatch: {validity}')
        for tool in (latest.get('business') or {}).get('tools',[]):
            if tool.get('validity') in (2,4) and ('position_mm' in tool or 'quaternion' in tool):reasons.append('missing/disabled tool falsely has transform')
        if ndi.get('source_kind')!='replay':reasons.append('replay incorrectly labeled as device acquisition')
        if ndi.get('built',0)>1 and not ndi.get('duplicate_source_frames',0):reasons.append('repeated source frames not exposed')
    return not reasons,reasons

def run_case(sender_path,out,name,profile,seconds=2,extra=(),receiver_extra=(),early_stop=None,expected_sender_exit=0,receiver_path=None):
    out=Path(out)/name;out.mkdir(parents=True,exist_ok=False)
    data_port,command_port=free_ports();run_id=f'{name}-{uuid.uuid4().hex[:12]}'
    sender_report=out/'sender.json';receiver_report=out/'receiver.json';ready=out/'receiver-ready.txt'
    receiver_path=Path(receiver_path or ROOT/'tools/v2_receiver.py').resolve()
    receiver_hash_before=binary_sha256(receiver_path)
    receiver_command=[sys.executable,str(receiver_path),'--port',str(data_port),'--source-port',str(command_port),
                      '--duration','0','--report',str(receiver_report),'--run-id',run_id,'--ready-file',str(ready),*receiver_extra]
    sender_command=[str(sender_path),'--protocol','v2','--network-profile',profile,'--port',str(data_port),'--command-port',str(command_port),
                    '--allowed-nack-port',str(data_port),'--ndi-tools','40','--ep-bytes','204800','--duration',str(seconds),
                    '--run-id',run_id,'--report',str(sender_report),*extra]
    flags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name=='nt' else 0
    env=os.environ.copy();env['PYTHONIOENCODING']='utf-8'
    env['PYTHONPATH']=str(ROOT/'tools')+os.pathsep+env.get('PYTHONPATH','')
    start=time.monotonic();memory=BoundedMemorySampler(start);sender=receiver=None
    failure=None;forced=[];cpu={};cleanup=[]
    binary_before=binary_sha256(sender_path)
    write_json(out/'commands.json',{'sender':sender_command,'receiver':receiver_command,'physical_network_test':False,'sender_sha256':binary_before,'receiver_sha256':receiver_hash_before})
    with (out/'sender.log').open('w',encoding='utf-8') as sl,(out/'receiver.log').open('w',encoding='utf-8') as rl:
        try:
            receiver=subprocess.Popen(receiver_command,stdout=rl,stderr=subprocess.STDOUT,creationflags=flags,env=env)
            deadline=time.monotonic()+8
            while not ready.exists():
                if receiver.poll() is not None or time.monotonic()>deadline:raise RuntimeError('receiver failed to become ready')
                time.sleep(.01)
            sender=subprocess.Popen(sender_command,stdout=sl,stderr=subprocess.STDOUT,creationflags=flags,env=env)
            sender_start=time.monotonic();deadline=sender_start+seconds+15
            while sender.poll() is None:
                now=time.monotonic();memory.sample('sender',sender,now);memory.sample('receiver',receiver,now)
                if receiver.poll() is not None:raise RuntimeError('receiver exited while sender active')
                if early_stop is not None and now-sender_start>=early_stop:
                    if stop_owned(sender):forced.append('sender')
                    break
                if now>deadline:raise TimeoutError('sender timeout')
                time.sleep(.05)
            cpu['sender_seconds']=cpu_seconds(sender)
            if sender.returncode!=expected_sender_exit:failure=f'sender exit {sender.returncode}; expected {expected_sender_exit}'
        except (Exception,KeyboardInterrupt) as exc:
            failure=type(exc).__name__+': '+str(exc)
        finally:
            for label,process in [('sender',sender),('receiver',receiver)]:
                try:
                    if label=='receiver' and process is not None and process.poll() is None:
                        time.sleep(.15);memory.sample('receiver',process,time.monotonic(),force=True)
                    if process is not None:cpu[label+'_seconds']=cpu_seconds(process)
                    if stop_owned(process):forced.append(label)
                except Exception as exc:cleanup.append(label+': '+repr(exc))
    s=read_json(sender_report);r=read_json(receiver_report)
    passed,reasons=functional_check(name,s,r)
    if receiver is not None and receiver.returncode != 0:
        passed=False;reasons.append(f'receiver exit {receiver.returncode}; expected 0')
    if name.startswith(('soak-','long-')) and (not s or s.get('elapsed_us',0)<seconds*1e6 or s.get('ended_reason')=='user_stop'):
        passed=False;reasons.append('requested sustained duration was not completed')
    if failure:reasons.append(failure)
    if forced:reasons.append('forced termination: '+','.join(forced))
    binary_after=binary_sha256(sender_path)
    if not binary_before or binary_before!=binary_after:
        reasons.append('sender binary changed or became unavailable during test');passed=False
    receiver_hash_after=binary_sha256(receiver_path)
    if not receiver_hash_before or receiver_hash_before!=receiver_hash_after:
        reasons.append('receiver script changed or became unavailable during test');passed=False
    reasons.extend(cleanup);passed=passed and not failure and not forced and not cleanup
    joint=build_acceptance(s or {},r);write_json(out/'acceptance.json',joint)
    telemetry={'wall_seconds':time.monotonic()-start,'cpu':cpu,'memory':memory.report(),
               'note':'Windows process CPU time and observed memory; loopback software profile does not verify physical Ethernet.'}
    write_json(out/'resources.json',telemetry)
    result={'name':name,'profile':profile,'seconds_requested':seconds,'functional_pass':passed,'reasons':reasons,
            'strict_acceptance':joint['summary'],'directory':str(out),'run_id':run_id,'loopback':True,
            'sender_exit':sender.returncode if sender else None,'receiver_exit':receiver.returncode if receiver else None,
            'sender_binary':str(sender_path),'sender_sha256':binary_before,'sender_sha256_after':binary_after,
            'receiver_script':str(receiver_path),'receiver_sha256':receiver_hash_before,'receiver_sha256_after':receiver_hash_after,
            'timing':{m:{key:s.get('modules',{}).get(m,{}).get(key) for key in ('built','skipped_slots','initial_incomplete','deadline_misses','initial_duration','sampling_deviation','packet_gap_deviation')} for m in ('1','2')} if s else {}}
    write_json(out/'result.json',result)
    print(json.dumps({'case':name,'functional_pass':passed,'strict_acceptance':joint['summary'],'reasons':reasons}),flush=True)
    return result

def reject_case(sender,out,name,args,expected_reason=None):
    directory=out/name;directory.mkdir();command=[str(sender),'--protocol','v2',*args,'--duration','.1','--report',str(directory/'sender.json')]
    binary_before=binary_sha256(sender)
    p=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=10,text=True,errors='replace')
    (directory/'output.log').write_text(p.stdout,encoding='utf-8')
    # Missing executable/DLL is not evidence of a correct argument rejection.
    binary_after=binary_sha256(sender)
    passed=bool(binary_before and binary_before==binary_after and p.returncode==2 and p.stdout.strip() and (expected_reason is None or expected_reason.lower() in p.stdout.lower()))
    result={'name':name,'functional_pass':passed,'exit':p.returncode,'command':command,'directory':str(directory),'expected':'configuration rejection',
            'sender_binary':str(sender),'sender_sha256':binary_before,'sender_sha256_after':binary_after}
    write_json(directory/'result.json',result);print(json.dumps(result),flush=True);return result

def replay_fixtures(out):
    """Synthetic, byte-exact Rev.9 BX records; never claimed as device captures."""
    directory=Path(out)/'replay-fixtures';directory.mkdir(exist_ok=True)
    def bx(records):
        body=bytes([len(records)])+b''.join(records)+b'\0\0'
        header=struct.pack('<HH',0xa5c4,len(body))
        data=header+struct.pack('<H',crc16(header))+body+struct.pack('<H',crc16(body))
        decode_bx(data)
        return data
    def valid(handle):return bytes([handle,1])+struct.pack('<8fII',1,0,0,0,10,20,30,.2,0x31,8)
    missing=bytes([2,2])+struct.pack('<II',0x31,8);disabled=bytes([3,4])
    values={'mixed':bx([valid(1),missing,disabled]),'missing_disabled':bx([missing,disabled]),
            'capacity':bx([valid(i) for i in range(1,256)]),'ep':bytes((i*17+3)&255 for i in range(204800))}
    invalid=bytearray(values['mixed']);invalid[-1]^=1;values['invalid']=bytes(invalid)
    paths={}
    for name,data in values.items():
        path=directory/(name+('.bin' if name=='ep' else '.bx'));path.write_bytes(data);paths[name]=str(path.resolve())
    write_json(directory/'manifest.json',{'synthetic':True,'note':'Rev.9 format fixtures; 255 records exercise byte-count boundary, not physical device capacity.',
        'files':{name:{'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()} for name,data in values.items()}})
    return paths

def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sender',required=True,type=Path);p.add_argument('--qt-sender',type=Path);p.add_argument('--output',required=True,type=Path)
    p.add_argument('--profiles',default='100m,1g,2g5');p.add_argument('--functional-seconds',type=float,default=2)
    p.add_argument('--soak-seconds',type=float,default=0);p.add_argument('--long-seconds',type=float,default=0)
    p.add_argument('--skip-functional',action='store_true');p.add_argument('--skip-multiport',action='store_true')
    a=p.parse_args(argv);a.sender=a.sender.resolve();a.output=a.output.resolve()
    profiles=a.profiles.split(',')
    if any(x not in ('100m','1g','2g5') for x in profiles):p.error('profiles must be 100m,1g,2g5')
    if any(x<0 or not x<31536000 for x in (a.functional_seconds,a.soak_seconds,a.long_seconds)):p.error('invalid duration')
    if a.skip_functional and not a.soak_seconds and not a.long_seconds:p.error('no tests requested')
    if not a.sender.is_file():p.error('sender executable does not exist')
    if a.qt_sender and not a.qt_sender.is_file():p.error('Qt sender executable does not exist')
    a.output.mkdir(parents=True,exist_ok=False)
    write_json(a.output/'provenance.json',{'source':validation_provenance(ROOT),'sender':str(a.sender),'sender_sha256':binary_sha256(a.sender),
                                        'qt_sender':str(a.qt_sender.resolve()) if a.qt_sender else None,'qt_sender_sha256':binary_sha256(a.qt_sender) if a.qt_sender else None,
                                        'started_utc':__import__('datetime').datetime.now(__import__('datetime').timezone.utc).isoformat()})
    results=[]
    def record(result):
        results.append(result);write_json(a.output/'matrix.json',{'results':results,'functional_pass':all(r['functional_pass'] for r in results),
                       'physical_network':'not_tested','actual_hardware':'not_tested','report_note':'Functional checks and strict timing acceptance remain separate.'})
    if not a.skip_functional:
        fixtures=replay_fixtures(a.output)
        cases=[('normal',[]),('first_loss',['--drop-first','2']),('ndi_loss',['--drop-first','0','--fault-module','1']),
               ('crc_damage',['--corrupt-first','4']),('duplicates',['--duplicate-first','1']),('reverse',['--reverse']),
               ('permanent_loss',['--drop-always','2']),('phase',['--ndi-phase-us','7000']),('status',['--status-interval-us','10000']),
               ('wrap',['--ndi-start-id','4294967294','--ep-start-id','4294967294','--status-start-seq','4294967294','--status-interval-us','10000']),
               ('status_only',['--no-ndi','--no-ep','--status-interval-us','10000'])]
        for profile in profiles:
            for name,extra in cases:record(run_case(a.sender,a.output,f'{name}-{profile}',profile,a.functional_seconds,extra))
        for length in (1,1436,1437,2097152):
            record(run_case(a.sender,a.output,f'boundary-{length}','2g5',a.functional_seconds,['--ep-bytes',str(length),'--no-ndi','--frames','4']))
        record(run_case(a.sender,a.output,'stop-1g','1g',30,early_stop=.3))
        record(reject_case(a.sender,a.output,'invalid-profile',['--network-profile','10g']))
        record(reject_case(a.sender,a.output,'profile-conflict',['--network-profile','1g','--link-mbps','100']))
        record(reject_case(a.sender,a.output,'impossible-100m',['--network-profile','100m','--ep-bytes','2097152']))
        record(reject_case(a.sender,a.output,'replay-combined-capacity-100m',['--network-profile','100m','--ndi-tools','4','--ep-bytes','1','--ndi-replay',fixtures['capacity'],'--ep-replay',fixtures['ep']],expected_reason='capacity'))
        for name in ('mixed','missing_disabled'):
            record(run_case(a.sender,a.output,f'replay_{name}-1g','1g',a.functional_seconds,['--ndi-replay',fixtures[name],'--ep-replay',fixtures['ep']]))
        record(run_case(a.sender,a.output,'producer_error-ndi-isolated','1g',a.functional_seconds,['--ndi-replay',fixtures['invalid'],'--status-interval-us','10000'],expected_sender_exit=2))
        if a.qt_sender:
            for profile in profiles:record(run_case(a.qt_sender.resolve(),a.output,f'qt-normal-{profile}',profile,a.functional_seconds))
        if not a.skip_multiport:
            # Separate source ports sharing a host/network do not multiply link capacity.
            # This is explicitly a concurrent functional stress run, not a profile benchmark.
            with ThreadPoolExecutor(max_workers=2) as pool:
                futures=[pool.submit(run_case,a.sender,a.output,f'multiport-{i}','1g',a.functional_seconds) for i in (1,2)]
                for future in futures:record(future.result())
    if a.soak_seconds:
        for profile in profiles:record(run_case(a.sender,a.output,f'soak-{profile}',profile,a.soak_seconds))
    if a.long_seconds:record(run_case(a.sender,a.output,'long-1g','1g',a.long_seconds))
    return 0 if all(r['functional_pass'] for r in results) else 1

if __name__=='__main__':raise SystemExit(main())
