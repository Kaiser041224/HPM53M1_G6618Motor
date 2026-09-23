# FreeRTOS 基础骨架设计（A：内核移植 + superloop 任务化；B：RTT 交给 RTOS 调度）

- 日期：2026-09-23
- 分支：`feat/rtos-foundation`（从 `main` / `a8c5d2c` 切出，不直接改 `main`）
- 状态：设计已由 Kaiser 确认，进入实施

---

## 1. 背景与目标

当前 `App/main.c` 为裸机 superloop：`board_init → app_init → while(1) app_run()`。
全部 bring-up 测试逻辑集中在 `App/Logic/app_logic.c`，以 `mcycle` 忙等对齐 25kHz 主节拍，
1ms/1s 分频跑慢任务与心跳。后续 FOC 需要「硬实时快车道 + 后台任务域」的结构，本次先把
FreeRTOS 作为后台调度底座引入，并把测试代码逐步任务化。

### 历史教训（护栏来源）

`feat/freertos-test-tasks` 分支曾引入 FreeRTOS，遇到无法排查的问题，**大概率由 USB 终端引发**。
因此本次硬性约束：

1. **不复用** `feat/freertos-test-tasks` 的任何代码，只作反面教材。
2. **USB 路径零改动**：`App/Comm/**`、`intf_usb_cdc`、`drv_usb_cdc`、`app_usb*` 在 Kaiser 明确指令前不迁、不重构。
3. **细分 commit**：每一步对应一个可上板验证的断言，可独立回滚。
4. **分步交付**：A 完成即 commit；B 完成后**不 commit**，等 Kaiser 测试反馈后再 commit。

### 本次交付范围（A + B）

| 步骤 | 内容 | Git 动作 |
| :--- | :--- | :--- |
| **A** | FreeRTOS 骨架：superloop 原样变 1 个任务 | 立即 commit |
| **B** | RTT 打印输出交给 RTOS 调度（log 任务） | 只改工作树，等指令再 commit |

**明确不在范围**：多任务拆分（diag/terminal/…）、FOC、ISR→任务通知、syscall 中断优先级模型、
USB 终端任务化、tick 源改 GPTMR。

---

## 2. 架构

### 2.1 A — FreeRTOS 骨架

```
main()
  board_init()
  xTaskCreate(app_bringup_task, "app", 1024 words, prio 2)
  vTaskStartScheduler()

app_bringup_task()
  app_init()          // 原样，零改动（内含 intf_clock_init）
  app_rtos_selfcheck_tick()
  app_run()           // 原样，内含 for(;;) 超循环，永不返回
```

- bring-up 代码（`app_logic.c`）**零改动**。`app_run()` 内部 `mcycle` 25kHz/1ms/1s 分频是业务节拍，
  与 RTOS tick 无关，保留。
- 单任务 = 单一执行流，与裸机串行语义一致，不引入并发读写问题。
- `app_init()` 必须在调度器启动之后执行（电流零标定等依赖 ISR，而 `CONFIG_DISABLE_GLOBAL_IRQ_ON_STARTUP=1`
  使全局中断由调度器开启）。

### 2.2 B — RTT 输出交给 RTOS 调度

```
app_debug_printf(fmt, ...)
  vsnprintf → 行缓冲
  s_writer(buf, len)        // 同步旁路（保持 Terminal capture 语义）
  enqueue(buf)              // 入环形队列；满则丢弃并计数

rtt_log_task (prio 1, 512 words)
  阻塞等队列 → SEGGER_RTT_WriteString
  每 1s 输出 rtt_hb（队列水位 / 丢弃计数），证明调度在跑
```

- `app_debug_printf` 签名不变，调用方零改动。
- 旁路 writer（Terminal capture）**仍在生产者上下文同步调用**，保证 dump 捕获时序不被异步化破坏。
- ISR 内禁止 `app_log_printf` / `app_debug_printf`（现状本就无 ISR 打印，保持不变）。

---

## 3. tick 源与中断模型（按 Kaiser 决策：参考 HPM SDK 设计）

| 项 | 选型 | 依据 |
| :--- | :--- | :--- |
| tick 源 | **MCHTMR**（SDK 默认，`portasmHAS_MTIME=1`） | Kaiser：「tick 源参考 hpm sdk 中的设计」；`samples/rtos/freertos/freertos_hello` 同款 |
| `configCPU_CLOCK_HZ` | **24000000** | MCHTMR 时钟 = `osc24m/1`（`drv_clock.c`），与 CPU 480MHz 无关 |
| `configTICK_RATE_HZ` | 1000 | SDK 默认 |
| 中断模型 | **简化模型**（不定义 `USE_SYSCALL_INTERRUPT_PRIORITY`） | critical section = 全局清 `mstatus.MIE`；A/B 阶段无任何 ISR 调 FreeRTOS API |
| 向量模式 | SDK 默认（不强制 `USE_NONVECTOR_MODE`） | 不新增变量 |
| tickless | 不启用 | 不需要低功耗 |

