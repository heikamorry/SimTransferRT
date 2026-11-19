# Windows UDP 接收測試樁：修復、證據與限制

本文件記錄 2026-11-16 在 Windows 11、Python 3.13.14、IPv4 回環上的接收測試工具驗證。正式產品仍只有 Sim 發送端。以下資料不是實體 10Gbps 網路、網卡時間戳或硬體多模組同步驗收；TS 仍為 SIMULATED。

## 已證實的問題與尚未證實的因果

| 狀態 | 原始入口／行為 | 證據與修復 |
|---|---|---|
| 已證實 | `udp_feedback_stub.py` 的 `recvfrom` 只捕捉 would-block，`sendto` 沒有 10054 防禦；報告位於迴圈之後 | 10054 或其他異常可跳過報告。現在分別處理接收／發送 10054，未知錯誤保留堆疊、非零退出，清理後仍嘗試原子保存。 |
| 已證實 | `ReceiverModel.retire()` 只留下最近 256 幀的終態字典 | 完成 ID 1..299 後再次送入 ID 1 第 0 片，原模型重新建立舊幀並產生 NACK；修正模型不重建。 |
| 已證實的性能瓶頸 | 原 `ingest()` 每包掃所有活動幀；`mark_tail()` 重複掃尾部；原 `poll()` 每包掃所有缺片 | 同一虛擬輸入、16 個停滯幀時有顯著掃描成本；下面保留正反結果，正常輸入並沒有加速。 |
| 已證實 | 原 `completed % 50 == 1` 決定 `LOCAL COMPLETE` 抽樣列印 | 連續日誌 ID 相差 50 不能用來推論漏掉 49 幀。新版本預設每秒摘要，以完整報告和幀清單核對。 |
| 可重現機制，原事件順序未知 | Windows UDP socket 向關閉的目的埠發送後，可以在後續 `recvfrom()` 收到 10054 | 真實 Winsock 實驗已重現。沒有原始雙端時間線，不能斷言原使用者先停止了 Sim，也不能將它解釋為 TCP 握手失敗。 |
| 尚未證實 | 歷史 38 萬／53 萬 NACK 是否全部來自終態重建、CPU 積壓、其他接收進程或發送超期 | 原始報告沒有充分時間線。這次短測沒有重現同等 NACK 數，不能將所有歷史現象歸因於單一修復。 |

原始工作區 `sim-rt-report-massive.json` 已保留，其內容是 `nacks=387372`、`cacheMisses=386986`、`unsentExpired=649487`、`deadlineMisses=2219`；這與提示詞另外列出的歷史觀測不是同一組數字。兩者都不算本次修復後結果。

## P0：Windows socket 和退出路徑

`tools/runtime_support.py::configure_udp_connreset()` 對測試樁真正使用的 socket 執行 `SIO_UDP_CONNRESET=FALSE`。本機 SDK `10.0.26100.0/um/MSWSock.h` 定義為 `_WSAIOW(IOC_VENDOR,12)`，得到 `0x9800000C`；SDK 的 `SOCKET` 是 `UINT_PTR`。ctypes 使用指標寬度 `c_size_t`、32 位 BOOL／DWORD，明確設定 `argtypes/restype`，失敗時呼叫 `WSAGetLastError()`。本機 Python `socket.ioctl` 文件只列出 RCVALL、KEEPALIVE_VALS、LOOPBACK_FAST_PATH，故沒有假設它接受任意控制碼。非 Windows 明確報告 `not_applicable`。

`ReceiverRuntime` 將 socket 建立／綁定、執行迴圈、錯誤處理和清理分開。每個錯誤方向分別統計 `recv_10054`、`send_10054`、`recv_10040`、`send_10040` 和 would-block。10054 與 10040 警告每種最多每秒一次。其他不可恢復 OSError 會保存原始堆疊並非零退出；不會吞掉全部 OSError。10040 的緩衝內容從未交給解碼器；成功返回但超過 1472 B 的資料報也不能被接受為合法 DATA。

Ctrl+C 和 Windows 新進程群組的 CTRL_BREAK_EVENT 都走 KeyboardInterrupt 清理路徑。socket.close、最近完整幀輸出、trace 輸出分別保護，彼此失敗不阻止最終報告。若完整報告組裝本身失敗，另存包含原始錯誤、組裝錯誤和原始 runtime 計數的最小報告。JSON 先寫入同目錄臨時檔、flush/fsync，最後以 `os.replace` 替換；路徑不可寫會明確輸出 stderr 並非零退出。這不保證強制終止、斷電、無法寫入磁碟時也能保存報告。

