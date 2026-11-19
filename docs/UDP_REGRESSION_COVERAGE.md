# UDP 回歸覆蓋對照

本表按本次任務的 15 類必要回歸逐項核對**實際測試源碼與已保存的結果**。模型測試使用虛擬單調時鐘；作業系統錯誤模擬、真實 Windows socket 實驗、Qt 應用測試、真實 UDP 與性能驗收是不同證據層級，不能互相冒充。

覆蓋審查在長時間性能對照期間只讀取源碼與既有結果，沒有同時啟動測試、UDP 或微型基準。第一組長測結束後，針對實際發現的 checkpoint 回歸修補並全量執行 Python，結果 [85/85、退出碼 0](../artifacts/udp-diagnostics/python-after-checkpoint-fix.log)。重跑長測的最後結果以主交付報告為準；下表不預先宣稱尚未結束的長測通過。

## 測試入口與證據

| 簡稱 | 源碼或入口 | 證據範圍 |
|---|---|---|
| P | [tools/test_protocol.py](../tools/test_protocol.py) | 原有 18 項 Python 協定／接收模型測試。修改前 [python-before.log](../artifacts/udp-diagnostics/baseline-20261116-105100/python-before.log) 為 18/18。 |
| M | [tools/test_receiver_model_regression.py](../tools/test_receiver_model_regression.py) | 24 項新增模型回歸；[receiver-model-after-tests.log](../artifacts/udp-diagnostics/baseline-20261116-105100/receiver-model-after-tests.log) 與後續總測試記錄。 |
| R | [tools/test_receiver_runtime_patch.py](../tools/test_receiver_runtime_patch.py) | 當前 [23 項 runtime 測試通過](../artifacts/udp-diagnostics/runtime-checkpoint-regression.log)，含 SIGBREAK、報告組裝失敗、checkpoint 分享衝突；P0 的 [runtime-tests.log](../artifacts/udp-diagnostics/p0-runtime-snapshot/runtime-tests.log) 仍保留早期 17 項記錄。 |
| D | [tools/test_diagnostics.py](../tools/test_diagnostics.py) | 編排器驗收與發送清單分母檢查；不是 UDP 性能測試。以最後全量測試記錄確認當前版本執行結果。 |
| C++ | [tests/test_core.cpp](../tests/test_core.cpp) | 測試名稱為 C++ 測試表中的字串；[sender-core-detail.log](../artifacts/udp-diagnostics/sender-core-detail.log) 記錄 54/54。 |
| Qt | [tests/test_application.cpp](../tests/test_application.cpp)／CTest `rt_application` | 真實 QUdpSocket、TransferController／SessionManager／CommandDispatcher、DATA/NACK 和本地生命週期；不繞過 Qt 層。 |
| GUI | CTest `rt_gui_smoke`／實際 Release GUI 操作 | smoke 只驗證啟動；實際 [GUI 收發報告](../artifacts/udp-diagnostics/gui-release-repeat-20261116/) 另驗證 500 幀。 |

CTest 實際執行證據：[Release](../artifacts/udp-diagnostics/sender-ctest-release-delivery-detail.log)、[Debug](../artifacts/udp-diagnostics/sender-ctest-debug-delivery-detail.log)。舊歷史日誌未被覆寫。

## 15 類必要回歸

