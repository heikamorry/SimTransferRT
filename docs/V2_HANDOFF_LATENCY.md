# V2 帧移交与首片发送延迟

本篇记录 V2 双模块发送器的帧移交诊断。测试使用 Release 的 `SimTransferCli`、独立 Python 接收桩、IPv4 本机回环和 1,000 Mbit/s 的调度预算。NDI 使用 4 个模拟 BX 工具、60 Hz，EP 使用固定的 1 MiB 模拟载荷、50 Hz。周期状态包和故障注入关闭。上一轮性能对照见 [V2 Release 传输抖动对比](V2_JITTER_COMPARISON.md)。

## 从采样到首片发送

`--handoff-trace on` 按帧记录下列时刻，按模块保留至多 32 个就绪至首发的长延迟样本。普通运行默认关闭这项诊断。

| 字段 | 事件 |
| --- | --- |
| `sample_us`、`ready_us` | 实际采样和帧准备完成。固定 EP 的准备阶段包含帧号、时间戳和 CRC 修正值就绪 |
| `enqueued_us`、`notify_issue_us` | 生产线程将帧放入邮箱以及即将发出通知。发送线程已在运行时，可能先取走帧 |
| `wait_begin_us`、`wait_return_us` | 发送线程与该帧关联的用户态等待边界。没有关联等待时字段为空 |
| `sender_observed_us`、`dequeued_us`、`submitted_us` | 发送线程在入队后的首次可观测循环或队列检查，随后取帧并交给发送引擎 |
| `selected_us`、`first_attempt_us` | 发送引擎选中该帧，以及第一次尝试通过本地 UDP 套接字发送其分片 |

`ready_to_first_us` 包含帧移交、发送仲裁、节拍和系统调度。发送器另记提交时的队首、已有帧的剩余片数，以及这帧首次发送前两路 DATA、状态包和补发的发送尝试数。`notify_issue_us` 是程序调用边界。Windows 的 `ReadyThread` 和 `CSwitch` 分别记录线程变为可运行和实际调度切换，需从内核追踪读取。

## Windows ETW 实测

这轮诊断用 QPC 对齐了应用事件与 Windows 内核事件，验证了发送线程和两路生产线程属于同一发送进程。保存窗口约 20.69 秒，覆盖负载开始后约 4.92 秒。ETL 记录的事件与缓冲区丢失均为零。下表列出触发快照中保留的全部 4 个 EP 就绪至首片至少 7 ms 的样本，计时单位为 µs。详见 [脱敏调度证据](test-results/v2-handoff-scheduler.json)。

| EP 帧 | 就绪至提交 | 提交至首片 | 就绪至首片 | Running | Ready | Waiting | 未分类 | 期间 EP 发送尝试 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 215 | 30 | 17,888 | 17,918 | 13,864 | 4,047 | 0 | 7 | 399 |
| 216 | 675 | 21,083 | 21,758 | 20,463 | 1,255 | 0 | 40 | 697 |
| 217 | 14 | 17,011 | 17,025 | 17,025 | 0 | 0 | 0 | 828 |
| 218 | 15 | 7,645 | 7,660 | 7,660 | 0 | 0 | 0 | 530 |

**EP 第 218 帧**在采样后 9 µs 就绪，5 µs 后入队，再过 9 µs 被发送线程观察到。观察至取出为 1 µs，取出和提交落在同一微秒计时单位内。提交后 7,644 µs 被选中，1 µs 后尝试首片。发送线程在这段 7,660 µs 的窗口内持续处于内核 Running 状态。提交时，第 217 帧还剩 530 片，期间恰好有 530 次旧 EP 分片发送尝试和 1 次 NDI 发送尝试。

第 215、216 帧提交时，前一 EP 帧分别剩 399、697 片，对应期间的 EP 发送尝试数。第 217 帧提交时，第 215 帧剩 97 片，随后还发送了第 216 帧的 731 片，合计 828 次。4 帧的大部分延迟都发生在提交之后。第 215、216 帧还出现了内核 Ready 时间，即线程可运行但暂未获调度。第 215 帧其中一次以 Ready 状态切出，2,088 µs 后再次切入。Running 表示线程被调度到处理器，期间仍可能有 DPC 或硬件中断。

4 个长延迟窗口均无关联的应用等待或 `ReadyThread` 事件。对于已经处于 Running 或 Ready 的线程，可用 `CSwitch` 的切入、切出和切出状态计算调度占用，再与提交时的队首及中间发送数对照。线程从等待变为可运行时，可以配对 `ReadyThread` 与随后的 `CSwitch in`。短时对照中，第 211 帧相应区间为 **32.2 µs**，第 219 帧为 **98.3 µs**，两次用户态等待均返回工作事件。两路共用通知机制，配对时间无法单独确定由哪次通知解除等待。

