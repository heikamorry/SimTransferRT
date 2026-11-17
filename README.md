# SimTransferRT

本輪檔案變更與驗證摘要 / Change inventory and validation: [2026-11-16](docs/CHANGES_2026-11-16.md)。

最新增量優化與回歸證據：[Windows UDP 發送穩定性](docs/UDP_STABILITY_2026-11-16.md)。CLI 可用 `--timing-trace on|off` 控制有界本地異常時間線；不改線上協定。

64 KiB Release 最新修復與復測：[到期採樣、分片間隔及接收完成計時](docs/TIMING_FIX_2026-11-17.md)。新增 `A64` 場景；`A1` 仍為 1 MiB。最新 trace 按原因保留最嚴重事件，整體驗收已包含分片間隔。

**多模組即時 UDP 模擬發送器｜Real-time UDP simulator for multi-module data**

SimTransferRT 0.2.0 是以 C++17 與 Qt 6 開發的模擬發送平台，提供桌面介面、命令列工具、分片重傳及故障注入，協助驗證《UDP 多模組即時資料傳輸協定》。專案包含開發用接收測試樁，不包含正式接收平台或醫療資料渲染。

SimTransferRT 0.2.0 is a C++17 / Qt 6 simulation sender for a multi-module real-time UDP protocol. It includes a desktop interface, command-line tools, fragment retransmission, and fault injection. A development receiver stub is included; a production receiver and medical-data rendering are outside the project scope.

## 功能｜Features

| 中文 | English |
|---|---|
| 純 C++ 建立的 Qt Widgets 介面，無 `.ui` 或 QML；GUI 與 Qt CLI 共用 `TransferController`、`SessionManager`、`CommandDispatcher` 和即時核心。 | Qt Widgets interface built entirely in C++, without `.ui` files or QML. The GUI and Qt CLI share `TransferController`, `SessionManager`, `CommandDispatcher`, and the streaming core. |
| 以 20 ms（50 Hz）為採樣目標，每次將在線模組組成一個邏輯幀；支援 32 個模組及執行中更新在線位圖。 | Targets a 20 ms / 50 Hz sampling cycle, combining online modules into one logical frame. Supports 32 modules and online-mask updates while running. |
| 模組資料來自可重複的模擬位元組或原始檔案，完整邏輯幀最大為 2 MiB。 | Module bodies contain deterministic synthetic bytes or raw file contents, with logical frames up to 2 MiB. |
| DATA 分片、CRC32、三槽幀快取，以及依 NACK 區間補傳；支援丟片、損壞、重複與逆序故障注入。 | DATA fragmentation, CRC32, a three-slot frame cache, and range-based NACK retransmission, with loss, corruption, duplication, and reordering fault injection. |
| 提供原生 Socket 測試入口、獨立 Python 協定實作與接收測試樁，以及自動 UDP 回環測試。 | Includes a native-socket probe, an independent Python protocol implementation and receiver stub, and automated UDP loopback tests. |

## 環境需求｜Requirements

| 項目 / Component | 需求 / Requirement |
|---|---|
| C++ | C++17 編譯器 / C++17 compiler |
| CMake | 3.21 或以上 / 3.21 or newer |
| Qt | 6.5 或以上，Core / Network / Widgets；Windows 使用 MSVC x64 套件 / 6.5 or newer, Core / Network / Widgets; use the MSVC x64 package on Windows |
| Windows 工具鏈 / Windows toolchain | 腳本指定 MSVC v143 **14.44**，並使用 NMake / Scripts select MSVC v143 **14.44** and use NMake |
| Python | Python 3，用於接收測試樁與網路測試，僅依賴標準庫 / Python 3 for the receiver stub and network tests; standard library only |
| Ninja | 僅 `core-only` 預設需要 / Required by the `core-only` preset |

Windows 腳本的預設路徑如下；它們是可調整的設定，並非必須安裝到這些位置。CMake 與 Python 須在 `PATH` 中。

The Windows scripts use the following configurable defaults. CMake and Python must be available on `PATH`.

```text
Qt:          C:\Qt\6.12.0\msvc2022_64
Build Tools: C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools
MSVC:        -vcvars_ver=14.44
```

## 快速開始｜Quick start

### 1. 取得原始碼｜Clone

