# V2 Python 接收桩读包顺序修复

日期：2026-11-17。基于 `dev` 提交 `c1f8010`；本次只修改 Python 接收桩、验证工具和相关测试，不改变线上协议与验收阈值。

## 问题与处理

原接收循环先执行缺片/重试检查，再从 UDP socket 读取。若接收线程暂停超过 2ms，尚未被应用读取的首发分片可能已经排在系统队列中；原循环可能先把这些分片列为缺片并请求补发。一次请求可包含很多片，因此补发包数不等于 NACK 次数或丢帧数。

新循环先读取可用数据，再执行完整超时检查。每批最多 64 个数据报，使用 500us 处理预算；来源拒绝、CRC 错误和故意丢片同样计入预算。单包处理或操作系统暂停可能使实际批耗时超过预算，报告保留实际最大值。队列为空也执行超时检查，持续收包也不能无限推迟重试和过期处理。

真正观察到帧内缺口、或收到同模块更新帧引起旧帧尾部判断时，仍立即处理该模块的首轮请求。这个立即路径不执行其它模块的时间超时，也不提前推进已有重试或作废。完整计时器在批末执行；原有 2ms 尾片检测、10ms 后续间隔、最多三轮及第三轮后再等 10ms 作废保持不变，不通过重置原计划时间隐藏迟到。

正常数据包不逐包扫描所有重试状态。只有新发现上述事件时才调用立即处理路径，避免为保持“立即”而给每个正常分片增加无效扫描和计时开销。初版顺序调整的对照未显示稳定改善，因此保留其记录，并以完成此事件触发优化后的独立最终对照作为结论。

## 报告和兼容性

- 现有启动参数、模块统计、NACK 格式与报告字段保留。新增 `receive_loop` 诊断对象，记录批处理上限命中、最大批耗时、循环间隔、请求生成/发送/失败/取消及生成请求时 socket 仍可读的次数。
- `feedback_polls_with_readable_socket` 只说明当时还有数据可读；可能属于另一模块或无效来源，不能直接解释为误补发次数。
- 发送端成功提交的补发包数、接收端生成的 NACK 数与真正唯一丢包数仍分别解释。接收完成后的迟到补发通常计入 `late`，不是全部计入 `duplicates`。
- 验证工具支持为单轮指定旧接收脚本，用于保留版本的对照；记录接收脚本前后 SHA256。接收进程非零退出会使功能验证失败，即使进程退出时留下了完整报告。
- 修改后的 Python 桩在下次启动时生效，继续使用原有命令即可。GUI 发送端可使用现有 Release 程序；本次未改 GUI 内置 C++ 接收路径。

## 验证范围

新增 19 项确定性运行循环测试，覆盖积压尾片优先读取、第三轮到期但尾片已排队、真缺尾的 2ms/10ms/三轮边界、缺口立即请求、跨模块隔离、持续合法/非法/外来源/故意丢弃流量的预算、Windows UDP 错误及 Stop 中拆分 NACK 的取消核对。

本轮原始证据目录：`artifacts/udp-stability/receiver-order-20261117-1814/`。修复前脚本和用户此前双方报告已独立保存在该目录；用户此前轮次提前停止，不能称为完整 30 分钟长测。

实际 UDP 对照使用同一 Qt CLI 发送器、1GbE 软件档、NDI 40 工具/60Hz、电生理 204800 字节/50Hz。按用户后续要求，最终采用旧版、新版各 600 秒顺序实测。先前 60 秒开发对照单独保留，不代替最终十分钟结果。发送器、两版接收脚本及其共享依赖均保留指纹。该对照使用本机环回，不含 GUI 渲染负载，不证明真实网卡性能。

## 边界

有界读包并不等于彻底清空系统接收队列。达到批上限时，相关分片仍可能排在后面；此时计时器仍必须运行，以免其它模块或持续异常流量拖住重试。因此本修复减少一条过早判断路径，不承诺所有补发消失。当前报告没有逐包内核到达时间，不能由汇总数字精确证明每一次补发的原因。

## 最终 1GbE 各十分钟对照结果

电生理补发包数由 25,423 变为 23,659，本轮相对减少 6.94%；额外补发率由 0.592611% 变为 0.551492%。 但修复版电生理有 1 帧首发超期，最大 19.849ms，超过 18ms；本轮不能认定整体实时性能达标。

