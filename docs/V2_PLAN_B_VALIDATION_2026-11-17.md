# 方案 B 逐项验证与长测报告

日期：2026-11-17。分支：`dev`。Release：0.3.0。

**结论：三档共 330000 帧全部收齐并消费。1GbE 的 30 分钟与 2.5GbE 的 10 分钟本地完整首发时限通过；100Mbps 的 10 分钟首发时限失败。三档均存在采样或首发包间隔偏差达到/超过 1ms，尚未全面满足严格实时目标。**

本报告区分功能正确性、严格软件时序和真实硬件验收。50 项功能用例通过不代表所有实时指标通过；正常场景和故意注入的负例分别判断。

## 本轮完成的范围

- 保留实施前 19 个已有修改文件；其中 17 个逐字节未变，README 与原生 CLI 在保留原内容的基础上扩展。已有源文件及未跟踪正式文档已备份。
- 清除 137 个 PDF 解析临时文件，空目录保留；没有删除原 PDF 或正式报告。
- 新增 V2 独立双路准备、统一发送、分模块缓存/重传/接收消费、Aurora BX 模型及三档网络配置；V1 明确入口保留。
- 按用户最新要求，1GbE 长测 30 分钟，100Mbps 与 2.5GbE 各 10 分钟，顺序执行。原建议两小时已由用户调整，本报告不将短测称为两小时证据。

## 回归结果

| 验证 | 结果 | 证据 |
| --- | --- | --- |
| Release C++ / Qt | 12 / 12 通过 | unit/ctest-release-final.log |
| Python 协议、接收、诊断和验收 | 166 项通过 | unit/python-unittest.log |
| V2 实际 UDP 功能矩阵 | 50 / 50 通过 | functional-final/matrix.json |
| V1 实际 UDP 兼容回归 | 8 / 8 通过 | v1-compatibility/e2e-24506ae44e1e/summary.json |
| 跨语言字节向量 | 30 合法 + 14 非法 + 4 反向输出通过 | cross-language.json |
| 提前结束长测负例 | 正确判失败，验证通过 | duration-guard/long-deliberately-interrupted/result.json |
| Windows UDP 重置兼容 | 实际丢片恢复通过，WSAIoctl 已配置 | windows-receiver-compatibility/first_loss-windows-receiver/ |
| 最终发行程序本机启动 | 3 / 3 通过，PATH 不含 Qt SDK | packaged-smoke-final.json |
| 冻结版本一致性 | 运行/测试代码与三个二进制均未改变 | final-integrity.json |

功能矩阵包含三档、丢片、CRC 损坏、重复、逆序、永久丢片、相位、状态、序号回绕、1 字节至 2MiB 边界、快速停止、预检拒绝、BX 回放、源故障隔离、Qt 路径及多端口隔离。永久丢片等负例验证预期失败，不能据其功能通过推导正常交付通过。

## 长测条件与完成情况

发送器为打包后的 Qt CLI，使用与 GUI 相同的 V2 发送引擎；独立 Python 接收器负责重组和业务消费。目标为本机 127.0.0.1 环回，三档是软件线路预算，未通过真实 100M / 1G / 2.5G 网卡链路。

负载：NDI 模拟 60Hz、40 条有效 BX 记录（1691 字节/帧）；电生理模拟 50Hz、204800 字节/帧。204800 是将“200KB”按保守 200KiB 解释的测试默认值，真实输入按实际长度。未注入丢包，未启动 GUI 渲染负载。

| 档位 | 要求时长 | 发送器实测时长 | 功能对账 | 自动验收：通过/失败/未检测 | 全局逐包补充判定 |
| --- | --- | --- | --- | --- | --- |
| 1GbE | 1800 s | 1800.161 s | 通过 | 9/4/9 | 失败 |
| 100Mbps | 600 s | 600.147 s | 通过 | 7/7/8 | 失败 |
| 2.5GbE | 600 s | 600.148 s | 通过 | 9/4/9 | 失败 |

自动六列表原样保留，共22行。文末另补全局混合序列偏差、NDI请求生成迟到、电生理请求生成迟到，共3行，均不混入原自动计数。补充后完整25行的通过/失败/未检测分别为：1GbE 9/6/10，100Mbps 7/9/9，2.5GbE 9/6/10。时长包含运行结束后的补发收尾，因此略大于设定时长。

## 三项时序指标

首发阈值：NDI ≤15ms、电生理 ≤18ms。采样和逐包间隔偏差延续原附加工程目标，均严格 <1ms。首发是模拟采样到最后一次本地 send 返回，采样偏差是实际生产间隔与配置周期之差；不冒充硬件采样或网卡出线。表内超限计数使用全部样本。

