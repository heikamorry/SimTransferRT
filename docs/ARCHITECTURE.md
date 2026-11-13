# 架构与线程所有权

```text
Qt GUI / 主线程
 MainWindow → TransferController
                  └── SessionManager [QObject、100ms统计轮询、最近20条摘要]
                           ├── CommandDispatcher [线程安全有界队列]
                           └── QThread::create 工作线程
                                 ├── QtUdpTransport [此线程创建/使用/销毁]
                                 └── StreamRunner
                                       ├── RealtimeStream
                                       │     ├── ModuleDataSource / LogicalFrameBuilder
                                       │     ├── FrameCache [严格3槽，不保留外部payload引用]
                                       │     ├── PacketPacer [绝对时间、非整帧突发]
                                       │     └── 有界重传队列 / 临时去重集合
                                       └── PacketCodec / CRC32
```

## 三个管理类

**TransferController**：GUI/CLI共同门面，检查IPv4、端口与来源过滤配置，路由启动/停止/模块位图变更。它不拥有数据报缓冲、不直接操作窗口外的socket。

**SessionManager**：独占一次持续运行实例，管理工作线程的创建/结束；通过互斥保护的最新快照邮箱把统计限频为10Hz，日志邮箱上限128条，历史最多20条且不含大载荷；运行结束发布next_logic_id供GUI下一次启动使用。shutdown发送优先Stop命令并wait，禁止terminate线程。

**CommandDispatcher**：线程安全有界队列（512个普通命令），接收SetMask、NackFeedback、Stop。Stop具有独立优先位，不会被反馈洪泛阻止；SetMask合并为最新待应用位图；每轮最多分发32个命令。NACK字节必须先完整通过解析/CRC检查。它不是永久NACK去重器；后续10ms再请求同一片会继续补发。

## 工作线程和计时

QtUdpTransport的QUdpSocket在同一专用线程中同步发送、轮询接收并销毁，不依赖该线程的readyRead槽或QTimer。没有在GUI线程忙等，也没有把一个socket跨线程直接使用。应用主线程的QTimer只用来取统计快照，绝不负责20us发包。

运行循环在有首轮数据待发时约100us处理一次反馈/命令；空闲时约1ms。每轮I/O与命令处理有限额。PacketPacer使用单调绝对时刻，间隔为 `max(1,min(20,15000/frag_cnt)) us`。小帧不强行拉长到15ms，避免产生大于2ms的正常片间静默而误触发接收端尾片检测。
迟到时允许有限追赶，但至少保留半个计划片间隔，不整帧突发；真实节奏变慢会记录deadlineMisses。如果下一采样已到而上一帧首轮未发完，旧帧未发片计数为unsentExpired并停止首轮，仍可在缓存中接受NACK。此为本地过载降级，必须作为未达时序指标报告，不能视为正常性能。
Windows短等待用QPC读数和忙等；较长等待尝试高分辨率WaitableTimer，失败回退sleep；Linux使用steady_clock。该设计仍不是硬实时，某些负载下可能占用一个CPU核心的大部分时间。

## DATA与NACK并行

没有等待READY、FIN或SUCCESS的状态。流状态只有准备、运行、有限测试的缓存收尾、停止/本地错误。历史帧重传和新帧首发共存。
先发已经到期的初发片；空档处理一个重传片，补发最小本地间隔10us。此间隔是公平调度策略，不写进报文，不代表拥塞控制。若输入重传量超过可用带宽，会出现重传延迟；没有“无论丢多少都能30ms恢复”的保证。

缓存中原始字节、TS、mask均不可变；队列只保存帧号与片号，不拥有帧payload。缓存覆盖时清除过期重传任务。发送时再次查缓存，防止任务指向已经覆盖的帧。

## 观测语义

- datagrams/bytesSent：本地socket接受的DATA，包含重传和测试重复，不代表远端收到。
- firstAttempts：包含故意丢弃的首发尝试。
- retransmissions：实际被socket接受的补发数据报；表格retries是补发尝试数，可能被故障注入丢弃。
- nacks：通过字节级解析/CRC的NACK；区间越界还会计入invalidNacks。
- maxSampleLatenessUs：相对绝对采样网格的调度迟到，不是网卡出包抖动。
- lastInitialDurationUs：用户态调度记录的首轮跨度，不是完整以太网线上传输耗时。
- 没有对端成功率、逐片ACK或网卡硬件发送时间戳。

## 适配真实模块

本版数据体是固定仿真模板/文件体，多个模块在一次sample中共享同一TS。真实硬件适配时需用设备采样快照和硬件TS替换ModuleDataSource/SamplingClock；不应逐模块取不同的“发送时刻”填入同一帧。

模块文件准备在工作线程完成，不阻塞GUI；Stop需要等待当前有界文件读取/模板准备返回，尚未实现逐块取消。原生适配器仅用于CLI/测试，Qt GUI生产入口创建QThread并在其中创建QUdpSocket。
