# FOC 快车道与 RTOS 后台域任务结构设计

- 日期：2026-09-23
- 分支：`feat/rtos-foundation`（接 FreeRTOS 骨架与根因修复之后）
- 状态：设计定稿，进入实施
- 参考：`feat/freertos-test-tasks` 的步骤函数拆分与单一所有者纪律；
  `2026-09-22-freertos-foc-fastlane-design.md` 的硬实时/后台域隔离契约

---

## 1. 目标

把 bring-up 测试代码规整到**可推进 FOC 算法引入**的状态：

1. **FOC 核心强实时**（采样 / 换算 / 保护 / 控制输出）→ **25kHz 快速通道**，
   硬约束：无 RTOS API、无 printf、无动态分配、无等待。
2. **测试、通讯、回报** → **全部走 RTOS 任务调度**。
3. 主循环快车道内**零调试代码**——`app_run()` 中的 UART/CAN/Terminal/LED/统计全部迁出。

---

## 2. 任务结构

```
main()
  board_init()
  intf_clock_init()            ← 移至调度器前（对齐 SDK 惯例 + freertos-test-tasks）
  app_application_start()
    ├─ app_debug_rtt_start_task()   → rtt_log（prio 4，队列驱动）
    ├─ xTaskCreate(app_io_task)     → prio 3
    └─ vTaskStartScheduler()

app_io_task (prio 3)
  app_init()                   ← 一次性初始化（电流零点标定依赖 ISR，须调度器后）
  xTaskCreate(app_fast_task)   → prio 2
  xTaskCreate(app_diag_task)   → prio 3
  for(;;) { app_io_step(); vTaskDelay(1); }

app_fast_task (prio 2)
  for(;;) {                    ← 25kHz mcycle 忙等超循环（精确节拍）
    app_fast_step();           ← FOC 快车道五件套
    忙等对齐 next += loop_cycles;
  }

app_diag_task (prio 3)
  for(;;) { app_diag_step(); vTaskDelay(1000); }

rtt_log (prio 4)
  for(;;) { 队列阻塞 → SEGGER RTT 写出 + 1s 心跳 }
```

### 2.1 优先级与调度可行性

| 任务 | prio | 阻塞方式 | CPU 份额 |
| :--- | :--- | :--- | :--- |
| `rtt_log` | 4（最高） | 队列阻塞 | 仅日志到达时短占用 |
| `app_io` | 3 | `vTaskDelay(1)` | 每 1ms 短占用（≤200µs 约束不变） |
| `app_diag` | 3 | `vTaskDelay(1000)` | 每 1s 轻占用 |
| `app_fast` | 2 | 忙等（不阻塞） | 其余全部（25kHz 节拍） |
| idle / timer | 0 / 31 | — | — |

**关键机制**：`app_fast` 忙等不 yield，只会饿死**同/低**优先级；`app_io`/`app_diag`
用 `vTaskDelay` 阻塞后由 **tick（1kHz）唤醒并抢占** `app_fast`（3 > 2）。
故形成「快车道独占 CPU + 慢任务按需短抢占」结构——正是 FOC 期望形态。
`rtt_log`（prio 4）阻塞在队列上，无日志零打扰。

**代价**：`app_io` 每 1ms 抢占 `app_fast` 约 200µs（≈5 个 25kHz 节拍），
`g_enc_loop_late_us` 会体现——列入测试断言监控。

### 2.2 快车道内容（`app_fast_step`，25kHz）

| 调用 | 语义（FOC 视角） |
| :--- | :--- |
| `app_debug_encoder_sample()` | 转子/出轴角度采样（FOC 反馈；将来移入 FOC 模块时改名） |
| `app_analog_signal_process()` | 三相电流/母线电压换算 + 滤波（FOC 反馈） |
| `app_fault_process()` | 三相电流 RMS 累加 + L2 保护判断（FOC 保护） |
| `app_debug_motor_run_once()` | 控制输出（开环 V/F；**将来由 FOC 控制环原位替换**） |

**硬约束**（FOC 引入前提）：无 RTOS API / 无 printf / 无动态分配 / 无等待。
本 step 内 5 个调用全部满足（已逐函数核对）。

### 2.3 IO 内容（`app_io_step`，1ms）

| 调用 | 语义 |
| :--- | :--- |
| `app_adc_slow_process()` | ADC1 慢通道采样（VBUS/NTC/CANID 原始码生产者） |
| `app_debug_adc_update()` | Ozone 观测变量刷新（`.noncacheable`，1kHz 刷新） |
| `app_debug_uart_run_once()` | UART0 单字符调试 |
| `app_debug_can_run_once()` | MCAN3 自检 |
| `app_terminal_run_once()` | USB Terminal（通讯） |

### 2.4 诊断内容（`app_diag_step`，1s，回报）