```bat
git clone https://github.com/heikamorry/SimTransferRT.git
cd SimTransferRT
```

此倉庫為私人倉庫，複製前須使用具有存取權限的 GitHub 帳號完成 Git 驗證。

This is a private repository. Authenticate Git with a GitHub account that has access before cloning.

### 2. 建置與啟動｜Build and run

在專案根目錄的 **Windows CMD** 執行：

Run these commands from the project root in **Windows CMD**:

```bat
scripts\build.cmd
scripts\run.cmd
```

建置腳本會啟用 MSVC 環境、設定 Qt 路徑、建置 GUI／CLI 與測試，並執行 CTest。執行檔位於 `build/windows-debug/`，不在其 `src/` 子目錄。

The build script initializes MSVC, configures Qt, builds the GUI, CLI, and tests, then runs CTest. Executables are placed directly in `build/windows-debug/`, not its `src/` subdirectory.

若安裝位置不同，請在同一個 CMD 視窗先設定環境變數。腳本固定選用工具集 14.44，因此也須安裝對應版本。

If your installations use different paths, set these variables in the same CMD window first. The scripts explicitly select toolset 14.44, which must also be installed.

```bat
set "QT_PREFIX=C:\Qt\6.12.0\msvc2022_64"
set "VS_BUILD_TOOLS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
scripts\build.cmd
```

Release 建置與啟動，執行檔位於 `build/windows-release/`：

For a Release build and launch, with executables in `build/windows-release/`:

```bat
scripts\build.cmd Release
scripts\run.cmd Release
```

**PowerShell：** 可透過 CMD 呼叫完整腳本。本文的 `set`、`call` 與直接啟動 CLI 範例須在 CMD 中執行；子程序無法將編譯器與 Qt 環境傳回父 PowerShell。

**PowerShell:** Invoke the complete scripts through CMD as shown below. The `set`, `call`, and direct CLI examples use CMD syntax; a child CMD process cannot export its compiler and Qt environment back to the parent PowerShell.

```powershell
cmd /d /c scripts\build.cmd
cmd /d /c scripts\run.cmd
cmd /d /c scripts\test-network.cmd
```

### 3. 本機收發測試｜Local sender and receiver

在專案根目錄開啟兩個終端。終端 A 啟動 Python 接收測試樁：

Open two terminals at the project root. In terminal A, start the Python receiver stub:

```bat
python tools\udp_feedback_stub.py --bind 127.0.0.1 --port 5650 --cmd-ip 127.0.0.1 --cmd-port 5500
```

終端 B 啟動 GUI，按下「开始实时发送」。GUI 已包含發送控制與背景工作執行緒，無須額外的後端服務；接收測試樁則獨立執行。

In terminal B, launch the GUI and select **「开始实时发送」** (Start real-time sending). The GUI includes the sender controller and background worker; no separate backend service is required. The receiver stub runs separately.

```bat
scripts\run.cmd
```

| 預設項目 / Default | 值 / Value |
|---|---|
| DATA 目標 / DATA destination | `127.0.0.1:5650` |
| 本地綁定位址、NACK 監聽端點 / Local bind and NACK listener | `127.0.0.1:5500` |
| 允許的 NACK 來源 / Allowed NACK source | IP `127.0.0.1`，來源連接埠不限 / IP `127.0.0.1`, any source port |
| 在線模組 / Online module | 模組 0 / Module 0 |
| 邏輯幀大小 / Logical frame size | 65,531 B 模組資料 + 5 B 模組標頭 = 65,536 B / 65,531 B body + 5 B module header = 65,536 B |

接收端的 `net.cmd_dst_ip/port` 必須指向 Sim 的 NACK 監聽端點。跨機器使用時，請改為實際網卡位址。測試樁只在自己的終端顯示 `LOCAL COMPLETE`，**不會傳送 SUCCESS 訊息**。

The receiver's `net.cmd_dst_ip/port` must point to the sender's NACK listener. Use actual interface addresses when testing across machines. The stub reports `LOCAL COMPLETE` locally; **it sends no SUCCESS message**.

在故障頁將「每帧首轮丢片」設為 `2`、首輪損壞設為 `4`，停止後重新開始，即可觀察 NACK 與補傳。將「持续丢片」設為 `2` 時，接收測試樁最多請求三次後作廢該幀，發送端仍繼續後續幀。