**对冲护栏**：MCHTMR 在 bring-up 中曾被疑异常（后归因欠压复位，未隔离复测）。因此 A 必须含
**tick 基频自检**：`app_rtos_selfcheck_tick()` 在 `app_init()` 之后、`app_run()` 之前，用 `mcycle`
实测 32 个 tick 的平均周期，与 1ms 偏差 >2% 即 RTT 报错（`tick_selfcheck: FAIL`）。
把「tick 是否真的在走」变成可观测量。

若后续要改 GPTMR tick：独立 commit，需补 `BOARD_FREERTOS_*` 宏并屏蔽 `drv_gptmr.c` 中
`IRQn_GPTMR0` 的 ISR 声明（符号冲突）。

---

## 4. 配置与内存

### 4.1 `config/FreeRTOSConfig.h`（新建，以 SDK `freertos_hello` 为底）

| 项 | 值 | 说明 |
| :--- | :--- | :--- |
| `configUSE_PREEMPTION` | 1 | SDK 默认 |
| `configCPU_CLOCK_HZ` | 24000000 | MCHTMR |
| `configTICK_RATE_HZ` | 1000 | |
| `configMAX_PRIORITIES` | 32 | SDK 默认；`configUSE_PORT_OPTIMISED_TASK_SELECTION` 保持 0 |
| `configMINIMAL_STACK_SIZE` | 256 words | idle |
| `configTOTAL_HEAP_SIZE` | 16KB | heap_4，落 `.bss`→DLM |
| `configSUPPORT_DYNAMIC_ALLOCATION` | 1 | |
| `configSUPPORT_STATIC_ALLOCATION` | 0 | |
| `configUSE_TIMERS` | 1 | **修订**：见 §4.4 CherryUSB OSAL 连带约束 |
| `configTIMER_TASK_STACK_DEPTH` | 256 | port.c 的 `vApplicationGetTimerTaskMemory` **无条件编译**，必须定义 |
| `configUSE_MUTEXES` / `configUSE_COUNTING_SEMAPHORES` | 1 | 同上，供 `usb_osal_freertos.c` 编译 |
| `configCHECK_FOR_STACK_OVERFLOW` | 2 | 护栏 |
| `configUSE_MALLOC_FAILED_HOOK` | 1 | 护栏 |
| `configASSERT(x)` | `app_rtos_fatal(__FILE__, __LINE__)` | RTT 报错 + 三相紧急关断 + 停机 |

`FreeRTOSConfig.h` 放 `config/`：该目录已 `sdk_inc`（为 `usb_config.h`），SDK 内核编译可见，无需改 include。

### 4.4 CherryUSB OSAL 连带约束（实施中发现，重要）

`CONFIG_FREERTOS=1` 会让 `middleware/cherryusb/CMakeLists.txt:86` 自动编入
`osal/usb_osal_freertos.c`，该文件引用 `xSemaphoreCreateMutex` / `xSemaphoreCreateCounting` /
`xTimerCreate`。**但 USB 运行时路径零变化**：

- `config/usb_config.h` 中 `CONFIG_USBDEV_EP0_THREAD` 处于注释状态（未定义）；
- `usbd_core.c` 对 `usb_osal_*` 的调用全部包在 `#ifdef CONFIG_USBDEV_EP0_THREAD` 内，
  未定义时走 `#else` 裸机路径（`__usbd_event_ep0_setup_complete_handler` 直调）；
- 因此 `usb_osal_freertos.c` 仅需**编译链接通过**，运行时不会被调用。

对应处理：仅在 `FreeRTOSConfig.h` 打开 `configUSE_MUTEXES` / `configUSE_COUNTING_SEMAPHORES` /
`configUSE_TIMERS`（+ `configTIMER_TASK_STACK_DEPTH`，port.c 无条件需要），**不动 `usb_config.h`**。
若未来启用 `CONFIG_USBDEV_EP0_THREAD`，USB 行为将改变，须单独评估。

### 4.5 任务预算

| 任务 | 栈 | 优先级 | 阶段 |
| :--- | :--- | :--- | :--- |
| `app`（bring-up） | 1024 words = 4KB | 2 | A |
| `rtt_log` | 512 words = 2KB | 1 | B |
| idle | 256 words = 1KB | 0 | A |

### 4.3 内存落点与风险