| # | 任務情境 | 實際測試名稱／證據 | 能證明什麼與限制 |
|---:|---|---|---|
| 1 | recvfrom／sendto 模擬 10054、配置成功／失敗、持續運作並保存報告 | R `test_recv_reset_continues_and_reports_both_option_outcomes`、`test_failed_nacks_consume_only_three_requests`、`test_native_signature_success_failure_and_pointer_width`、`test_real_windows_wsaioctl`；[真實 Windows ICMP 實驗](../artifacts/udp-diagnostics/p0-runtime-snapshot/windows-icmp-experiment.json) | 錯誤分方向計數，設定失敗仍有防禦處理；失敗送出不造成無限重試。真實實驗確認預設 Winsock 收到 10054、FALSE 對照沒有 reset；模擬 sendto10054 不冒充實際 Winsock sendto 產生該錯誤。 |
| 2 | 10040、would-block、其他 OSError、bind 失敗、報告路徑不可寫、Ctrl+C、限時 | R `test_oversized_and_wouldblock_not_valid_data`、`test_send_wouldblock_and_message_size_are_bounded`、`test_unknown_receive_error_saved_nonzero`、`test_unknown_send_error_saved_nonzero`、`test_bind_failure_and_close_failure_preserve_primary_exception`、`test_report_directory_unwritable_is_error`、`test_keyboard_interrupt_and_duration_save`、`test_trace_failure_does_not_skip_report`、`test_atomic_replace_keeps_previous_valid_file_on_failure`、`test_report_generation_failure_preserves_primary_error`、`test_ctrl_break_handler_uses_interrupt_cleanup_path`；[真實 Ctrl+Break 報告](../artifacts/udp-diagnostics/p0-runtime-snapshot/ctrl-break-report.json) | 過長／截斷資料不作合法 DATA；未知錯誤非零退出並保留上下文，清理失敗不蓋掉原始異常。路徑測試使用缺失父目錄／PermissionError 模擬，沒有對使用者磁碟改 ACL 或填滿磁碟；實體鍵盤 Ctrl+C 由 KeyboardInterrupt 模擬，真實執行的是 CTRL_BREAK_EVENT。 |
| 3 | CRC 已知向量、小端 DATA／NACK 偏移、截斷與非法長度 | P `test_crc`、`test_data_offsets`、`test_data_corruption`、`test_nack_ranges`、`test_bad_ranges`；M `test_wire_boundaries_overlap_and_truncation`；C++ `CRC IEEE known vector`、`CRC fast path matches reference`、`DATA LE offsets`、`wrong length and magic`、`NACK invalid reserved and CRC`、`NACK range overflow` | `123456789 → CBF43926`；檢查關鍵偏移、重算 CRC 後仍非法的欄位、截斷 NACK、過長 DATA。不是所有可能位元錯誤的窮舉或完整 fuzzing。 |
| 4 | 模組體 1048571B／整幀 1MiB／731 片／末片 296B；2MiB 格式邊界 | M `test_full_1mib_module_body_geometry_and_retransmission`、`test_more_than_363_ranges_is_one_budget_per_fragment`；P `test_boundaries`；C++ `1MiB geometry`、`2MiB geometry`、`2MiB@1Gb15ms rejected unless explicit lab override` | 模組頭 5B 已納入；2MiB 測試明確是格式／容量邊界，不是 1Gbps 15ms 性能通過。 |
| 5 | 正常完整幀不產生無原因 NACK | M `test_complete_frame_no_nack_and_timers_cancelled`；[模型同輸入基準](../artifacts/udp-diagnostics/baseline-20261116-105100/receiver-model-comparison.json) 正常 20×1MiB；真實 E2E `normal` | 虛擬有序完整輸入為零 NACK，終態任務不再回饋。真實 A1 高負載偶發 NACK 需結合排程／缺片證據，不能因模型測試通過就斷言任何真實 NACK 都是錯誤。 |
| 6 | 缺片 2 立即、10ms、20ms 請求；30ms 作廢；無第四次；提前補回完成 | M `test_fragment_two_exact_three_requests_then_expiry`、`test_original_without_retry_flag_recovers_and_cancels_expiry`、`test_packet_after_final_deadline_without_poll_cannot_revive`；P `test_gap_immediate`、`test_three_requests_then_expire` | 精確邊界用虛擬時鐘，不以 Windows sleep 的實際誤差修改規則。第三次後到期但尚未 poll 的遲到片也不能復活。 |
| 7 | 多片不同時間發現，各自計時 | M `test_missing_fragments_have_independent_clocks`、`test_ingest_after_final_request_recovery_of_one_fragment_updates_expiry` | 每片各自的發現／10ms 輪次／到期時間保持獨立；較早缺片補回後，不能使用它已取消的到期時間作廢另一片。 |
| 8 | 同輪 >363 區間、多 NACK、重複／重疊區間、整數／非法範圍 | M `test_more_than_363_ranges_is_one_budget_per_fragment`、`test_wire_boundaries_overlap_and_truncation`；P `test_multi_nack_same_round`、`test_bad_ranges`；C++ `same round multiple NACK and pending dedup`、`overlapping NACK worst case bounded and later repeat still eligible`、`NACK max ranges`、`NACK range overflow`、`atomic NACK validation` | 2MiB 交錯缺片拆三包，一輪每片只扣一次；發送端重疊請求只合併待發，不永久壓制後續合法請求；整包非法時不部分執行。 |
| 9 | NACK 丟失、補片丟失、重複 NACK、遲到原片 flags=0 補回 | M `test_lost_nack_and_lost_repair_do_not_create_fourth_request`、`test_original_without_retry_flag_recovers_and_cancels_expiry`；R `test_failed_nacks_consume_only_three_requests`、`test_send_success_separate_from_generated`；C++ `later repeated requests not permanently suppressed`；真實 E2E `duplicate_nack`、`permanent_loss` | 丟棄所有請求／補片的有限性以模型驗證；真實測試含重複 NACK 和持續丟片。**未做獨立網路代理分別只丟 NACK、只丟重傳 DATA 的實體鏈路實驗**。 |
| 10 | 終態遲到、歷史淘汰後遲到、積壓、重複尾部，不重生／無限請求 | M `test_completed_history_eviction_never_reactivates`、`test_terminal_status_eviction_inside_replay_bitmap`、`test_expired_history_eviction_never_reactivates`、`test_pause_backlog_is_finite_and_does_not_catch_up_retries`、`test_duplicate_does_not_postpone_tail_or_register_again`、`test_resource_eviction_distinct_from_protocol_expiry`；[重生前後證據](../artifacts/udp-diagnostics/baseline-20261116-105100/receiver-resurrection-proof.json) | 同時覆蓋 256 終態摘要外、4096 replay 位圖內，以及滑出視窗；任務失效項有界。完全未曾觀察的視窗內舊幀仍可建立一次，不能簡單因舊 ID 拒絕；退役後位圖阻擋再次重建。 |
| 11 | uint32 回繞、三槽快取、未命中、FIFO 必須顯式 | M `test_wrap_safe_terminal_and_inflight_comparison`、`test_older_unseen_frame_and_active_beyond_replay_window_remain_valid`；C++ `three-slot cache exact ID`、`document modulo wrap caveat explicit`、`opt-in FIFO preserves three across wrap`、`expired cache NACK ignored`、`cache miss diagnostics bounded and wrap-safe`；真實 E2E `wrap` | 預設 modulo 在 FFFFFFFF→0 的碰撞仍由測試明確保留，沒有借 FIFO 宣稱修好原規則。完整 2³² 周期重用／半區間歧義不是本次長測覆蓋範圍。 |
| 12 | 重傳 flags／CRC 改變，TS／mask／payload 不變 | M `test_full_1mib_module_body_geometry_and_retransmission`；C++ `retry preserves fields recomputes CRC` | 逐位元組比對 flags 和尾 CRC 之外的內容，並重新解碼重傳。沒有為降低耗時更換採樣 TS。 |
| 13 | 元資訊衝突作廢、模組上下線、跨片、舊快取不隨新配置改變 | M `test_metadata_conflict_terminal_even_after_correct_packet`、`test_module_header_crosses_fragment_and_module_offline_next_frame`、`test_module_structure_failure_is_terminal_and_classified`；C++ `module hotplug snapshot immutable`、`modules cross fragment boundary and >64K`、`hotplug applies only next sampling frame`、`all-off skip explicit policy` | 跨片模組頭與資料、次幀 mask 變化、舊快取 mask 都有斷言。**接收元資訊衝突測試使用 TS 變化；frame_len／frag_cnt／mask 的逐欄位衝突沒有逐一參數化測試**，來源比較包含完整 tuple 但靜態閱讀不算動態覆蓋。 |
| 14 | NACK 洪泛與補發積壓，停止有效、新幀不被無限餓死 | C++ `feedback flood remains bounded, new samples and stop progress`、`overlapping NACK worst case bounded and later repeat still eligible`、`dispatcher bounded and stop priority`；真實 C 情境持續缺片 | 假 transport 持續提供合法最大區間 NACK，檢查新採樣、佇列上界和停止完成；普通命令隊列滿時 Stop 優先。**未執行外部進程在真實 UDP 上持續以最大速率洪泛 NACK 的壓力測試**，不可用此單元測試宣稱任何網路洪泛負載都滿足即時性。 |
| 15 | 單調時鐘、採樣／發送完成統計、取消／排空守恆 | M `test_clock_injection_and_backwards_clock_rejection`；C++ `actual send completion uses injected clock; TS and legacy span unchanged`、`planned send lateness includes encode time before socket call`、`sampling interval deviation is independent of absolute lateness`、`planned versus adjusted pacing lateness remains visible`、`clock reversal detected both between and during ticks`、`cancellation and replacement conserve planned first fragments`、`failed or injected first sends excluded from submitted distribution`、`frame limit drains without FIN`、`pacer no burst flush` | 送出後的真實時刻與 tick 入口分開；完成分布排除未完成／失敗幀；首發片數守恆與 Stop 冪等有斷言。**尚未建立涵蓋任意錯誤交錯的重傳佇列完整形式化守恆證明**；目前是有界案例回歸。 |