| 档位/模块 | 首发最大 ms | 首发超期/完整首发 | 采样偏差最大 ms | 采样超限/区间 | 逐包偏差最大 ms | 逐包超限/区间 |
| --- | --- | --- | --- | --- | --- | --- |
| 1GbE/NDI | 4.204 | 0 / 108,000 | 3.124 | 169 / 107,999 | 3.923 | 602 / 215,999 |
| 1GbE/电生理 | 9.139 | 0 / 90,000 | 2.940 | 107 / 89,999 | 7.124 | 25,694 / 12,869,999 |
| 100Mbps/NDI | 17.298 | 786 / 36,000 | 3.281 | 58 / 35,999 | 6.524 | 197 / 71,999 |
| 100Mbps/电生理 | 24.363 | 6,783 / 30,000 | 2.623 | 59 / 29,999 | 4.575 | 187 / 4,289,999 |
| 2.5GbE/NDI | 3.740 | 0 / 36,000 | 2.725 | 57 / 35,999 | 4.510 | 107 / 71,999 |
| 2.5GbE/电生理 | 6.235 | 0 / 30,000 | 2.375 | 56 / 29,999 | 5.190 | 2,770 / 4,289,999 |

全局交错发送指标单列：

| 档位 | 全局原始包间隔最大 ms | 全局包间隔偏差最大 ms | 超限/全部区间 | 结论 |
| --- | --- | --- | --- | --- |
| 1GbE | 18.892 | 4.901 | 29,551 / 13,107,840 | 失败 |
| 100Mbps | 6.901 | 76.282 | 918 / 4,370,614 | 失败 |
| 2.5GbE | 19.269 | 5.482 | 4,830 / 4,363,928 | 失败 |

原始包间隔包含正常帧间空闲，不能把它直接同 1ms 比较；验收比较的是相对原计划的偏差。原计划发包迟到另见各模块 `original_plan_lateness`，不能和包间隔偏差混用。

全局混合序列还包含补发：首发使用原排程，补发使用历史 NACK 请求时刻；计划序列切换可能显著放大偏差。因此百兆全局 76.282ms 不代表连续停发 76ms，其实际相邻发送调用的最大间隔为 6.901ms。保留的最差事件均是等待约 76.5～76.8ms 的补发，发生于运行中段；不是结束收尾。未保留产生最大差值的完整相邻包对，不能声称已逐包精确定位。全局项仅按既有计数定义作为补充工程诊断，不能替代分模块首发验收，也不能和实际线路抖动混为一谈。

## 连续性、交付与消费

| 档位/模块 | 构建帧 | 跳采/队列丢弃/源错误 | 首发不完整 | 接收完成 | 消费完成 | 业务错误 |
| --- | --- | --- | --- | --- | --- | --- |
| 1GbE/NDI | 108000 | 0/0/0 | 0 | 108000 | 108000 | 0 |
| 1GbE/电生理 | 90000 | 0/0/0 | 0 | 90000 | 90000 | 0 |
| 100Mbps/NDI | 36000 | 0/0/0 | 0 | 36000 | 36000 | 0 |
| 100Mbps/电生理 | 30000 | 0/0/0 | 0 | 30000 | 30000 | 0 |
| 2.5GbE/NDI | 36000 | 0/0/0 | 0 | 36000 | 36000 | 0 |
| 2.5GbE/电生理 | 30000 | 0/0/0 | 0 | 30000 | 30000 | 0 |

联合对账检查同轮来源端口及数量、ID、CRC32、时间戳摘要。摘要一致是有界对账证据，不是每帧逐字节相同的数学证明；也不抵消跳采、首发超期或消费失败。

## 补发及接收重试

| 档位/模块 | 成功发送包 | 补发包 | 补发占全部发送 | NACK轮/包/请求片 | 后续请求间隔 min～max us | 生成计划最大迟到 us |
| --- | --- | --- | --- | --- | --- | --- |
| 1GbE/NDI | 216000 | 0 | 0.000000% | 0/0/0 | 无后续重试样本 | 无请求样本 |
| 1GbE/电生理 | 12891841 | 21841 | 0.169417% | 375/375/21841 | 无后续重试样本 | 1110 |
| 100Mbps/NDI | 72000 | 0 | 0.000000% | 0/0/0 | 无后续重试样本 | 无请求样本 |
| 100Mbps/电生理 | 4298615 | 8615 | 0.200413% | 362/362/28445 | 10002～10319 | 889 |
| 2.5GbE/NDI | 72000 | 0 | 0.000000% | 0/0/0 | 无后续重试样本 | 无请求样本 |
| 2.5GbE/电生理 | 4291929 | 1929 | 0.044945% | 25/25/1929 | 无后续重试样本 | 516 |

上表补发占比的分母是对应模块的全部成功发送包；若合并两模块，1GbE 为 21841 / 13107841 = 0.166625%。补发率不是实际唯一网络丢包率。NACK 可能由暂时迟到、接收调度或真实缺片触发，同一片可能重复请求；没有外部抓包证据不能据此推算线路丢包。无重试为未检测；2ms 尾片、10ms 间隔、三轮及隔离分支另由确定性测试覆盖。实际表测量请求生成时刻，不是实际网卡出线。`whole_missing` 是观察到的前向序号缺口，不是最终丢帧率。