To exercise recovery, set initial fragment loss to `2` and initial corruption to `4`, then stop and restart sending. The stub should request retransmission through NACKs. With permanent loss set to `2`, the stub expires the affected frame after at most three requests while the sender continues producing new frames.

執行中只可切換模組的「在线」勾選，再按「应用在线位图」；變更從下一次採樣生效，既有快取保持不變。`64 KiB / 1 MiB / 2 MiB` 預設皆為**包含模組標頭的完整邏輯幀大小**。

While running, change module availability with the **「在线」** checkboxes and **「应用在线位图」** button. Changes apply at the next sample without modifying cached frames. The `64 KiB / 1 MiB / 2 MiB` presets describe the **entire logical frame, including module headers**.

## CLI 與測試｜CLI and tests

### Windows UDP 診斷與回歸（2026-11-16）

接收測試樁現在會對自己的 socket 設定 `SIO_UDP_CONNRESET=FALSE`，並分別處理收／發 10054。
正常限時、Ctrl+C 與可捕獲異常退出都會嘗試原子保存 JSON；不可寫路徑會輸出錯誤並以非零碼退出。
強制殺程序、斷電或不可寫磁碟仍不能保證保存報告。`--quiet` 只關閉常規終端輸出。

The diagnostic stub configures its own Windows UDP socket and handles reset errors defensively.
Reports distinguish generated NACKs, send attempts, local acceptance, and user-space timer lateness.
The receiver remains a bounded test tool, not a production receiver.

以下 PowerShell 命令會保留每輪獨立目錄、收發報告、實際建置類型、來源／執行檔雜湊、程序日誌及幀清單對帳：

```powershell
Set-Location D:\SimTransferRT
cmd /d /c scripts\build.cmd Release
cmd /d /c scripts\run.cmd Release
cmd /d /c scripts\test-network.cmd Release
cmd /d /c scripts\test-diagnostics.cmd --suite short
# 3 x (A0-drain 60s + A1 60s)，再 A1 120s；先檢查短測的收齊與停滯情況
cmd /d /c scripts\test-diagnostics.cmd --suite long
python tools\probe_windows_udp_reset.py --report artifacts\udp-diagnostics\windows-reset-probe.json
```

診斷工具預設 Release，會核對 `CMakeCache.txt`，不只相信參數。可用 `--build Debug` 做功能對照。
短測包括 A64、A0-closed、A0-drain、A1、B64、B1、C、CRC；`--cases A64 --seconds 600` 可單獨做 64 KiB 的 10 分鐘測試，`A1` 為 1 MiB。
性能目標或功能驗收失敗時退出碼為 1，證據仍保留在 `artifacts/udp-diagnostics/<唯一目錄>/`。
長測不會降低負載、放寬協定逾時或改用 FIFO。A0-drain 不做 CRC／重組，不能驗證接收正確性。
新版接收器由編排器管理到發送結束後再正常停止，不把發送器冷啟動時間扣入接收窗口。

接收器也支援 `--summary-every 1 --trace-nacks <檔案> --trace-limit 2048 --run-id <本地ID>`。
`--ready-file` 提供本地啟動同步；`--status-file` 與 `--checkpoint-every 1` 提供可選低頻檢查點。
這些檔案與 run_id 都不進入 UDP 協定。GUI 可在啟動前設定程序環境 `$env:SIM_RT_RUN_ID='gui-test-id'`，
按「導出本地報告」保存相同 schema 的 JSON；測完以 `Remove-Item Env:SIM_RT_RUN_ID` 清除本終端設定。

所有此輪網路測試均為 `127.0.0.1` 回環；`linkMbps` 是容量估算輸入。實體 10Gbps、硬體時間戳與硬即時尚未驗證。
新欄位 `actualInitialDeadlineMisses`、完整首輪耗時分布與取樣間隔偏差，須連同舊計數及未完成首輪一起看。
完整結果、未達標項與回退方式見 [本次診斷報告](docs/UDP_DIAGNOSTICS_2026-11-16.md)。

直接啟動 CLI 時須在 CMD 中先載入 Qt DLL 與編譯器環境。若要觀察接收結果，請同時執行上述測試樁。

For direct CLI invocation, initialize the Qt DLL and compiler environment in CMD first. Run the receiver stub alongside it to observe reception.

