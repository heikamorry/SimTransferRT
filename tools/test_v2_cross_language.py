"""Run explicitly with --probe path/to/SimRtV2ProtocolTests.exe.

Independent Python encodings are validated/reencoded by C++, including malformed
vectors; C++ originated BX and packets are decoded independently by Python.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import zlib
from v2_protocol import *
from v2_receiver import decode_bx,crc16

def run(probe):
    vectors=[]
    for module in (1,2):
        for size in (1,1436,1437,2097152):
            frame=bytes((i*17)&255 for i in range(size));last=(size-1)//STEP
            for index in sorted({0,last}):
                for retry in (False,True):vectors.append(encode_data(module,0xffffffff,0x0102030405060708,frame,index,retry).hex())
    for indices in ([0],list(range(724)),[1460,0,1460]):vectors.append(encode_nack(2,0x12345678,indices).hex())
    for size in (0,1444):vectors.append(encode_status(0xffffffff,2**63,b'x'*size).hex())
    body=b'\x03'+b'\x01\x01'+struct.pack('<8fII',1,0,0,0,1.5,-2,3,.2,0,8)+b'\x02\x02'+struct.pack('<II',0,8)+b'\x03\x04'+b'\x01\x00'
    header=struct.pack('<HH',0xa5c4,len(body));bx=header+struct.pack('<H',crc16(header))+body+struct.pack('<H',crc16(body))
    vectors.append('bx:'+bx.hex())
    bad=[];original=encode_data(1,1,0,b'x',0)
    for offset in (0,4,6,7,8,9,10,11,16,20,22):
        data=bytearray(original);data[offset]=255;data[-4:]=struct.pack('<I',zlib.crc32(data[:-4]));bad.append(data.hex())
    bad.extend([original[:-1].hex(),original.hex()[:-2]+'00','bx:'+(bx[:-1]+bytes([bx[-1]^1])).hex()])
    process=subprocess.run([str(probe),'--echo-golden'],input='\n'.join(vectors+bad)+'\n',text=True,capture_output=True,timeout=20,check=True)
    lines=process.stdout.splitlines()
    assert lines[:len(vectors)]==vectors,'C++/Python encoded byte mismatch'
    assert lines[len(vectors):]==['ERROR']*len(bad),'C++ malformed vector acceptance mismatch'
    emitted=subprocess.run([str(probe),'--emit-golden'],text=True,capture_output=True,timeout=10,check=True).stdout.splitlines()
    reply=decode_bx(bytes.fromhex(emitted[0][3:]));assert len(reply['tools'])==2
    data=decode_data(bytes.fromhex(emitted[1]));assert data.payload==bytes.fromhex(emitted[0][3:]) and data.module_id==1
    assert decode_nack(bytes.fromhex(emitted[2])).indices==(0,7,1460)
    assert decode_status(bytes.fromhex(emitted[3])).payload==bytes([0,1,255])
    return {'pass':True,'python_to_cpp_valid_vectors':len(vectors),'python_to_cpp_invalid_vectors':len(bad),'cpp_to_python_vectors':len(emitted)}

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--probe',required=True,type=Path);parser.add_argument('--output',type=Path);a=parser.parse_args()
    result=run(a.probe.resolve());text=json.dumps(result,indent=2);print(text)
    if a.output:a.output.write_text(text,encoding='utf-8')
