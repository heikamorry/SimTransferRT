# Windows UDP 修復與驗證紀錄 — 2026-11-16

本次直接修改既有 SimTransferRT，保留三個管理類、Qt 工作執行緒、兩種 UDP 適配器、mswsock.h 修復和預設 modulo 三槽快取。沒有建立正式接收平台，也沒有修改線上協定。

**結論：確定性缺陷已修復且有回歸證據；不能宣告所有性能目標穩定達標。** 初次 Release A1 短測雖然 500/500 幀最終收齊，仍有 128 個首發未送片在下一採樣替換時過期、9 次舊口徑首輪超期。後面的成功輪次不能抹除這次失敗。

## 檢查、基線與歷史證據

開始時讀取 README、CMake、docs/PROTOCOL.md、ARCHITECTURE.md、SPEC_DECISIONS.md 與實際來源／腳本。工作區及 D: 父層未找到 AGENTS.md。協定依工作區 docs 副本核對。

開始時 tracked 檔案沒有未提交修改；未追蹤的 `sim-rt-report-massive.json` 和 `PkgExplorerPlugin.dll.log` 保持原樣。未 reset、未清除歷史報告、未安裝 SDK、未修改全域 PATH。原始碼 HEAD 另存 zip 和隔離的 Release 建置，以免修改後程式冒充修改前基線。