以下以各帧准备完成为 0，所有列均为 µs。应用记录的通知与内核记录的 Ready 分列。QPC 直接相减得到的亚微秒精度区间，比两个各自取整后的相对时刻更精确。

| EP 帧 | 入队 | 发出通知 | 内核 Ready | 实际切入 CPU | 等待返回 | 发送线程取帧 | 选中 | 首片发送尝试 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 218 | +5 | +6 | 已在运行 | 持续运行 | 无关联等待 | +15 | +7,659 | +7,660 |
| 211 | +3 | +3 | +4 | +37 | +38 | +41 | +267 | +267 |
| 219 | +2 | +3 | +5 | +103 | +106 | +107 | +112 | +113 |

本次捕获支持这 4 个长尾样本主要受到引擎内旧帧发送顺序的影响，Windows 调度延迟叠加在其中两帧。诊断本身开启了帧时间线与 ETW。以上样本用于定位，正式性能对照另行测量。

独立的 30 分钟优化前测试关闭了 WPR。该轮全部 7 次 EP 首发超期都有逐帧记录，从就绪到首片为 9.397–11.228 ms，从交给引擎到选中为 9.394–11.218 ms。每帧等候期间都有 636–692 次旧 EP 分片发送。这份记录复现了排队机制。上一轮历史报告只记录就绪和首次发送时刻，无法倒推出当时每一帧的内核调度状态。

## 改动范围

- [Runner](../src/v2/Runner.cpp) 按已处理的发送尝试数和经过时间安排反馈检查。忙时每处理 8 次发送尝试或距上次检查约 100 µs 就安排检查，空闲检查的名义间隔约为 500 µs。每轮最多读取 4 个反馈包，约 40 µs 的软预算适用于首包之后。每次发送循环仍查看生产者队列和停止标志。新增 `feedback_poll_calls`、`feedback_empty_polls`，用于确认空轮询次数。本轮处理中若计划发送时刻已到，会立即重新检查。到期但引擎拒绝发送时仍做有界等待。
- [Engine](../src/v2/Engine.cpp) 在进入数据包选择前直接检查本地节拍时刻，减少重复扫描候选。保存最多 32 条最差包记录时，缓存其中最小分数的位置。普通包只做一次阈值比较，仅替换保留记录时扫描这 32 条记录。
- 发送优先级、同模块帧顺序、V2 重传规则、NDI 15 ms 与 EP 18 ms 的首发时限沿用既有定义。本轮减少该顺序下的发送循环开销。严格验收仍分别核对首发、采样、片间隔、跳采和收发对账。

## 改动后的对照结果

性能基线是从远端 `dev` 提交 `1848ac8` 加入测点后的 `cb9525f`。优化前后均打开 `--handoff-trace on`，使用同一份 Python 接收桩，前后各连续运行 1,800 秒。Windows ETW 和触发快照仅用于单独的定位轮次。P99.9 采用项目原有的 100 µs 分桶，最大值保留实测微秒精度。重点观察就绪到首片的等待与整帧首发是否超期，同时报告两路抖动、跳采及完整率，分别以实际构建帧数为接收完整率分母。

| 模块 | 版本 | 整帧首发超期 | 就绪至首片 P99.9 / 最大 ms | 整帧首发 P99.9 / 最大 ms | 采样抖动 P99.9 / 最大 ms | 包间隔抖动 P99.9 / 最大 ms | 跳采 | 完整接收 / 构建 | 接收完整率 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| EP | 优化前 | 7 | 0.200 / 11.228 | 12.100 / 23.169 | 1.100 / 79.041 | 0.200 / 73.565 | 29 | 89,971 / 89,971 | 100% |
| EP | 优化后 | 1 | 0.200 / 5.271 | 12.100 / 22.450 | 1.100 / 16.740 | 0.200 / 16.692 | 0 | 90,000 / 90,000 | 100% |
| NDI | 优化前 | 1 | 7.500 / 19.074 | 7.600 / 19.129 | 1.500 / 73.694 | 2.700 / 76.285 | 44 | 107,956 / 107,956 | 100% |
| NDI | 优化后 | 1 | 7.600 / 15.917 | 7.600 / 19.409 | 1.400 / 6.495 | 2.700 / 13.315 | 0 | 108,000 / 108,000 | 100% |