```bat
call scripts\env.cmd
build\windows-debug\SimTransferCli.exe --frames 100 --frame-bytes 65536 --report local-report.json
build\windows-debug\SimTransferCli.exe --help
python tools\udp_feedback_stub.py --help
```

自動檢查：

Automated checks:

```bat
scripts\build.cmd
scripts\test-network.cmd
```

第一個腳本執行 C++ 核心、Qt 應用整合與 GUI 啟動測試；第二個腳本執行 Python 協定測試與真實 Qt CLI 的八種 UDP 回環情境：正常、首輪丟片、CRC 損壞、重複、逆序、重複 NACK、持續丟片及幀編號回繞。

The first script runs the C++ core, Qt application integration, and GUI startup tests. The second runs the Python protocol tests and eight UDP loopback scenarios using the actual Qt CLI: normal operation, initial loss, CRC damage, duplicates, reverse order, duplicate NACKs, permanent loss, and frame-ID wraparound.

**2026-11-13 本機驗證：** Windows Debug 建置成功，CTest **3/3**、Python **18/18**、Qt CLI UDP 回環 **8/8** 通過。使用 MSVC 14.44.35207、Qt 6.12.0、CMake 4.4.5、Python 3.13.14。詳見[建置與測試狀態](docs/BUILD_STATUS.md)與[本次驗證紀錄](docs/test-results/windows-2026-11-13/)。

**Local verification on 2026-11-13:** Windows Debug build succeeded; CTest **3/3**, Python **18/18**, and Qt CLI UDP loopback **8/8** passed with MSVC 14.44.35207, Qt 6.12.0, CMake 4.4.5, and Python 3.13.14. See [build and test status](docs/BUILD_STATUS.md) and the [verification records](docs/test-results/windows-2026-11-13/).

### 不含 Qt 的核心建置｜Core-only build

有 C++17 編譯器、CMake 與 Ninja 時，可在已啟用編譯器的終端執行：

With a C++17 compiler, CMake, and Ninja available, run these commands in a compiler-enabled terminal:

```sh
cmake --preset core-only
cmake --build --preset core-only
ctest --preset core-only
```

此設定建置 `SimRtProbe` 與核心測試，不驗證 Qt 層。原生測試適配器與 Qt GUI／CLI 共用即時核心，但不能以原生測試結果替代 Qt、Windows 網路堆疊或目標網卡驗收。

This configuration builds `SimRtProbe` and the core tests without validating the Qt layer. The native probe shares the streaming core with the Qt GUI/CLI, but its results do not replace Qt, Windows networking, or target-NIC validation.

## 協定摘要｜Protocol summary

| 項目 / Item | 格式與行為 / Format and behavior |
|---|---|
| 封包類型 / Packet types | `DATA (0x01)`、`NACK (0x02)` |
| 位元組順序 / Byte order | 小端序 / Little endian |
| Magic | `0xAA55A55A` → `5A A5 55 AA` |
| DATA | 32 B 標頭 + 最多 1,436 B 載荷 + 4 B CRC32 / 32 B header + up to 1,436 B payload + 4 B CRC32 |
| UDP 大小上限 / Maximum UDP payload | 1,472 B |
| 邏輯幀 / Logical frame | 按模組 ID 遞增串接 `[module_id:uint8][data_len:uint32 LE][data]`，模組可跨片 / Concatenate modules in ascending ID order; modules may span fragments |
| CRC32 | 覆蓋包尾 CRC 之前的全部位元組 / Covers all bytes preceding the trailing CRC |
| NACK | 依 `(start_frag, count)` 區間請求 / Requests fragment ranges using `(start_frag, count)` |
| 補傳 / Retransmission | 保留原 TS、mask 與載荷，設定 flags bit 0，重新計算 CRC / Preserves the original timestamp, mask, and payload; sets flags bit 0 and recomputes CRC |

協定無握手，也無 `START / READY / FIN / SUCCESS / FAIL / CANCEL` 或 SHA-256 欄位；與舊 SimUDP/1 的大端序 48 B 標頭不相容。接收端負責最多三次 NACK 與逾時作廢，發送端不會因某片被請求三次就終止整條資料流。未啟動接收端不一定使本地發送失敗；介面不將「沒有 NACK」或 Socket 接受資料視為對端成功接收。

