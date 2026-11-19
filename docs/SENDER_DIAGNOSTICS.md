# 發送端診斷與計時口徑（2026-11-16）

第二輪有界異常時間線、快照排程及P99.9的附加欄位見[穩定性驗證](UDP_STABILITY_2026-11-16.md)。原有欄位口徑保留。

本文件記錄 C++ 發送端的定向修復。真實 UDP 與長時間性能結果由本次總報告及各 run 目錄提供；本文件不以單元測試或本地 send 成功宣告收齊。

## 已證實缺陷與修復

| 位置 | 證據 | 處理 |
|---|---|---|
| `RealtimeStream::tick` | 原碼使用 tick 入口時間計算最後首發完成；組幀、CRC、最後 send 的時間可能未納入。虛擬 send 增加 16000µs 時，舊跨度仍是 0。 | 執行器注入真實單調時鐘；新增工作完成後計時。舊 `lastInitialDurationUs` 保留原口徑供歷史比較。 |
| `PacketPacer::due` | 遲到追趕後，`due` 取原計劃和最後發送加半間隔的較大值。相對該值的遲到不能代表相對原計劃的遲到。 | 提供獨立 `plannedDue`，保留原絕對計劃與調整時刻兩種統計。不更改 20µs 上限間隔、半間隔追趕及一輪一片策略。 |
| `RealtimeStream::acceptNack` | 合法的 363 個重疊區間可以使一包在 1MiB 幀上重複走訪 265353 個片號；最多只有 731 個不同片。 | 完整驗證後排序合併區間，每包最多走訪該幀的不同片數。保留重疊項的 `duplicatePending` 計數。補發完成後後續合法請求仍可排隊。 |
| `runStream` | 接收／命令原來只有每輪 32 條的數量限制，沒有時間限制。 | 兩階段各有 32 條及 50µs 預算；至少執行一個單元，預算可超出一個有界操作的耗時，不能宣稱硬截止。Stop 保持獨立優先位。 |
| `RealtimeStream::stop` | 原停止沒有記錄當前尚未嘗試首發的片數。 | 新增 `unsentAtStop`、未完成幀與取消重傳任務統計；停止不再把這些片算作下一採樣替換。 |
| `runStream` 發布快照 | 原來 tick 結束後仍使用 tick 之前的 `now`。 | 發布前重新讀取單調時鐘，量測快照建構及發布耗時。 |
| CTest Qt 環境 | 直接從新終端執行 CTest 不能依賴另一個 cmd 程序留下的 PATH。 | 在 Qt 測試目標環境中加入實際 Qt Core DLL 目錄，保留腳本內 PATH，不修改全域設定。 |

NACK 暴增是否主要源於 Python 積壓、發送端首發未完成、來源混入或系統排程，需以同條件收發報告判定。上述原碼缺陷不能單獨解釋歷史的全部 `cacheMisses`。沒有 NACK TS，報告不計算虛構 RTT。

## 統計定義

舊 JSON 字段仍存在，報告增加 `report_schema_version=2`。`run_id` 只進本地報告，不進 DATA/NACK。

| 字段／區域 | 定義 |
|---|---|
| `firstAttempts` | 首發嘗試，包含故障注入丟棄。 |
| `initialFragmentsPlanned` | 所有成功建幀的分片總數。 |
| `unsentExpired` | 下一次採樣替換尚未首發完的幀時，剩餘未嘗試分片。不是網路丟包。 |
| `unsentAtStop` | Stop 或有限收尾結束時未嘗試的首發分片。 |
| `initialPendingFragments` | 此次快照仍待嘗試首發的分片。 |
| `initialFinishedFrames` | 全部首發嘗試已結束的幀；包含注入丟棄／本地 send 失敗。 |
| `initialFullySubmittedFrames` | 所有首發片均被本地 socket 接受，且未注入丟棄。仍不證明遠端收齊；故意損壞但本地接受的包仍屬本地提交。 |
| `initialIncompleteFrames` | 因下一次採樣替換或停止而放棄首輪的幀。 |
| `lastInitialDurationUs` | **舊口徑**：最後首發所在 tick 入口減原採樣時間，不包含該次工作時間。 |
| `deadlineMisses` | **舊口徑**：舊跨度 >15ms，或下一採樣替換未完成幀。 |
| `lastSampleToInitialEndUs` | 原採樣時間至最後首發處理完成後的真實單調時間。 |
| `actualInitialDeadlineMisses` | 首輪嘗試已結束且真實跨度 >15ms 的幀；未完成幀由 `initialIncompleteFrames` 獨立呈現。 |
| `timing.sample_to_last_initial_submit` | 僅完全本地提交幀的採樣至最後 send 完成分布。 |
| `timing.first_send_call_to_last_completion` | 同一群幀的首次初發 send 呼叫開始至最後完成分布。 |
| `timing.sampling_interval` | 相鄰實際採樣的間隔。 |
| `timing.abs_interval_minus_20000us` | 相鄰採樣間隔與 20000µs 的絕對差；不是 `maxSampleLatenessUs`。 |
| `samplingSpanUs` | 最後實際採樣減第一次實際採樣。 |
| `samplingActiveUs` | `samplingSpanUs + 20000µs`，最後一個名義採樣週期補入的觀察窗，非量測到的完整程序耗時。 |
| `elapsedUs` | 準備完成並重設採樣原點後至快照的時間，包含收尾；程序建立／準備總耗時另見編排紀錄。 |
| `maxSendLatenessUs` | 相對 PacketPacer 動態 due 的調度遲到。 |
| `maxPlannedSendLatenessUs` | 相對不調整的原始片計劃的遲到；實際 send 呼叫前再次更新，計入編碼／CRC 時間。 |
| `built_manifest` | 起始 ID、成功建幀數及下一 ID，可依 uint32 模數展開本次建幀清單；跳過／拒絕不消耗 ID。 |

