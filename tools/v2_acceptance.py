"""Six-column V2 acceptance from paired reports; absence is never success."""
import argparse
import json
from pathlib import Path

def unsigned(value):
    if isinstance(value,bool):return None
    if isinstance(value,str):
        if not value or not value.isascii() or not value.isdecimal():return None
        value=int(value)
    return value if type(value) is int and 0<=value<2**64 else None

def ledger_values(ledger):
    if not isinstance(ledger,dict):return None
    values={k:unsigned(ledger.get(k)) for k in ('count','id_sum','crc_sum','stamp_sum')}
    return values if all(v is not None for v in values.values()) else None

def row(identifier,module,requirement,measurement,threshold,measured,status,reason):
    return dict(id=identifier,module_id=module,requirement=requirement,measurement=measurement,
                threshold=threshold,measured=measured,status=status,reason=reason)

def retry_timing_rows(module, stats):
    intervals = stats.get('retry_intervals') or {}
    timing = stats.get('request_lateness') or {}
    rounds = unsigned(stats.get('nack_rounds'))
    requests = unsigned(timing.get('count'))
    lateness = unsigned(timing.get('max_us'))
    count = unsigned(intervals.get('count'))
    shortest = unsigned(intervals.get('min_us'))
    longest = unsigned(intervals.get('max_us'))

    request_status = 'not_tested'
    if requests is not None and rounds is not None and requests != rounds:
        request_status = 'fail'
    elif requests and lateness is not None:
        if lateness > 0:
            request_status = 'fail'
        elif rounds == requests:
            request_status = 'pass'

    interval_status = 'not_tested'
    if count:
        if ((rounds is not None and count >= rounds)
                or (shortest is not None and shortest != 10000)
                or (longest is not None and longest != 10000)):
            interval_status = 'fail'
        elif rounds is not None and shortest == longest == 10000:
            interval_status = 'pass'

    parts = (request_status, interval_status)
    combined = 'fail' if 'fail' in parts else 'pass' if parts == ('pass', 'pass') else 'not_tested'
    return [
        row('receiver_retry_timing', module, '已观测NACK生成时序', '请求原定时刻偏差及后续生成间隔',
            '原计划偏差0；后续间隔10000us',
            {'retry_intervals': intervals, 'request_lateness': timing, 'rounds': rounds}, combined,
            '任一已测项失败即判失败，两项都有完整合格样本才判通过。'),
        row('receiver_request_lateness', module, 'NACK请求生成迟到', '全部已生成请求与各自原定时刻的偏差',
            '最大迟到0us', {'rounds': rounds, 'request_lateness': timing}, request_status,
            '按接收端主机时钟统计；样本数应覆盖所有请求轮次。'),
        row('receiver_retry_interval', module, '后续NACK生成间隔', '同帧相邻两轮请求的最小及最大间隔',
            '每个间隔10000us', intervals, interval_status,
            '只有同一帧生成第二轮或更后请求时才有间隔样本。'),
    ]

