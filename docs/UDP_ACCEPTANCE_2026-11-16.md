# UDP协议统一验收表

本次修改将协议要求、测量口径、阈值、实测值、结果和原因放入同一张验收表。它补充观测与报告，不改变DATA/NACK字节、重传规则、缓存策略或发送周期。缺少硬件、实际链路、接收渲染证据的项目明确保留“未检测”；不能把本地程序退出正常当成协议全面达标。

## 导出方法

- GUI“导出本地报告”默认仍保存JSON，新增`acceptance_table`，原统计字段仍在。文件类型选择“验收表 (*.md)”可保存直接阅读的六列表格；不自动生成或覆盖旁边的JSON。需要两种格式时分别导出。运行中报告仅代表当前快照；正常停止后导出才包含最终计数和有限异常窗口。
- Qt CLI与原生CLI的`--report`仍保存JSON，使用与GUI相同的发送端验收表。
- 接收桩的`receiver-report.json`增加`receiver_timing`及接收端验收表，保留所有旧字段。
- `run_diagnostics.py`每个场景新增`acceptance.json`及`acceptance.md`，`summary.json.acceptance_table`保存同一张联合表，`summary.md`直接展示表格。收发报告必须来自同一run_id，并完成原始帧清单核对。
- `run_e2e.py`的每个功能案例新增`案例名-acceptance.json/.md`；旧功能用例通过与完整协议验收是两个独立结论。

示例：

```powershell
python tools\run_diagnostics.py --cases A1,B1,C,CRC --seconds 10 --out artifacts\udp-stability\acceptance-check
python tools\acceptance_report.py <同一次诊断的场景目录> --out <新的输出目录>
```

离线重建不会覆盖历史报告；旧版没有新增测量的数据标为未检测，不能倒推出逐包间隔或硬件指标。输入文件有64MiB上限，清单沿用既有有界核对；超长会话需要分段。

## 状态与兼容性

`acceptance_table.schema_version=1`，`scope`区分`sender-local`、`receiver-local`和`sender-receiver-combined`。每行固定包含`id`、`requirement`、`measurement`、`threshold`、`actual`、`status`、`status_label`、`reason`、`evidence`。JSON保留证据字段路径，Markdown呈现用户要求的六列。

`status`为`pass`（通过）、`fail`（失败）、`not_measured`（未检测）。未检测也包括样本不足、测量口径不能证明要求、原文未给可判定容差。即使显示实测值，也可能尚无足够条件判定通过。总体有任一失败就是失败；否则存在未检测就是未检测，不以“已测部分通过”冒充完整协议通过。

原报告schema 2、诊断脚本既有验收字段及功能用例返回码语义保留；新表另行给出完整范围状态。GUI和CLI本地报告不能独立判定接收完整率。故意持续丢片C的联合功能预期是全部作废，表中会说明它不代表完整交付；首发完整性仍如实失败。

## 新增发送观测

`stats.samplingIntervalViolations`统计全部实际相邻采样间隔偏离20000µs达到或超过1000µs的次数，独立于trace开关。`timing.abs_interval_minus_20000us.over_limit`已修复为相同严格阈值计数；旧版该字段没有配置阈值、可能恒为0，不能混用旧语义。新增`timing.sampling_violation_rule`标明口径。

新增四个固定容量直方图：

| 字段 | 口径 |
|---|---|
| `timing.initial_packet_gap` | 同一帧内相邻首份实际send调用的起始间隔 |
| `timing.initial_packet_gap_abs_deviation` | 上述间隔偏离名义片间隔的绝对值；最大值须<1000µs |
| `timing.retry_packet_gap` | 相邻补发send调用间隔，包含空闲及穿插首发，不等于接收器的同片10ms请求周期 |
| `timing.socket_call_gap` | 全部send调用的相邻间隔，包含帧间空闲、重复副本和补发，无统一20µs阈值 |

跨帧重置首发相邻段；注入丢弃没有send调用，断开相邻段；额外重复副本只进入全部调用分布；本地发送返回失败仍是实际调用，纳入间隔观测并另外计发送错误。逆序注入按发送ordinal测量，不伪称片号顺序符合正常协议。

分母守恒：`initialFirstCopySendCalls = initialGapSegments + initial_packet_gap.count`；`socketSendCalls = initialFirstCopySendCalls + initialDuplicateSendCalls + retrySendCalls`；全部调用间隔数为`max(socketSendCalls-1,0)`。`stats.initialPacketGapViolations`独立计数偏差≥1000µs；`minInitialNominalGapUs/maxInitialNominalGapUs`显示实际观测段对应的名义间隔范围。`minObservedDatagramBytes/maxObservedDatagramBytes`来自实际调用处包长，不等于网卡抓包。