| 调用 | 语义 |
| :--- | :--- |
| `app_gpio_toggle(PIN_LED_STATUS)` | LED 心跳 |
| `app_debug_encoder_print_stats()` | 编码器统计汇总（printf） |

---

## 3. 并发面与数据所有权

| 数据 | 写者 | 读者 | 安全性判定 |
| :--- | :--- | :--- | :--- |
| `s_raw_codes[]` / `s_valid_mask` | `app_io`（adc_slow） | `app_fast`（analog_signal） | **单写者单读者**；`uint16` 对齐写原子；`s_valid_mask \|=` 无并发写者 → 安全 |
| `s_phys_values[]` | `app_fast`（analog_signal） | `app_fast`（fault）；`app_io`（debug_adc_update 供 Ozone） | 同任务安全；跨任务读**容忍撕裂**（纯观测，不参与控制） |
| `s_zero_volts[]` | `app_io`（Terminal calibrate 命令） | `app_fast`（channel_to_physical） | 单元素 float 对齐写原子；标定有**停机窗口联锁**（`app_debug_motor_is_running`）；标定完成即新零点生效，语义正确 |
| `s_calib`（标定上下文） | `app_io` | `app_io` | 同任务安全 |
| `s_fault_ctx` | `app_fast`（fault_process）+ ADC ISR（app_fault_tick） | — | **ISR 与任务并发**（原有裸机形态，非本次引入；遗留项，见 §6） |
| `g_enc_*` / `g_adc_*` 观测变量 | `app_fast` / `app_io` | `app_diag` / Ozone | `volatile` 单标量，容忍撕裂（纯观测） |

**单一所有者纪律**：所有**控制状态变更**（编码器 SPI、滤波器、RMS、V/F 输出）集中在
`app_fast`；所有**通讯/参数/标定**变更集中在 `app_io`。二者通过上述明确的生产者-消费者
数据面交换，无锁。

---

## 4. `intf_clock_init()` 位置修订

**原状**：`app_init()` 内（调度器启动后）执行——偏离 SDK 惯例（`board_init()` 含
`board_init_clock()`，全部在调度器前）。

**修订**：移到 `main()`、`app_application_start()` 之前。理由：
1. 对齐 SDK 惯例（57/57 示例的 board_init 含时钟初始化，均在 `vTaskStartScheduler` 前）；
2. `freertos-test-tasks` 分支已做过同样修正并注明"保证 MCHTMR tick 基频正确"；
3. 本工程 `intf_clock_init()` 动 `clock_mchtmr0`（`drv_clock.c:32/72`），而 FreeRTOS tick
   恰用 MCHTMR——在 tick 已启动后重配时钟树是风险窗口；
4. 消除 §6 遗留待验证项之一。

**代价**：`boot:`/`params:` 打印（在 `intf_clock_init` 之前）时 CPU 仍在默认时钟——
但这两行只依赖 mcycle/printf，不依赖 480MHz；`clock: cpu=...` 行仍在 `app_init` 内打印
（此时时钟已就绪），语义不变。

---

## 5. 内存预算

| 项 | 栈 | 落点 |
| :--- | :--- | :--- |
| `app_fast` | 2048 words = 8KB | ucHeap |
| `app_io` | 1536 words = 6KB | ucHeap |
| `app_diag` | 512 words = 2KB | ucHeap |
| `rtt_log` | 512 words = 2KB | ucHeap |
| idle / timer | 256w×2 = 2KB | ucHeap（静态） |
| 日志队列 | 8×256B = 2KB | `.bss` |
| **ucHeap 合计** | **≈20.5KB** | `configTOTAL_HEAP_SIZE` **16KB → 24KB** |

DLM 用量 92.4KB（72.64%）+8KB ≈ 100.4KB（≈79%），仍安全。

---

## 6. 遗留项（本次不做）

- **`app_fault_tick`（ADC ISR 回调）与 `app_fault_process` 共享 `s_fault_ctx`**：
  裸机时代既有形态（ISR 1kHz + 主循环 25kHz 并发），非本次引入。
  FOC 引入前需审计保护状态机的并发正确性。
- 25kHz 硬触发快车道（ADC PMT ISR 直跑 FOC）——留给 FOC 阶段。
- 中断优先级重排与 `configMAX_SYSCALL_INTERRUPT_PRIORITY` 评估。
- 快车道函数去 `debug_` 前缀改名（`app_encoder_sample` 等）——随 FOC 模块化一并做。

---

## 7. 测试断言

见交付说明「测试步骤」。核心：启动全程完整、`tick_selfcheck` PASS、`rtt_hb` 稳定、
UART/Terminal 可用、电机控制正常、`g_enc_loop_late_us` 不显著恶化、`g_rtos_exc_*` 全 0。