## 资源观测

| 档位/进程 | CPU 核心等效占用 | 首次/末次工作集 MiB | 采样峰值 MiB | 末次私有提交 MiB |
| --- | --- | --- | --- | --- |
| 1GbE/发送 | 104.66% | 1.95 / 14.54 | 15.21 | 4.46 |
| 1GbE/接收 | 16.80% | 23.46 / 24.11 | 24.11 | 15.14 |
| 100Mbps/发送 | 103.27% | 1.91 / 15.18 | 15.20 | 4.75 |
| 100Mbps/接收 | 42.03% | 23.40 / 24.07 | 24.09 | 15.04 |
| 2.5GbE/发送 | 104.81% | 1.91 / 15.15 | 15.16 | 4.75 |
| 2.5GbE/接收 | 13.89% | 23.47 / 23.96 | 23.96 | 15.07 |

100% CPU 表示约一个逻辑核心的等效占用，不是整机 CPU 百分比。内存按约 1s 采样，首次值可能尚未加载完成；首末差与采样峰值不能证明无泄漏，也可能漏掉采样间尖峰。

## 尚未完成的真实接入与未检测项目

- 尚未接入：具体 Aurora COM/SDK 通信驱动、GUI/CLI 真机入口、电生理实时采集和真实通道/样本布局波形解码。目前提供可注入 NDI 通信接口、模拟、单 BX 循环回放和电生理字节预览。
- 自动热插拔/断线恢复尚未贯通：已有设备生命周期和工具刷新基础接口，运行链尚未自动完成停止、重枚举、重新跟踪。
- 共同硬件时间域待接入：NDI 映射接口已有，但 NDI、电生理、状态和主机 QPC 尚未形成经过验证的统一时基。不能把主机收到数据的时间当成真实采样时间。
- 未检测：真实 Aurora 型号、工具数和 40/66Hz 模式，三档物理链路、真实网卡出线、真实采样至发送期限及物理屏幕呈现。模拟 60Hz 不证明硬件 60Hz。
- 状态“立即”没有数值门槛；保留实测延迟，不自行添加通过标准。NACK 文档歧义统一采用 CRC 偏移 20+2N、总长 24+2N，对端需同步本决定。
- 本轮属于新版协议和双路负载验证，不与旧 V1 的组合帧基线作同条件性能提升宣称。

## 证据与交付

- 程序：`dist/v2-release/bin/SimTransfer.exe`；Qt CLI 与原生 CLI 同目录。
- 使用约定：`docs/V2_IMPLEMENTATION_CONTRACT_2026-11-17.md`。
- 实施前备份：`artifacts/udp-stability/before-plan-b-20261117-153019/`。
- 本轮结果根目录：`artifacts/udp-stability/plan-b-release-20261117/`。
- 最终冻结源码：`source-final-tested.zip`、`final-freeze.json`；最终文件修改清单：`implementation-files-final.json`。冻结后仅补写最终验证报告，运行代码与测试代码未变。
- 各轮原始证据：`sender.json`、`receiver.json`、`acceptance.json`、`resources.json`、`result.json`、`commands.json`。
- `long30m/` 是中途停止的旧预跑，已显式排除；仅 `long30m-final/long-1g/` 是本次 30 分钟证据。

本轮 Qt CLI SHA256：`d75402c90c7ad59e911e985409974f2bf423c205635566c7cf9ae1c56292f620`。

## 1GbE 完整六列验收

证据子目录：`long30m-final/long-1g`。