只有`initial_packet_gap_abs_deviation.over_limit`应用片间隔偏差阈值；原始gap、retry gap和all-socket gap未配置统一阈值，它们沿用的通用`over_limit=0`不表示已通过1ms或10ms验收。验收表使用明确的偏差与违规计数，不使用这些无阈值字段判定。

这些观测复用已有时间戳，不新增逐包时钟读取、锁、日志或动态容器。新增直方图固定约32KiB；仍有计数运算成本，不能声称零开销。分位数仍为100µs桶上界；20µs正常间隔可能显示P99上界100µs，不能把桶上界当精确测量。最大值保留原始观测。

## 新增接收重试时序

`receiver_timing.version=1`记录13种有界直方图：缺片发现到首请求、同片请求间隔、请求调度迟到、静默尾部迟到/年龄、更新帧尾部登记/年龄、第三次请求到作废、最早仍缺片发现到整帧作废、首/末请求到补回、首片到完整重组/作废。

每项导出观察数、min/max/mean、P50/P95/P99/P99.9及明确分母。无样本时`observed=false`且极值/分位数为null，不填零冒充通过。时间为用户态处理时刻，按微秒舍入；不是网卡到达时间或跨机单向延迟。

首请求检查发现后的首个可用poll轮，不把调用方暂停当作逻辑轮次错误，但实际等待仍进入直方图。10ms行检验没有提前；迟到单独显示。第三次请求到作废仅统计确实到期的缺片，整帧作废时被连带取消的较晚缺片不进入该分母。协议“约30ms”未定义容差，因此显示实际跨度并注明无法自动给出严格合规判定，不擅自放宽阈值。

事件环保存最近128条；每次发现批/每帧请求轮最多8片明细，完整直方图仍计全部缺片。总事件数、保留数、覆盖数及片明细截断数均导出。正常完整帧也会写一条完成事件，可能覆盖早期缺片事件；这不是全程逐片抓包记录。收到请求过的原片flags=0仍算补回，原有预算与重传调度不变。

## 联合核对与未覆盖范围

联合表重新从原始`built_manifest`和`frame_manifest`计算结果并与汇总核对，检查run_id、退出码、完整清单、丢弃/作废/未见/在途分类；错配、缺失、截断或排水模式不能通过联合完整性。不能用接收端看到的帧数代替发送端构建分母。

真实硬件TS、各模块触发时差、实际线上DF/带宽/时间戳、20ms显示缓冲、按TS消费和同步渲染仍需相应设备及生产接收端。当前报告明确显示未检测，没有为通过验收而增加协议字段或假造数据。

本轮构建、功能回归及实际导出样本保存在`artifacts/udp-stability/acceptance-20261116/`。

## 本轮验证结果

- 最终Release与Debug均CTest 6/6，通过原核心55项、新核心稳定性21项、runner7项、Qt应用、验收JSON/Markdown/表格及GUI启动检查。日志为`build-release-v3.log`、`build-debug.log`。
- Python共133项通过。最终UDP功能回归Release和Debug各8/8，目录分别为`artifacts/udp-diagnostics/network-windows-release/e2e-3a9bf8743de6/`和`artifacts/udp-diagnostics/network-windows-debug/e2e-3eba42fa668f/`。早期验收表格式测试失败日志保留，修正后通过，不覆盖失败证据。
- 1MiB 10秒矩阵A1、首片丢失B1、CRC损坏各500/500完整；持续丢片C按预期500帧作废。C记录500次首请求、1000次后续请求、500片第三次请求后的到期，最小请求间隔和作废等待均10000µs。四场景为`matrix/20261116-144329-3c1febe6/`。
- 120秒A1为`endurance/20261116-144514-02d66885/A1-0-21eb2a0dfe3d/`：6000/6000完整、0首发未完成，但首发超期3次（最大15811µs）、采样偏差≥1000µs共2次（最大1163µs）、首发片间隔偏差≥1000µs共4次（最大1513µs，原始gap最大1533µs）。4386000次发送调用、4380000个帧内相邻首发gap，分母守恒。双方exit 0，诊断套件exit 1和验收表“失败”正确反映时序未达标。
- 旧版发送器+新接收桩3秒150/150完整，新指标不伪造，表为未检测；历史30分钟报告可离线重建，新测量项目保留未检测，已有接收失败仍为失败。历史原文件未覆盖。
- trace关闭的实际5秒运行250/250完整，仍记录182500个帧内首发gap；正常停止后的JSON有效。原生CLI真实Ctrl+Break测试exit 0，无强杀，约5807µs退出并保存最终报告，停止未发片计账守恒。

这些是报告功能与本次回归证据，不是一次新的30分钟稳定性证明，不将跨时段桌面负载下的较好结果归因为性能优化。本轮未改变系统优先级、绑核或电源策略，也未执行Git提交。
