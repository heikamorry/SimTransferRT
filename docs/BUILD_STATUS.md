# 建置與測試狀態｜Build and test status

## 2026-11-13：Windows 本機驗證｜Local Windows verification

本節記錄此次在 Windows 專案目錄實際執行的結果；下方保留的 Linux 紀錄屬於先前環境，不代表目前的 Windows／Qt 驗證狀態。

This section records checks executed in the local Windows project directory. The Linux records below describe an earlier environment and do not represent the current Windows/Qt verification status.

| 環境 / Environment | 版本 / Version |
|---|---|
| MSVC v143 | 14.44.35207 |
| Qt MSVC x64 | 6.12.0 |
| CMake | 4.4.5 |
| Python | 3.13.14 |

| 執行項目 / Check | 結果 / Result | 證據 / Evidence |
|---|---|---|
| `scripts\build.cmd` | Windows Debug 建置成功 / Windows Debug build passed | [環境與執行紀錄 / Environment and execution record](test-results/windows-2026-11-13/environment.txt) |
| CTest：`rt_core`、`rt_application`、`rt_gui_smoke` | **3/3 通過 / passed** | [CTest 日誌 / CTest log](test-results/windows-2026-11-13/ctest.log) |
| `python tools/test_protocol.py` | **18/18 通過 / passed** | 本次測試終端輸出 / Test terminal output from this run |
| `scripts\test-network.cmd`：Qt CLI 真實 UDP 回環 / Actual Qt CLI UDP loopback | **8/8 通過 / passed** | [網路測試摘要 / Network test summary](test-results/windows-2026-11-13/network-summary.json) |

八個情境為正常、首輪丟片、CRC 損壞、重複、逆序、重複 NACK、持續丟片與幀編號回繞。每個情境使用 16 KiB 邏輯幀與 15 次採樣；七個可完成情境各收到 15/15 幀，持續丟片情境作廢 15 幀且發送端繼續產生 15 幀，符合預期。此次測試使用 `SimTransferCli.exe`，涵蓋 Qt UDP 路徑。

The eight scenarios cover normal operation, initial loss, CRC damage, duplicates, reverse order, duplicate NACKs, permanent loss, and frame-ID wraparound. Each uses 16 KiB logical frames and 15 samples. The seven completion scenarios each received 15/15 frames; permanent loss expired 15 frames while the sender still produced 15, as expected. This run used `SimTransferCli.exe` and exercised the Qt UDP path.

完整的本機日誌由腳本產生於 `build/windows-debug/Testing/Temporary/LastTest.log` 與 `build/network-tests/`；`build/` 不隨原始碼提交，上方連結提供此次驗證的留存紀錄。此結果驗證功能與 GUI 啟動，不代表已驗收完整人工介面操作、實體網卡吞吐、網路封包 DF 位、硬體採樣時間戳、15 ms／小於 1 ms 時序目標、長時間穩定性，或無 Qt 開發環境機器的 Release 部署。

The scripts generate complete local logs at `build/windows-debug/Testing/Temporary/LastTest.log` and `build/network-tests/`. The `build/` directory is excluded from source control; the links above provide retained evidence from this run. These results validate functionality and GUI startup, not a full manual interface review, physical-NIC throughput, on-wire DF behavior, hardware acquisition timestamps, the 15 ms / sub-1 ms timing targets, long-run stability, or Release deployment on a machine without the Qt development environment.

## 歷史紀錄：先前 Linux 環境｜Historical record: earlier Linux environment

以下保留原始 Linux 測試紀錄，原文中的「本次」「本運行環境」「尚未執行」均僅指該次歷史測試。對應證據位於 [`test-results/`](test-results/)。目前 Windows 驗證結果以上方日期章節為準。

The original Linux test record is preserved below. References to “this run,” “this environment,” or checks “not yet performed” apply only to that historical run. Its evidence is stored in [`test-results/`](test-results/). Refer to the dated section above for current Windows results.

### 当时通过的功能检查

| 项目 | 结果 | 证据 |
|---|---|---|
| Linux/GNU C++17 Release 核心构建 | 通过 | core-configure.log / core-build.log |
| C++核心测试 | 45/45通过 | core-release.log |
| ASan/UBSan/LeakSanitizer核心测试 | 45/45通过，无报告诊断 | asan-tests.log |
| CTest（核心配置） | 通过 | ctest-release.log |
| 独立Python协议/接收计时模型 | 18/18通过 | python-tests.log |
| C++编码与Python重编码 | 字节完全一致 | cross-language.log / data-vector.bin |
| 两进程真实UDP回环矩阵 | 8/8通过 | e2e/summary.json及每场景日志 |

回环矩阵：正常、首发丢片、CRC损坏、重复数据、逆序、重复NACK、持续丢片、logic_id回绕。
测试条件主要为16KiB完整帧、50Hz、15次采样；正常场景接收15帧。持续丢片场景接收端作废15帧、Sim仍产生15帧；这属于预期故障测试通过，不代表数据送达。
所有真实UDP测试都使用 **SimRtProbe原生socket适配器**，它复用同一C++协议与运行核心，不经过QtUdpTransport或QObject管理类。

### 当时尚未验证，不能计为通过

当时的Linux运行环境没有Qt SDK，实际配置Qt目标失败，详见qt-configure.log。因此，以下仅记录该历史环境的限制：
- Qt纯代码界面、QtUdpTransport、三个管理类的Qt集成 **尚未在此环境编译运行**。
- Windows 11 + MSVC v143 + Qt 6.12构建 **尚未执行**。
- Windows DF实际报文、真实网卡吞吐、硬件采样TS、多模块真实采样偏差 **未验证**。

工程当时已包含SimRtAppTests、GUI --smoke-test和使用真实Qt CLI的网络测试，但该Linux环境未能运行。这里的qt-configure失败不是对当前Windows Qt SDK的诊断；当前本机Qt层结果见本文开头的2026-11-13记录。

### 共享Linux环境的大帧测量

下表是本次真实运行日志，**只测了发往本机未监听端口的本地发送路径**，不是完整接收测试，也不是千兆/2.5G物理链路测量。最后一行的2500只是本地可行性校验输入。

|逻辑帧字节|采样次数|首轮尝试片数|旧帧未发即过期片数|首轮超期计数|最大采样调度迟到 μs|
|---:|---:|---:|---:|---:|---:|
|65536|20|920|0|0|42|
|1048576|20|14620|0|0|68|
|2097152|20|29220|0|2|5|

数据见load-*.json/log。该环境曾测到1MiB/2MiB负载发送窗口超期，说明 **实时指标尚未完成验收**。代码会报告超期和未发片，不把本地socket成功当成满足15ms。共享操作系统调度会变化；单次没有超期也不构成稳定性保证。

DF选项调用成功不等于已抓包验证网络上的DF位。<1ms时序目标、15ms首轮上线时长和连续运行需要在目标硬件实测。

### 原文约束冲突仍保留可见

原文logic_id%3在回绕点会提前覆盖上一帧，默认兼容模式保留此行为并有测试；可选FIFO为候选勘误。2MiB/1Gbps/15ms无法同时成立，默认拒绝不可能的容量配置；超带宽开关只允许格式测试。