| 要求 | 测量口径 | 阈值 | 实测值 | 通过／失败／未检测 | 原因 |
| --- | --- | --- | --- | --- | --- |
| NDI source continuity | Original independent sample/query slots; no interpolation | 0 skipped slots / queue loss / source error / unmapped device frame | 0 skipped, 0 queue drops, 0 errors, 0 unmapped | 通过 | A source failure remains failed even before its first sample. Hardware sampling is separate. |
| NDI sampling deviation (retained engineering target) | Absolute actual production/query interval minus configured nominal period; all intervals | strictly < 1000 us | 3124 us; 169 / 107999 violations | 失败 | Device host query intervals cannot establish hardware sample jitter; the <1 ms engineering threshold is unchanged |
| NDI complete initial transmission | Simulation/playback sample to last local send return; device input retains host-request proxy separately | <= 15000 us | 4204 us; 0 late; 0 incomplete | 通过 | Local submission only; device common timestamp does not independently establish source-to-host clock offset or physical egress |
| NDI packet interval deviation (engineering target) | Actual first-copy interval minus original global scheduling plan interval | strictly < 1000 us | 3923 us; 602 / 215999 violations | 失败 | Full-population counters; original due time is never replaced by catch-up due time |
| NDI new hardware sampling and common timestamp | Device frame counter, calibrated clock mapping and measured source rate | V2 ~60 Hz and verified common microsecond clock | No physical device timing evidence | 未检测 | Simulation/playback cannot verify real Aurora 40/66 Hz mode or EP sampling clock |
| EP source continuity | Original independent sample/query slots; no interpolation | 0 skipped slots / queue loss / source error / unmapped device frame | 0 skipped, 0 queue drops, 0 errors, 0 unmapped | 通过 | A source failure remains failed even before its first sample. Hardware sampling is separate. |
| EP sampling deviation (retained engineering target) | Absolute actual production/query interval minus configured nominal period; all intervals | strictly < 1000 us | 2940 us; 107 / 89999 violations | 失败 | Device host query intervals cannot establish hardware sample jitter; the <1 ms engineering threshold is unchanged |
| EP complete initial transmission | Simulation/playback sample to last local send return; device input retains host-request proxy separately | <= 18000 us | 9139 us; 0 late; 0 incomplete | 通过 | Local submission only; device common timestamp does not independently establish source-to-host clock offset or physical egress |
| EP packet interval deviation (engineering target) | Actual first-copy interval minus original global scheduling plan interval | strictly < 1000 us | 7124 us; 25694 / 12869999 violations | 失败 | Full-population counters; original due time is never replaced by catch-up due time |
| EP new hardware sampling and common timestamp | Device frame counter, calibrated clock mapping and measured source rate | V2 50 Hz and verified common microsecond clock | No physical device timing evidence | 未检测 | Simulation/playback cannot verify real Aurora 40/66 Hz mode or EP sampling clock |
| Declared load capacity | Ethernet serialization feasibility screening; not performance measurement | Demand within effective rate and 15/18 ms windows | 8.866400 percent | 通过 | Capacity screening only; host stalls, state bursts, device timing and physical egress remain unverified |
| Actual network egress deadlines | Hardware NIC or external capture timestamps | NDI <=15 ms; EP <=18 ms | Unavailable | 未检测 | Windows/host loopback; no physical link or wire timing evidence |
| 按模块收发交付对账 | 同 run_id 的构建帧与校验完成帧：数量、ID、CRC32、时间戳摘要 | 各项一致且帧数>0 | {"sender":{"count":108000,"id_sum":"5832054000","crc_sum":"231918029734356","stamp_sum":"2114896835279440"},"receiver":{"count":"108000","id_sum":"5832054000","crc_sum":"231918029734356","stamp_sum":"2114896835279440"}} | 通过 | 同轮来源端口及计数一致；有界汇总一致不等于逐帧逐字节证明，不抵消跳采、首发超期或显示失败。 |
| 重组后独立消费 | 通过业务解析且未被更新帧覆盖的消费数量 | 消费数=完成数；业务解析错误=0 | {"completed":108000,"consumed":108000,"business_errors":0} | 通过 | NDI使用BX校验；电生理为字节消费接口，真实波形定义尚未提供；缺字段不默认计零。 |
| 已观测NACK生成时序 | 全量已生成请求的原定时刻偏差，以及相邻请求间隔最小/最大值 | 原计划偏差0；后续间隔10000us | {"retry_intervals":{"count":0,"min_us":null,"max_us":0,"mean_us":null,"p99_upper_us":null},"request_lateness":{"count":0,"min_us":null,"max_us":0,"mean_us":null,"p99_upper_us":null},"rounds":0} | 未检测 | 没有重试或缺少完整统计为未检测；生成时间不是实际网卡出线。三轮耗尽、2ms尾部及隔离的完整分支覆盖另由确定性测试证明。 |
| 按模块收发交付对账 | 同 run_id 的构建帧与校验完成帧：数量、ID、CRC32、时间戳摘要 | 各项一致且帧数>0 | {"sender":{"count":90000,"id_sum":"4050045000","crc_sum":"19976739840000","stamp_sum":"1762413879402125"},"receiver":{"count":"90000","id_sum":"4050045000","crc_sum":"19976739840000","stamp_sum":"1762413879402125"}} | 通过 | 同轮来源端口及计数一致；有界汇总一致不等于逐帧逐字节证明，不抵消跳采、首发超期或显示失败。 |
| 重组后独立消费 | 通过业务解析且未被更新帧覆盖的消费数量 | 消费数=完成数；业务解析错误=0 | {"completed":90000,"consumed":90000,"business_errors":0} | 通过 | NDI使用BX校验；电生理为字节消费接口，真实波形定义尚未提供；缺字段不默认计零。 |
| 已观测NACK生成时序 | 全量已生成请求的原定时刻偏差，以及相邻请求间隔最小/最大值 | 原计划偏差0；后续间隔10000us | {"retry_intervals":{"count":0,"min_us":null,"max_us":0,"mean_us":null,"p99_upper_us":null},"request_lateness":{"count":375,"min_us":0,"max_us":1110,"mean_us":220.79733333333334,"p99_upper_us":800},"rounds":375} | 未检测 | 没有重试或缺少完整统计为未检测；生成时间不是实际网卡出线。三轮耗尽、2ms尾部及隔离的完整分支覆盖另由确定性测试证明。 |
| 共同硬件时间域 | 分层独立证据 | 协议约定或待明确 | None | 未检测 | 未接入并验证共同硬件时间映射；主机单调时钟不是设备采样证据。 |
| 实际网卡出线期限 | 分层独立证据 | 协议约定或待明确 | None | 未检测 | 本地send返回不证明实际出线；需要真实链路或硬件时间戳。 |
| 真实业务波形与显示 | 分层独立证据 | 协议约定或待明确 | None | 未检测 | 独立接收桩不证明GUI渲染；真实电生理格式及硬件尚未提供。 |
| 状态立即发送/应用 | 分层独立证据 | 协议约定或待明确 | None | 未检测 | 文档未给“立即”的数值上限；需报告实际延迟并另行确定验收边界。 |
| 全局混合发送序列的原计划间隔偏差（附加工程诊断） | 所有模块、状态及补发的连续本地发送调用，相对原计划间隔的绝对偏差；补发使用历史NACK请求时刻，含补发排队影响 | 严格 <1000us | {"count":13107840,"max_us":4901,"p50_us":100,"p95_us":100,"p99_us":300,"p999_us":1800,"violations":29551} | 失败 | 补充自动六列表尚未纳入的全局项；该定义可能放大首发与历史补发请求计划之间的切换，不能当作连续停发时长，也不证明物理出线。 |
| NDI 已观测NACK生成计划迟到（拆分核查） | 全部已生成请求相对原定生成时刻的迟到，独立于是否出现第二轮请求 | 沿用联合表严格判定：计划偏差0us | {"count":0,"min_us":null,"max_us":0,"mean_us":null,"p99_upper_us":null} | 未检测 | 整体重试项可能因缺少后续间隔样本而未检测；已观测的计划迟到单列，不据此声称验证了完整重试链。 |
| 电生理 已观测NACK生成计划迟到（拆分核查） | 全部已生成请求相对原定生成时刻的迟到，独立于是否出现第二轮请求 | 沿用联合表严格判定：计划偏差0us | {"count":375,"min_us":0,"max_us":1110,"mean_us":220.79733333333334,"p99_upper_us":800} | 失败 | 整体重试项可能因缺少后续间隔样本而未检测；已观测的计划迟到单列，不据此声称验证了完整重试链。 |