旧版和修复版各运行 600 秒，先旧后新；使用同一个 Release Qt CLI 发送器，1GbE 软件档、Windows 本机环回（127.0.0.1），未加入 GUI 渲染负载。NDI 为 40 工具、1691 字节/帧、60Hz；电生理为 204800 字节（200KiB）/帧、50Hz。未注入丢包。

| 指标 | 旧版 | 修复版 |
| --- | --- | --- |
| 发送器实际运行时长（秒，含结束宽限） | 600.100701 | 600.113974 |
| NDI 构建 / 完整接收 / 业务消费（帧） | 36000 / 36000 / 36000 | 36000 / 36000 / 36000 |
| 电生理 构建 / 完整接收 / 业务消费（帧） | 30000 / 30000 / 30000 | 30000 / 30000 / 30000 |
| NDI 补发包数 | 0 | 0 |
| 电生理首发包数（补发率分母） | 4290000 | 4290000 |
| 电生理补发包数 | 25423 | 23659 |
| 电生理额外补发率 | 0.592611% | 0.551492% |
| 电生理 NACK 轮数 / NACK 包数 | 320 / 320 | 284 / 284 |
| 电生理 尾片触发 / 帧内缺口触发 | 320 / 0 | 284 / 0 |
| 电生理迟到包 / 重复包 | 25423 / 0 | 23659 / 0 |
| NDI / 电生理首发超期帧数 | 0 / 0 | 0 / 1 |
| NDI / 电生理跳采数 | 0 / 0 | 0 / 0 |
| NDI / 电生理接收作废帧数 | 0 / 0 | 0 / 0 |
| 完整功能校验 | 通过 | 通过 |
| 严格验收：通过 / 失败 / 未检测 | 9 / 4 / 9 | 8 / 5 / 9 |

电生理额外补发率 = 成功提交的补发包数 ÷（成功提交的全部数据包数 − 补发包数）。它衡量额外发送开销，不是实际唯一丢包率，也不是丢帧率。完整接收与消费按收发两方计数和 ID/CRC32/时间戳加和摘要联合核对；有限摘要一致不等于逐帧逐字节的独立证明。

### 时序指标（阈值保持不变）

| 指标 | 阈值 | 旧版实测 | 修复版实测 |
| --- | --- | --- | --- |
| NDI 首发最大耗时 | <= 15000 us | 3839 us；超限 0 / 36000 | 9403 us；超限 0 / 36000 |
| NDI 采样间隔最大偏差 | < 1000 us | 9101 us；超限 97 / 35999 | 11540 us；超限 89 / 35999 |
| NDI 首发逐包间隔最大偏差 | < 1000 us | 6690 us；超限 843 / 71999 | 12738 us；超限 696 / 71999 |
| 电生理 首发最大耗时 | <= 18000 us | 8988 us；超限 0 / 30000 | 19849 us；超限 1 / 30000 |
| 电生理 采样间隔最大偏差 | < 1000 us | 3043 us；超限 44 / 29999 | 2003 us；超限 70 / 29999 |
| 电生理 首发逐包间隔最大偏差 | < 1000 us | 6679 us；超限 25226 / 4289999 | 16145 us；超限 28528 / 4289999 |
| 全流间隔偏差（含补发） | < 1000 us；补充观测 | 最大 6690 us；样本 4387422；超限 27446 | 最大 18142 us；样本 4385658；超限 30534 |
| 全流实际发送调用间隔 | 仅报告；不能脱离帧周期单判超期 | 最大 17527 us；样本 4387422 | 最大 17878 us；样本 4385658 |

首发耗时为采样到最后一次本地发送返回，逐包偏差为首发包实际间隔相对原始调度间隔的偏差；均不等于网线上硬件时间。分位数来自既有直方图，最大值和超限计数按全量统计。

修复版严格验收共 22 项：通过 8 项，失败 5 项，未检测 9 项。功能校验通过不代表全部实时指标通过。

全流偏差包含补发使用历史计划时刻造成的计划差异，不能将其最大值解释为网络卡住同样长时间。自动 22 项验收摘要未单列全流间隔偏差，也未在缺少后续轮次时单列请求迟到失败；本报告将它们补充展示，不隐去超限。

### 接收重试与资源