def build_acceptance(sender,receiver=None):
    # Replace sender-only placeholders with receiver evidence, avoiding two
    # contradictory verdicts for the same requirement in the joint table.
    rows=[dict(r) for r in sender.get('acceptance',[]) if r.get('id') not in ('receiver_delivery','nack_timing','status_immediate','display')]
    paired=bool(receiver and sender.get('run_id') and sender.get('run_id')==receiver.get('run_id')
                and sender.get('protocol_version')==receiver.get('protocol_version')==2)
    for module in ('1','2'):
        s=sender.get('modules',{}).get(module,{})
        r=receiver.get('modules',{}).get(module,{}) if paired else {}
        ledger_s,ledger_r=s.get('ledger'),r.get('ledger')
        ls,lr=ledger_values(ledger_s),ledger_values(ledger_r)
        built,received=unsigned(s.get('built')),unsigned(r.get('completed'))
        complete=ls is not None and lr is not None and built is not None and received is not None
        consistent=complete and ls['count']==built and lr['count']==received
        endpoint=receiver.get('endpoint') if paired else None
        local_port=unsigned(sender.get('local_port'))
        endpoint_known=isinstance(endpoint,(list,tuple)) and len(endpoint)==2 and unsigned(endpoint[1]) is not None and local_port is not None
        endpoint_match=endpoint_known and unsigned(endpoint[1])==local_port
        bind_ip=sender.get('config',{}).get('bind_ip')
        if endpoint_known and bind_ip and bind_ip!='0.0.0.0':endpoint_match=endpoint_match and endpoint[0]==bind_ip
        match=consistent and ls==lr and endpoint_match
        count=ls['count'] if ls else 0
        status=('pass' if match else 'fail') if paired and complete and count and endpoint_known else 'not_tested'
        rows.append(row('joint_delivery',int(module),'按模块收发交付对账','同 run_id 的构建帧与校验完成帧：数量、ID、CRC32、时间戳摘要','各项一致且帧数>0',{'sender':ledger_s,'receiver':ledger_r},status,
            '同轮来源端口及计数一致；有界汇总一致不等于逐帧逐字节证明，不抵消跳采、首发超期或显示失败。' if status=='pass' else '收发汇总、独立帧计数或来源端口不一致。' if status=='fail' else '缺少同轮报告、有效uint64摘要、独立帧计数、来源端口或有效帧。'))
        if paired and r:
            consumed=unsigned(r.get('consumed'));completed=unsigned(r.get('completed'));errors=unsigned(r.get('business_errors'))
            consumption_known=all(v is not None for v in (consumed,completed,errors)) and completed>0
            ok=consumption_known and consumed==completed and errors==0
            rows.append(row('consumer_delivery',int(module),'重组后独立消费','通过业务解析且未被更新帧覆盖的消费数量','消费数=完成数；业务解析错误=0',{'completed':completed,'consumed':consumed,'business_errors':errors},'pass' if ok else 'fail' if consumption_known else 'not_tested','NDI使用BX校验；电生理为字节消费接口，真实波形定义尚未提供；缺字段不默认计零。'))
        rows.extend(retry_timing_rows(int(module), r))
    for identifier,title,why in [
        ('hardware_time','共同硬件时间域','未接入并验证共同硬件时间映射；主机单调时钟不是设备采样证据。'),
        ('wire_departure','实际网卡出线期限','本地send返回不证明实际出线；需要真实链路或硬件时间戳。'),
        ('display','真实业务波形与显示','独立接收桩不证明GUI渲染；真实电生理格式及硬件尚未提供。'),
        ('status_immediate','状态立即发送/应用','文档未给“立即”的数值上限；需报告实际延迟并另行确定验收边界。')]:
        rows.append(row(identifier,None,title,'分层独立证据','协议约定或待明确',None,'not_tested',why))
    return {'protocol_version':2,'report_schema_version':1,'kind':'v2_joint_acceptance','run_id':sender.get('run_id'),
            'paired':paired,'columns':['要求','测量口径','阈值','实测值','通过／失败／未检测','原因'],'acceptance':rows,
            'summary':{state:sum(r['status']==state for r in rows) for state in ('pass','fail','not_tested')}}

def main():
    p=argparse.ArgumentParser();p.add_argument('--sender',required=True);p.add_argument('--receiver');p.add_argument('--output',required=True);a=p.parse_args()
    sender=json.loads(Path(a.sender).read_text(encoding='utf-8-sig'))
    receiver=json.loads(Path(a.receiver).read_text(encoding='utf-8-sig')) if a.receiver else None
    result=build_acceptance(sender,receiver);Path(a.output).write_text(json.dumps(result,indent=2,ensure_ascii=False),encoding='utf-8')
    print(json.dumps(result['summary']))

if __name__=='__main__':main()
