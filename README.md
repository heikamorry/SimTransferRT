# SimTransferRT

SimTransferRT 是用 C++17 和 Qt 6 编写的多模块 UDP 传输模拟器。本分支基于 `dev` 的 0.3.0，提供 V2 双模块发送、接收重组与业务消费，以及独立的 Python 参考接收桩。可以用它复现分片丢失、观察补发、核对收发数据，也可以测量 Windows 主机调度和本地 UDP 提交的时间偏差。

| 入口 | V2 双模块协议 | V1 兼容协议 |
|---|---|---|
| 桌面程序 `SimTransfer` | 默认打开 V2 收发窗口 | 启动时加 `--v1`，或在 V2 窗口打开兼容窗口 |
| Qt 命令行 `SimTransferCli`、原生套接字工具 `SimRtProbe` | 显式加入 `--protocol v2` | 默认按 V1 解析，也可指定 `--protocol v1` |
| 独立接收桩 | `tools/v2_receiver.py` | `tools/udp_feedback_stub.py` |

两版线格式和 NACK 编码不同。测试时选同一协议的两端，同时运行两套会话时分配独立端口。**1 MiB、50 Hz Release 的首发超期、P99.9、最大抖动及接收完整率**见[本次 V2 前后对照](docs/V2_JITTER_COMPARISON.md)。

本分支 `mid-term` 减少了旧帧发送分片时的逐包开销。两次 30 分钟、1 MiB EP 与 4 工具 NDI 的双路回环中，EP 就绪至首片的最大等待从 11.228 ms 降至 5.271 ms。两路的首发超期、P99.9、最大抖动、跳采、接收完整率和 Windows 调度测点见[帧移交诊断与本轮优化对比](docs/V2_HANDOFF_LATENCY.md)。

V2 的“接收与显示”页可打开本地 PLY、VTP、legacy VTK POLYDATA、Aurora BX 帧序列和本程序的 HDF5 过程档案。模型可单向导出为标准 Aurora BX 帧序列：每个顶点一帧，坐标换算为毫米，帧号从 8 起每帧加 8；颜色、面连接、纹理和法向不写入 BX。BX 只做严格校验、解码和点云预览，不反向导出 PLY/VTP/VTK，也不恢复原模型属性。右侧预览支持旋转、缩放、属性切换、点大小调整和经过逐帧回读校验的转换。分发版从 `dist\model-preview\bin\SimTransfer.exe` 启动。参见[操作与报错](docs/MODEL_PREVIEW.md)、[格式能力矩阵](docs/MODEL_FORMATS.md)、[HDF5 数据布局](docs/MODEL_ARCHIVE.md)及[Windows 构建与验证结果](docs/MODEL_VERIFICATION.md)。

V2 与 V1 窗口共用医疗数据链路主题：浅色临床渐变、阿斯克勒庇俄斯之杖水印和心电图基线；EXE、任务栏与窗口图标使用同一单蛇绕杖图形：`resources/app.ico` 提供 9 个尺寸给 Windows 外壳，`resources/icons/app-*.png` 由 `tools/make_app_icon.py` 一并生成，窗口与任务栏图标按 16/20/24/32/40/48/64/128/256 像素直接加载，不依赖可选的 ICO 图片插件。右侧预览以“打开”为主操作，模型、BX 帧序列与过程档案入口集中在同一菜单；转换、取消和重置视图按主次排布，窄窗口下优先保证表格与三维视口可见。

## 1. 数据路径与 C++ 技术栈

V2 的 NDI 定位数据（模块 1）和电生理数据 EP（模块 2）独立成帧，分别持有帧号、采样时间戳与重传缓存。默认 NDI 使用 4 个工具、60 Hz，EP 使用 200 KiB、50 Hz。可用模拟数据或分别回放完整 Aurora BX 应答、原始 EP 二进制文件。GUI 能显示 NDI 姿态和质量字段，以及 EP 字节预览。真实设备采样率、共同硬件时钟、实际网卡出线时刻以及电生理波形解释均需独立验证。