## 100Mbps 完整六列验收

证据子目录：`alternate-profiles/soak-100m`。

| 要求 | 测量口径 | 阈值 | 实测值 | 通过／失败／未检测 | 原因 |
| --- | --- | --- | --- | --- | --- |
| NDI source continuity | Original independent sample/query slots; no interpolation | 0 skipped slots / queue loss / source error / unmapped device frame | 0 skipped, 0 queue drops, 0 errors, 0 unmapped | 通过 | A source failure remains failed even before its first sample. Hardware sampling is separate. |
| NDI sampling deviation (retained engineering target) | Absolute actual production/query interval minus configured nominal period; all intervals | strictly < 1000 us | 3281 us; 58 / 35999 violations | 失败 | Device host query intervals cannot establish hardware sample jitter; the <1 ms engineering threshold is unchanged |
| NDI complete initial transmission | Simulation/playback sample to last local send return; device input retains host-request proxy separately | <= 15000 us | 17298 us; 786 late; 0 incomplete | 失败 | Local submission only; device common timestamp does not independently establish source-to-host clock offset or physical egress |
| NDI packet interval deviation (engineering target) | Actual first-copy interval minus original global scheduling plan interval | strictly < 1000 us | 6524 us; 197 / 71999 violations | 失败 | Full-population counters; original due time is never replaced by catch-up due time |
| NDI new hardware sampling and common timestamp | Device frame counter, calibrated clock mapping and measured source rate | V2 ~60 Hz and verified common microsecond clock | No physical device timing evidence | 未检测 | Simulation/playback cannot verify real Aurora 40/66 Hz mode or EP sampling clock |
| EP source continuity | Original independent sample/query slots; no interpolation | 0 skipped slots / queue loss / source error / unmapped device frame | 0 skipped, 0 queue drops, 0 errors, 0 unmapped | 通过 | A source failure remains failed even before its first sample. Hardware sampling is separate. |
| EP sampling deviation (retained engineering target) | Absolute actual production/query interval minus configured nominal period; all intervals | strictly < 1000 us | 2623 us; 59 / 29999 violations | 失败 | Device host query intervals cannot establish hardware sample jitter; the <1 ms engineering threshold is unchanged |
| EP complete initial transmission | Simulation/playback sample to last local send return; device input retains host-request proxy separately | <= 18000 us | 24363 us; 6783 late; 0 incomplete | 失败 | Local submission only; device common timestamp does not independently establish source-to-host clock offset or physical egress |
| EP packet interval deviation (engineering target) | Actual first-copy interval minus original global scheduling plan interval | strictly < 1000 us | 4575 us; 187 / 4289999 violations | 失败 | Full-population counters; original due time is never replaced by catch-up due time |
| EP new hardware sampling and common timestamp | Device frame counter, calibrated clock mapping and measured source rate | V2 50 Hz and verified common microsecond clock | No physical device timing evidence | 未检测 | Simulation/playback cannot verify real Aurora 40/66 Hz mode or EP sampling clock |
| Declared load capacity | Ethernet serialization feasibility screening; not performance measurement | Demand within effective rate and 15/18 ms windows | 88.664000 percent | 通过 | Capacity screening only; host stalls, state bursts, device timing and physical egress remain unverified; less than 1 ms simultaneous-frame wire margin |
| Actual network egress deadlines | Hardware NIC or external capture timestamps | NDI <=15 ms; EP <=18 ms | Unavailable | 未检测 | Windows/host loopback; no physical link or wire timing evidence |
| 按模块收发交付对账 | 同 run_id 的构建帧与校验完成帧：数量、ID、CRC32、时间戳摘要 | 各项一致且帧数>0 | {"sender":{"count":36000,"id_sum":"648018000","crc_sum":"77275061591652","stamp_sum":"748201635066790"},"receiver":{"count":"36000","id_sum":"648018000","crc_sum":"77275061591652","stamp_sum":"748201635066790"}} | 通过 | 同轮来源端口及计数一致；有界汇总一致不等于逐帧逐字节证明，不抵消跳采、首发超期或显示失败。 |
| 重组后独立消费 | 通过业务解析且未被更新帧覆盖的消费数量 | 消费数=完成数；业务解析错误=0 | {"completed":36000,"consumed":36000,"business_errors":0} | 通过 | NDI使用BX校验；电生理为字节消费接口，真实波形定义尚未提供；缺字段不默认计零。 |
| 已观测NACK生成时序 | 全量已生成请求的原定时刻偏差，以及相邻请求间隔最小/最大值 | 原计划偏差0；后续间隔10000us | {"retry_intervals":{"count":0,"min_us":null,"max_us":0,"mean_us":null,"p99_upper_us":null},"request_lateness":{"count":0,"min_us":null,"max_us":0,"mean_us":null,"p99_upper_us":null},"rounds":0} | 未检测 | 没有重试或缺少完整统计为未检测；生成时间不是实际网卡出线。三轮耗尽、2ms尾部及隔离的完整分支覆盖另由确定性测试证明。 |
| 按模块收发交付对账 | 同 run_id 的构建帧与校验完成帧：数量、ID、CRC32、时间戳摘要 | 各项一致且帧数>0 | {"sender":{"count":30000,"id_sum":"450015000","crc_sum":"6658913280000","stamp_sum":"623501312714225"},"receiver":{"count":"30000","id_sum":"450015000","crc_sum":"6658913280000","stamp_sum":"623501312714225"}} | 通过 | 同轮来源端口及计数一致；有界汇总一致不等于逐帧逐字节证明，不抵消跳采、首发超期或显示失败。 |
| 重组后独立消费 | 通过业务解析且未被更新帧覆盖的消费数量 | 消费数=完成数；业务解析错误=0 | {"completed":30000,"consumed":30000,"business_errors":0} | 通过 | NDI使用BX校验；电生理为字节消费接口，真实波形定义尚未提供；缺字段不默认计零。 |
| 已观测NACK生成时序 | 全量已生成请求的原定时刻偏差，以及相邻请求间隔最小/最大值 | 原计划偏差0；后续间隔10000us | {"retry_intervals":{"count":181,"min_us":10002,"max_us":10319,"mean_us":10074.430939226519,"p99_upper_us":10300},"request_lateness":{"count":362,"min_us":0,"max_us":889,"mean_us":180.54972375690608,"p99_upper_us":800},"rounds":362} | 失败 | 没有重试或缺少完整统计为未检测；生成时间不是实际网卡出线。三轮耗尽、2ms尾部及隔离的完整分支覆盖另由确定性测试证明。 |
| 共同硬件时间域 | 分层独立证据 | 协议约定或待明确 | None | 未检测 | 未接入并验证共同硬件时间映射；主机单调时钟不是设备采样证据。 |
| 实际网卡出线期限 | 分层独立证据 | 协议约定或待明确 | None | 未检测 | 本地send返回不证明实际出线；需要真实链路或硬件时间戳。 |
| 真实业务波形与显示 | 分层独立证据 | 协议约定或待明确 | None | 未检测 | 独立接收桩不证明GUI渲染；真实电生理格式及硬件尚未提供。 |
| 状态立即发送/应用 | 分层独立证据 | 协议约定或待明确 | None | 未检测 | 文档未给“立即”的数值上限；需报告实际延迟并另行确定验收边界。 |
| 全局混合发送序列的原计划间隔偏差（附加工程诊断） | 所有模块、状态及补发的连续本地发送调用，相对原计划间隔的绝对偏差；补发使用历史NACK请求时刻，含补发排队影响 | 严格 <1000us | {"count":4370614,"max_us":76282,"p50_us":0,"p95_us":100,"p99_us":200,"p999_us":600,"violations":918} | 失败 | 补充自动六列表尚未纳入的全局项；该定义可能放大首发与历史补发请求计划之间的切换，不能当作连续停发时长，也不证明物理出线。 |
| NDI 已观测NACK生成计划迟到（拆分核查） | 全部已生成请求相对原定生成时刻的迟到，独立于是否出现第二轮请求 | 沿用联合表严格判定：计划偏差0us | {"count":0,"min_us":null,"max_us":0,"mean_us":null,"p99_upper_us":null} | 未检测 | 整体重试项可能因缺少后续间隔样本而未检测；已观测的计划迟到单列，不据此声称验证了完整重试链。 |
| 电生理 已观测NACK生成计划迟到（拆分核查） | 全部已生成请求相对原定生成时刻的迟到，独立于是否出现第二轮请求 | 沿用联合表严格判定：计划偏差0us | {"count":362,"min_us":0,"max_us":889,"mean_us":180.54972375690608,"p99_upper_us":800} | 失败 | 整体重试项可能因缺少后续间隔样本而未检测；已观测的计划迟到单列，不据此声称验证了完整重试链。 |