守恆式：

```text
initialFragmentsPlanned = firstAttempts + unsentExpired + unsentAtStop + initialPendingFragments
built = initialFinishedFrames + initialIncompleteFrames + (有正在首發的幀 ? 1 : 0)
```

分布使用固定記憶體直方圖，納入整輪所有符合群體的觀察，不抽取最近幾幀。P50/P95/P99 是 100µs 桶的上界（可被真實 max 截短）；超過 100ms 的溢位桶以該群體真實最大值作保守上界。max 與超期次數保留原始值。未完成幀、注入丟棄和本地 send 錯誤不進成功本地提交分布，不能以排除它們掩蓋整體失敗率。

`profiles` 記錄各階段 calls、total_us、max_us：收包、解碼及排隊、命令處理、組幀及快取、編碼及 CRC、send、快照及發布。CRC 計時包含 DATA 編碼，不能直接稱作獨立 CRC 演算法成本。單調讀鐘本身也有成本，真實性能測試須比較同樣診斷設定。

命令及補傳列的高水位／最大等待時間在 `stats`；Stop 優先位也保留原入列時刻，不能用 0 當作入列時刻。快取未命中保留最後 32 個事件，包含請求 ID、最新 ID、快取 ID 與模數安全距離；捨棄數單獨記錄，該環形事件只作本地診斷。

## 驗證證據

修改前：`artifacts/udp-diagnostics/baseline-20261116-105100/ctest-debug-before.log`，CTest 3/3。修改前來源歸檔位於同目錄 `original-source/`，保留原始報告不覆寫。

本次 sender Release：

```powershell
cmd /d /c scripts\build.cmd Release
ctest --test-dir build/windows-release --output-on-failure
```

- `artifacts/udp-diagnostics/sender-build-release-delivery.log`：最終 Release 構建、CTest 3/3，退出碼 0。
- `artifacts/udp-diagnostics/sender-ctest-clean-terminal.log`：未在命令中手動加入 Qt PATH 的直接 CTest，3/3。
- `artifacts/udp-diagnostics/sender-core-detail.log`：早期核心 **54/54**；最終 `build/windows-release/Testing/Temporary/LastTest.log` 為 **55/55**。新增 10 項覆蓋真實完成時間、停止／替換守恆、失敗分布、間隔抖動、原始計劃、重疊 NACK、回繞診斷、倒退時鐘、計入編碼時間的計劃遲到和持續反饋下的採樣／停止進展。
- `artifacts/udp-diagnostics/sender-json-smoke.json`：共享 Qt CLI 的 2 幀本地提交煙霧檢查，退出碼 0；JSON 成功解析，run_id/build_type 存在，分片守恆式通過。該次沒有接收器，不能當作端到端接收結果。

Debug 同樣經 `scripts\build.cmd` 執行構建和 CTest；`sender-build-debug-reviewed.log` 為 3/3、退出碼 0；最終幾何資料報告增補後的重建見 `sender-build-debug-delivery.log`。

同條件合法重疊 NACK 微基準：相同來源 harness，分別連結歸檔原版和本次 Release core；1MiB、731 片，1000 包，每包 363 個 `[0,731)` 區間。三輪耗時中位數原版 **665130µs**，本次 **3256µs**。六次的 `duplicatePending` 均為 **265352269**，退出碼均為 0。證據：`artifacts/udp-diagnostics/nack-overlap-comparison.json`，來源及編譯命令在同目錄 `nack-overlap-bench.cpp`、`compile-overlap-bench.cmd`。這是合法最壞重疊輸入的 CPU 微基準，不代表正常 NACK 流量同幅度加速，也不替代真實 UDP 或 15ms 驗收。

GUI 可透過該子程序的 `SIM_RT_RUN_ID` 環境變數與接收器對齊本地 run_id。`ConfigPanel` 只讀取診斷字串；不新增網路邏輯。GUI 顯示鏈路容量估算、實測首輪完成超期及原採樣到最後首發完成的用戶態時間。JSON 的 `initial_modules` 使用已準備來源的真實體積（含檔案），`latest_frame_geometry` 保留最多三個快取幀的實際幾何；`faults` 記錄注入設定。

`tests/test_application.cpp` 驗證 TransferController、SessionManager、CommandDispatcher、工作線程 QUdpSocket、NACK 補傳和 DF。GUI smoke 只證明可啟動；實際 GUI／CLI 對照需讀總報告，不能以 smoke 代替。

## 保留的協定與運行邊界

- 三個管理類、單工作線程 socket 所有權及 shutdown/wait 邏輯保留；MainWindow 不新增網路狀態機。
- DATA/NACK 包型、LE、magic、1436B 步長、CRC、原始 TS、mask、payload、DF 均保留。
- 快取仍是三槽 `logic_id % 3`；uint32 回繞衝突仍存在。FIFO 仍須明確指定。
- 沒有擴大快取、降低頻率、取消 CRC、增加線上訊息、逐片 Sleep(1)、即時優先級或全域網路設定。
- 接收端不回 ACK/SUCCESS；任何 send 接受計數均不表示遠端完成。
- Windows 仍是盡力排程。性能驗收必須同時檢查舊指標、真實完成超期、未完成數、停止尾部及接收報告，不能只看平均 50Hz。
