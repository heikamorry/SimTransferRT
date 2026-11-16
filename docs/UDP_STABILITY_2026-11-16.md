# 第二輪：發送穩定性與相容性驗證

本輪增量修改第一輪成果，保留 DATA/NACK、CRC、20ms 採樣、15ms 首發目標、正常 20µs 片間隔、三槽 cache、重傳規則及三個管理類。沒有提高優先級、綁核或修改全域網路／電源設定。Windows 回環不代表物理 10Gbps 或硬即時保證。

## 基線與改動

修改前來源、SHA256、未提交差異和 Release 可執行檔保存在 `artifacts/udp-stability/baseline-20261116-123439/`。`source-before.zip` 包含第一輪未提交成果，不是 Git HEAD 舊版。沒有刪除歷史報告或覆蓋前次交付包。

保存的舊執行檔重測位於 `artifacts/udp-stability/baseline-runs/20261116-123538-4697a910/`。120秒 A1：6000/6000 完整、3幀首發未完成、237片未首發過期、實測首輪超期104次、完整首發最大19002µs、最大採樣週期偏差1822µs。60秒 A1 其中一輪有1幀最終作廢。失敗保留，不能拿第一輪較好看的數字代替本輪前測。

- **詳細快照避開首發。** 到期後須首發完成、距下採樣至少1000µs才發布詳細內容；最多延期100ms後給輕量統計。實際發布後再計100ms週期。這是軟排程，OS停頓仍可能超出；最終快照始終完整。新增詳細／輕量／延期次數及最長延期，舊計數不改。
- **固定分片狀態。** 三個固定 metadata 槽和 enum 取代逐包字串更新、每幀 map 配置；詳細快照才生成原有文字。metadata依實際cache駐留更新，保留modulo/FIFO回繞、mask、TS、故障及補發語義。未改payload所有權。
- **短鎖郵箱。** SessionManager鎖外建立不可變snapshot，鎖內只交換shared_ptr；UI持有引用後在鎖外消費。socket仍由原worker建立／使用／銷毀。歷史摘要不保留大事件窗口。
- **輕量心跳。** `detailsIncluded=false`時省略frames/cacheMissEvents，統計保留。GUI保留上次分片表並提示等待刷新；JSON以`details_included=false`明示。新會話及最終完整快照仍清表／更新。
- **停止兼容。** 原生CLI signal透過命令階段的pollControl入隊，不依賴延期的快照。GUI/Qt CLI控制流程不變，Stop仍優先於滿普通佇列。

原`runStream`五參數和`snapshot(now)`來源呼叫繼續有效；新增RunnerHooks供確定性時鐘／可選控制輪詢。報告schema 2保留原鍵，新增欄位是附加資料，接收器不用理解它們。本輪需完整重建，沒有聲稱二進位ABI兼容。

## 增量構建兼容性修復

Debug首次增量連結失敗暴露了原構建目錄的缺失依賴：MSVC中文showIncludes前綴與CMake記錄不一致，舊`compiler_depend.make`只有.cpp、沒有RealtimeStream.h等頭檔，因此消費者仍引用舊函式簽名。本機沒有英文clui資源，未用安裝語言包或SDK解決。