The protocol has no handshake, `START / READY / FIN / SUCCESS / FAIL / CANCEL` messages, or SHA-256 field. It is incompatible with the older SimUDP/1 big-endian 48-byte header. The receiver handles the three-request NACK policy and expiration; the sender does not terminate the stream after three requests for a fragment. Local sends may succeed without a receiver, so the interface does not interpret absent NACKs or socket acceptance as confirmed delivery.

完整欄位與網路設定見[協定文件](docs/PROTOCOL.md)。

See the [protocol reference](docs/PROTOCOL.md) for field layouts and network settings.

## 時序與相容性邊界｜Timing and compatibility limits

- **模擬資料與時間戳：** TS 來自主機單調計數器（Windows 為 QPC），介面標示為 **SIMULATED**，不是外部採樣硬體時鐘。模組資料是不透明模擬位元組或檔案內容，未定義醫療波形或座標編碼。
  **Simulated data and timestamps:** TS comes from the host monotonic counter (QPC on Windows), is labeled **SIMULATED**, and is not an external acquisition clock. Module bodies are opaque synthetic bytes or file contents; no medical waveform or coordinate encoding is defined.
- **效能目標：** 15 ms 首輪傳送與小於 1 ms 抖動須在目標硬體量測，並非程式保證。1 MiB 為 731 片、2 MiB 為 1,461 片；2 MiB 在 1 Gbps 上僅原始資料就需約 16.78 ms，因此預設拒絕該容量組合。「超带宽格式测试」只供格式驗證。
  **Performance targets:** The 15 ms initial-send window and sub-1 ms jitter require target-hardware measurement. A 1 MiB frame has 731 fragments; 2 MiB has 1,461. Raw 2 MiB data alone takes about 16.78 ms at 1 Gbps, so this capacity combination is rejected by default. Oversubscription mode is for format testing only.
- **快取與回繞：** 預設依協定使用 `logic_id % 3`，在編號回繞時有已知槽位衝突；`--cache-policy fifo` 是須明確選用的候選修正。
  **Cache and wraparound:** The default `logic_id % 3` policy preserves the protocol's known slot collision at ID wraparound. `--cache-policy fifo` is an explicitly selected proposed correction.
- **邊界處理：** 所有模組離線時跳過採樣；超長邏輯幀計為組幀拒絕並跳過；僅成功組幀才分配 `logic_id`。這些行為仍需與正式接收平台確認。
  **Boundary behavior:** Samples with no online modules are skipped; oversized frames are counted as rejected and skipped. A `logic_id` is assigned only after successful assembly. These choices still require agreement with the production receiver.
- **重啟與網路：** 程序重啟須協調清除或重置對端幀狀態，協定未新增 session ID。來源 IP 過濾及 CRC32 不提供身分驗證；本工具用於隔離實驗網路。
  **Restart and networking:** Coordinate receiver-state cleanup or reset after process restarts; the protocol adds no session ID. Source-IP filtering and CRC32 do not authenticate peers. This tool is intended for isolated laboratory networks.

測試通過不代表已完成實體網卡吞吐、網路封包 DF 位、硬體採樣、長時間穩定性或無開發環境機器的部署驗收。相關決策見[相容性與邊界決策](docs/SPEC_DECISIONS.md)，驗收步驟見[測試文件](docs/TESTING.md)。

Passing these tests does not establish physical-NIC throughput, on-wire DF behavior, hardware acquisition timing, long-run stability, or deployment on a machine without development tools. See [compatibility decisions](docs/SPEC_DECISIONS.md) and the [testing guide](docs/TESTING.md).

## 打包與開發工具｜Packaging and development tools

```bat
scripts\package.cmd
```

此腳本建立 Release 並在安裝階段部署 Qt，產物位於 `dist/bin`。發佈前仍須在未安裝 Qt SDK 的 Windows 機器驗證啟動、Qt 平台外掛與 MSVC 執行階段。

This script builds Release and deploys Qt during installation into `dist/bin`. Before distribution, validate startup, Qt platform plugins, and the MSVC runtime on a Windows machine without the Qt SDK.

VS Code 可使用 `scripts\open-vscode.cmd` 載入編譯器與 Qt 環境；先關閉舊視窗，避免沿用其他工具集環境。`Ctrl+Shift+B` 執行 Debug 建置，`F5` 使用附帶的偵錯設定。

