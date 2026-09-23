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

## 2. 任务结构（2026-09-23 修订：控制环路移入 ADC PMT 中断）

```
main()
  board_init()
  intf_clock_init()            ← 调度器前（对齐 SDK 惯例 + freertos-test-tasks）
  app_application_start()
    ├─ app_debug_rtt_start_task()   → rtt_log（prio 4，队列驱动）
    ├─ xTaskCreate(app_io_task)     → prio 3
    └─ vTaskStartScheduler()

ADC PMT 完成中断（25kHz 硬件触发，PLIC prio 3 = 最高）
  isr_adc0 → adc_generic_isr → adc_current_pmt_cb（收相电流）
           → fast_cb = app_fast_step_hook
             └─ app_fast_step()   ← FOC 快车道五件套（40µs 周期内 ~20-25µs）

app_io_task (prio 3)
  app_init()                   ← 一次性初始化（电流零点标定依赖 ISR，须调度器后）
  xTaskCreate(app_diag_task)   → prio 3
  for(;;) { app_io_step(); vTaskDelay(1); }

app_diag_task (prio 3)
  for(;;) { app_diag_step(); vTaskDelay(1000); }

rtt_log (prio 4)
  for(;;) { 队列阻塞 → SEGGER RTT 写出 + 1s 心跳 }
```

### 2.1 实时性硬化依据（2026-09-23 实测）

前一版「app_fast 任务 + mcycle 忙等」实测：

| 工况 | rate | 丢拍/s | late/max |
| :--- | :--- | :--- | :--- |
| 静止 | 21.9kHz | 3100 | 1000/s, 2.2ms |
| 旋转 | 16.5kHz | 8500 | 4300/s, 2.4ms |

`read: rotor max=2370µs`（正常 7-12µs）= SPI 轮询被任务抢占拉长 200 倍。
根因：`app_io`/`app_diag`/`rtt_log` 抢占 `app_fast` 忙等任务——「回报走 RTOS」
的结构性代价。**FOC 25kHz 电流环不可接受** → 控制环路移入 ADC PMT 完成中断。

修订后契约：
- **节拍 = 硬件触发**（PWM1 CMP10 → TRGM → ADC0 PMT），25kHz 精确，无软件抖动；
- **ISR 优先级最高**（PLIC 3），不被任务/日志抢占；
- `app_fast` 任务删除，任务域收窄为纯后台（io/diag/rtt_log）；
- 超时观测：`app_fast_step_hook` 内 mcycle 自测执行时长，超一个节拍周期记 late。

### 2.2 优先级与调度

| 单元 | 优先级 | 角色 |
| :--- | :--- | :--- |
| **ADC PMT 中断（FOC 快车道）** | PLIC 3（最高） | 25kHz 控制环路（采样/换算/保护/输出） |
| `rtt_log` | 4（任务最高） | 日志队列 → RTT（阻塞，无日志零打扰） |
| `app_io` / `app_diag` | 3 | 1ms IO / 1s 回报（vTaskDelay 阻塞） |
| idle | 0 | — |

ADC0 PMT 中断 > ADC1 SEQ 中断（PLIC 1）；`DISABLE_IRQ_PREEMPTIVE=1` 非抢占 →
两个 ADC ISR **串行**，`s_fault_ctx`（fault_process 25kHz 与 fault_tick 1kHz 共享）
并发面**消除**（原 §6 遗留项闭环）。

### 2.3 快车道内容（`app_fast_step`，25kHz，ADC PMT 中断内）

| 调用 | 语义（FOC 视角） | 执行时间 |
| :--- | :--- | :--- |
| `app_debug_encoder_sample()` | 转子/出轴角度采样（FOC 反馈） | ~8-12µs（SPI 轮询，ISR 内不会被抢占） |
| `app_analog_signal_process()` | 三相电流/母线电压换算 + 滤波 | ~3-5µs |
| `app_fault_process()` | 三相电流 RMS + L2 保护判断 | ~2-3µs |
| `app_debug_motor_run_once()` | 控制输出（**FOC 控制环原位替换点**） | ~2-3µs |
| **合计** | | **~20-25µs（40µs 周期，余量 25-50%）** |

**硬约束**：无 RTOS API / 无 printf / 无动态分配 / 无等待（已逐函数核对）。

### 2.4 IO 内容（`app_io_step`，1ms）

| 调用 | 语义 |
| :--- | :--- |
| `app_adc_slow_process()` | ADC1 慢通道采样（VBUS/NTC/CANID 原始码生产者） |
| `app_debug_adc_update()` | Ozone 观测变量刷新（`.noncacheable`，1kHz 刷新） |
| `app_debug_uart_run_once()` | UART0 单字符调试 |
| `app_debug_can_run_once()` | MCAN3 自检 |
| `app_terminal_run_once()` | USB Terminal（通讯） |

### 2.5 诊断内容（`app_diag_step`，1s，回报）

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

- ~~**`app_fault_tick`（ADC ISR 回调）与 `app_fault_process` 共享 `s_fault_ctx`**~~
  **已闭环（2026-09-23）**：控制环路移入 ADC PMT 中断后，二者均在 ISR 上下文
  且 `DISABLE_IRQ_PREEMPTIVE=1` 非抢占 → 串行化，并发面消除。
- 25kHz 硬触发快车道 = **已完成**（ADC PMT 完成中断驱动，见 §2.1）。
- 编码器 SPI 改 DMA + 中断回调（消除 ISR 内 8-12µs 轮询，为 FOC 计算腾余量）。
- 中断优先级重排 = **已完成**（ADC0 PMT 提至 PLIC 3 最高）；后续评估
  `configMAX_SYSCALL_INTERRUPT_PRIORITY`（仅当需要 ISR 调 FreeRTOS API 时）。
- 快车道函数去 `debug_` 前缀改名（`app_encoder_sample` 等）——随 FOC 模块化一并做。

---

## 7. 测试断言

见交付说明「测试步骤」。核心：启动全程完整、`tick_selfcheck` PASS、`rtt_hb` 稳定、
UART/Terminal 可用、电机控制正常、`g_enc_loop_late_us` 不显著恶化、`g_rtos_exc_*` 全 0。