真實 Windows 證據在 [windows-icmp-experiment.json](../artifacts/udp-diagnostics/p0-runtime-snapshot/windows-icmp-experiment.json)：預設 socket 發出合法 NACK 到已關閉回環埠，約 52µs 後收到真正的 `ConnectionResetError`／10054；設定 FALSE 的對照組在 200ms 觀察期間沒有 reset，且仍可收到後續資料報。未做封包擷取，這是 Winsock 行為證據；關閉 ICMP 錯誤交付不表示對端在線或 NACK 必達。

另以真實新進程群組發送 CTRL_BREAK_EVENT，退出碼 0，報告 `exit_reason=keyboard_interrupt`：[ctrl-break-report.json](../artifacts/udp-diagnostics/p0-runtime-snapshot/ctrl-break-report.json)。原 P0 檔案快照、原接收模型及測試記錄保存在 [p0-runtime-snapshot](../artifacts/udp-diagnostics/p0-runtime-snapshot/)，後續修正另由當前來源與報告雜湊追蹤。

### 長測發現的 checkpoint 缺陷及補修

第一輪完整長測在 [A1-3-d9c320d6247a](../artifacts/udp-diagnostics/20261116-112831-8b178533/A1-3-d9c320d6247a/) 發現新的真實回歸：可選 `receiver-status.json` 的 `os.replace` 拋出 `PermissionError: WinError 5`，當時它被當作主迴圈致命錯誤。接收器因此非零退出，但原始 traceback 與最終報告成功保存；編排器停止發送端。結果是 built=705、completed=697、仍在途 1、全幀未見 7，該輪明確失敗，並未因 `sendErrors=0` 或零首輪超期而算通過。

本機隨後用普通 `Path.open('r')` 持有同一檔案，再呼叫相同 `atomic_json → os.replace`，重現同樣的 WinError 5；讀取 handle 關閉後替換成功，證據在 [checkpoint-reader-sharing/evidence.json](../artifacts/udp-diagnostics/checkpoint-reader-sharing/evidence.json)。這與 Windows 的檔案分享語義一致：既有 handle 必須允許 FILE_SHARE_DELETE，才能容許其他 handle 刪除／重新命名。[Microsoft CreateFile 文件](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilea)。這證明讀取者與原子替換衝突是可重現機制；沒有取得失敗瞬間所有 handle，仍不能斷言當時持有者一定是編排器。

補修只隔離**可選 checkpoint** 的 OSError：累計 `checkpoint_attempts/successes/failures`，保留最近錯誤（路徑、錯誤碼、單調時間）和最近成功時間，限頻警告，等下一個正常 checkpoint 週期再嘗試。沒有同步立即重試或新增 sleep，不讓觀察者中斷 DATA 接收或改變協定計時。最終 `--report` 仍是必要輸出，寫入／替換失敗依然 stderr＋非零退出。未知協定／程式錯誤仍走原致命錯誤路徑。

長測結束後才套用補修，避免同一組運行期間變更來源；原失敗目錄完整保留。新增四項測試涵蓋單次 checkpoint 失敗後恢復、持續失敗仍有界、必要報告失敗仍非零，以及真實 Windows 普通 reader 持有檔案時接收 runtime 不中斷。補修後 [runtime-checkpoint-regression.log](../artifacts/udp-diagnostics/runtime-checkpoint-regression.log) 為 23/23，[python-after-checkpoint-fix.log](../artifacts/udp-diagnostics/python-after-checkpoint-fix.log) 為 85/85，兩個命令退出碼均為 0。重跑長測結果另見最終總報告，不以單元測試代替。

## 接收狀態機與資源邊界

`ReceiverModel` 支援注入單調時鐘；測試不依賴作業系統睡眠。缺片的 `(attempts,last_request_time)` 獨立保存，重複登記不重設預算。首次發現於當前處理輪請求，後續間隔至少 10ms，共三次，第三次後 10ms 仍缺即終結。阻塞後醒來不將錯過的請求一次突發補發。第三次已逾期時，即使下一次 poll 還沒執行，遲到片也不能使幀完成。flags=0 的原片只要先前曾請求，仍計為補回。