For VS Code, use `scripts\open-vscode.cmd` to initialize the compiler and Qt environment. Close existing windows first to avoid inheriting another toolset's environment. `Ctrl+Shift+B` runs the Debug build; `F5` uses the supplied debug configuration.

若直接使用 CMake 預設，請依安裝位置調整 Qt 路徑，或將 `CMakeUserPresets.json.example` 複製為本機 `CMakeUserPresets.json` 後修改。僅調整 `CMakeLists.txt` 中的最低 Qt 版本不會改變 SDK 路徑。

When using CMake presets directly, update the Qt prefix or copy `CMakeUserPresets.json.example` to a local `CMakeUserPresets.json` and customize it. Changing the minimum Qt version in `CMakeLists.txt` does not change the SDK path.

## 專案結構與文件｜Project layout and documentation

| 路徑 / Path | 用途 / Purpose |
|---|---|
| [`src/ui/`](src/ui/) | 主視窗、設定面板、分片表格 / Main window, configuration panel, fragment table |
| [`src/application/`](src/application/) | 控制器、工作階段、命令分派與執行迴圈 / Controllers, sessions, command dispatch, run loop |
| [`src/core/`](src/core/) | 採樣、組幀、快取、發送節奏與狀態 / Sampling, assembly, cache, pacing, state |
| [`src/protocol/`](src/protocol/) | 小端序編解碼與 CRC32 / Little-endian codec and CRC32 |
| [`src/network/`](src/network/) | Qt／原生 UDP 適配器與 DF 設定 / Qt/native UDP adapters and DF configuration |
| [`tests/`](tests/) | C++ 核心與 Qt 應用測試 / C++ core and Qt application tests |
| [`tools/`](tools/) | Python 協定、接收測試樁與網路測試 / Python protocol, receiver stub, network tests |
| [`scripts/`](scripts/) | Windows 建置、啟動、測試與打包 / Windows build, run, test, packaging scripts |

- [架構與執行緒 / Architecture and threading](docs/ARCHITECTURE.md)
- [協定欄位 / Protocol reference](docs/PROTOCOL.md)
- [相容性與邊界決策 / Compatibility decisions](docs/SPEC_DECISIONS.md)
- [測試與驗收 / Testing and acceptance](docs/TESTING.md)
- [統一驗收表與導出 / Unified acceptance tables and exports](docs/UDP_ACCEPTANCE_2026-11-16.md)
- [建置與測試狀態 / Build and test status](docs/BUILD_STATUS.md)
- [變更紀錄 / Changelog](docs/CHANGELOG.md)
- [技術參考 / Technical references](docs/TECHNICAL_REFERENCES.md)

README 為中英文對照；其餘技術文件目前主要使用中文。

This README is bilingual. The remaining technical documentation is currently primarily in Chinese.

`SHA256SUMS.txt` 記錄 Git 中檔案內容的 SHA-256；文字以 Git 儲存的 LF 換行計算，本機 `.cmd` 的 CRLF 換行可能產生不同雜湊。

`SHA256SUMS.txt` records SHA-256 hashes of Git blob contents, using Git's LF line endings. Local `.cmd` files checked out with CRLF may have different hashes.

## V2 多帧方案 B｜V2 multi-frame Plan B

0.3.0 新增独立采集准备、统一发送、Aurora BX、正式接收消费及 100Mbps／1GbE／2.5GbE 三档切换。GUI 默认 V2；原 V1 界面使用 `--v1`，原 CLI 脚本保持兼容，V2 CLI 使用 `--protocol v2`。

运行 `scripts\build-v2.cmd` 在独立目录重新构建 Release 并执行测试。实际硬件同步、网卡出线和真实电生理波形需独立验证；软件功能通过不等于严格实时指标全部达标。

Version 0.3.0 adds separate preparation workers, one shared sender, Aurora BX, receiver consumers and three network profiles. Build with `scripts\build-v2.cmd`; legacy V1 entries remain available. Hardware synchronization, physical egress and real EP waveform interpretation require separate evidence.

- [V2 实现约定与使用说明 / Implementation contract and usage](docs/V2_IMPLEMENTATION_CONTRACT_2026-11-17.md)
- [方案 B 逐项验证 / Plan B validation](docs/V2_PLAN_B_VALIDATION_2026-11-17.md)