## 2.5GbE 完整六列验收

证据子目录：`alternate-profiles/soak-2g5`。

| 要求 | 测量口径 | 阈值 | 实测值 | 通过／失败／未检测 | 原因 |
| --- | --- | --- | --- | --- | --- |
| NDI source continuity | Original independent sample/query slots; no interpolation | 0 skipped slots / queue loss / source error / unmapped device frame | 0 skipped, 0 queue drops, 0 errors, 0 unmapped | 通过 | A source failure remains failed even before its first sample. Hardware sampling is separate. |
| NDI sampling deviation (retained engineering target) | Absolute actual production/query interval minus configured nominal period; all intervals | strictly < 1000 us | 2725 us; 57 / 35999 violations | 失败 | Device host query intervals cannot establish hardware sample jitter; the <1 ms engineering threshold is unchanged |
| NDI complete initial transmission | Simulation/playback sample to last local send return; device input retains host-request proxy separately | <= 15000 us | 3740 us; 0 late; 0 incomplete | 通过 | Local submission only; device common timestamp does not independently establish source-to-host clock offset or physical egress |
| NDI packet interval deviation (engineering target) | Actual first-copy interval minus original global scheduling plan interval | strictly < 1000 us | 4510 us; 107 / 71999 violations | 失败 | Full-population counters; original due time is never replaced by catch-up due time |
| NDI new hardware sampling and common timestamp | Device frame counter, calibrated clock mapping and measured source rate | V2 ~60 Hz and verified common microsecond clock | No physical device timing evidence | 未检测 | Simulation/playback cannot verify real Aurora 40/66 Hz mode or EP sampling clock |
| EP source continuity | Original independent sample/query slots; no interpolation | 0 skipped slots / queue loss / source error / unmapped device frame | 0 skipped, 0 queue drops, 0 errors, 0 unmapped | 通过 | A source failure remains failed even before its first sample. Hardware sampling is separate. |
| EP sampling deviation (retained engineering target) | Absolute actual production/query interval minus configured nominal period; all intervals | strictly < 1000 us | 2375 us; 56 / 29999 violations | 失败 | Device host query intervals cannot establish hardware sample jitter; the <1 ms engineering threshold is unchanged |
| EP complete initial transmission | Simulation/playback sample to last local send return; device input retains host-request proxy separately | <= 18000 us | 6235 us; 0 late; 0 incomplete | 通过 | Local submission only; device common timestamp does not independently establish source-to-host clock offset or physical egress |
| EP packet interval deviation (engineering target) | Actual first-copy interval minus original global scheduling plan interval | strictly < 1000 us | 5190 us; 2770 / 4289999 violations | 失败 | Full-population counters; original due time is never replaced by catch-up due time |
| EP new hardware sampling and common timestamp | Device frame counter, calibrated clock mapping and measured source rate | V2 50 Hz and verified common microsecond clock | No physical device timing evidence | 未检测 | Simulation/playback cannot verify real Aurora 40/66 Hz mode or EP sampling clock |
| Declared load capacity | Ethernet serialization feasibility screening; not performance measurement | Demand within effective rate and 15/18 ms windows | 3.546560 percent | 通过 | Capacity screening only; host stalls, state bursts, device timing and physical egress remain unverified |
| Actual network egress deadlines | Hardware NIC or external capture timestamps | NDI <=15 ms; EP <=18 ms | Unavailable | 未检测 | Windows/host loopback; no physical link or wire timing evidence |
| 按模块收发交付对账 | 同 run_id 的构建帧与校验完成帧：数量、ID、CRC32、时间戳摘要 | 各项一致且帧数>0 | {"sender":{"count":36000,"id_sum":"648018000","crc_sum":"77275061591652","stamp_sum":"769823624037602"},"receiver":{"count":"36000","id_sum":"648018000","crc_sum":"77275061591652","stamp_sum":"769823624037602"}} | 通过 | 同轮来源端口及计数一致；有界汇总一致不等于逐帧逐字节证明，不抵消跳采、首发超期或显示失败。 |
| 重组后独立消费 | 通过业务解析且未被更新帧覆盖的消费数量 | 消费数=完成数；业务解析错误=0 | {"completed":36000,"consumed":36000,"business_errors":0} | 通过 | NDI使用BX校验；电生理为字节消费接口，真实波形定义尚未提供；缺字段不默认计零。 |
| 已观测NACK生成时序 | 全量已生成请求的原定时刻偏差，以及相邻请求间隔最小/最大值 | 原计划偏差0；后续间隔10000us | {"retry_intervals":{"count":0,"min_us":null,"max_us":0,"mean_us":null,"p99_upper_us":null},"request_lateness":{"count":0,"min_us":null,"max_us":0,"mean_us":null,"p99_upper_us":null},"rounds":0} | 未检测 | 没有重试或缺少完整统计为未检测；生成时间不是实际网卡出线。三轮耗尽、2ms尾部及隔离的完整分支覆盖另由确定性测试证明。 |
| 按模块收发交付对账 | 同 run_id 的构建帧与校验完成帧：数量、ID、CRC32、时间戳摘要 | 各项一致且帧数>0 | {"sender":{"count":30000,"id_sum":"450015000","crc_sum":"6658913280000","stamp_sum":"641519636832955"},"receiver":{"count":"30000","id_sum":"450015000","crc_sum":"6658913280000","stamp_sum":"641519636832955"}} | 通过 | 同轮来源端口及计数一致；有界汇总一致不等于逐帧逐字节证明，不抵消跳采、首发超期或显示失败。 |
| 重组后独立消费 | 通过业务解析且未被更新帧覆盖的消费数量 | 消费数=完成数；业务解析错误=0 | {"completed":30000,"consumed":30000,"business_errors":0} | 通过 | NDI使用BX校验；电生理为字节消费接口，真实波形定义尚未提供；缺字段不默认计零。 |
| 已观测NACK生成时序 | 全量已生成请求的原定时刻偏差，以及相邻请求间隔最小/最大值 | 原计划偏差0；后续间隔10000us | {"retry_intervals":{"count":0,"min_us":null,"max_us":0,"mean_us":null,"p99_upper_us":null},"request_lateness":{"count":25,"min_us":3,"max_us":516,"mean_us":191.04,"p99_upper_us":516},"rounds":25} | 未检测 | 没有重试或缺少完整统计为未检测；生成时间不是实际网卡出线。三轮耗尽、2ms尾部及隔离的完整分支覆盖另由确定性测试证明。 |
| 共同硬件时间域 | 分层独立证据 | 协议约定或待明确 | None | 未检测 | 未接入并验证共同硬件时间映射；主机单调时钟不是设备采样证据。 |
| 实际网卡出线期限 | 分层独立证据 | 协议约定或待明确 | None | 未检测 | 本地send返回不证明实际出线；需要真实链路或硬件时间戳。 |
| 真实业务波形与显示 | 分层独立证据 | 协议约定或待明确 | None | 未检测 | 独立接收桩不证明GUI渲染；真实电生理格式及硬件尚未提供。 |
| 状态立即发送/应用 | 分层独立证据 | 协议约定或待明确 | None | 未检测 | 文档未给“立即”的数值上限；需报告实际延迟并另行确定验收边界。 |
| 全局混合发送序列的原计划间隔偏差（附加工程诊断） | 所有模块、状态及补发的连续本地发送调用，相对原计划间隔的绝对偏差；补发使用历史NACK请求时刻，含补发排队影响 | 严格 <1000us | {"count":4363928,"max_us":5482,"p50_us":100,"p95_us":100,"p99_us":300,"p999_us":1100,"violations":4830} | 失败 | 补充自动六列表尚未纳入的全局项；该定义可能放大首发与历史补发请求计划之间的切换，不能当作连续停发时长，也不证明物理出线。 |
| NDI 已观测NACK生成计划迟到（拆分核查） | 全部已生成请求相对原定生成时刻的迟到，独立于是否出现第二轮请求 | 沿用联合表严格判定：计划偏差0us | {"count":0,"min_us":null,"max_us":0,"mean_us":null,"p99_upper_us":null} | 未检测 | 整体重试项可能因缺少后续间隔样本而未检测；已观测的计划迟到单列，不据此声称验证了完整重试链。 |
| 电生理 已观测NACK生成计划迟到（拆分核查） | 全部已生成请求相对原定生成时刻的迟到，独立于是否出现第二轮请求 | 沿用联合表严格判定：计划偏差0us | {"count":25,"min_us":3,"max_us":516,"mean_us":191.04,"p99_upper_us":516} | 失败 | 整体重试项可能因缺少后续间隔样本而未检测；已观测的计划迟到单列，不据此声称验证了完整重试链。 |
