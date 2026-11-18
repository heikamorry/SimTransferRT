# SimTransferRT

**用于验证多模块实时 UDP 数据传输协议的模拟发送器。**

SimTransferRT 0.2.0 使用 C++17 和 Qt 6。它按 20 ms（50 Hz）的目标周期采集合成数据或原始文件内容，将最多 32 个在线模块组装成一个逻辑帧，通过 UDP 分片发送。发送器保留最近的帧供 NACK 补传，也能模拟首轮丢片、CRC 损坏、重复、逆序和持续丢片。仓库包含桌面程序、命令行发送器和独立的 Python 接收测试桩，适合在 IPv4 回环或隔离实验网络验证组帧、补传和时序。

- [1. 项目架构与 C++ 技术栈](#1-项目架构与-c-技术栈)
- [2. 获取源码并构建](#2-获取源码并构建)
- [3. 运行发送器与接收测试桩](#3-运行发送器与接收测试桩)
- [4. Debug 和 Release 测试](#4-debug-和-release-测试)
- [5. 日志与报告](#5-日志与报告)
- [6. 打包与部署检查](#6-打包与部署检查)
- [7. 协议摘要与运行边界](#7-协议摘要与运行边界)

## 1. 项目架构与 C++ 技术栈

```text
SimTransfer（Qt Widgets） / SimTransferCli（QCoreApplication）
        │
        ▼
TransferController
        └─ SessionManager
           ├─ QTimer：主线程读取统计快照、分发日志
           ├─ CommandDispatcher：传递停止、在线位图和 NACK
           └─ QThread：持有 QUdpSocket，执行 StreamRunner / RealtimeStream

发送路径   采样 → 组帧 → 三槽缓存 → 分片调度 ── DATA ──→ Python 接收测试桩
反馈路径   三槽缓存补发 ←── NACK 缺片区间 ────────────── CRC 校验、重组、缺片定时
```

[SimRtProbe](src/native_cli.cpp) 是另一条入口。它通过 [原生 UDP 适配器](src/network/NativeUdpTransport.cpp) 直接运行同一个 C++ 核心，供无 Qt 环境和原生 Socket 路径验证。

| 层次 | 主要组件与职责 |
| --- | --- |
| 界面 | [src/ui/](src/ui/) 用纯 C++ 构建 Qt Widgets 界面，显示模块配置、分片、状态与运行日志 |
| 应用 | [src/application/](src/application/) 中的控制器校验参数，会话管理器管理工作线程，命令分发器传递停止、位图和 NACK，`StreamRunner` 驱动 I/O 和采样调度 |
| 实时核心 | [src/core/](src/core/) 提供模块数据源、逻辑帧构建、三槽帧缓存、分片节奏、补传队列和计时统计 |
| 协议与网络 | [src/protocol/](src/protocol/) 负责编解码、长度检查和 CRC32，[src/network/](src/network/) 提供 Qt Network 和原生 Socket 两套 UDP 适配器 |
| 接收与测试 | [tools/](tools/) 用 Python 标准库实现独立协议编解码、接收模型、回环和诊断编排，[tests/](tests/) 包含 C++ 与 Qt 测试 |

CMake 将通用逻辑编入 `sim_rt_core`，Qt 会话和 `QUdpSocket` 适配编入 `sim_rt_app`，Winsock/POSIX Socket 适配编入 `sim_rt_native`。`IUdpTransport` 抽象网络 I/O，测试可通过 `RunnerHooks` 注入时钟和等待函数，复现过载、超期和停止等时序场景。构建目标见 [src/CMakeLists.txt](src/CMakeLists.txt)。

### 线程、原子操作与资源所有权

- **线程归属**　GUI 和 Qt CLI 在各自的主线程处理 Qt 事件。`SessionManager` 使用 `QThread::create` 启动专用工作线程，`QUdpSocket` 在该线程内创建、使用和销毁。同一个工作循环交替处理 DATA 首发、NACK 接收和缓存补发。主线程的 100 ms `QTimer` 用于读取快照，微秒级分片调度由工作线程执行。
- **原子完成标志**　`SessionManager::done_` 是 `std::atomic<bool>`。工作线程发布结束状态后执行 `store(true, std::memory_order_release)`，主线程通过 `load(std::memory_order_acquire)` 观察完成，再调用 `wait()` 回收线程。共享的快照指针、更新标志和日志队列由互斥锁保护。
- **互斥与有界队列**　`std::mutex` 和 `std::lock_guard` 保护 `CommandDispatcher` 的普通命令队列，容量为 512 条。待处理的在线位图合并到最新值，`Stop` 使用独立优先标志。统计通过 `std::shared_ptr<const Snapshot>` 发布，历史只保留最近 20 轮摘要。
- **RAII 与帧缓存**　会话用 `std::unique_ptr<QThread>` 管理工作线程，退出先请求停止再等待。帧缓存用固定的 `std::array<std::unique_ptr<Frame>, 3>` 保存完整帧，补发任务只携带帧号和片号，实际补发前再次查缓存。原帧的时间戳、在线位图和负载保持原样。
- **单调时钟与分片调度**　Windows 使用 QPC，长等待优先使用高分辨率可等待定时器，失败时回退到 `std::this_thread::sleep_for`。短等待在工作线程内自旋。其他平台的时钟基于 `std::chrono::steady_clock`。`PacketPacer` 按绝对时刻计算片间隔，迟到时有限追赶。正常情况下优先发送已到期首发片，在间隙处理补发。

线程归属与代码入口见 [架构说明](docs/ARCHITECTURE.md)、[SessionManager.cpp](src/application/SessionManager.cpp)、[CommandDispatcher.cpp](src/application/CommandDispatcher.cpp) 和 [SamplingClock.cpp](src/core/SamplingClock.cpp)。

## 2. 获取源码并构建

### 准备环境

| 组件 | 要求 |
| --- | --- |
| C++ / Windows 工具链 | C++17。Windows 脚本通过 `vcvars64.bat` 选择 MSVC v143 **14.44**，需要安装该工具集、Windows SDK 和 NMake |
| CMake | 3.21 或更高版本，加入 `PATH` |
| Qt | 6.5 或更高版本，带 Core、Network、Widgets。Windows 使用 **MSVC x64** SDK |
| Python | Python 3，加入 `PATH`，已验证 3.13。接收测试桩和网络测试使用标准库 |
| Ninja | 使用 `core-only` 预设时需要 |

以下命令在源码根目录的 **Windows CMD** 中执行。需要时先完成 GitHub 登录或 Git 凭据验证。

```bat
git clone https://github.com/heikamorry/SimTransferRT.git
cd SimTransferRT
```

脚本默认 `QT_PREFIX` 为 `C:\Qt\6.12.0\msvc2022_64`，`VS_BUILD_TOOLS` 为 `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools`。安装位置有差异时，在执行脚本的同一终端设置实际路径。下面以 Qt 6.11.2 和 VS 2022 Build Tools 为例。

```bat
set "QT_PREFIX=D:\Qt\6.11.2\msvc2022_64"
set "VS_BUILD_TOOLS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
scripts\build.cmd Debug
scripts\build.cmd Release
```

`build.cmd` 依次加载 MSVC 和 Qt 环境、执行 CMake 配置与构建、运行对应的 CTest。省略版本参数时默认 Debug。构建目录中的可执行文件位于目录根部。

| 配置 | CMake 预设 / 输出目录 | 用途 |
| --- | --- | --- |
| Debug | `windows-debug` / `build/windows-debug/` | 本地调试、断点和功能回归 |
| Release | `windows-release` / `build/windows-release/` | 优化构建、性能诊断和打包 |

两种配置均构建 `SimTransfer.exe`、`SimTransferCli.exe`、`SimRtProbe.exe` 和测试。在 **PowerShell** 中可用 `cmd /d /c scripts\build.cmd Release` 执行完整脚本。安装路径覆盖使用 `$env:QT_PREFIX` 和 `$env:VS_BUILD_TOOLS`，在调用前设置。直接运行 CLI 的 `call scripts\env.cmd` 示例应在 CMD 中执行，环境只在调用它的 CMD 会话及其子进程中生效。

VS Code 可通过 `scripts\open-vscode.cmd` 加载开发环境，使用 `Ctrl+Shift+B` 构建 Debug。使用 `F5` 前，按本机环境检查 [.vscode/launch.json](.vscode/launch.json) 中的调试 `PATH` 和可选 `visualizerFile` 路径。直接使用 CMake 预设时，可将 [CMakeUserPresets.json.example](CMakeUserPresets.json.example) 复制为本机 `CMakeUserPresets.json` 再调整 Qt 路径。

### 只构建 C++ 核心

在已加载 C++17 编译器且可以调用 Ninja 的终端执行以下命令。该预设以 Release 构建 `build/core/src/SimRtProbe`（Windows 为 `.exe`），运行三组核心测试，覆盖原生 Socket 入口依赖的核心代码。Qt 集成和 GUI 由 Windows Qt 预设测试。

```sh
cmake --preset core-only
cmake --build --preset core-only
ctest --preset core-only
```

## 3. 运行发送器与接收测试桩

回环联调使用两个终端，均从源码根目录运行。先启动接收桩，看到 `LISTEN` 后再启动 GUI 或 CLI，两种发送入口使用同一个默认本地端口，应依次运行。

| 默认配置 | 值 |
| --- | --- |
| 发送 DATA / 接收桩监听 | `127.0.0.1:5650` |
| 发送器本地绑定 / 接收 NACK | `127.0.0.1:5500` |
| 允许的 NACK 来源 | `127.0.0.1`，来源端口不限 |
| 在线模块与帧大小 | 模块 0，65,531 B 数据 + 5 B 模块头 = 65,536 B |

### 终端 A：启动接收测试桩

```bat
if not exist artifacts\udp-diagnostics\manual mkdir artifacts\udp-diagnostics\manual
python tools\udp_feedback_stub.py --bind 127.0.0.1 --port 5650 --cmd-ip 127.0.0.1 --cmd-port 5500 --run-id local-demo --report artifacts\udp-diagnostics\manual\receiver.json
```

测试桩在收到 DATA 后校验 CRC、按帧号和片号重组，发现缺片时向发送器返回 NACK。缺片再次请求的间隔至少为 10 ms，每片最多请求三次，第三次请求后 10 ms 仍缺片时作废该帧。终端默认每秒打印 `SUMMARY`。发送侧运行结束后，在接收桩终端按 `Ctrl+C` 保存最终报告。也可以用 `--seconds N` 设置接收窗口，默认 `0` 表示持续运行。

### 终端 B：启动 GUI 或 CLI

**GUI**　运行下列任一版本，确认窗口里的网络设置和在线模块符合上表。GUI 会记住上次的网络设置和下一帧编号。然后按「开始实时发送」，观察接收桩 `SUMMARY` 中的 `completed` 增长。结束时按「停止发送」，等待状态变为已停止，再点「导出本地报告」。

```bat
scripts\run.cmd Debug
scripts\run.cmd Release
```

GUI 已内置发送控制和工作线程。运行中可通过「应用在线位图」提交模块在线状态，从下一次采样生效。此前已缓存的逻辑帧保留原位图。

**CLI**　下例使用 Release 采样 100 次，正常结束时写出发送报告，实际成功组帧数见 `stats.built`。Debug 用相同参数，将可执行文件路径换成 `build\windows-debug\SimTransferCli.exe` 即可。要分别保存两轮证据，请为两次收发选用不同的 `run_id` 和输出目录。

```bat
call scripts\env.cmd
build\windows-release\SimTransferCli.exe --frames 100 --frame-bytes 65536 --run-id local-demo --report artifacts\udp-diagnostics\manual\sender.json
```

`--frames 0` 表示持续发送，可按 `Ctrl+C` 请求停止。`--frame-bytes` 是完整逻辑帧大小，包含模块头。选用 `--file` 时，模块 0 使用文件体。完整参数可通过 `SimTransferCli.exe --help` 和 `python tools\udp_feedback_stub.py --help` 查看。

跨机器联调时，分别绑定各自实际网卡 IPv4，接收桩的 `--cmd-ip` 和 `--cmd-port` 指向发送器的 NACK 监听端点。在 GUI 中同时把「数据目标 IP」和「NACK 来源 IP」设为接收桩网卡地址。CLI 的 `--nack-ip` 默认跟随 `--target`，也可显式指定。

### 验证缺片与补传

| 场景 | CLI 参数或 GUI 操作 | 观察结果 |
| --- | --- | --- |
| 首轮丢片和 CRC 损坏 | CLI 增加 `--drop-first 2 --corrupt-first 4`。GUI 故障页把首轮丢片设为 `2`、首轮损坏设为 `4` 后重新开始 | 接收桩请求缺片，发送器统计出现 NACK 和补传，接收报告中的 `recovered` 与 `completed` 增长 |
| 持续丢片 | CLI 增加 `--drop-always 2`，或在 GUI 把持续丢片设为 `2` | 该帧达到重试上限后记入接收端 `expired`，发送器继续产生后续帧 |
| 在线模块切换 | GUI 运行中勾选或取消「在线」，再按「应用在线位图」 | 新采样使用新位图，旧帧可按原位图补发 |

每次手工测试前重新启动接收桩，便于用独立报告核对帧号和计数。接收完成在接收桩本地确认，协议只传输 DATA 和 NACK。

## 4. Debug 和 Release 测试

### 功能回归

```bat
scripts\build.cmd Debug
scripts\test-network.cmd Debug
scripts\build.cmd Release
scripts\test-network.cmd Release
```

- **CTest** 由 `build.cmd` 自动执行。启用 Qt 时共有六组，分别为 `rt_core`、`rt_stability_core`、`rt_stability_runner`、`rt_application`、`rt_stability_ui`、`rt_gui_smoke`，覆盖协议与核心、调度和停止、Qt 应用与 UDP 集成、界面模型和离屏启动。需要重跑测试时用 `ctest --preset windows-debug` 或 `ctest --preset windows-release`，加 `-V` 查看通过用例的输出。
- **Python 与 UDP 回环** 由 `test-network.cmd` 执行。先以 `python -m unittest discover -s tools -p "test_*.py"` 运行协议、接收模型和诊断工具测试，再启动对应版本的 Qt CLI 与测试桩。八种回环场景使用 16 KiB 逻辑帧、各采样 15 次，覆盖正常、首轮丢片、CRC 损坏、重复、逆序、重复 NACK、持续丢片、帧号回绕。持续丢片的通过条件包含按预期作废旧帧并继续发送。

2026-11-16 的 Windows 回环记录中，Debug 和 Release 各通过 **CTest 6/6、Python 133/133、UDP 回环 8/8**。2026-11-13 的 Debug 记录使用当时的较小测试集。见 [验收摘要 JSON](docs/test-results/2026-11-16-acceptance-summary.json)、[统一验收记录](docs/UDP_ACCEPTANCE_2026-11-16.md) 和 [历史构建记录](docs/BUILD_STATUS.md)。

### 时序诊断

先构建相应版本。`test-diagnostics.cmd` 默认使用 Release，核对发送器目录旁的 `CMakeCache.txt`，并记录实际构建配置和文件哈希。Debug 用于功能对照，性能复测通常用 Release。

```bat
scripts\test-diagnostics.cmd --build Release --suite short
scripts\test-diagnostics.cmd --build Debug --cases A1 --seconds 10
scripts\test-diagnostics.cmd --build Release --suite long
```

- **短测** 默认每个场景 10 秒。`A0-closed` 无接收器，`A0-drain` 只收包并丢弃，`A1` 为 1 MiB 完整收发，`B64` 和 `B1` 分别在 64 KiB 和 1 MiB 注入首轮丢片，`C` 持续丢片，`CRC` 注入首轮损坏。`A0-drain` 用于观察发送路径，完整收发场景负责校验 CRC 和重组。
- **长测** 依次执行三轮 `A0-drain` 60 秒加 `A1` 60 秒，最后再跑一次 `A1` 120 秒，名义测试时长约 8 分钟，另有启动和收尾开销。
- **结果判读** 功能回环看用例退出码和 `summary.json` 中的 `pass`。诊断脚本的时序或功能目标失败时返回 `1`，同时保留报告。统一验收表另列出通过、失败和未检测，设备或样本不足的指标保留未检测。

历史 Release `A1` 120 秒测试在 1 MiB、50 Hz 回环下重组了 **6000/6000** 帧，同时记录到 **3 次首轮超期、2 次采样间隔偏差超限、4 次帧内首发片间隔偏差超限**，联合验收结论为失败。见 [完整诊断记录](docs/UDP_DIAGNOSTICS_2026-11-16.md) 和 [统一验收规则](docs/UDP_ACCEPTANCE_2026-11-16.md)。

## 5. 日志与报告

两种构建配置使用相同的应用日志和报告格式。发送侧 JSON 用 `build_type` 标识编译配置。需要比较两次测试时，确认两端的 `run_id`、逻辑帧大小、故障开关和运行时长，再读统计。

| 来源 | 输出与位置 |
| --- | --- |
| 构建与 CTest | 构建脚本输出到终端。CTest 完整记录在 `build/windows-debug/Testing/Temporary/LastTest.log` 或 `build/windows-release/Testing/Temporary/LastTest.log`，重跑会更新对应文件 |
| GUI | 「运行日志」显示带本地时间的近期事件，最多保留 2,000 个文本块。点击「导出本地报告」可分别保存 JSON 快照或 Markdown 验收表，正常停止后导出最终计数 |
| Qt CLI | Qt 运行消息和错误通过 Qt 日志输出，正常结束时将最终 JSON 写到标准输出。`--report` 另外原子保存同一份 JSON。本地执行成功返回 `0`，配置、发送或报告写入失败返回 `2` |
| Python 接收桩 | 默认打印 `LISTEN` 和周期性 `SUMMARY`，结束时保存 `--report` 指定的 JSON，默认文件名为 `receiver-report.json`。`--quiet` 关闭常规终端输出，保留警告和报告 |
| 功能回环 | `artifacts/udp-diagnostics/network-windows-debug/e2e-<ID>/` 或 `artifacts/udp-diagnostics/network-windows-release/e2e-<ID>/`。含 `summary.json`、各用例的收发 `.log`、收发 JSON、环境 JSON 与验收表 |
| 时序诊断 | `artifacts/udp-diagnostics/<日期时间>-<ID>/`。先看套件 `summary.md`，再到场景子目录查看 `sender.log`、`receiver.log`、`sender-report.json`、`receiver-report.json`、`environment.json` 和 `acceptance.json/.md` |

在 PowerShell 中可用 `Tee-Object` 同时保留终端显示和构建日志，`$LASTEXITCODE` 是脚本的退出码。网络测试的终端输出也可用相同方式保存。

```powershell
New-Item -ItemType Directory -Force build\logs | Out-Null
cmd /d /c scripts\build.cmd Debug 2>&1 | Tee-Object -FilePath build\logs\build-debug.log
$LASTEXITCODE
cmd /d /c scripts\build.cmd Release 2>&1 | Tee-Object -FilePath build\logs\build-release.log
$LASTEXITCODE
```

保留 Qt CLI 和接收桩的手工终端输出时，可以在各自的 CMD 中用以下命令替换原启动命令。先创建输出目录，并让终端 A 先启动。重定向后可从 `receiver.log` 查看 `LISTEN`。CLI 仍需在同一 CMD 中先调用 `scripts\env.cmd`。

```bat
rem 终端 A，接收日志和错误写到同一个文件
python tools\udp_feedback_stub.py --run-id local-demo --report artifacts\udp-diagnostics\manual\receiver.json --trace-nacks artifacts\udp-diagnostics\manual\receiver-nacks.jsonl --trace-limit 2048 >artifacts\udp-diagnostics\manual\receiver.log 2>&1

rem 终端 B，分别保存最终 JSON 和 Qt 日志
build\windows-release\SimTransferCli.exe --frames 100 --run-id local-demo --report artifacts\udp-diagnostics\manual\sender.json 1>artifacts\udp-diagnostics\manual\sender.stdout.json 2>artifacts\udp-diagnostics\manual\sender.log
```

- **跟踪与导出**　CLI 的 `--timing-trace on|off` 默认开启，保存有界异常时间线，关闭后仍保留汇总计数。接收桩的 `--trace-nacks` 在退出时写出最近的 NACK 和诊断事件，`--trace-limit` 控制保留条数。`--output` 可写出最近一次完整逻辑帧。自动化运行还可用 `--ready-file` 标记绑定成功，通过 `--status-file` 和 `--checkpoint-every` 保存运行进度。
- **收发标识**　GUI 如需与接收桩使用同一 `run_id`，在启动前于 CMD 执行 `set "SIM_RT_RUN_ID=local-demo"`。CLI 和接收桩使用 `--run-id`。这些标识只写入本地报告。
- **先读发送计数**　`built` 是成功组出的帧数，`datagrams` 是本地 Socket 接受的 DATA 总数，包含 `retransmissions` 记录的补发和故障注入的重复包。`sendErrors` 是本地发送错误，`cacheMisses` 统计 NACK 请求的帧已不在缓存中的情况，`unsentExpired` 是下一次采样到来时上一帧尚未尝试发送的片数。
- **再核对接收结果**　看 `completed`、`expired`、`recovered` 及 `frame_manifest`。`nack_packets`、`nack_send_attempts`、`nack_send_accepted` 分别表示生成、尝试发送和被本地 Socket 接受的 NACK 数。联合验收以发送端 `built_manifest` 为分母，按帧号核对接收完成、作废、在途和未见。
- **最后看时序**　将 `actualInitialDeadlineMisses` 与 `initialIncompleteFrames` 一起看，再查 `timing` 中的首轮耗时、采样偏差和片间隔，以及接收侧的 `receiver_timing`。这些数据来自用户态计时。确认 `acceptance_table` 中的测量口径、阈值、实测值和失败原因，避免用单个计数推断整段运行结果。

字段说明见 [发送端诊断](docs/SENDER_DIAGNOSTICS.md)、[接收端诊断](docs/RECEIVER_DIAGNOSTICS.md) 和 [统一验收表](docs/UDP_ACCEPTANCE_2026-11-16.md)。

## 6. 打包与部署检查

在源码根目录的 CMD 中运行。自定义安装路径时沿用构建阶段的 `QT_PREFIX` 和 `VS_BUILD_TOOLS`。

```bat
scripts\package.cmd
```

脚本先调用 `build.cmd Release` 完成构建和 CTest，再通过 `cmake --install` 安装到 `dist/`。Windows 默认由 Qt 部署脚本复制运行时 DLL 和平台插件。产物为可分发的目录，将整个 `dist/` 一起复制或压缩。

| 路径 | 内容 |
| --- | --- |
| `dist/bin/SimTransfer.exe` | Release 桌面发送器 |
| `dist/bin/SimTransferCli.exe` | Release Qt 命令行发送器 |
| `dist/` 下的运行库和插件 | 包括 `dist/bin/` 中的 Qt DLL 和 `qt.conf`，以及 `dist/plugins/platforms/qwindows.dll`。交付时保留原目录结构 |

接收测试桩位于源码的 `tools/` 目录，独立使用 Python 运行。`SimRtProbe` 随构建生成，安装规则只安装两个 Qt 程序。

发布前，在未装 Qt SDK 的目标 Windows x64 环境检查 MSVC 运行库、Qt 的 Windows 平台插件和防火墙设置。直接启动 `dist\bin\SimTransfer.exe`，运行 `dist\bin\SimTransferCli.exe --help`，再用 Python 接收桩做一次本地收发与停止测试。目标机器的验证结果以实际运行记录为准。

## 7. 协议摘要与运行边界

| 项目 | 格式与行为 |
| --- | --- |
| DATA 与 NACK | `DATA (0x01)` 携带分片，`NACK (0x02)` 用 `(start_frag, count)` 请求片号区间 |
| 编码 | 多字节整数为小端序，Magic `0xAA55A55A` 在线上为 `5A A5 55 AA` |
| DATA 大小 | 32 B 头部 + 最多 1,436 B 负载 + 4 B CRC32，一个 UDP 数据报最多携带 1,472 B |
| 逻辑帧 | 按模块 ID 升序串联 `[module_id:uint8][data_len:uint32 LE][data]`，可跨片，完整逻辑帧最大 2 MiB |
| 补传 | 保留原时间戳、在线位图和负载，设置 `flags` bit 0 后重新计算 CRC32 |

- **采样与容量**　20 ms 采样周期、15 ms 首轮窗口和小于 1 ms 的偏差是待实测的目标。1 MiB 有 731 片，2 MiB 有 1,461 片。2 MiB 在 1 Gbps 上仅原始数据就需要约 16.78 ms，默认配置校验会拒绝这一组合。`--link-mbps` 是容量估算输入，`--allow-oversubscribed` 用于超带宽格式测试。所有模块下线时跳过组帧，超长帧被拒绝，两种情况均不消耗逻辑帧号。
- **缓存与回绕**　默认按 `logic_id % 3` 选槽，保留回绕处的已知提前覆盖行为。`--cache-policy fifo` 可显式启用候选修正。进程重启后应协调重置接收帧状态。协议没有会话 ID 或成功确认消息，与旧版 SimUDP/1 的大端头部不兼容。
- **验收范围**　时间戳来自主机单调时钟，模块体是合成字节或文件内容。Python 桩用于 CRC 校验、补传和重组验证。物理网卡吞吐、线上 DF 位、硬件采样时间戳、长期稳定性和生产端渲染需要目标设备或生产接收端的独立验证。CRC32 用于发现数据损坏，建议在隔离实验网络使用。

详细资料见 [协议字段](docs/PROTOCOL.md)、[兼容性与边界决策](docs/SPEC_DECISIONS.md)、[测试与验收](docs/TESTING.md)、[Windows UDP 稳定性记录](docs/UDP_STABILITY_2026-11-16.md)、[变更清单](docs/CHANGES_2026-11-16.md)、[更新日志](docs/CHANGELOG.md) 和 [技术参考](docs/TECHNICAL_REFERENCES.md)。

[SHA256SUMS.txt](SHA256SUMS.txt) 记录 Git 中的文件内容哈希。文本以 Git 存储的 LF 换行计算，本地检出的 `.cmd` 文件采用 CRLF，直接计算时可能得到不同结果。