| 版本 | 请求计时样本 | 最大请求迟到 | 后续重试间隔样本 | 后续间隔最小 / 最大 | 请求迟到单项判定 |
| --- | --- | --- | --- | --- | --- |
| 旧版 | 320 | 1294 us | 0 | 没有后续轮次，未检测 | 失败（原计划偏差要求为 0） |
| 修复版 | 284 | 3581 us | 0 | 没有后续轮次，未检测 | 失败（原计划偏差要求为 0） |

有首轮请求但没有后续轮次时，后续 10ms 间隔未检测；不能把请求最大迟到当成 0。2ms 尾片、立即缺口、10ms 间隔、三轮重试及作废边界另有确定性测试和故障注入回归。

| 进程 | 旧版 | 修复版 |
| --- | --- | --- |
| 发送器 | CPU 106.87% 单核当量；采样峰值私有提交 4.91 MiB | CPU 106.12% 单核当量；采样峰值私有提交 4.96 MiB |
| 接收桩 | CPU 26.67% 单核当量；采样峰值私有提交 15.06 MiB | CPU 30.29% 单核当量；采样峰值私有提交 15.31 MiB |

CPU 为总进程 CPU 时间 / 本轮墙钟时间的单核当量，不是整机 CPU 占用；内存为定期观测峰值，不用于宣称无泄漏。

修复版生成 284 个反馈包，实际成功发送 284 个，发送错误 0 个；生成请求时 socket 仍可读共 283 次。最大批处理耗时 5737us，最大循环入口间隔 17641us。这些观测与有界批末仍有排队数据、线程仍可能受调度影响相符；可读数据未必属于被请求的帧，不能据此精确计算误补发次数。本轮补发下降有限，且时序并未整体改善；短测中的零补发不能代替此次十分钟结论。

修复版接收循环诊断：

```json
{
  "service_calls": 704380,
  "batches": 677707,
  "received_datagrams": 4385659,
  "packet_limit_hits": 19,
  "time_limit_hits": 107137,
  "max_batch_datagrams": 64,
  "max_batch_us": 5737,
  "max_service_interval_us": 17641,
  "timer_polls": 704373,
  "immediate_polls": 0,
  "feedback_generated_packets": 284,
  "feedback_send_attempts": 284,
  "feedback_sent_packets": 284,
  "feedback_cancelled_packets": 0,
  "feedback_polls_with_readable_socket": 283,
  "feedback_send_errors": 0,
  "batch_packet_limit": 64,
  "batch_time_budget_us": 500,
  "order": "bounded_receive_then_timer_poll; observed_gap_first_request_immediate",
  "note": "Host processing times only. A batch limit can leave queued data; readability does not identify which frame is queued or prove network loss."
}
```

### 完整验收表