證據根目錄為 `D:\SimTransferRT\artifacts\udp-diagnostics\`，以下路徑均相對此目錄：

| 證據 | 位置 |
|---|---|
| 初始 git status/diff、原始來源 zip | `baseline-20261116-105100/` |
| 修改前 Python 18/18、Debug CTest 3/3 | 同目錄 `python-before.log`、`ctest-debug-before.log` |
| 原始來源 Release 構建與 CTest 3/3 | 同目錄 `build-release-before.log` |
| P0 最小修復、原始接收模型、尚未批次優化的 runtime | `p0-runtime-snapshot/` |
| 實際 Windows 10054 時間線 | `p0-runtime-snapshot/windows-icmp-experiment.json` |
| 真實 CTRL_BREAK_EVENT 正常保存報告 | `p0-runtime-snapshot/ctrl-break-report.json` |
| 終態重生的前後確定性復現 | `baseline-20261116-105100/receiver-resurrection-proof.json` |
| 同輸入接收模型基準 | 同目錄 `receiver-model-comparison.json` |
| 同輸入 C++ 重疊 NACK 基準 | `nack-overlap-comparison.json` |

工作區找到的歷史 massive 報告是另一份：sampled/built=6077、nacks=387372、cacheMisses=386986、unsentExpired=649487、deadlineMisses=2219、bytesSent=5581954812。已原樣另存 `baseline-20261116-105100/historical-massive.json`。提示詞的 5988 幀／538657 NACK 數字沒有對應的另一個原檔，兩組歷史數字均不當成本次結果。累計位元組計數保留 uint64／Python 整數。

交付前存在性檢查曾發現根目錄歷史 JSON 不在原路徑，原因無法從本次操作紀錄確定；各代理未執行該檔的刪除／移動。已在確認原路徑不存在時，從開始時備份還原，未覆蓋任何現有檔案。兩份 SHA256 均為 `2CAE5530AAB5A0184F94D4BD37FAC49FDF319EA73F13929AC13A6B5A87AD2B49`，另存 `historical-report-preservation.txt`。

## 已證實缺陷、修復與待驗證假設

| 類別 | 位置及發現 | 驗證／處理 |
|---|---|---|
| 確定性錯誤 | 舊 `udp_feedback_stub.main` 未處理 recvfrom/sendto 的 Windows 10054，例外會跳過報告 | 型別明確的 ctypes WSAIoctl；收發各自計數、限頻警告；分層 finally 原子保存；mock 分支與實際 Windows socket 試驗 |
| 確定性錯誤 | `ReceiverModel.retire` 淘汰第 257 筆以上終態後，遲到包可重新建幀 | 完成 IDs1..299 後重送 ID1 第0片：原版重新產生 NACK，新版不建立幀。4096 位有界回播視窗仍允許活動舊幀及視窗內未見亂序幀 |
| 確定性錯誤 | 原模型若尚未 poll，第三次請求後逾 10ms 到達的片仍可救回已逾期幀 | ingest 也核對最終期限；虛擬時鐘回歸 |
| 性能放大 | 原 ingest 每包遍歷活動幀，poll 掃描每個 missing；mark_tail 重複掃描相同尾部 | 增量缺片、一次尾登記、有界到期堆、失效任務回收；正常與積壓兩種同輸入基準 |
| 計時錯誤 | `RealtimeStream.tick` 及 `runStream` 使用組幀／CRC／send 之前的 now 當完成時刻 | 注入真實單調時鐘及虛擬測試時鐘；舊欄位保留，新欄位量測真正結束 |
| 排程風險 | 每輪限制 32 個 NACK/命令，沒有時間預算；一包 363 個重疊合法範圍可走訪 265353 個片號 | 每階段 32 條／50µs 預算；驗證後合併範圍，最多走訪不同片；Stop 優先；假 transport 洪泛回歸 |
| 觀測混淆 | `completed % 50 == 1` 使原 LOCAL COMPLETE 的 ID 約每次跳 50 | 這是日誌抽樣，不能推定中間幀遺失。新版預設每秒摘要，接收率由雙方幀清單對帳 |
| 編排缺陷 | 固定接收器總生命期包含使用者 GUI 操作／Debug 冷啟動，可能早於發送器退出 | 保留兩次失敗證據；GUI 重跑，e2e 改由發送器完成後有界收尾，再正常結束接收器 |

**尚未證實的歷史根因：** 沒有歷史雙方事件時間線，不能斷言使用者先關閉 Sim，也不能把全部 NACK 歸因於 Python。實際閉埠試驗證明「UDP 發出 NACK 後收到不可達錯誤，再由 recvfrom 報 10054」可發生，沒有封包擷取，因此不聲稱擷取到原事故 ICMP。終態重生和掃描放大都能造成陳舊請求，但不能據此計算它們在原歷史數字中各佔多少。

**待驗證的剩餘性能因素：** Windows 排程停頓、回環 socket 成本與其他程序競爭。初輪 A1 的 profile 中，365824 次 send 共約 3.375s，encode+CRC 約 0.534s，500 次 build/cache 約 0.330s，snapshot/publish 約 0.022s；這支持先觀察 socket 與排程停頓，不能證明 CRC 是唯一瓶頸。沒有 ETW、硬體到達時間戳或 CPU 排程追蹤，故不給出更強因果結論。

## 同條件比較

全部性能測試為 Windows、Release、127.0.0.1、只開模組0、模組體1048571B、完整幀1048576B、731片、末片296B、50Hz；2MiB 僅作格式邊界測試。故障關閉，除表中明示案例。沒有提高優先級或綁定 CPU。

| 同輸入局部基準 | 修改前中位數 | 修改後中位數 | 證據邊界 |
|---|---:|---:|---|
| 20 個正常完整1MiB幀的模型輸入 | 57.39ms | 60.29ms | 新觀測有成本，沒有正常路徑加速證據 |
| 16 個未完成幀＋731包模型輸入 | 1089.61ms | 9.64ms | 兩版相同11680請求片、16 NACK；是 CPU 放大改善，不是協定違例證明 |
| 1000包、每包363個重疊合法NACK範圍 | 665130µs | 3256µs | duplicatePending同為265352269；不是網路吞吐試驗 |

原始 Release A0-closed 短測：500幀、unsentExpired/deadlineMisses/sendErrors均0；原始 A1：500/500完成、NACK0、deadlineMisses1。P0 最小修復＋原始模型的 A1 同樣500/500、NACK0、deadlineMisses1。原檔在基線目錄的 `performance/` 與 `p0-performance/`。

原始接收器不支援相同 checkpoint 與新增 profile，不能將上述原始 A1 和新版整套儀表的 CPU 差異當成純演算法提升。A0-drain 前後使用同一 discard 實作和日誌／檢查點設定；短輪仍存在時間差與主機排程變異，沒有足夠證據宣稱端到端穩定提速。

初次修改後七情境10秒短測保留於 `20261116-110739-f2a6a8b6/`；完整驗收補算另存 `reviewed-acceptance.json`，沒有覆蓋原摘要：

| 情境 | built／complete／expired | NACK | unsentExpired | 舊 deadlineMisses | 結果 |
|---|---:|---:|---:|---:|---|
| A0-closed | 500／不驗證／不驗證 | 0 | 0 | 0 | 本地目標通過，無接收成功結論 |
| A0-drain | 500／不驗證／不驗證 | 0 | 898 | 15 | 時序失敗 |
| A1 | 500／500／0 | 4 | 128 | 9 | 接收完整，時序失敗 |
| B64 首輪丟片2 | 500／500／0 | 500 | 0 | 0 | 恢復成功 |
| B1 首輪丟片2 | 500／500／0 | 500 | 0 | 3 | 恢復成功，時序失敗 |
| C 持續丟片2 | 500／0／500 | 1500 | 0 | 6 | 每片3次後作廢、新幀繼續；時序失敗 |
| CRC 首輪損壞4 | 500／500／0 | 503 | 0 | 6 | 恢復成功，時序失敗 |

以上所有 sendErrors=0、cacheMisses=0。這不等於每一片首發已送出；A1 的未首發片由後續 NACK 補回，清單對帳才證明收齊。新版完整時序驗收另核對 `actualInitialDeadlineMisses`，缺少該欄位的舊報告標示不可驗證。

## GUI 與共用 Qt CLI

實際操作 Release GUI：設定500次採樣、1MiB預設、啟動、等待本地結束、透過「導出本地報告」保存 JSON，並關閉本次啟動的視窗。沒有繞過 TransferController/SessionManager/CommandDispatcher。

第一次 `gui-release-20261116/` 的接收器45秒限時包括人工操作時間，早於發送完成，故保留為編排失敗證據，不能驗收。
第二次 `gui-release-repeat-20261116/` 同一 GUI 程序／本地 run_id，獨立接收器和目錄，收齊500/500，NACK0、unsentExpired0、新舊deadline0、sendErrors0。兩次的 logic_id 區間不同，不合併成功率。120秒接收器生命期只是操作餘裕，發送仍為10秒500幀；協定規則沒有放寬。

CLI 八種功能情境為16KiB、15幀，僅作功能驗證，不能與1MiB性能直接比較。Debug 首輪7/8的 normal 失敗保存於 `network-windows-debug/e2e-2d004c18c6d0/`，時間線分析在 `debug-e2e-lifetime-analysis.json`；它準確報出了13個全幀缺失，並未把本地 send 成功誤報成收齊。

## 指標與不變協定

詳細欄位口徑見 [發送端說明](SENDER_DIAGNOSTICS.md) 及 [接收端說明](RECEIVER_DIAGNOSTICS.md)。

- `report_schema_version=2`。本地 run_id、PID、來源／執行檔雜湊、dirty狀態、建置、Qt/Python版本、端點、socket緩衝區、fault設定、起止與退出原因均在報告或同目錄 environment.json；本地時間均不標稱 NIC 到達時間。
- `lastInitialDurationUs` 和舊 deadlineMisses 保留原口徑。新分布只含所有首輪片皆被本地 socket 接受的幀，未完成、注入丟片和 send error 分開計數。所有觀測使用有界 histogram，分位數為100µs桶上界，max仍為實測值。
- `initialFragmentsPlanned = firstAttempts + unsentExpired + unsentAtStop + initialPendingFragments`。unsentExpired 是下次採樣替換時的未首發數，不是網路丟包。
- 分開採樣到最後首發完成、第一個 send 呼叫到最後完成、採樣間隔偏差、組幀與CRC耗時、原計劃遲到與動態pacer遲到。`samplingActiveUs` 是最後／首次採樣差加一個名義週期；並非含啟動排空的牆鐘總時長。
- Receiver NACK budget 在生成時消耗；sendto失敗不重置預算。生成／嘗試／本地接受分開計數。未知OSError非零退出並嘗試保存；quiet不關閉統計。trace有固定上限。
- DATA0x01/NACK0x02、IPv4、magic小端、32B頭、4B CRC、1472B上限、1436B步長、1..2MiB、模組升序及跨片、原TS/mask/payload不變、重傳bit0和重新CRC全部維持。
- 尾片2ms、逐片10ms／最多3次／第三次後10ms作廢、不要求第0片先到、同輪多NACK全部處理、DF、CRC和非法包拒絕維持。預設logic_id%3三槽；FIFO仍明確選用。
- 回繞 modulo 槽衝突未暗改；新接收回播視窗4096幀的有界拒絕策略明確揭露，活動舊幀例外。超過半個uint32空間、完整ID循環、重啟重用ID仍需雙端協調清除狀態，沒有加入session wire欄位。
- 無硬體同步、物理10Gbps、網卡時間戳、抓包DF位或Windows硬即時的驗收聲明。關閉ICMP錯誤不代表對端在線或NACK送達。

## 直接重現與匯出

```powershell
Set-Location D:\SimTransferRT
cmd /d /c scripts\build.cmd
cmd /d /c scripts\build.cmd Release
cmd /d /c scripts\run.cmd Release
cmd /d /c scripts\test-network.cmd Release
cmd /d /c scripts\test-network.cmd Debug
cmd /d /c scripts\test-diagnostics.cmd --suite short
cmd /d /c scripts\test-diagnostics.cmd --suite long
# 所有輪次獨立目錄，腳本會核對實際CMake建置類型
python tools\probe_windows_udp_reset.py --report artifacts\udp-diagnostics\windows-reset-new.json
# 匯出當前完整本地證據；使用新時間戳，不覆蓋舊壓縮檔
$destination = 'artifacts\udp-diagnostics-' + (Get-Date -Format yyyyMMdd-HHmmss) + '.zip'
Compress-Archive -Path artifacts\udp-diagnostics -DestinationPath $destination
```

腳本只對自身啟動的程序正常停止／超時清理，檢查並選用空閒埠，不結束無關程序。`--suite long` 為3輪A0-drain60秒與A1 60秒交替，再A1 120秒。時序或功能未達標時退出碼1並保存報告。

## 修改檔案與回退

Python：`udp_feedback_stub.py`、`runtime_support.py`、`receiver_model.py`、`run_diagnostics.py`、`run_e2e.py`、`probe_windows_udp_reset.py`、`benchmark_receiver_model.py`，以及新增 runtime／model／diagnostics 測試。

C++：`RealtimeStream.*`、`PacketPacer.h`、`Types.h`、新增`Diagnostics.h`；`StreamRunner.cpp`、`CommandDispatcher.*`、`SessionManager.cpp`；`IUdpTransport.h`、`QtUdpTransport.*`；`CliOptions.cpp`、兩種CLI入口；GUI僅調整診斷run_id及顯示口徑。未將網路與狀態機移入MainWindow。

建置／文件：`src/CMakeLists.txt`、`tests/CMakeLists.txt`、`tests/test_core.cpp`、`scripts/test-network.cmd`、新增`test-diagnostics.cmd`、README、本次3份技術／覆蓋文件及`.gitignore`的本地證據排除。Qt DLL環境在CTest目標與既有腳本內設定，不修改系統PATH。

不自動提交。基線zip保留開始時tracked內容；原先兩個untracked檔沒有變更。交付補丁另存本地證據目錄。若要回退，先保存後續個人修改，再對補丁執行 `git apply --reverse --check <補丁>`，確認成功才 `git apply --reverse <補丁>`；有衝突則逐檔三方合併，不能reset --hard或直接覆蓋後續修改。歷史與本次報告不應刪除。

## 最終構建、回歸與長測結果

詳細測試映射和未覆蓋邊界見 [回歸覆蓋表](UDP_REGRESSION_COVERAGE.md)。

| 已執行命令／方式 | 結果 | 退出碼與證據 |
|---|---|---|
| `scripts/build.cmd Release` | 構建成功、CTest3/3、核心55/55 | 0；`sender-build-release-delivery.log`、`sender-ctest-release-delivery-detail.log` |
| `scripts/build.cmd` | Debug構建成功、CTest3/3 | 0；`sender-build-debug-delivery.log` |
| 新終端直接CTest（不先手動加Qt PATH） | Release／Debug均3/3 | 0；`sender-ctest-*-delivery-detail.log`、`sender-ctest-clean-terminal.log` |
| `python -m unittest discover -s tools -p 'test_*.py'`（checkpoint修補後） | 85/85，含真實Windows與mock分開標示 | 0；`python-after-checkpoint-fix.log` |
| `scripts/test-network.cmd Release`（生命週期修補後） | 八情境8/8、每個預期ID有完整結局 | 0；`network-release-lifecycle-fixed.log`、`network-windows-release/e2e-3e658ac17699/summary.json` |
| `scripts/test-network.cmd Debug`（生命週期修補後） | 八情境8/8 | 0；`network-debug-lifecycle-fixed.log`、`network-windows-debug/e2e-498a0e2df788/summary.json` |
| 實際GUI按鈕操作及匯出 | 第二輪500/500，新舊deadline及unsent均0 | GUI退出碼未記錄，不捏造；雙方報告在`gui-release-repeat-20261116/` |
| `run_diagnostics.py --suite short` | 五個完整接收／故障案例功能通過；性能部分失敗 | 1；`short-after.log`、`20261116-110739-f2a6a8b6/` |
| 首套 `run_diagnostics.py --suite long` | 三輪drain、兩輪完整60s A1、一輪中止A1、120s A1全部留下證據 | 1；`long-after.log`、`20261116-112831-8b178533/` |

第一套長測的120秒A1收齊6000/6000，但unsentExpired563、舊／實測deadline27／22、首輪max18474µs、P9914800µs、取樣間隔偏差max4592µs／P99800µs；P99較好不代表最大值通過。兩輪完整60秒A1各收齊3000，首輪未發及超期均0，但最大週期間隔偏差仍為2843µs／1211µs，不能宣稱最大抖動小於1ms。

中止的60秒A1不是性能通過：705幀已建、697完整、1活動、7全幀缺失、停止時381片未首發。原因為可選checkpoint原子替換收到WinError5；完整錯誤報告仍保存。另在本機持有普通讀取handle時重現同錯誤，見 `checkpoint-reader-sharing/evidence.json`。不能確定原輪次的特定持有者。

修補後可選checkpoint失敗只計數及限頻告警，下一正常週期再試，不忙迴圈重試；最終必要報告不可寫仍非零。真實持有handle及模擬失敗的四項回歸通過。穩定性重跑另存獨立目錄，以下追加最終結果，不覆蓋第一套。

最終長測在 `20261116-113901-d2009c64/`，命令 `python tools/run_diagnostics.py --suite long`，套件退出碼 **1（時序未達標）**。全部收發子程序退出碼0，無強殺、無清理錯誤、無快取未命中，A1的預期幀清單全數完成：

| 輪次 | 秒 | built／complete | NACK | unsentExpired | 舊／實測deadline | sendErrors |
|---|---:|---:|---:|---:|---:|---:|
| A0-drain 1 | 60 | 3000／不驗證 | 0 | 0 | 5／5 | 0 |
| A1 1 | 60 | 3000／3000 | 1 | 251 | 1／0 | 0 |
| A0-drain 2 | 60 | 3000／不驗證 | 0 | 25 | 3／4 | 0 |
| A1 2 | 60 | 3000／3000 | 1 | 6 | 1／0 | 0 |
| A0-drain 3 | 60 | 3000／不驗證 | 0 | 60 | 1／2 | 0 |
| A1 3 | 60 | 3000／3000 | 3 | 21 | 4／1 | 0 |
| A1 穩定性 | 120 | 6000／6000 | 4 | 764 | 5／1 | 0 |

120秒中有5996幀完成全部本地首輪提交、4幀首輪未完成，因此首輪分布分母為5996，不能寫成6000個成功首發。採樣至最後首發完成max15160µs、P5014700µs、P9514700µs、P9914800µs；第一個send至最後完成max14777µs、P9914400µs。5999個採樣間隔的最大值31246µs，絕對20ms偏差max11614µs、P95600µs、P99900µs。

接收端重組位元組6291456000（超過4GiB，無32位溢出），764片補回、0作廢／全幀缺失／在途。接收任務最大遲到5422µs，最大迴圈間隔約19252µs；recv/send10054均0，實際socket選項成功。A1第1與第3個60秒輪次各發生1次checkpoint失敗，皆繼續到3000/3000並正常保存最終報告，證明此錯誤不再致命。

剩餘瓶頸未被掩蓋：A0-drain沒有CRC／重組／NACK，仍有超期，故不能將全部違約歸因於Python模型。完整接收依靠有限補發仍可恢復，但首輪與最大週期間隔目標未達；需更細的Windows排程／socket路徑量測才能確定下一步改善。沒有以減少負載、放寬協定、擴大快取、降低50Hz或整幀突發換取通過。

所有新增檔及tracked差異保存在 `task.patch`，`rollback-check.log` 記錄逆向檢查，`delivery-manifest.json` 記錄完整檔案清單與SHA256。可下載本地 `artifacts/udp-diagnostics-delivery-20261116-final.zip`，包含修改檔、原始來源zip、原始與新報告及日誌，未加入使用者的其他未追蹤檔。

最後再將性能編排器的接收器生命週期改為發送完成後2秒才正常停止，避免冷啟動／送端延後讓接收器提早退出；舊版接收器基線因不支援相同停止處理，保留有限時長並明示限制。長測執行檔和接收模型沒有因此改動。
新增兩項生命週期回歸後，**最終Python87/87通過，退出碼0**（`python-final-lifecycle.log`）。最終編排器再跑10秒A1，雙方退出0、500/500完成、NACK／未首發／新舊超期／sendErrors均0，沒有強殺；報告在 `20261116-114805-9c49ed71/A1-0-59a912b326e2/`。這一輪不能用來覆蓋長測的違約結果。
