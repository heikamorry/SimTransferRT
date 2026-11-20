"""在 Windows 本机回环交错比较旧 Qt、Asio 同步、Asio 异步和异步邮箱优化。

例：python tools/compare_asio.py --seconds 3 --rounds 2 --output artifacts/asio/short-01
只测独立基线：追加 --baseline-only。长测：--seconds 1800 --rounds 4 --cases ep-1m
每个 case 使用同一独立 Python 接收器；功能、严格时序、发送端注入和最终交付分别报告。
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import os
from pathlib import Path
import platform
import shutil
import sys

from run_v2_validation import ROOT, binary_sha256, read_json, run_case, validation_provenance, write_json

BASE_SHA = '84c9843f7430076367b2e735c2b46b93ba123ccc'
LOOPBACK = '127.0.0.1'
# 顺序轮转使四轮内每种实现分别出现在每个执行位置；不并发、不同负载不混算。
ARMS = {'A': (), 'B': ('asio-sync', 'off'), 'C': ('asio-async', 'off'), 'D': ('asio-async', 'on')}
# 名称、工具数、EP 帧长、是否丢弃每一帧 EP 的第 2 号首发分片。
CASES = {'ep-200k': (4, 204800, False), 'ep-1m': (4, 1048576, False),
         'ndi-multifrag': (40, 204800, False), 'first_loss-ep': (4, 204800, True)}
BUILD_KEYS = ('CMAKE_GENERATOR', 'CMAKE_BUILD_TYPE', 'CMAKE_CXX_COMPILER', 'CMAKE_CXX_FLAGS',
              'CMAKE_CXX_FLAGS_RELEASE', 'CMAKE_EXE_LINKER_FLAGS', 'CMAKE_EXE_LINKER_FLAGS_RELEASE',
              'Qt6_DIR', 'SIM_WITH_QT', 'SIM_MODEL_PREVIEW', 'SIM_BUILD_TESTS')


def schedule(cases, rounds, baseline_only):
    arms = ['A'] if baseline_only else list(ARMS)
    for r in range(rounds):
        for i, case in enumerate(cases):
            offset = (r + i) % len(arms)
            for arm in arms[offset:] + arms[:offset]:
                yield r + 1, case, arm


def sender_args(case, arm):
    tools, size, loss = CASES[case]
    args = ['--target', LOOPBACK, '--bind', LOOPBACK, '--allowed-nack-ip', LOOPBACK,
            '--ndi-hz', '60', '--ep-hz', '50', '--ndi-tools', str(tools), '--ep-bytes', str(size),
            '--frames', '0', '--ndi-phase-us', '0', '--ep-phase-us', '0', '--status-interval-us', '0',
            '--handoff-trace', 'off', '--fragment-trace', 'off']
    if loss:
        args += ['--fault-module', '2', '--drop-first', '2']
    if arm != 'A':
        backend, fast = ARMS[arm]
        args += ['--transport', backend, '--mailbox-fast-path', fast]
    return args


def build_identity(build):
    cache = build / 'CMakeCache.txt'
    values = {}
    for line in cache.read_text(encoding='utf-8-sig').splitlines():
        if not line.startswith(('#', '//')) and ':' in line and '=' in line:
            k, value = line.split('=', 1)
            values[k.split(':', 1)[0]] = value
    source = Path(values['CMAKE_HOME_DIRECTORY']).resolve()
    exe = (build / 'bin/SimTransferCli.exe').resolve(strict=True)
    compiler = Path(values['CMAKE_CXX_COMPILER'])
    return {'binary': str(exe), 'sha256': binary_sha256(exe), 'bytes': exe.stat().st_size,
            'source_dir': str(source), 'source_at_invocation': validation_provenance(source),
            'build_dir': str(build), 'cache_sha256': binary_sha256(cache),
            'build_flags': {k: values[k] for k in BUILD_KEYS}, 'sim_with_asio': values.get('SIM_WITH_ASIO'),
            'compiler_sha256': binary_sha256(compiler), 'build_ninja_sha256': binary_sha256(build / 'build.ninja')}


def check_builds(base, candidate):
    source = base['source_at_invocation']
    if source['git_head'] != BASE_SHA or source['git_status'] != '':
        raise ValueError('基线不是指定 SHA 的干净工作树')
    flags = base['build_flags']
    if not (flags['CMAKE_GENERATOR'] == 'Ninja' and flags['CMAKE_BUILD_TYPE'] == 'Release'
            and flags['SIM_MODEL_PREVIEW'] == 'OFF' and flags['SIM_WITH_QT'] == 'ON'
            and '14.44.' in flags['CMAKE_CXX_COMPILER'] and '6.12' in flags['Qt6_DIR']):
        raise ValueError(f'基线工具链或配置不符：{flags}')
    if candidate:
        if base['sha256'] == candidate['sha256'] or base['source_dir'] == candidate['source_dir']:
            raise ValueError('基线和候选必须来自不同源码树、不同二进制')
        if candidate['sim_with_asio'] != 'ON' or flags != candidate['build_flags']:
            raise ValueError(f"候选必须启用 Asio 并使用相同编译设置：{candidate['build_flags']}")


def summarize(case, arm, result, expected_sha, receiver_sha):
    directory = Path(result['directory'])
    s, r, joint, resources = [read_json(directory / f'{name}.json') or {}
                               for name in ('sender', 'receiver', 'acceptance', 'resources')]
    tools, size, loss = CASES[case]
    checks = {'ndi_tools': tools, 'ep_bytes': size, 'ndi_hz': 60, 'ep_hz': 50, 'ndi_enabled': True,
              'ep_enabled': True, 'selected_link_mbps': 1000, 'bind_ip': LOOPBACK, 'target_ip': LOOPBACK,
              'duration_us': round(result['seconds_requested'] * 1e6), 'frame_limit': 0,
              'fault_injection': loss, 'allow_oversubscribed': False}
    reasons = list(result['reasons'])
    for k, v in checks.items():
        if s.get('config', {}).get(k) != v:
            reasons.append(f'配置不符或缺失：{k}')
    if not s.get('finished') or s.get('elapsed_us', 0) < checks['duration_us'] or s.get('ended_reason') == 'user_stop':
        reasons.append('未完成所请求的时长')
    if result['sender_sha256'] != expected_sha or result['receiver_sha256'] != receiver_sha:
        reasons.append('跨试次程序或接收器哈希变化')
    backend = s.get('transport', '')
    actual = (backend + ' ' + (s.get('send_api') or '')).lower()
    if arm == 'A' and 'qudpsocket' not in actual or arm != 'A' and ('asio' not in actual or ('async' in actual) != (arm in 'CD')):
        reasons.append('运行时传输后端与试验组不符')
    rows = joint.get('acceptance', [])
    if not joint.get('paired') or any(not any(x['id'] == 'joint_delivery' and x['module_id'] == m
                                              and x['status'] == 'pass' for x in rows) for m in (1, 2)):
        reasons.append('收发报告、来源端口或模块摘要未对账')
    modules = {}
    for m in ('1', '2'):
        tx = s.get('modules', {}).get(m, {})
        rx = r.get('modules', {}).get(m, {})
        metrics = {k: tx.get(k) for k in ('built', 'skipped_slots', 'queue_drops', 'deadline_misses',
                   'initial_incomplete', 'send_errors', 'injected_drops', 'retransmissions', 'datagrams', 'queue_high_water',
                   'sampling_deviation', 'packet_gap_deviation', 'initial_duration')}
        metrics.update({k: rx.get(k) for k in ('completed', 'consumed', 'expired', 'business_errors', 'nack_packets',
                                              'requested_fragments', 'completion')})
        metrics['unaccounted_built_frames'] = (tx['built'] - rx['completed'] if joint.get('paired')
                                              and 'built' in tx and 'completed' in rx else None)
        payload = 11 + 42 * tools if m == '1' else size
        if tx.get('payload_bytes') != payload:
            reasons.append(f'模块 {m} 载荷字节不符')
        metrics['fragments_per_frame'] = (payload + 1435) // 1436
        modules[m] = metrics
    ep = modules['2']
    if loss and not (ep['injected_drops'] and ep['retransmissions'] and ep['nack_packets']):
        reasons.append('未观测到指定 EP 注入、反馈及重传')
    return {'case': case, 'arm': arm, 'run_id': result['run_id'], 'directory': str(directory),
            'functional_pass': bool(result['functional_pass'] and not reasons), 'reasons': reasons,
            'strict': joint.get('summary'), 'strict_failures': [(x['id'], x['module_id']) for x in rows if x['status'] == 'fail'],
            'transport': backend, 'send_api': s.get('send_api'), 'modules': modules, 'elapsed_us': s.get('elapsed_us'),
            'global_packet_gap_deviation': s.get('global_packet_gap_deviation'),
            'resources': resources}


def compact(rows):
    lines = ['单位：µs；每轮独立统计，不合并分位数。NACK 是反馈包，不是丢失数。`—` 为缺失或无样本。', '',
             '|负载/轮/组|NDI 收/建|EP 收/建|跳采 N/E|超期 N/E|首发未全 N/E|EP 首发 P99.9/最大|EP 包隔偏差 P99.9/最大|EP 注入/NACK/重传|功能|严格 过/败/未测|',
             '|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|']
    def fmt(*items):
        return '/'.join('—' if x is None else str(x) for x in items)
    for row in rows:
        n, e = row['modules'].values()
        def tail(key):
            d = e[key] or {}
            return fmt(d.get('p999_us'), d.get('max_us')) if d.get('count') else '—'
        st = row['strict'] or {}
        fields = [f"{row['case']}/{row['round']}/{row['arm']}", fmt(n['completed'], n['built']), fmt(e['completed'], e['built']),
                  fmt(n['skipped_slots'], e['skipped_slots']), fmt(n['deadline_misses'], e['deadline_misses']),
                  fmt(n['initial_incomplete'], e['initial_incomplete']), tail('initial_duration'), tail('packet_gap_deviation'),
                  fmt(e['injected_drops'], e['nack_packets'], e['retransmissions']), '通过' if row['functional_pass'] else '失败',
                  fmt(st.get('pass'), st.get('fail'), st.get('not_tested'))]
        lines.append('|' + '|'.join(fields) + '|')
    return '\n'.join(lines) + '\n'


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--base-build', type=Path, default=ROOT/'build/asio-base-release')
    p.add_argument('--candidate-build', type=Path, default=ROOT/'build/asio-release')
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--seconds', type=float, default=3)
    p.add_argument('--rounds', type=int, default=2)
    p.add_argument('--cases', choices=CASES, nargs='+', default=list(CASES))
    p.add_argument('--baseline-only', action='store_true')
    a = p.parse_args(argv)
    if not (0.001 <= a.seconds < 31536000 and a.rounds >= 1) or len(a.cases) != len(set(a.cases)):
        p.error('需要有效的非零 --seconds、正整数 --rounds 和不重复的场景')
    try:
        base = build_identity(a.base_build.resolve())
        candidate = None if a.baseline_only else build_identity(a.candidate_build.resolve())
        check_builds(base, candidate)
    except (OSError, ValueError, KeyError) as exc:
        p.error(str(exc))
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    receiver = Path(base['source_dir'])/'tools/v2_receiver.py'
    receiver_sha = binary_sha256(receiver)
    plan = list(schedule(a.cases, a.rounds, a.baseline_only))
    write_json(out/'manifest.json', {'utc': datetime.now(timezone.utc).isoformat(), 'command': [sys.executable, __file__,
               *(sys.argv[1:] if argv is None else argv)], 'working_directory': str(Path.cwd()), 'base': base, 'candidate': candidate,
               'python': sys.version, 'os': platform.platform(), 'cpu': platform.processor(), 'logical_processors': os.cpu_count(),
               'receiver': str(receiver), 'receiver_sha256': receiver_sha, 'harness_sha256': binary_sha256(__file__),
               'driver_sha256': binary_sha256(ROOT/'tools/run_v2_validation.py'), 'arms': ARMS, 'schedule': plan,
               'scope': '本机回环；运行时源码快照不等于构建时快照。独立接收进程；注入、反馈、重传及最终交付分开。'})
    for label, identity in [('base', base), ('candidate', candidate)]:
        if identity:
            shutil.copyfile(Path(identity['build_dir'])/'CMakeCache.txt', out/f'{label}-CMakeCache.txt')
    rows = []
    for r, case, arm in plan:
        identity = base if arm == 'A' else candidate
        raw = run_case(identity['binary'], out, f'{case}-r{r:02d}-{arm}', '1g', a.seconds, sender_args(case, arm),
                       ['--bind', LOOPBACK, '--source-ip', LOOPBACK], receiver_path=receiver)
        rows.append(dict(summarize(case, arm, raw, identity['sha256'], receiver_sha), round=r))
        write_json(out/'matrix.json', {'planned': len(plan), 'finished': len(rows), 'rows': rows})
        (out/'summary.md').write_text(compact(rows), encoding='utf-8')
    # 严格时序失败在独立列保留；退出码仅表示功能、试验配置和证据是否有效。
    return 0 if all(r['functional_pass'] for r in rows) else 1


if __name__ == '__main__':
    raise SystemExit(main())