| 要求 | 测量口径 | 阈值 | 旧版状态及实测 | 修复版状态及实测 | 原因 / 边界 |
| --- | --- | --- | --- | --- | --- |
| [NDI] NDI source continuity | Original independent sample/query slots; no interpolation | 0 skipped slots / queue loss / source error / unmapped device frame | 通过：0 skipped, 0 queue drops, 0 errors, 0 unmapped | 通过：0 skipped, 0 queue drops, 0 errors, 0 unmapped | A source failure remains failed even before its first sample. Hardware sampling is separate. |
| [NDI] NDI sampling deviation (retained engineering target) | Absolute actual production/query interval minus configured nominal period; all intervals | strictly < 1000 us | 失败：9101 us; 97 / 35999 violations | 失败：11540 us; 89 / 35999 violations | Device host query intervals cannot establish hardware sample jitter; the <1 ms engineering threshold is unchanged |
| [NDI] NDI complete initial transmission | Simulation/playback sample to last local send return; device input retains host-request proxy separately | <= 15000 us | 通过：3839 us; 0 late; 0 incomplete | 通过：9403 us; 0 late; 0 incomplete | Local submission only; device common timestamp does not independently establish source-to-host clock offset or physical egress |
| [NDI] NDI packet interval deviation (engineering target) | Actual first-copy interval minus original global scheduling plan interval | strictly < 1000 us | 失败：6690 us; 843 / 71999 violations | 失败：12738 us; 696 / 71999 violations | Full-population counters; original due time is never replaced by catch-up due time |
| [NDI] NDI new hardware sampling and common timestamp | Device frame counter, calibrated clock mapping and measured source rate | V2 ~60 Hz and verified common microsecond clock | 未检测：No physical device timing evidence | 未检测：No physical device timing evidence | Simulation/playback cannot verify real Aurora 40/66 Hz mode or EP sampling clock |
| [电生理] EP source continuity | Original independent sample/query slots; no interpolation | 0 skipped slots / queue loss / source error / unmapped device frame | 通过：0 skipped, 0 queue drops, 0 errors, 0 unmapped | 通过：0 skipped, 0 queue drops, 0 errors, 0 unmapped | A source failure remains failed even before its first sample. Hardware sampling is separate. |
| [电生理] EP sampling deviation (retained engineering target) | Absolute actual production/query interval minus configured nominal period; all intervals | strictly < 1000 us | 失败：3043 us; 44 / 29999 violations | 失败：2003 us; 70 / 29999 violations | Device host query intervals cannot establish hardware sample jitter; the <1 ms engineering threshold is unchanged |
| [电生理] EP complete initial transmission | Simulation/playback sample to last local send return; device input retains host-request proxy separately | <= 18000 us | 通过：8988 us; 0 late; 0 incomplete | 失败：19849 us; 1 late; 0 incomplete | Local submission only; device common timestamp does not independently establish source-to-host clock offset or physical egress |
| [电生理] EP packet interval deviation (engineering target) | Actual first-copy interval minus original global scheduling plan interval | strictly < 1000 us | 失败：6679 us; 25226 / 4289999 violations | 失败：16145 us; 28528 / 4289999 violations | Full-population counters; original due time is never replaced by catch-up due time |
| [电生理] EP new hardware sampling and common timestamp | Device frame counter, calibrated clock mapping and measured source rate | V2 50 Hz and verified common microsecond clock | 未检测：No physical device timing evidence | 未检测：No physical device timing evidence | Simulation/playback cannot verify real Aurora 40/66 Hz mode or EP sampling clock |
| Declared load capacity | Ethernet serialization feasibility screening; not performance measurement | Demand within effective rate and 15/18 ms windows | 通过：8.866400 percent | 通过：8.866400 percent | Capacity screening only; host stalls, state bursts, device timing and physical egress remain unverified |
| Actual network egress deadlines | Hardware NIC or external capture timestamps | NDI <=15 ms; EP <=18 ms | 未检测：Unavailable | 未检测：Unavailable | Windows/host loopback; no physical link or wire timing evidence |
| [NDI] 按模块收发交付对账 | 同 run_id 的构建帧与校验完成帧：数量、ID、CRC32、时间戳摘要 | 各项一致且帧数>0 | 通过：{"sender":{"count":36000,"id_sum":"648018000","crc_sum":"77275061591652","stamp_sum":"968773796518654"},"receiver":{"count":"36000","id_sum":"648018000","crc_sum":"77275061591652","stamp_sum":"968773796518654"}} | 通过：{"sender":{"count":36000,"id_sum":"648018000","crc_sum":"77275061591652","stamp_sum":"990396436567495"},"receiver":{"count":"36000","id_sum":"648018000","crc_sum":"77275061591652","stamp_sum":"990396436567495"}} | 同轮来源端口及计数一致；有界汇总一致不等于逐帧逐字节证明，不抵消跳采、首发超期或显示失败。 |
| [NDI] 重组后独立消费 | 通过业务解析且未被更新帧覆盖的消费数量 | 消费数=完成数；业务解析错误=0 | 通过：{"completed":36000,"consumed":36000,"business_errors":0} | 通过：{"completed":36000,"consumed":36000,"business_errors":0} | NDI使用BX校验；电生理为字节消费接口，真实波形定义尚未提供；缺字段不默认计零。 |
| [NDI] 已观测NACK生成时序 | 全量已生成请求的原定时刻偏差，以及相邻请求间隔最小/最大值 | 原计划偏差0；后续间隔10000us | 未检测：{"retry_intervals":{"count":0,"min_us":null,"max_us":0,"mean_us":null,"p99_upper_us":null},"request_lateness":{"count":0,"min_us":null,"max_us":0,"mean_us":null,"p99_upper_us":null},"rounds":0} | 未检测：{"retry_intervals":{"count":0,"min_us":null,"max_us":0,"mean_us":null,"p99_upper_us":null},"request_lateness":{"count":0,"min_us":null,"max_us":0,"mean_us":null,"p99_upper_us":null},"rounds":0} | 没有重试或缺少完整统计为未检测；生成时间不是实际网卡出线。三轮耗尽、2ms尾部及隔离的完整分支覆盖另由确定性测试证明。 |
| [电生理] 按模块收发交付对账 | 同 run_id 的构建帧与校验完成帧：数量、ID、CRC32、时间戳摘要 | 各项一致且帧数>0 | 通过：{"sender":{"count":30000,"id_sum":"450015000","crc_sum":"6658913280000","stamp_sum":"807311446860006"},"receiver":{"count":"30000","id_sum":"450015000","crc_sum":"6658913280000","stamp_sum":"807311446860006"}} | 通过：{"sender":{"count":30000,"id_sum":"450015000","crc_sum":"6658913280000","stamp_sum":"825330313924977"},"receiver":{"count":"30000","id_sum":"450015000","crc_sum":"6658913280000","stamp_sum":"825330313924977"}} | 同轮来源端口及计数一致；有界汇总一致不等于逐帧逐字节证明，不抵消跳采、首发超期或显示失败。 |
| [电生理] 重组后独立消费 | 通过业务解析且未被更新帧覆盖的消费数量 | 消费数=完成数；业务解析错误=0 | 通过：{"completed":30000,"consumed":30000,"business_errors":0} | 通过：{"completed":30000,"consumed":30000,"business_errors":0} | NDI使用BX校验；电生理为字节消费接口，真实波形定义尚未提供；缺字段不默认计零。 |
| [电生理] 已观测NACK生成时序 | 全量已生成请求的原定时刻偏差，以及相邻请求间隔最小/最大值 | 原计划偏差0；后续间隔10000us | 未检测：{"retry_intervals":{"count":0,"min_us":null,"max_us":0,"mean_us":null,"p99_upper_us":null},"request_lateness":{"count":320,"min_us":3,"max_us":1294,"mean_us":237.709375,"p99_upper_us":700},"rounds":320} | 未检测：{"retry_intervals":{"count":0,"min_us":null,"max_us":0,"mean_us":null,"p99_upper_us":null},"request_lateness":{"count":284,"min_us":1,"max_us":3581,"mean_us":256.27464788732397,"p99_upper_us":600},"rounds":284} | 没有重试或缺少完整统计为未检测；生成时间不是实际网卡出线。三轮耗尽、2ms尾部及隔离的完整分支覆盖另由确定性测试证明。 |
| 共同硬件时间域 | 分层独立证据 | 协议约定或待明确 | 未检测：None | 未检测：None | 未接入并验证共同硬件时间映射；主机单调时钟不是设备采样证据。 |
| 实际网卡出线期限 | 分层独立证据 | 协议约定或待明确 | 未检测：None | 未检测：None | 本地send返回不证明实际出线；需要真实链路或硬件时间戳。 |
| 真实业务波形与显示 | 分层独立证据 | 协议约定或待明确 | 未检测：None | 未检测：None | 独立接收桩不证明GUI渲染；真实电生理格式及硬件尚未提供。 |
| 状态立即发送/应用 | 分层独立证据 | 协议约定或待明确 | 未检测：None | 未检测：None | 文档未给“立即”的数值上限；需报告实际延迟并另行确定验收边界。 |

