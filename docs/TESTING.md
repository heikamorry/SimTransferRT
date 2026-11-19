# 测试与验收

## 可自动执行

```bat
scripts\build.cmd
scripts\test-network.cmd
```

第一条包含C++核心、Qt应用管理类+QUdpSocket回环、GUI启动测试。第二条包含独立Python协议测试和真实Qt CLI的8场景UDP矩阵。
只安装C++编译器时可使用 `cmake --preset core-only` 构建SimRtProbe和核心测试；它不验证Qt层。

核心测试包括：CRC known vector、报头偏移、小端、1MiB/2MiB、任意片先到、模块跨片/超过64KB、NACK区间/363限制/越界、重传flag与CRC、并行首发和NACK、重复请求、多包NACK、缓存过期、动态mask、原文回绕冲突、可选FIFO、全离线、超长组帧、调度过载、控制队列有界与停止优先。

Python测试桩按每片独立计时执行：发现缺片立即NACK，满10ms再次请求，总共3次，第3次后10ms仍缺作废整帧。2ms静默或更新帧到达登记尾片。它不会发送SUCCESS。
测试桩不实现正式渲染队列、20ms显示缓冲和医学业务；只把完整帧记为本地重组完成，不代表已按TS送到正式显示线程。

## 初测顺序

1. 两端先运行在同一台机器，64KiB帧，所有故障关闭。
2. 首轮丢片2、损坏4，确认收到NACK并补发；只看双方本地报告，不等待SUCCESS。
3. 持续丢片2，确认接收测试桩过3次请求作废旧帧，发送端仍持续发新帧。
4. 模块0/2在线，运行中切换mask，新帧变化、旧缓存重传仍保持旧TS/mask。
5. 1MiB/2MiB先测试格式边界，再测试真实网卡吞吐与时间。
6. 更改接收端NACK源端口，保持命令目的端口正确，确认Sim不会错误按数据端口过滤。

## 必须在目标硬件实测，不从单元测试推断

- 50Hz采样周期、<1ms抖动、20us级片间隔、首轮15ms结束。
- QPC模拟TS与实际采集硬件TS的差异；各模块真实采样触发偏差。
- 真实Windows MSVC v143 / Qt6.12构建、Windows DF位与网卡出包抓包。
- 真实链路MTU至少1500、无额外隧道开销；设置DF失败/包太大错误处理。
- 1MiB长期运行时CPU占用、丢包、发送队列、缓存命中；2MiB须先解决千兆/15ms矛盾。

本地writeDatagram/sendto返回成功不证明线上发包时刻。使用抓包或硬件时间戳验收；报告里的用户态调度指标只是辅助。

## 发布验收

运行scripts/package.cmd，在未安装Qt的Windows机器验证dist/bin/SimTransfer.exe；检查Qt平台插件、MSVC运行库、防火墙，以及大负载下暂停/停止和退出行为。