**按本次配对实测，保留优化版 `7797b41` 到 `mid-term`。** EP 等待至少 7 ms 的帧从 7 个降至 0 个，最大等待由 11.228 ms 降至 5.271 ms，整帧首发超期由 7 次降至 1 次。EP 与 NDI 的跳采分别从 29、44 次降至 0 次，两路所有构建帧均已接收并消费，帧数、帧号、CRC 与时间戳账本吻合。两轮均没有发送队列丢帧或未完成首发。

EP 就绪至首片 P99.9 两版均为 0.2 ms。本次收益集中在极端等待。剩余一次超期的 EP 第 30,610 帧准备耗时 5 µs，就绪后 8 µs 就尝试首片。首片开始到整帧首发结束用了 22.437 ms，总计 22.450 ms。本例应查首轮发送过程中各分片之间及本地调用的停顿。NDI 超期次数维持在 1 次，整帧首发最大值从 19.129 ms 升至 19.409 ms，NDI 就绪至首片 P99.9 从 7.5 ms 升至 7.6 ms。

仅比较同机各一次的 30 分钟回环。采样和包间隔的最大抖动均仍超过 1 ms，也没有测量物理网卡出线、真实设备采样时钟或多主机表现。两个版本的功能校验都通过，严格验收分别有 12 项和 10 项失败。基线没有空反馈读取计数，无法据此为两处代码改动分别估算贡献。

Debug 和 Release 各通过 12/12 项 CTest，Python 单元测试通过 188 项，三个带宽档位的 V2 功能矩阵通过 50/50 个场景，覆盖有反馈时的补传、丢片、损坏、乱序、停止及双端口收发。[下载两轮原始发送、接收和验收报告](test-results/v2-handoff-30min.zip)

另按优化版在先、基线在后的顺序，各做了一次关闭帧追踪的 30 秒启动与收发检查。两版均收齐 EP 1,500/1,500 帧、NDI 1,800/1,800 帧，超期、跳采均为零。短测的包间隔最大抖动分别为 EP 3.673 ms（基线）和 5.606 ms（优化版），NDI 为 3.802 ms 和 5.574 ms，说明最大值会随测量窗口变化。

| 版本 | 运行 ID | Release 发送器 SHA-256 |
|---|---|---|
| 优化前 | `soak-before-f58bd8ed0c32` | `11c5ec70d9580d4f68dee49c3d647bdef32b462f370b18c4ee9e95633cddc192` |
| 优化后 | `soak-after-bf4712b3d6b5` | `5ad648870b0866280ae772dfd4b63d71c909873c9ce477c8d981a65ec0b954c9` |

## 复现

在仓库根目录用 PowerShell 运行以下命令。先设置本机 `QT_PREFIX` 和 `VS_BUILD_TOOLS`，并确认 Python 在 PATH 上。命令会等待接收桩就绪，为两端分配各自的本地端口，并写出发送、接收和联合验收报告。

```powershell
cmd /d /c scripts\build.cmd Release
if ($LASTEXITCODE -ne 0) { throw 'Release 构建失败' }
$env:PATH = (Join-Path $env:QT_PREFIX 'bin') + [IO.Path]::PathSeparator + $env:PATH
@'
from pathlib import Path
import sys
import uuid

root = Path.cwd()
sys.path.insert(0, str(root / "tools"))
from run_v2_validation import run_case

out = root / "artifacts" / "udp-stability" / ("handoff-" + uuid.uuid4().hex[:8])
options = [
    "--ep-bytes", "1048576", "--ep-hz", "50",
    "--ndi-tools", "4", "--ndi-hz", "60",
    "--ndi-phase-us", "0", "--ep-phase-us", "0",
    "--status-interval-us", "0", "--handoff-trace", "on",
]
result = run_case(root / "build/windows-release/SimTransferCli.exe",
                  out, "loopback", "1g", seconds=1800, extra=options)
print(out / "loopback")
raise SystemExit(0 if result["functional_pass"] else 1)
'@ | python -
```

从 `sender.json` 读取 `handoff_trace` 和各模块的 `ready_to_first` 分布，从 `acceptance.json` 核对联合验收。复现本次正式对照时，分别检出 `cb9525f` 和优化版，在两次运行中均使用 `--handoff-trace on`，关闭 ETW 和触发快照。默认关闭帧追踪的运行可用于单独观察生产配置。

### 可选 Windows 内核追踪