同輪缺片被壓縮成區間，超過 363 區間拆成多包 NACK；每片預算只扣一次。預算在「生成」時消耗，與 sendto 是否成功無關，因此本地發送失敗不能觸發無限本地重試。`nack_packets` 是生成數；`nack_send_attempts` 是本地呼叫數；`nack_send_accepted` 是 socket 接受數。故意重複 NACK 的診斷選項增加後兩者，不重設模型預算。

完成、作廢、資源淘汰的幀會離開活動集合。終態字典仍只保留 256 筆摘要，但另外以 4096 幀的有界位圖防重放。合法活動舊幀可以超出視窗繼續處理；視窗內未見過的舊幀也可建立，沒有簡化成「小於最新 ID 就丟掉」。視窗之外且不再活動的 ID 被拒絕。這是接收端的有界重放／資源規則，與發送端三槽快取不同。接收端最多 32 個活動幀，超限另記 `active_evictions`，不算三次協定請求逾期。

序號比較使用 uint32 半區間規則；相隔至少 2³¹ 幀、完整 uint32 循環重用、進程重啟後重用 ID 的歧義仍無法靠現有線上協定消除。需協調重設接收狀態，沒有新增 session ID。發送端預設 `logic_id % 3` 與回繞槽位碰撞完全未暗改。

尾部只增量登記一次。只有真正更新的幀首次到來才暴露舊幀尾部；重複 DATA 不更新 `last_new`。到期工作使用堆及 generation 檢查，每幀最多一個有效任務；失效項超過固定門檻會壓縮。完成／作廢的任務被邏輯取消，不能再次建立幀或產生 NACK。

實際 runtime 每輪最多取 64 包，或已用 250µs 就結束收包批次，接著 poll 當前到期工作。因此新發現且仍缺的片在這一處理輪回饋，不等完整幀；忙碌 socket 不能無限推遲 poll。單次解碼、完整幀驗證或 OS 排程仍可能超過預算，報告明確記錄最大值。批次內先亂序、隨後補齊的片在 poll 時已不缺，不產生多餘請求。

## 同輸入微型基準與回歸

[receiver-resurrection-proof.json](../artifacts/udp-diagnostics/baseline-20261116-105100/receiver-resurrection-proof.json) 記錄：先完成 ID 1..299，再交付 ID 1 的第 0 片。原模型重建 ID 1，產生一包 NACK／四個缺片請求；修正模型不重建、不發 NACK。

[receiver-model-comparison.json](../artifacts/udp-diagnostics/baseline-20261116-105100/receiver-model-comparison.json) 保存來源 SHA256、三輪交替順序、相同虛擬輸入與全部樣本。這是 CPU 微型基準，沒有真實 UDP：

| 相同輸入情境 | 原模型中位數 | 修改模型中位數 | 協定輸出 |
|---|---:|---:|---|
| 正常 20 幀 × 1MiB、14620 包 | 57.3946ms | 60.2907ms | 兩者完成 20 幀、0 NACK；沒有加速 |
| 16 個停滯 1MiB 幀 + 一個完整 1MiB 幀 | 1089.6118ms | 9.6396ms | 兩者生成 16 包 NACK／11680 缺片請求 |

第二個情境證明全量掃描在積壓狀態下有可觀成本，不能由此宣稱正常吞吐提升 113 倍，也不能推論舊模型所有高 NACK 都違反協定。之後的逾期邊界補強由確定性回歸涵蓋；保存的微型基準結果以其記錄的來源雜湊為準。

接收模型新增 24 項回歸，覆蓋獨立時鐘、三次請求、不同時刻發現、多 NACK、重疊區間、重複尾部、原片補回、終態摘要淘汰、停頓、回繞、活動舊幀例外、超過三個活動幀、模組跨片、元資料衝突及有界任務。runtime 新增 23 項測試，覆蓋設定成功／失敗、真實 Windows 設定、模擬收發 10054、10040、would-block、未知錯誤、bind／close／trace／報告失敗、Ctrl+C／Ctrl+Break、限時、quiet、原子替換、可選 checkpoint 失敗和 drain。模擬錯誤測試沒有冒充真實 Winsock 驗證。

## 真實 UDP 與停止後行為

本次短測 [20261116-110739-f2a6a8b6/summary.json](../artifacts/udp-diagnostics/20261116-110739-f2a6a8b6/summary.json) 使用實際 Release Qt CLI，1MiB 模組體 1048571B／整幀 1048576B／731 片，目標 50Hz：