## 真實 UDP、Qt 與 GUI 證據

| 要求 | 實際結果與可檢查位置 | 邊界 |
|---|---|---|
| 正常、首輪丟片恢復、CRC 恢復、持續缺片作廢且新幀繼續 | Release [8 情境 summary](../artifacts/udp-diagnostics/network-windows-release/e2e-3e658ac17699/summary.json)、Debug [8 情境 summary](../artifacts/udp-diagnostics/network-windows-debug/e2e-498a0e2df788/summary.json) | 各 15 幀／16KiB 的功能測試，全量預期幀結局核對；不能作 1MiB 性能基線。兩種 build 各自保存結果，不混比。 |
| 64KiB 與 1MiB 首輪缺片 2 | [10s 矩陣](../artifacts/udp-diagnostics/20261116-110739-f2a6a8b6/summary.json) B64、B1 均 500/500 完成、各 500 補回 | 1MiB 存在實際首輪超期，功能恢復不等於時序通過。 |
| Sim 停止後測試樁存活、有限尾部、保存報告 | [C sender](../artifacts/udp-diagnostics/20261116-110739-f2a6a8b6/C-5-096f6dfed1b4/sender-report.json)、[C receiver](../artifacts/udp-diagnostics/20261116-110739-f2a6a8b6/C-5-096f6dfed1b4/receiver-report.json)、[最後 checkpoint](../artifacts/udp-diagnostics/20261116-110739-f2a6a8b6/C-5-096f6dfed1b4/receiver-status.json) | 接收器在發送端退出後再運作約 1.87s，500 幀作廢、1500 NACK、0 活動幀；停止後計數不再增長。不是任意斷電或 kill 都能保存的承諾。 |
| GUI 與共享應用層 CLI 對照 | [GUI 重測 sender](../artifacts/udp-diagnostics/gui-release-repeat-20261116/sender-report.json)、[GUI 重測 receiver](../artifacts/udp-diagnostics/gui-release-repeat-20261116/receiver-report.json)；CLI Release 矩陣 | GUI 接收端 365500 有效 DATA、500 完成幀、0 NACK、0 invalid；Qt CTest 另證明三管理類與 worker socket 生命週期。GUI 人工操作時間和 CLI 固定有限幀控制不同，僅作功能與同負載觀測對照。 |
| 真實 Windows 10054 | [Winsock 實驗](../artifacts/udp-diagnostics/p0-runtime-snapshot/windows-icmp-experiment.json) | 實際本機 Winsock 回環，不是 Linux 或純模擬；沒有原始使用者事件順序、實體鏈路封包擷取或跨機 RTT。 |
| 真實 2ms／10ms 時間精度 | 接收報告的 `max_timer_lateness_us`、`select_max_us`、`loop_gap_max_us` | **不宣稱達標**。例如 C 短測模型任務最大遲到 14243µs。虛擬時計規則正確不保證 Windows 排程無違約。 |