[本次使用的 WPR 配置](../tools/handoff_etw/SchedulerMemory.wprp) 分配 128 MiB 循环内存，收集线程生命周期、`CSwitch`、`ReadyThread`、DPC 及中断。先在普通权限终端完成 Release 构建并设置 Qt 的 `PATH`。再在有系统追踪权限的独立管理员 PowerShell 7 窗口，从仓库根目录执行下列命令。命令为本次录制创建独立的实例和目录，最多等待 45 秒。出现 `trace active` 后，回到普通权限终端，仅运行上一节的 Python 联调命令，可将持续时间改为 30 秒。若需要遇到长延迟就保存，在 `options` 中附加 `--handoff-trigger-report` 和该捕获目录内尚不存在的 `trigger.json` 的绝对路径。监控线程首次观察到 EP 就绪至首片达到 7 ms 后保存一次快照。

```powershell
$root = (Get-Location).Path
$tag = [guid]::NewGuid().ToString('N')
$capture = Join-Path $root "artifacts/udp-stability/handoff-etw-$tag"
New-Item -ItemType Directory -Path $capture | Out-Null
$instance = "SimTransferHandoff-$tag"
$profile = Join-Path $root 'tools/handoff_etw/SchedulerMemory.wprp'
wpr -start "$profile!SimTransferHandoff.Light" -instancename $instance
if ($LASTEXITCODE -ne 0) { throw 'WPR 启动失败' }
$saved = $false
try {
    Write-Host "trace active: $capture"
    $trigger = Join-Path $capture 'trigger.json'
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt 45 -and -not [IO.File]::Exists($trigger)) {
        Start-Sleep -Milliseconds 100
    }
    if ([IO.File]::Exists($trigger)) { Start-Sleep -Milliseconds 250 }
    wpr -stop (Join-Path $capture 'scheduler.etl') -skipPdbGen -instancename $instance
    $saved = $LASTEXITCODE -eq 0
    if (-not $saved) { throw 'WPR 保存失败' }
} finally {
    if (-not $saved) { wpr -cancel -instancename $instance }
}
```

录制结束后，在已加载 x64 MSVC 的普通权限 PowerShell 7 中，将 `$capture` 设为上一步输出的目录。使用 [原生提取器](../tools/handoff_etw/dump-scheduler.cpp) 读取 ETL 的原始 QPC 记录，[对齐工具](../tools/handoff_etw/align-scheduler.py) 将选定帧的时间窗划分为 Running、Ready、Waiting 和未分类。以下示例使用触发快照中 EP 延迟最大的一帧。若没有触发快照，可改用同轮 `sender.json` 中确认落在捕获窗口内的帧。使用 PowerShell 7 或 CMD 保存本地提取输出，便于按 UTF-8 读取。

```powershell
$tools = Join-Path (Get-Location).Path 'tools/handoff_etw'
$snapshot = Get-Content (Join-Path $capture 'trigger.json') -Raw | ConvertFrom-Json -Depth 50
$trace = $snapshot.handoff_trace
$frame = $trace.worst_first_dispatch.'2' | Sort-Object ready_to_first_us -Descending | Select-Object -First 1
Push-Location $capture
try {
    cl /nologo /std:c++17 /O2 /W4 /EHsc /utf-8 (Join-Path $tools 'dump-scheduler.cpp') advapi32.lib /Fe:dump-scheduler.exe
    if ($LASTEXITCODE -ne 0) { throw 'ETL 提取器编译失败' }
    .\dump-scheduler.exe scheduler.etl $trace.sender_tid 1>sender-scheduler.csv 2>trace-meta.json
    if ($LASTEXITCODE -ne 0) { throw 'ETL 提取失败，详见 trace-meta.json' }
    python (Join-Path $tools 'align-scheduler.py') --csv sender-scheduler.csv --meta trace-meta.json `
      --tid $trace.sender_tid --pid $trace.process_id --qpc-frequency-hz $trace.qpc_frequency_hz `
      --begin-us $frame.ready_us --end-us $frame.first_attempt_us
} finally { Pop-Location }
```

核对 `contains_full_window`、事件与缓冲区的丢失计数、目标 PID 和未分类时间。循环内存可能覆盖较早事件。工具接受本次捕获的内核记录版本和相应的经典布局。没有内核记录时，仍可通过应用阶段、队首、发送计数和关联的等待边界定位程序内延迟。内核 Ready 时间保留为未知。原始 ETL、提取文件和编译产物均留在本地 `artifacts/`。相关操作见 [Windows Performance Recorder 命令](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/wpr-command-line-options) 和 [Windows CPU 调度分析](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/cpu-analysis)。