- FreeRTOS `ucHeap`（heap_4）→ `.bss` → **DLM**（128K，已用约 62%）。
- A 新增约 16KB heap；B 再加 2KB 队列缓冲（`.bss`）。
- **构建后必须看 map**：若 DLM 越界，降 heap 到 8KB，或独立 commit 把 `ucHeap` 挪 AHB SRAM
  （`configAPPLICATION_ALLOCATED_HEAP=1` + `ATTR_PLACE_AT(".ahb_sram")`）。
- 热路径 `.fast`→ILM 部署不受影响（不改段属性）。

---

## 5. 新增/修改文件

### Commit A

| 文件 | 动作 | 职责 |
| :--- | :--- | :--- |
| `config/FreeRTOSConfig.h` | 新建 | 内核配置 |
| `App/Platform/Inc/app_rtos.h` | 新建 | `app_rtos_fatal` / `app_rtos_selfcheck_tick` / 任务优先级与栈常量 |
| `App/Platform/Src/app_rtos.c` | 新建 | 致命钩子（调 `app_3phase_inverter_emergency_stop`）+ tick 自检 |
| `App/main.c` | 修改 | 创建 `app` 任务 + 启动调度器 |
| `CMakeLists.txt` | 修改 | `set(CONFIG_FREERTOS 1)`（`find_package` 前）+ `sdk_app_src(app_rtos.c)` |

**不碰**：`App/Logic/app_logic.c`、`App/Comm/**`、`App/Debug/**`、`Driver/**`、`Board/**`、`Interface/**`。

### Commit B（暂不提交）

| 文件 | 动作 | 职责 |
| :--- | :--- | :--- |
| `App/Debug/Inc/app_debug_rtt.h` | 修改 | 增 `app_debug_rtt_start_task()`、水位/丢弃计数 API |
| `App/Debug/Src/app_debug_rtt.c` | 修改 | `app_debug_printf` → 同步 writer + 入队；新增 rtt_log 任务 |
| `App/main.c` | 修改 | 创建 `rtt_log` 任务（在 `app` 任务之前） |
| `config/FreeRTOSConfig.h` | 修改 | 视需要补 `INCLUDE_*` |

---

## 6. 错误处理

| 场景 | 行为 |
| :--- | :--- |
| `configASSERT` 失败 / malloc 失败 / 栈溢出 | `app_rtos_fatal`：RTT 打印文件行号 → `app_3phase_inverter_emergency_stop()` → 关中断死循环 |
| tick 自检失败 | RTT 打印 `tick_selfcheck: FAIL` + 实测/期望值，**继续运行**（不直接停机，便于继续观测其它输出） |
| `xTaskCreate` 失败 / 调度器异常返回 | `app_rtos_fatal` |
| B 日志队列满 | 丢弃新消息，`drop_cnt++`，不阻塞生产者（不卡控制环） |
| B 队列空中断/卡死 | `rtt_hb` 停止推进即可观测到（心跳即活性探针） |

---

## 7. 测试与验证

### A 的上板断言（commit 后由 Kaiser 验证）

1. RTT 输出既有 bring-up banner 与自检结果，与裸机一致（boot seq / params / clock / 各自检）。
2. `tick_selfcheck: PASS, avg=1000.x us`（偏差 <2%）。
3. Terminal/USB、双编码器、电机开环旋转（命令 `r`）行为与裸机一致。
4. 无卡死、无复位循环（先确认外部供电 + 测 +3.3V/+5V）。

### B 的上板断言（工作树状态由 Kaiser 验证，通过后再 commit）

1. 多任务切换可观测：`rtt_hb` 计数稳定推进（约 1Hz）。
2. 既有 RTT 内容完整（banner/自检不丢）。
3. 队列满只丢日志、不卡 25kHz 控制环（编码器 late 计数不显著恶化）。
4. USB/Terminal 无回归（dump capture 仍同步可用）。

### 构建验证（Mephisto 本地）

```bash
make configure && make build
# 产物：build/last_build.log 无 error
# map 复核：DLM 用量、ucHeap、任务栈落点
```

---

## 8. 回滚路径

- A 整步回滚：`git revert <commit-A>` 或 `git reset --hard main`（未合并前）。
- B 未提交：`git checkout -- App/main.c App/Debug config/FreeRTOSConfig.h` 即回到 A 状态。
- 分支未合并 `main` 前，`main` 始终保持裸机可用版本。

---

## 9. 后续（本次不做，仅占位）

- 模块分步任务化（diag / terminal / 慢通道），每模块独立 commit。
- 25kHz 快车道与 RTOS 后台域的硬隔离（FOC 接入点）。
- 中断优先级重排（ADC0/PWM1 提到 3、GPTMR 降到 2）与 `configMAX_SYSCALL_INTERRUPT_PRIORITY` 评估。
- 若 MCHTMR tick 实测不稳 → 独立 commit 切 GPTMR tick。