### 回归与证据

Python 单元测试 **185/185** 通过，其中新增运行循环测试 **19** 项；功能矩阵 **50/50** 通过（46 项实际运行、4 项配置拒绝），覆盖 100Mbps、1GbE、2.5GbE、首发丢片、CRC 错误、乱序、重复、永久缺片、边界尺寸、Stop 和多端口。接收进程非零退出的负例验证按预期判失败。故障负例的预期功能行为通过，不表示其严格验收列必须通过。

双方每轮运行编号：

| 版本 | run_id | 发送 / 接收进程退出码 |
| --- | --- | --- |
| before | soak-before-6028f8b21bd1 | 0 / 0 |
| after | soak-after-9ff2a958f863 | 0 / 0 |

冻结检查：发送器、旧/新接收脚本及共享依赖在两轮结束时全部未改变。完整 SHA256 在 `final-comparison/provenance.json` 和 `integrity.json`；双方原始报告、验收表、命令和资源记录在 `final-comparison/soak-before/` 与 `soak-after/`。

这是一组顺序对照，时序极值还受 Windows 调度和背景负载影响，不能把所有变化归因于修复。没有逐包内核到达时间，不能精确判定每一个补发是否多余；没有真实网卡出线时间、设备共同硬件时钟或 GUI 显示验证。10 分钟内的结果不能证明任意长时间稳定达标。