| 阶段 | 代码与职责 |
|---|---|
| 采集与组帧 | [数据源](src/sources/)、[V2 会话](src/v2/Runner.cpp)。每个启用的数据模块有自己的 `std::thread`，按主机单调时钟调度采样、校验并准备首发分片。每路准备队列最多容纳 4 帧，发送端按只读所有权使用准备好的数据。 |
| 发送与反馈 | [发送引擎](src/v2/Engine.cpp)、[UDP 适配器](src/network/)。一个发送线程管理 UDP、两路各自的四帧重传缓存和共用带宽预算。调度顺序为状态、首发、补发。按模块号、帧号和片号响应 NACK。 |
| 编解码 | [V2 协议](src/protocol/V2PacketCodec.cpp)。按小端序编码，使用 CRC32 校验。单模块帧最大 2 MiB，每个 DATA 包最多承载 1436 字节数据。STATUS 是单报文完整状态快照，拥有独立序号。NACK 每包最多携带 724 个片号。 |
| 并发与观测 | 工作线程通过有界队列、`std::mutex` 和短临界区交接帧与统计快照。`std::atomic` 用于停止请求、活动状态及下一采样时刻等独立标志或标量。发送时序使用单调时钟和等待唤醒机制，Windows 使用可等待计时器。网卡观测有独立线程。 |
| 重组与展示 | [C++ 接收器](src/v2/Receiver.cpp)、[V2 窗口](src/ui/V2Window.cpp)、[Python 接收桩](tools/v2_receiver.py)。接收侧按模块和帧号收片、校验、请求缺片、统计完成及消费帧数。Qt 界面线程读取快照，接收工作线程持续处理 UDP。 |

V1 由 [Qt 控制器和会话](src/application/) 调用[实时发送核心](src/core/)，每 20 ms 将在线位图选中的模块合成一个逻辑帧，支持 32 个模块和 2 MiB 上限。发端保留三槽缓存，收端按缺片区间反馈 NACK。V1 的首轮目标为 15 ms。协议字段和兼容选择分别见 [V1 协议](docs/PROTOCOL.md)、[架构与线程](docs/ARCHITECTURE.md) 和[边界约定](docs/SPEC_DECISIONS.md)。

两版均面向隔离实验网。CRC32 检查意外损坏，源端点过滤约束反馈来源。这两项都不提供身份认证。UDP 本地提交成功后，交付情况仍须由接收端报告确认。按 1436 字节载荷计算，1 MiB 为 731 片，2 MiB 为 1461 片。V1 的默认 `logic_id % 3` 槽位策略在编号回绕处存在已知冲突，`--cache-policy fifo` 可显式选择 FIFO 策略。全模块离线时 V1 跳过采样，超长帧拒绝构建，只有成功组帧才分配逻辑帧号。

## 2. 准备环境并构建

Windows 完整构建需要 CMake 3.21 或更高版本、支持 C++17 的 MSVC x64 工具链、NMake，以及 Qt 6.5 或更高版本的 MSVC x64 SDK（Core、Network、Widgets）。测试桩和网络测试使用 Python 3 标准库。将 CMake 和 Python 加入 `PATH`。以下命令在 **Windows CMD** 中执行，从仓库根目录开始。

```bat
git clone --branch mid-term https://github.com/heikamorry/SimTransferRT.git
cd SimTransferRT
```

[scripts/env.cmd](scripts/env.cmd) 默认使用以下路径，并调用 `vcvars64.bat -vcvars_ver=14.44` 选择 MSVC v143 工具集。自定义安装时，把下面的值改为本机实际位置。在每个需要直接运行 Qt 程序的终端中加载环境。

```bat
set "QT_PREFIX=C:\Qt\6.12.0\msvc2022_64"
set "VS_BUILD_TOOLS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
call scripts\env.cmd
```

```bat
:: 首次构建包含三维预览的桌面端时，安装与构建类型一致的 VTK，以及 HDF5 SDK。需要 Ninja。
powershell -NoProfile -File scripts\setup-model-deps.ps1 -Configuration Debug
powershell -NoProfile -File scripts\setup-model-deps.ps1 -Configuration Release
scripts\build.cmd Debug
scripts\build.cmd Release
```