## 驗收分母和「不應通過」的回歸

實際長測另發現並保留一份 [checkpoint 原子替換 WinError 5 失敗報告](../artifacts/udp-diagnostics/20261116-112831-8b178533/A1-3-d9c320d6247a/summary.json)。補修新增 R `test_optional_checkpoint_failure_recovers_on_regular_cadence`、`test_persistent_optional_checkpoint_failure_is_bounded_not_fatal`、`test_final_report_replacement_failure_stays_fatal`、`test_real_windows_reader_cannot_abort_optional_checkpoint`；最後一項使用真實 Windows 檔案 handle 分享衝突，UDP 則仍是假 socket。只有可選進度檔的 OSError 可延後到下一正常週期，最終報告錯誤仍必須非零。詳見 [接收診斷文件](RECEIVER_DIAGNOSTICS.md)。

D 的 `test_missing_entire_frame_and_wrap_use_sender_denominator`、`test_zero_send_errors_cannot_hide_wholly_missing_frame`、`test_overlap_and_counter_conflicts_are_not_a_success`、`test_unrelated_run_completion_does_not_inflate_fraction`、`test_sender_manifest_must_agree_with_counters` 和 `test_wrong_run_and_failed_process_never_pass`，防止把零 sendErrors、其他 run 的完成數或接收端看到的幀當作完整成功率。