Windows NMake preset改用`CMAKE_DEPENDS_USE_COMPILER=FALSE`，Release繼承，Ninja/core-only不改。這讓CMake自身掃描頭檔，避開本地化輸出解析；該變數自3.20提供，符合專案>=3.21。[CMake官方文件](https://cmake.org/cmake/help/latest/variable/CMAKE_DEPENDS_USE_COMPILER.html)

已做一次Debug/Release clean-first重建。之後只改Types.h時間戳、不改內容，再次構建證明test_core.cpp.obj實際重編譯，depend.make包含Types.h和RealtimeStream.h；證據為`header-incremental-check.json/.log`。失敗的初輪構建日誌保留。為減少舊增量產物的不確定性，修改前來源亦在隔離fresh-build目錄完整重建Qt CLI及核心／應用測試，再跑一輪新的120秒基線，不覆蓋原執行檔前測。

## 異常時間線

預設開啟；CLI/probe的`--timing-trace on|off`可做成本對照，不上線傳輸。啟動時配置固定儲存，記錄時不配置、不加鎖、不寫檔；發送迴圈結束後才匯出。

最終`timing_trace.version=2`為低成本篩選：幀／組幀／首發邊界及接收／命令／發布邊界保留；逐包Encode/Send僅耗時≥100µs時成對記錄原始時刻，等待僅未來喚醒遲到≥100µs時記錄。所有profiles仍包含全部操作。`total_events`是篩選後記錄數，沒有事件不代表沒有執行，窗口邊界也可能截斷事件對。初輪version 1完整逐包追蹤及其WaitLateness哨兵誤報結果保留，但不當作最終實作結果。

每窗最多512個觸發前事件（含觸發時已記錄事件）、128個後續事件，最多前8個不重疊窗口。這是事件數窗口，不是固定毫秒窗口。所有觸發仍計數；重疊／容量用盡列入`triggers_not_captured`，不能聲稱整段每次異常都保留事件。`rolling_events_overwritten`是正常滾動覆蓋，與漏捕不同。停止時不足後窗標`post_window_complete=false`。活躍快照不複製事件，`exported=false`不代表沒有異常；強殺／崩潰可能丟失尚未匯出的記錄。

觸發：採樣間隔絕對偏差≥1000µs；完成首輪跨度>15000µs；首發被取消／替換；等待返回晚於計畫喚醒≥1000µs。`at_us`是主機單調時鐘（Windows QPC換算），加上worker_thread_id及既有PID供外部追蹤，不是網卡時戳或跨機同步時鐘。

| 事件 | value含義 |
|---|---|
| Sample | 跳過過載週期前的計畫採樣時刻 |
| WaitBegin/WaitEnd | 計畫喚醒時刻 |
| InitialBegin/InitialEnd | 原採樣TS／採樣到最後首發嘗試結束跨度 |
| InitialAbandoned | 未首發片數 |
| EncodeBegin/End、SendBegin/End | 片號 |
| PublishBegin | 1詳細／0輕量 |
| ReceiveEnd | 收到byte數 |
| CommandsEnd/PublishEnd | 耗時µs |

先由sequence定位trigger，再配對Begin/End。WaitLateness僅在進入等待時deadline仍在未來才觸發；已到期任務／重傳的0哨兵不會誤報成巨大等待。它仍是使用者態觀測，不能直接歸因OS。階段耗時同樣包含執行期間被搶占的時間。進一步可用ETW/WPA分辨Running/Ready/Waiting、context switch和DPC/ISR，見[微軟CPU分析](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/cpu-analysis)、[WPR命令](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/wpr-command-line-options)。

## 驗收與重現

歷史timing_target_pass含義不改；acceptance_schema_version=3的整體case_pass額外要求全sampled−1個間隔的最大偏差嚴格<1000µs，以及全部built幀完成首輪槽位、沒有未完成／停止未發／在途未發。無故意drop案例還要求完整本地提交；故意drop案例保留故障語義。缺少證據為unverified，不冒充通過。完整接收仍靠雙方ID清單對帳。

新增P99.9，沿用100µs桶上界，max精確。編排器每秒外部讀Windows工作集／私有提交byte，只存首次、末次活躍觀測、峰值，空間固定；探測失敗非致命。這些觀測不能單獨證明無洩漏，也不是退出瞬間的精確值。

```powershell
cmd /d /c scripts\build.cmd Release
cmd /d /c scripts\build.cmd
cmd /d /c scripts\test-network.cmd Release
cmd /d /c scripts\test-network.cmd Debug
python tools\run_diagnostics.py --suite short --out artifacts\udp-stability\short
python tools\run_diagnostics.py --suite long --out artifacts\udp-stability\long
python tools\run_diagnostics.py --suite short --cases A1 --seconds 1800 --rounds 1 --out artifacts\udp-stability\soak
python tools\run_diagnostics.py --cases A1 --seconds 60 --timing-trace on --out artifacts\udp-stability\trace-on
python tools\run_diagnostics.py --cases A1 --seconds 60 --timing-trace off --out artifacts\udp-stability\trace-off
```

30分鐘使用short的自訂排程；long保持60s×6+120s固定排程。8–24小時單次會超過現有百萬幀對帳上限（50Hz約5小時33分）；本輪不擅自擴大接收器資源，更長驗收須先設計有界分段核對。

## 實測結果

回歸與性能分開判定，虛擬時鐘測試不代表Windows實時達標。性能測試期間沒有並行編譯或其他UDP測試，但桌面背景負載沒有被強制關閉，不能把跨時段差異全部歸因於程式。

| 階段 | 時長 | A1完整／built | 首發未完成幀／未首發片 | 實測首發超期 | 最大完整首發／採樣間隔偏差 |
|---|---:|---:|---:|---:|---:|
| 原保存執行檔基線 | 120s | 6000/6000 | 3/237 | 104 | 19002/1822µs |
| 修改前來源隔離重建基線 | 120s | 5996/6000 | 19/4532 | 313 | 19936/37921µs |
| 初版逐包trace（保留失敗） | 10s | 499/500 | 2/319 | 40 | 18980/5130µs |
| 初版trace關閉對照（保留失敗） | 60s | 2968/3000 | 見原報告/9094 | 502 | 見原報告/22584µs |
| 最終低成本trace短測 | 10s | 500/500 | 0/0 | 8 | 17990/1910µs |
| 最終低成本trace長測 | 1800s | 89996/90000 | 63/8566 | 1505 | 19992/11920µs |

隔離重建基線為`fresh-baseline-runs/20261116-125904-602af741/`；初版為`after-short/20261116-130217-ff6f948c/`和`trace-off-initial/`；最終短測為`filtered-short/20261116-131348-83999cd8/`（以上相對artifacts/udp-stability）。關閉trace的那輪仍明顯失敗，不能將所有停頓歸因於trace，也不能宣稱跨時段表格已證明端到端提速。

最終短測全部7情境皆保留：A1/B64/B1/CRC均500/500完整；持續丟片C為500幀按預期作廢且新幀繼續；A0不作完整接收結論。仍存在時序失敗。獨立審查`independent-filtered-short-review-v2.json`的7/7指的是守恆、樣本群體、trace容量／順序／篩選規則、最終完整快照及原JSON鍵兼容，**不是實時達標**。

同輸入微基準位於`micro/run-20261116-131951-6b705e67/`。同一bench編譯三個入口，3輪輪換執行順序；每輪1000幀×1MiB×731片，虛擬20ms/20µs，9次包數、byte數、CRC尾欄位滾動校驗和關鍵統計全部一致。

| 核心微基準中位數 | 熱循環wall time | 程序CPU時間 |
|---|---:|---:|
| 修改前 | 1031883µs | 1000000µs |
| 修改後trace off | 961595µs | 906250µs |
| 修改後trace on | 973736µs | 953125µs |

trace on的核心wall time中位數下降約5.6%，off約6.8%。此基準不含socket、StreamRunner收命令／等待／快照；虛擬時鐘也不產生慢操作或異常窗口，因此只能支持正常核心工作成本下降，不能推算UDP吞吐或異常採集總成本。

`filtered-trace-analysis.json`配對了5個真實窗口：A1曾有計畫1000µs、實等4214µs的等待；另有868µs及709µs的send跨度與同幀首發超期對應。send跨度可能包含被搶占時間，尚無OS根因結論。最大17.990ms首發缺少開頭2.280ms事件；B64最大68.085ms週期偏差也沒有被保留窗口定位，不能倒用附近正常等待作原因。

681次詳細發布只在已捕獲窗口看到17個唯一事件：16個能驗證首發已完成且起始採樣餘量≥1ms，1個因前文不完整無法判斷，沒有已證違規。這不是全程發布安全的完整追蹤證明。

ETW的8MiB自訂profile `SimTransferScheduling.wprp`已由本機WPR只讀解析驗證，沒有啟動recording。本機未提升且未有SeSystemProfilePrivilege，未自動提權或安裝WPA/SDK；因此沒有內核排程/DPC的實測因果結論。`etw-profile-validation.log`保留配置和權限前提。

Debug/Release最終低成本版本均CTest6/6；包含原核心55項、新增核心14項、runner7項、Qt應用、輕量表格／JSON及GUI smoke。日誌為`build-filtered-release.log`、`build-filtered-debug.log`。最終Release、Debug網路回歸分別為`artifacts/udp-diagnostics/network-windows-release/e2e-2636f349ca5e/`、`artifacts/udp-diagnostics/network-windows-debug/e2e-9189c9f6b1b1/`，各8/8通過，各自前置100項Python測試亦通過。情境覆蓋正常、首片丟失、CRC損壞、重複、逆序、重複NACK、持續丟失、ID回繞；功能通過不代表1MiB/50Hz實時驗收通過。

原生CLI另做實際Windows Ctrl+Break停止驗證，見`native-stop-37569421/result.json`：signal至正常退出5588µs、exit 0、沒有強殺，最終完整報告與trace均保存，325片停止未發有明確計帳且分片守恆成立。該測試使用關閉的回環端口，只驗證原生停止生命週期；虛擬時鐘測試另覆蓋在任何快照發布前停止。

## 30分鐘長測結論

最終Release A1以1MiB、50Hz連續運行1800秒，位於`artifacts/udp-stability/soak-final/20261116-132057-1a2c695c/A1-0-b92d29dc7b8e/`。雙方正常exit 0，沒有強殺或清理錯誤；編排器整體exit 1正確反映性能驗收失敗。運行記錄的Release程式SHA256與最終檔案相同。

| 長測指標 | 結果 |
|---|---:|
| 採樣／built／跳過過載週期 | 90000／90000／0 |
| 完整接收／作廢／完全未見／仍活躍 | 89996／4／0／0 |
| 完整首發／首發未完成 | 89937／63 |
| 實測首發超期／歷史舊口徑超期 | 1505／1433 |
| 完整首發跨度 P99／P99.9／max | 15200／16900／19992µs |
| 89999個採樣間隔絕對偏差 P99／P99.9／max | 800／1900／11920µs |
| 未首發過期／停止未發／在途未發片 | 8566／0／0 |
| NACK封包／實際重傳片／同片最多請求 | 132／22852／3 |
| 發送錯誤／cache miss／命令佇列丟棄 | 0／0／0 |
| 執行中詳細／輕量快照／最長延期 | 17316／0／58117µs |
| 詳細發布單次最大耗時 | 1472µs |

執行中快照計數不包含退出時強制發布的最終完整快照。最長延期低於100ms並不代表發布本身沒有成本；本輪最大發布耗時仍超過1ms採樣餘量，因此軟排程沒有硬時限保證。

獨立審查`independent-soak-final-review.json`完成110項通過、1項身份歸因未知、0項核對失敗，另11項長測補充核對通過。作廢ID為28711、37889、37890、38585；缺少逐幀完整首發身份清單，不能把4幀作廢直接歸因於63幀首發未完成。請求守恆為28296＝5416重複＋22852實際重傳＋28取消。接收端模組、元資料、CRC、幾何及資源淘汰錯誤皆為0。**帳目一致和99.9956%的最終完整率，不等於實時達標：15ms首發與嚴格小於1ms採樣偏差目標均未通過。**

trace共3333次觸發，僅保留前8個窗口，3325次未另保留窗口；其中採樣偏差391次、首發超期1505次、首發未完成63次、等待遲到1374次。各原因可能描述同一停頓，不應加總為獨立事故數。它提供可追查的片段，無法證明最後或最大異常的OS原因。

外部記憶體觀測：sender共1751次、receiver共1754次，讀取失敗均0。sender末次／峰值私有提交為8,224,768／9,347,072 bytes，receiver為18,522,112／21,204,992 bytes；末次／峰值工作集分別為21,913,600／22,978,560及31,846,400／34,529,280 bytes。中途觀測沒有持續單向攀升的趨勢，但30分鐘與每秒抽樣不足以宣稱無洩漏；首次觀測還包含啟動階段，不可直接作穩態差值。

`soak-scheduler-observed.json`確認sender為Normal優先級、16個邏輯CPU全部可用，worker base/current priority均8。主機為Windows 11、Core Ultra 7 356H、平衡電源方案；沒有變更這些設定。未做物理雙機測試、2小時或8–24小時驗收，亦沒有ETW核心追蹤，下一步應把已知異常窗口與內核排程關聯，並在有界分段對帳就緒後擴大長測。本輪可證明核心正常工作成本下降、功能相容性通過；尚不能證明長期穩定實時達標。

## 交付與回退

交付zip包含最終來源、原始成功／失敗證據、獨立核對、微基準、重現工具及`stability-incremental.patch`。`delivery-manifest.json`列出來源SHA256與本輪變更清單；`stability-rollback-check.log`記錄`git apply --reverse --check`的只讀驗證。本輪沒有執行回退或Git提交。

補丁以本輪開始時的未提交來源為基線，反向套用只撤銷本輪穩定性改動，保留第一輪修復。日後先保存新修改，再執行`git apply --reverse --check artifacts/udp-stability/stability-incremental.patch`；核對通過後才按需反向套用。前次交付包和歷史`sim-rt-report-massive.json`保留不覆寫。