构建脚本依次配置工程、编译并运行 CTest，遇到错误会返回非零退出码。Debug 和 Release 分别输出到 `build\windows-debug\` 和 `build\windows-release\`，GUI、Qt CLI、原生探针均位于对应构建目录的顶层。Debug 便于断点和功能回归，抖动对比使用 Release。`scripts\build-v2.cmd` 可另建 `build\windows-v2-release\` 并运行 Release CTest。使用 PowerShell 时可运行 `cmd /d /c scripts\build.cmd Release`。自定义路径用 `$env:QT_PREFIX` 和 `$env:VS_BUILD_TOOLS` 设置。

若只需无 Qt 的核心、原生探针和相关测试，可以在已初始化 C++ 编译器且装有 Ninja 的终端运行以下命令。

```bat
cmake --preset core-only
cmake --build --preset core-only
ctest --preset core-only
```

## 3. 启动本体和匹配的接收端

### V2 桌面端本机自测

1. 运行 `scripts\run.cmd Release`（或 `scripts\run.cmd Debug`）。在“接收与显示”页保持默认 `127.0.0.1:5650`，点击“启动接收”。
2. 设置发送目标为同一地址和端口，反馈绑定为 `127.0.0.1:5500`，选择数据源和网络档位，点击“开始 V2 发送”。`100m`、`1g`、`2g5` 分别对应 100、1000、2500 Mbps 的软件调度预算。环回测试的档位不能证明物理线路达到该速率。
3. 查看 NDI 和 EP 各自的完整首发、超期、补发和接收消费。先停止发送，接收端继续运行至少 100 ms，再查看“逐项验收”页。点击“导出当前收发报告”保存 JSON，或点击“打开每轮运行档案”查看自动生成的本轮档案。
4. 使用下面的独立接收桩前，先停止占用同一端口的 GUI 接收端。跨机运行时，发送目标填接收机地址，绑定地址填各自的本地接口，并相应设置反馈允许的源地址和 UDP 端口。

### V2 命令行与 Python 接收桩

以下例子发送两路默认模拟数据 10 秒。先在终端 A 建立独立输出目录并启动接收桩，看到 `"ready": true` 后再启动发送器。接收桩把反馈发回实际收到数据的源端点。

```bat
mkdir artifacts\manual-v2
python tools\v2_receiver.py --bind 127.0.0.1 --port 5650 --source-ip 127.0.0.1 --source-port 5500 --run-id demo-v2 --report artifacts\manual-v2\receiver.json
```

在终端 B 的项目根目录加载 Qt 环境并启动发送器。

```bat
call scripts\env.cmd
build\windows-release\SimTransferCli.exe --protocol v2 --target 127.0.0.1 --port 5650 --bind 127.0.0.1 --command-port 5500 --network-profile 1g --duration 10 --run-id demo-v2 --report artifacts\manual-v2\sender.json
```

等待发送器退出、尾部反馈处理完，再在终端 A 按 `Ctrl+C`，接收桩会落盘 `receiver.json`。用以下命令合并同一轮的两端报告。

```bat
python tools\v2_acceptance.py --sender artifacts\manual-v2\sender.json --receiver artifacts\manual-v2\receiver.json --output artifacts\manual-v2\acceptance.json
```

复现本次双模块负载时，在发送命令中加 `--ep-bytes 1048576 --ep-hz 50 --ndi-tools 4 --ndi-hz 60`。若要单独观察 EP，可再加 `--no-ndi`。切换 Debug 时把可执行路径改为 `build\windows-debug\SimTransferCli.exe`。也可用同目录的 `SimRtProbe.exe --protocol v2` 走原生套接字。比较不同版本时固定发送器类型、接收器实现、档位、测试时长与模块设置，每轮写入单独目录。`--run-id` 是本地报告关联字段，协议中没有会话标识。重启两端或重置帧号时可更换端口隔离迟到数据。选项全集可执行 `SimTransferCli.exe --protocol v2 --help` 查看。

### V1 兼容入口

测 V1 时，在终端 A 启动其专用接收桩。

```bat
mkdir artifacts\manual-v1
python tools\udp_feedback_stub.py --bind 127.0.0.1 --port 5650 --cmd-ip 127.0.0.1 --cmd-port 5500 --run-id demo-v1 --report artifacts\manual-v1\receiver.json
```

在终端 B 运行 V1 发送器。

```bat
call scripts\env.cmd
build\windows-release\SimTransferCli.exe --protocol v1 --frame-bytes 65536 --frames 100 --run-id demo-v1 --report artifacts\manual-v1\sender.json
```

示例使用 64 KiB 逻辑帧。发送结束后等待尾部收片，再按 `Ctrl+C` 结束接收桩。需要旧版图形界面时，在已加载 Qt 环境的 CMD 运行 `build\windows-release\SimTransfer.exe --v1`。V1 可用 `--timing-trace on|off` 控制有界异常时间线。

## 4. 跑测试并保留日志

先完成对应配置的构建。下例在已加载 Qt 环境的 CMD 中分别运行 Debug 和 Release。重定向符 `2>&1` 将标准错误并入日志。CTest 把失败用例的输出写入终端，同时在对应构建目录的 `Testing\Temporary\LastTest.log` 保留最近一次运行记录。

```bat
if not exist artifacts\logs mkdir artifacts\logs
ctest --preset windows-debug --output-on-failure > artifacts\logs\ctest-debug.log 2>&1
ctest --preset windows-release --output-on-failure > artifacts\logs\ctest-release.log 2>&1
scripts\test-network.cmd Debug > artifacts\logs\network-debug.log 2>&1
scripts\test-network.cmd Release > artifacts\logs\network-release.log 2>&1
```

| 测试层次 | 覆盖内容与输出 |
|---|---|
| CTest | C++ 核心、协议、Aurora 编解码、V2 引擎／会话／接收器、Qt 应用与 GUI 冒烟等；含 GUI 启动参数／截图回归与 BX 档案回读。`ctest --preset windows-release -N` 列出本次构建注册的用例。 |
| `scripts\test-network.cmd` | 先运行 Python 单元测试，再用对应构建的 Qt CLI 跑 V1 UDP 环回，包括正常、首轮丢片、CRC 损坏、重复、逆序、重复 NACK、持续丢片和帧号回绕。每轮案例的收发日志、JSON 和汇总放在 `artifacts\udp-diagnostics\network-windows-debug\` 或 `network-windows-release\` 下的 `e2e-<编号>\` 子目录。 |
| V2 UDP 功能矩阵 | 下方命令运行独立 Python 接收桩、原生发送器及 Qt CLI，覆盖三档线路预算、故障注入、边界长度、回绕、状态和回放。输出根目录必须是新目录。根部 `matrix.json` 汇总场景，每个正常启动的收发场景含 `commands.json`、`sender.log`、`receiver.log`、两端 JSON、`acceptance.json`、`resources.json` 和 `result.json`。 |
| V1 定时诊断 | `scripts\test-diagnostics.cmd --build Release --cases A64,A1 --seconds 10` 分别跑 64 KiB 和 1 MiB。可用 `--build Debug` 做功能对照。证据在 `artifacts\udp-diagnostics\` 下的独立目录。`A0-drain` 只丢弃收到的报文，接收完整性应看完整重组场景。 |

```bat
python tools\run_v2_validation.py --sender build\windows-debug\SimRtProbe.exe --qt-sender build\windows-debug\SimTransferCli.exe --output artifacts\v2-matrix-debug
python tools\run_v2_validation.py --sender build\windows-release\SimRtProbe.exe --qt-sender build\windows-release\SimTransferCli.exe --output artifacts\v2-matrix-release
```

需要构建全日志时，可用 `scripts\build.cmd Release > artifacts\logs\build-release.log 2>&1`（Debug 同理），用 `echo %ERRORLEVEL%` 查看刚结束命令的退出码。GUI 自动归档位于 Qt 的应用本地数据目录 `v2-runs` 下，每轮一个 JSON。窗口会显示实际路径。CLI 通过 `--report` 指定文件，Qt CLI 也向标准输出打印最终 JSON。保留两端报告、命令行、构建版本及同轮运行编号，才能复查异常。

### 读懂 V2 验收字段

V2 报告按 `modules["1"]`（NDI）、`modules["2"]`（EP）拆分，计时单位为微秒。

| 要回答的问题 | 报告与口径 |
|---|---|
| 首发超了几次？ | 发送端 `deadline_misses` 按模块统计。本地计时从采样到首轮结束，NDI 超过 15,000 µs、EP 超过 18,000 µs 计一次。`initial_complete`、`initial_incomplete` 分开记录，`initial_duration` 分布只统计完整首轮。|
| 采样与发包是否抖动？ | `sampling_deviation` 是实际采样间隔与模块标称周期的绝对差。`packet_gap_deviation` 是同模块相邻首发包的实际间隔与原始计划间隔之差的绝对值。分别查看两者的 `p999_us`、`max_us` 和 `violations`。工程目标为严格小于 1000 µs。分位数按 100 µs 桶估算，最大值保留实测微秒数。 |
| 完整率是多少？ | 在同轮、同模块且 `built > 0` 时，接收完整率为 `receiver.completed / sender.built`，消费率为 `receiver.consumed / sender.built`。同时核对两端 `ledger` 的数量、帧号、CRC 和时间戳汇总，以及发送侧 `skipped_slots`、`queue_drops`。跳过的采样槽不会进入已构建帧的分母。 |
| 整轮是否达标？ | 看 `acceptance.json` 中各项结果及原因。功能交付、严格时序和需要硬件证据的项目分别判断。即使完整率为 100%，迟到的首发或补发恢复的帧仍可能出现。 |

## 5. 打包并验证分发目录

```bat
scripts\package.cmd
scripts\package-v2.cmd
```

择一运行即可。`package.cmd` 会构建普通 Release、运行 CTest，并将 GUI、Qt CLI 和 Qt 运行时安装到 `dist\bin\` 等目录。`package-v2.cmd` 使用独立的 `build\windows-v2-release\`，安装到 `dist\v2-release\`，再将原生探针 `SimRtProbe.exe` 复制到该包的 `bin\`。两种包的 GUI 都支持 V2 和 V1。Python 测试桩随源码保存在 `tools\`。

分发时复制整个安装目录，保留 Qt DLL 和平台插件的目录结构。在没有 Qt SDK 的目标 Windows x64 机器上，先启动包内 `bin\SimTransfer.exe`，再运行 `bin\SimTransferCli.exe --protocol v2 --help`，最后按第 3 节用匹配的接收端完成一次收发。若提示缺少 MSVC 运行库，应核实目标机的 x64 Visual C++ 运行库。

## 6. 协议、诊断与历史结果

- V2 的帧格式、队列上限、接收超时、设备时间边界和网络预算见[实现约定](docs/V2_IMPLEMENTATION_CONTRACT_2026-11-17.md)、[网络档位设计](docs/V2_NETWORK_PROFILE_PLAN_2026-11-17.md)和[Aurora 接入说明](docs/V2_AURORA_UPGRADE_OPTIONS_2026-11-17.md)。
- V2 以前的功能矩阵、长测和主机实测约束见[方案 B 验证记录](docs/V2_PLAN_B_VALIDATION_2026-11-17.md)和[接收桩读包顺序修复](docs/V2_RECEIVER_ORDER_FIX_2026-11-17.md)。本次优化按同负载配对比较，见[1 MiB 抖动实测](docs/V2_JITTER_COMPARISON.md)。
- V1 的[发送端诊断](docs/SENDER_DIAGNOSTICS.md)、[接收端诊断](docs/RECEIVER_DIAGNOSTICS.md)、[统一验收表](docs/UDP_ACCEPTANCE_2026-11-16.md)、[UDP 诊断实例](docs/UDP_DIAGNOSTICS_2026-11-16.md)、[稳定性改动](docs/UDP_STABILITY_2026-11-16.md)和[采样时序复测](docs/TIMING_FIX_2026-11-17.md)包含历史指标及其适用条件。
- 其他入口包括[测试说明](docs/TESTING.md)、[历史构建状态](docs/BUILD_STATUS.md)、[变更记录](docs/CHANGELOG.md)、[变更清单](docs/CHANGES_2026-11-16.md)和[技术参考](docs/TECHNICAL_REFERENCES.md)。`SHA256SUMS.txt` 按 Git 储存的文件内容计算校验值，文本以 Git 中的换行符为准。