`test_actual_deadline_miss_blocks_timing_even_if_legacy_counter_zero` 和 `test_missing_new_timing_counter_is_unknown_not_pass` 防止以舊 tick 入口跨度掩蓋真正完成時刻的超期。`test_recovery_requires_both_sender_and_receiver_evidence`、`test_persistent_loss_requires_all_expected_frames_expired_with_three_requests`、`test_truncated_inventory_never_claims_exact_success` 和 `test_drain_and_closed_never_claim_protocol_acceptance` 將功能恢復、預期作廢、證據不足、純排水模式分開。

清單展開和輸出 ID 列表也有上界回歸：`test_inventory_expansion_is_bounded`、`test_large_missing_inventory_is_counted_but_list_is_capped`、`test_malformed_ranges_and_active_ids_are_rejected`。這些測試處理本地報告，不增加線上欄位。

## 本次未執行或不能聲稱已驗證的範圍

- 兩台真實有線設備、10Gbps 實體鏈路、網卡實際時間戳、封包擷取中的 DF 位，以及真實多模組硬體同步。
- 實際磁碟滿、電源中斷、不可恢復程序強殺後必定保存報告；這些情境本來就不能保證。
- 真實 UDP 最大速率 NACK 洪泛和獨立 NACK-only／repair-only 丟包代理；目前對應的是模型、假 transport 與持續丟片聯調。
- uint32 全週期跑完、跨越 2³¹ 不確定區間、重啟後舊包與新 run 同 ID 的完全辨識；現有協定沒有 session ID。
- 每一個 metadata 欄位衝突的參數化回歸、完整 fuzzing、所有取消／錯誤交錯的形式化驗證。

這些限制不代表已執行測試失敗，也不把缺少證據記為通過。性能目標的任何未達標必須保留原始報告與違約數。