| 情境 | 發送 built | 接收完成 | 接收作廢 | 重要結果 |
|---|---:|---:|---:|---|
| A1 無主動故障，10s | 500 | 500 | 0 | 4 NACK；首發超期與未完成首發仍存在，不能算時序驗收通過 |
| B64 首輪缺片 2，10s | 500 | 500 | 0 | 500 次補回，最多每片 1 次請求 |
| B1 首輪缺片 2，10s | 500 | 500 | 0 | 500 次補回，最多每片 1 次請求 |
| C 持續缺片 2，10s | 500 | 0 | 500 | 正好 1500 NACK，最多每片 3 次請求；後續幀一直前進 |
| CRC 首輪損壞片 4，10s | 500 | 500 | 0 | CRC 拒絕與補回閉環有效；另有真實排程延遲造成的補片 |

各完整接收情境均使用發送 built 幀清單對照完成／作廢／全幀缺失／仍在途，不以接收端看到的幀作分母。這些是該輪結果，後續長測與最終性能結論以總診斷報告為準。

C 情境可直接驗證停止順序：[sender-report.json](../artifacts/udp-diagnostics/20261116-110739-f2a6a8b6/C-5-096f6dfed1b4/sender-report.json) 的結束 UTC 為 `03:08:49.764Z`；[receiver-report.json](../artifacts/udp-diagnostics/20261116-110739-f2a6a8b6/C-5-096f6dfed1b4/receiver-report.json) 正常限時結束於 `03:08:51.633425Z`，晚約 1.87 秒。發送停止後的 checkpoint 已是 500 作廢／1500 NACK／0 活動幀，最終數值沒有再增長。沒有產生新幀或無限陳舊 NACK；兩端退出碼均為 0。

同一 C 報告的 `max_timer_lateness_us=14243`、`select_max_us≈16549` 表明：模型的 2ms／10ms 規則保持不變，但 Windows 實際喚醒延遲並不滿足微秒硬即時。空 socket 的 `select(...,0.001)` 在這台機器也觀察到約 15.9ms 等待。用戶態處理時間不能標成網卡實際收包時間。

## 計數和報告閱讀方式

- `datagrams` 保留交給模型的資料報數；`valid`／`invalid` 是協定解碼結果。Winsock 10040 未返回可驗證資料，所以另計 `recv_10040`；`socket_datagrams` 只計成功 recvfrom，drain 模式也計入。重複／遲到屬於 valid DATA 的後續分類，不與 valid 相加。
- `total_bytes` 是完成的邏輯幀位元組總數，Python 整數不會在 4GiB 溢出。`socket_bytes` 是用戶態實際取得的 UDP payload 位元組，不能拿截斷緩衝推測原報文長度。
- `completed` 僅在 CRC、幀元資訊、全部分片、模組結構驗證後增加。`expired` 是三次請求耗盡；`active_evictions` 是資源壓力，兩者分開。
- `source_sha256`、git revision／dirty、Python／平台／PID、run_id、socket 實際緩衝、端點、模式、故障與日誌選項、起止時間和退出原因均為本地報告資料，不寫入線上報頭。Qt／Debug／Release 由發送報告和編排器 environment 提供，Python 本身不假裝有 Qt build type。
- trace 是最多 `--trace-limit` 筆的近期事件環形緩衝，在退出時輸出；同時報告產生、保留、淘汰筆數。`--quiet` 只停常規終端輸出，錯誤仍寫 stderr，統計和最終報告仍保留。checkpoint 預設不開；編排器顯式每秒開啟，同條件比較須保持一致。
- `--drain` 不 CRC、不重組、不發 NACK，仍使用接收 CPU。它用來避免目標埠無人監聽，不能作端到端正確性或協定驗收。

## 可重現命令（PowerShell，專案根目錄）

```powershell
python -m unittest discover -s tools -p 'test_*.py' -v
python tools/probe_windows_udp_reset.py --report artifacts/udp-diagnostics/windows-reset-new.json
python tools/benchmark_receiver_model.py --before artifacts/udp-diagnostics/baseline-20261116-105100/receiver_model.original.py --report artifacts/udp-diagnostics/model-comparison-new.json
python tools/run_diagnostics.py --build Release --suite short
python tools/run_diagnostics.py --build Release --suite long
```

請為手動輸出指定新路徑，不覆蓋歷史證據；編排器自動建立唯一 run 目錄。回退須以任務開始時保存的原始檔或反向套用本次差異，先保存任務後新增的使用者修改，不能執行 `reset --hard`。原始測試報告與本次證據不屬於待清除產物。
