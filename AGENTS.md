<!--
  Canonical development guide for all HPM5361 projects.
  Distributed by: hpmdev sync-config
  Source of truth: <env-repo>/config/AGENTS.md
  Do NOT edit the distributed copy in a project; edit the env repo and re-run sync-config.
-->
# AGENTS.md - 嵌入式 C17 解耦架构开发指南

## 0. 本文件来源与环境约定

本文件由环境仓库统一分发，是**所有工程共享的开发规范**。项目本地如需追加约定，请写在本文件末尾的「项目附加约定」小节，升级时不会被覆盖。

环境仓库（`Alliance-HPM-Dev`）只负责：容器 / 工具链 / SDK / 脚本 / 工程模板 / 共享规范。
具体工程各自独立成库，统一放在工作区的 `projects/` 目录下开发，并复用环境变量指向的 SDK 与工具。

关键环境变量（由 `.envrc` 导出）：

| 变量 | 含义 |
| :--- | :--- |
| `HPMDEV_ROOT` | 环境仓库根目录 |
| `HPMDEV_SDK_DIR` | HPM SDK 路径（等价于 `HPM_SDK_BASE`） |
| `HPMDEV_TOOLS_DIR` | 工具/脚本根目录 |
| `HPMDEV_TEMPLATES_DIR` | 工程模板根目录 |
| `HPMDEV_SHARED_DIR` | 共享代码根目录（可选，见第 6 节） |
| `HPMDEV_PROJECTS_DIR` | 各独立工程存放目录 |

---

## 1. 核心设计哲学 (Core Philosophy)

本工程采用 **“物理隔离 + 契约驱动”** 模式。其核心目标是实现业务逻辑（App）与芯片外设（SDK/HAL）的深度解耦。

- **原子目标**：App 层代码与硬件外设深度解耦，仅通过 Interface 层访问。
- **现代化 C**：强制使用 **C17** 标准（匿名结构体、泛型选择 `_Generic`、静态断言 `static_assert`）。
- **高性能**：针对 HPM 系列 RISC-V 架构优化，强制执行 L1 Cache 对齐与 ILM (RAMFUNC) 部署。

---

## 2. 目录分层与依赖守则 (Strict Layering)

当前工程维持 **App 内部分层 + Interface 契约层 + Driver 适配层 + Board 板级层** 的结构，不额外引入独立 `Module/` 目录。

### 2.1 顶层分层

| 目录层级 | 职责 (Responsibility) | 允许包含的头文件 | 禁止项 (Hard Bans) |
| :--- | :--- | :--- | :--- |
| **1. App/** | 应用级代码总入口；内部再细分为 `Application/`、`Control/`、`Algorithm/`、`Platform/`、`Debug/` | `App/*`, `Interface/`, `<stdint.h>`, `<stdbool.h>`, `<stddef.h>` | 禁止直接包含任何 `hpm_*.h`；禁止跨层直接依赖 `Board/` 私有实现 |
| **2. Interface/** | **契约定义层**。定义硬件抽象对象 (HAL) 与跨层访问协议 | 纯 C 标准库头文件 | 禁止包含私有变量、静态函数或驱动实现 |
| **3. Driver/** | SDK 适配实现。将物理寄存器操作映射至 Interface 契约 | `hpm_sdk.h`, `board_pins.h`, `Interface/` | 禁止在此处编写业务逻辑、控制策略或状态机 |
| **4. Board/** | 硬件底表。定义原始引脚、时钟、板级资源与 IOCFG | `hpm_soc.h` (仅宏定义) | 禁止承载业务逻辑、控制算法或应用编排 |

### 2.2 App/ 内部分层

| 子目录 | 职责 | 允许依赖 | 禁止项 |
| :--- | :--- | :--- | :--- |
| **App/Application/** | 应用编排层；负责系统启动后的业务流程、状态机、模态切换、任务组织、上位机通讯编排 | `App/Control/`, `App/Platform/`, `App/Algorithm/`, `Interface/` | 禁止直接调用 `Driver/` 私有接口；禁止直接操作寄存器或 `hpm_*` API |
| **App/Control/** | 实际控制器层；负责闭环控制、保护策略、控制状态、调节器组合与控制输出决策 | `App/Algorithm/`, `App/Platform/`, `Interface/` | 禁止承担上位机协议编排；禁止直接依赖 `Driver/` 私有接口 |
| **App/Algorithm/** | 纯算法库；提供 PID、PLL、滤波、RMS、斜坡等可复用计算模块 | C 标准库、`<math.h>`、`<float.h>`、必要的通用类型头 | 禁止依赖 `Driver/`、`Board/`，尽量避免依赖具体业务状态机 |
| **App/Platform/** | 面向应用的硬件能力封装；把 `Interface/` 组合成可供 `Application/Control/` 使用的稳定能力 | `Interface/`, 必要的 App 公共类型头 | 禁止在此处写具体业务状态机；禁止把 SDK/HAL 细节向上泄漏 |
| **App/Debug/** | 调试、验证、自检、RTT 输出、临时 bring-up 入口 | `Interface/`, `App/Platform/`, `App/Control/` | 禁止成为正式业务默认路径；禁止让调试逻辑反向主导生产代码结构 |

### 2.3 推荐调用方向

```text
Application -> Control -> Platform -> Interface -> Driver -> Board
            \-> Platform -> Interface -> Driver -> Board
Control     -> Algorithm
Debug       -> Platform / Control / Interface
```

补充约束：

- `Application/` 负责“做什么、什么时候做、处于什么模式”。
- `Control/` 负责“控制怎么算、如何保护、输出什么控制量”。
- `Platform/` 负责“硬件能力如何被安全、稳定地提供给上层”。
- `Debug/` 仅作为验证辅助层，不能长期承载正式运行主流程。
- 若某个对象天然属于控制语义（如功率级控制器、采样同步控制器、保护控制器），优先落在 `Control/`，而不是额外拆出 `Module/`。

---

## 3. C17 编码与性能规范 (Coding Standards)

### 3.1 接口对象化 (Object-Oriented C17)

接口定义必须使用 **匿名结构体**。所有物理参数需在驱动层完成归一化（例如：占空比统一为 `float` [0.0-1.0]）。

```c
// 示例：Interface/intf_pwm.h
typedef struct {
    uint8_t instance_id;
    struct {
        status_t (*init)(void);
        status_t (*set_duty)(float duty);
    }; // 匿名结构体：允许对象通过 dev->set_duty() 直接调用
} pwm_if_t;
```

### 3.2 强制 C17

- 以 `-std=c17` 编译；`CMakeLists.txt` 中设置 `CMAKE_C_STANDARD 17` / `CMAKE_C_STANDARD_REQUIRED ON`。
- 允许：匿名结构体 / 联合体、`_Generic`、`_Static_assert`、指定初始化器。
- 禁止：GNU 扩展语法（除非 SDK 头文件强制要求）。

### 3.3 性能约定

- 中断热路径与高频调用函数使用 `ATTR_RAMFUNC` 放入 ILM，常量表使用 `ATTR_DLM` / 对应段属性放入 DLM。
- 频繁访问的全局状态放到 fast RAM，避免 AHB SRAM 访问延迟。
- 避免在热路径使用函数指针间接调用，必要时内联；`clampf` 之类的微函数标注 `always_inline`。
- 浮点域统一 `float`；仅在必要处使用 `double`（HPM5361 具备双精度 FPU，但 ISR 内优先单精度）。

---

## 4. 构建与调试

```bash
# 在具体工程目录下
make configure        # 生成构建系统
make build            # 编译（产物在 build/）
make artifacts        # 导出到 output/
make clean            # 清理
```

常用覆盖项：

```bash
make build BOARD=<board_name> CMAKE_BUILD_TYPE=Release HPM_BUILD_TYPE=flash_xip
```

- 排障先看 `build/last_build.log`。
- 调试信息路径重映射由 Makefile 的 `DEBUG_PREFIX_MAP` 控制；宿主机路径通过 `HPMDEV_HOST_WORKSPACE` 提供。
- 烧录：`make flash`（OpenOCD）或 `make flash-jlink`。

---

## 5. 配置分发与项目独立性

- `.clang-format` / `.clang-tidy` / `.clangd` / `.editorconfig` / `.cspell.json` / 本文件均由环境仓库 `config/` 分发。
- 修改规范请改环境仓库，然后运行 `hpmdev sync-config <project>`（或 `--all`）重新下发。
- 项目必须能仅依赖「环境仓库 + 自身」完成构建；不得记录对本工作区绝对路径的硬编码（一律走环境变量）。

---

## 6. 共享代码策略

- 旧的 `alliance_hpm_base_platform` 共享库已废弃，不再作为子模块引入。
- 可复用算法/驱动的传播途径有两条：
  1. **模板传播**：成熟的通用模块（如 `App/Algorithm`）由环境仓库的工程模板携带，新工程创建时自动获得。
  2. **环境变量共享**：需要跨工程共享的代码放到 `HPMDEV_SHARED_DIR` 指向的目录，工程通过该环境变量可选引用；脱离工作区时自动降级为「不启用」，不破坏独立构建。
- 任何被多个工程使用的独立想法，优先沉淀进模板或环境仓库，而不是在各工程间复制粘贴。

---

## 7. 模板内置共享模块（`templates/hpm5361-4layer`）

模板已内置以下经实战验证的模块，新工程创建后按实际硬件改配置即可，无需从零实现：

| 层 | 模块 | 说明 |
| :--- | :--- | :--- |
| Interface | `intf_adc / hrpwm / gptmr / gpio / can / trgm / synt / uart / clock / ws2812` | 契约层，含 `intf_default.c` 分发实现 |
| Driver | `drv_clock / adc / hrpwm / gptmr / trgm / gpio / mcan / synt / ws2812 / uart` | SDK 适配实现 |
| Driver | `IrqProfiler` / `WS2812` | 中断占用分析 / 灯带驱动 |
| Platform | `app_adc`（ADC16 PMT 采样链） / `app_analog_signal`（标定 + 多类型滤波） / `app_hrpwm`（含移相与软启动预热） / `app_gptmr` / `app_gpio` / `app_can` / `app_ws2812` / `app_buzzer` | 应用可用能力 |
| Debug | `app_debug` / `app_debug_adc` / `app_debug_hrpwm` / `app_debug_can` / `app_debug_profiler` / `app_debug_rtt` / `app_debug_trap` | RTT 输出与自检入口 |
| Algorithm | `algo_pid / algo_pll / algo_filter / algo_ramp / algo_rms / algo_hyst / algo_ffd` | 纯算法，零硬件依赖 |

命名约定（中性名，按实际拓扑改名即可）：

- ADC 通道：`ADC_CH_V_IN / I_IN / I_L / V_OUT / I_OUT / I_AUX`
- 模拟量项：与 ADC 通道一一对应
- HRPWM：`HRPWM_PAIR_A/B` → PWM0，`PAIR_C/D` → PWM1；实例 `HRPWM_INST_0/1`
- GPTMR：`APP_GPTMR_CH_0/1/2`

模板板级：`Board/user_board`（HPM5361，默认，`hpmdev new` 会改名）；`Board/hpm5301evklite_board` 为 HPM5301 参考板，无法构建 HPM5361 平台栈。

---

## 项目附加约定

<!-- 各项目在此追加本地约定，sync-config 不会覆盖本小节。 -->

### 硬件选型教训（2026-09-18）

- **HPM53M1 的 GPIO 仅存在于 PA 组**：PA00~PA15、PA26~PA29（共 20 个）。
- **PB/ADCIN 引脚（PB00/PB01、PB08~PB14 = ADCIN1~6/11/14/15）为纯模拟端口，无数字功能**：
  - 禁止将 LED、使能、片选、PWM 等数字信号分配到这些引脚；
  - 本板状态 LED 接在 PB01（ADCIN14）上，即因此无法由固件控制（需改板）；
  - PB00（CANID）作为 ADC 输入使用不受影响。
- 新增外设或改板时，先对照 HPM53M1 datasheet §2.2（引脚表）与 §2.5（"GPIO 都分配在 PA 组"）核对引脚能力，不要沿用 HPM5361 的引脚分配（HPM5361 上 PB 为 GPIO，HPM53M1 上不是）。
- 数字外设（UART/SPI/CAN/PWM）的引脚分配必须来自 PA 组，且优先核对 datasheet §2.2 中的 ALT 功能。

### 电流采样符号（2026-09-21，硬件事实）

- 低侧三电阻采样：**`I_*+` 接低侧 MOSFET 源极、`I_*−` 接 PGND**（原理图
  `Inverter_Power_Stage` R30~R32），运放 `+IN` 接 `I_*+` → 硬件测得电流对应
  **"流出电机"方向**，与 FOC/马达约定（流入电机为正）**反相**。
- 软件侧统一在 `app_analog_signal` 按 `hardware.current_sense.invert`（默认 1）取反；
  新增/更换电流采样硬件时**必须重新核实该符号**（符号错误 → 电流环正反馈 → 失控）。
- 教训：开环 V/F 无电流反馈，无法暴露符号错误；FOC 首次上电务必先做零给定
  （`foc on` 不设转矩）观察电流 ≈ 0，再做闭环。

### 供电与调试纪律（2026-09-18）

- **本板必须使用外部电源供电（XTAN/VIN），禁止依赖调试器供电**：
  - 调试器供电电流不足，480MHz 运行时 +3.3V 轨跌落至 VPMC 欠压复位阈值（典型 2.6V）以下，导致芯片反复复位；
  - 现象：RTT 高频重复输出、启动计数恒为 1、程序疑似"跑飞"（实为欠压复位循环）；
  - 排查记录：`docs/superpowers/specs/2026-09-18-m1-board-bringup-design.md` §13。
- 类似"程序反复重启/跑飞"的问题，**先确认供电（外部电源 + 测量 +3.3V/+5V）**，再查代码。

### 驱动目录与参数存储约定（2026-09-19）

- `Driver/hpm_impl/`：**HPM MCU 外设**驱动（clock/sys/uart/spi/adc/hrpwm/gptmr/mcan/flash/…）
- `Driver/encoder/`：**编码器器件**驱动（`drv_kth7823.c`；后续不同编码器在此并列新增，
  互不影响；器件驱动只依赖 `Interface/` 契约，不直接操作 HPM 寄存器）
- `App/Platform/app_param.*`：**通用 flash 键值参数存储**（magic + key + CRC32；
  各模块在 `app_param.h` 登记 key 并自定义数据布局；`store` = 整扇区读-改-写 + 回读校验）
- flash 布局：末尾 8KB 由链接脚本预留（倒数第 2 扇区 = 参数区，最后 1 扇区 = 自检用）
- 编码器零点用**软件方案**（存 flash），不消耗编码器 MTP（Z 寄存器寿命仅 1000 次）

### 驱动缺陷修复（2026-09-19）

- `drv_gptmr.c` 的 `gptmr_drv_init()` 曾存在 **reload 赋值顺序缺陷**：
  `gptmr_apply_duty()` 先于 `gptmr_state[ch].reload = reload` 执行 → PWM 初始化时
  `reload == 0` → `CMP0 = CMP1 = 0` → 按手册 §43.2.2"**CMP0 与 CMP1 相等时输出无变化**"，
  通道输出恒定不翻转（触发链静默失效）。已修复（赋值移至 `switch` 之前）。
- **该缺陷影响模板驱动的所有 PWM/PWM_TIMER 用法，需同步回环境仓库模板**
  （`templates/hpm5361-4layer`）。
- 排查提示：GPTMR 输出异常时优先核对 `CMP0/CMP1/RLD`（注意 CMP 值位于寄存器
  bit[27:4]，即"值 << 4"）与 `CNT` 是否推进。

### 代码风格统一（2026-09-20）

- **版权头**（SDK 原生文件除外——`Board/hpm5301evklite_board/**`、`linkers/**` 保持 HPMicro）：
  统一为组织 `Alliance HardwareGroup`、作者 `Kaiser`，格式：

  ```c
  /**
   * @file    app_fault.h
   * @brief   故障保护与错误处理（v1：纯判断）
   * @author  Kaiser
   *
   * <原有说明文字保留>
   *
   * Copyright (c) 2026 Alliance HardwareGroup
   * SPDX-License-Identifier: BSD-3-Clause
   */
  ```

- **Doxygen 注释**：文件头 `@file/@brief/@author`；公开函数 `@brief/@param/@return`；
  类型 `@brief` + 字段行尾 `/**< 说明 */`；`.c` 内部静态函数 `@brief`；
  函数体内说明性注释保持 `/* */` 不变。
- **命名**：
  - 文件级静态：`s_` + 完整语义名；跨文件/调试观测全局：`g_`；无前缀静态补前缀；
  - **禁止缩写**：`hw`→`hardware`、`sw`→`software`；禁止 `s_f`/`s_cfg`/`s_ctx`（无模块限定）
    等过度省略名；
  - 局部变量：单字母仅 `i/j/k`（循环）与 `x/y`（数学）；`ok/rc/id/ch` 等语义明确者保留；
  - 既有改名基准：`s_f`→`s_fault_ctx`、`s_hw`→`s_hardware_params`、`s_sw`→`s_software_params`、
    `s_cfg`→`s_adc_cfg`、`s_ctx`→`s_spi_ctx`/`s_uart_ctx`/`s_kth7823_ctx`；
    模块 `app_hw_params`→`app_hardware_params`、`app_sw_params`→`app_software_params`。

### Comm 层与 USB Terminal（2026-09-21，同日模块化改名）

- **`App/Comm/` 通讯层**（与 Control 平级）：通讯接口与协议的唯一归属，**按模块分子目录**：
  - `terminal/`（`Inc/` + `Src/`）：USB CDC 终端（CherrySH 绑定、命令层、常驻状态区）；
  - `can/`（`Inc/` + `Src/`，预留）：CAN 协议——电机控制报文 / 反馈报文（独立 spec）；
  - UART 单字符调试暂留 Debug 层（现状）。
- **术语**：本层模块统一用 **terminal**（终端）表述，不使用 shell（`app_terminal_*`）；
  "CherrySH / chry_shell / csh_*" 仅指 SDK 中间件本身，保持不变。
- **通讯子系统统一约定**：
  - 生命周期 `init()` + `run_once()`（1kHz 慢任务）；ISR 零协议逻辑（中断仅收发搬运）；
  - 输入输出经 Interface 契约（`intf_usb_cdc` 等，经 Platform `app_usb_*` 封装），不直接操作寄存器；
  - 单次处理有界（目标 ≤200 µs）；长操作一律走 job 框架（`app_terminal_job`）；
  - flash 操作仅允许停机窗口（命令前置联锁 + 停顿提示）。
- **通道分工**：USB CDC = Terminal 交互；RTT = 高频 trace（`app_debug_printf` 不变）；
  UART0 = 单字符调试（现状）。
- **Terminal 命令扩展**：新命令按域归入
  `App/Comm/terminal/Src/app_terminal_cmd_{sys,diag,param,motor}.c`；
  模板见 `app_terminal_cmd.h`（`CSH_CMD_EXPORT_ALIAS(func, name, )`；usage 由命令自身打印）。
- **参数访问约定**：消费者统一经 `app_*_params_current()` 读取；
  调试写入仅经 Terminal `param` 命令（`_mutable()`）；名称空间 `<域>.<路径>`
  （如 `hardware.current_sense.a_per_volt`）；生效语义见元数据 `apply` 字段
  （live = 实时读取；reboot = init 期消费，v1 不持久化）。
- **第三方豁免**：SDK 中间件源文件（`middleware/cherrysh`、`middleware/cherryrb`）保持原样，
  不适用本工程版权头/命名规范；`config/csh_config.h` 为 SDK 模板派生（保留原归属，注明工程修改）。

### FreeRTOS 引入纪律（2026-09-23）

- **必须 `sdk_compile_definitions(-DUSE_NONVECTOR_MODE=1)` + `(-DDISABLE_IRQ_PREEMPTIVE=1)`**
  （对齐 SDK 官方惯例：57/57 个 FreeRTOS 示例用非向量模式，55/57 配非抢占）。
  调用顺序：`set(CONFIG_FREERTOS 1)` 在 `find_package(hpm-sdk)` **之前**；
  两条 `sdk_compile_definitions` 在 `find_package`（即 `project()`）**之后**——该函数此时才可用。
- **向量模式是未验证路径，且含地址 bug（差 2MB）**：`middleware/FreeRTOS/.../freertos_hpmicro_vectors.h`
  的 `default_isr_N` 硬编码 PLIC complete 到 `0xE4200000`，而 HPM5300 系列
  `HPM_PLIC_BASE = 0xE4000000`（`soc/HPM5300/HPM5361/hpm_soc_ip.h:37`）。
  每个 ISR 结束都写错地址；25kHz 等密集中断首次触发时总线异常 →
  落入 `portASM.S` 的 `j .` 死循环 → 全系统静默冻结（RTT 无输出 / USB 不枚举 / ADC 不刷新）。
  官方示例全用非向量模式（走 `freertos_handle_interrupt` + `HPM_PLIC_BASE` 宏做 claim/complete），
  故该 bug 从未被暴露。**不要用向量模式。**
- **FreeRTOS 异常 = 静默死亡陷阱**：`freertos_risc_v_application_exception_handler` 在
  `freertos_exception_handler()` 之后无条件 `j .`（`portASM.S:323`），且 trap 入口硬件清
  `mstatus.MIE` → 冻结；SDK 弱实现用 `printf`（stdout）**不走 RTT** → 零征兆。
  **必须强符号覆盖 `freertos_exception_handler`**（见 `App/Platform/Src/app_rtos.c`）：
  先纯内联汇编读 CSR、零函数调用写 `.noncacheable` 全局 `g_rtos_exc_*`（栈烂时也存活，
  J-Link/Ozone 直读 `0x8b000–0x8b018`），再尝试 RTT 直写。**排查冻结先读这 7 个字。**
- **任务栈预算不能照搬裸机主栈**：裸机 `_stack_size=16KB`；FreeRTOS 任务栈从 `ucHeap` 抠，
  且**首层中断帧（含 FPU 约 300B）压在任务栈上**（`portContext.h` 先 SAVE 再切 ISR 栈）。
  `configCHECK_FOR_STACK_OVERFLOW=2` 只在任务切换时检查，忙等路径溢出静默。
  bring-up 类重任务至少 2048 words（8KB）。**指纹**：`mepc` 落在 `ucHeap` 区间 = 控制流被栈砸烂。
- **时钟初始化在调度器之前**（SDK 惯例：`board_init()` 含 `board_init_clock()`）。
  本工程 `intf_clock_init()` 在任务里（调度器后）执行——已实测可用，但属偏离惯例；
  若后续 tick 不稳优先复查此处与 `clock_mchtmr0`（`drv_clock.c` 自证 MCHTMR 本板曾"跑飞"）。
- **CherryUSB OSAL 连带**：`CONFIG_FREERTOS` 会自动编入 `usb_osal_freertos.c`，需
  `configUSE_MUTEXES` / `configUSE_COUNTING_SEMAPHORES` / `configUSE_TIMERS` = 1，
  以及 `configTIMER_TASK_STACK_DEPTH`（`port.c` 的 `vApplicationGetTimerTaskMemory`
  无条件引用，与 `configUSE_TIMERS` 无关）。但 `config/usb_config.h` 的
  `CONFIG_USBDEV_EP0_THREAD` **保持未定义** → USB 仍走裸机 ISR 路径，`usb_osal_*` 运行时不被调用，
  USB 行为零变化。**启用 `CONFIG_USBDEV_EP0_THREAD` 会改变 USB 行为，须单独评估。**
- **驱动层 `SDK_DECLARE_EXT_ISR_M` 零改动**：它是条件宏，非向量模式自动切换展开形态
  （生成 `default_isr_N` 直通包装，PLIC complete 由 `freertos_handle_interrupt` 统一做）。
- **冻结排查顺序**：先外部供电（欠压复位会伪装成"跑飞"）→ 读 `g_rtos_exc_*` 指纹
  （`mcause` bit31=中断/低 31 位=异常码；`mepc`=肇事指令；`mscratch`=中断嵌套深度）→
  按 `mepc` 落点分类（Flash=代码逻辑、`ucHeap`=栈砸烂、外设区=寄存器/总线错误）→
  再查调度。**不要先改调度代码。**

### Flash XPI 读写与烧录纪律（2026-10-22）

- **现象**：`cal rl save` 进入 `app_param_store()` 后跑飞；Ozone 停在
  `portASM.S` 的 trap 上下文保存处。实测指纹：
  ```text
  g_param_store_stage = 1        # 整扇区 read，尚未 erase/program
  mcause = 0x2                   # illegal instruction
  mtval  = 0xFFFFFFFF            # 取指得到全 1
  pc     = 0x000007EE            # freertos_vector_save_context，不是故障指令
  sp     = 0x0009CE40            # ISR/main 栈区域
  ```
  `stage=1` 说明参数区尚未擦除，FENC/FCUR/SPRM 没有因这次失败被主动破坏。
- **根因（高置信）**：`Driver/hpm_impl/drv_flash.c` 的 `flash_read_impl()` 当时
  **在 XIP flash 中执行，且 `rom_xpi_nor_read()` 期间未关闭全局中断**。
  25kHz ADC/USB ISR 可在 XPI ROM read 窗口运行并从 flash 取指；取到全 1 指令后
  触发 `mcause=2`，最终落入 FreeRTOS trap 保存上下文。`program`/`erase` 已有关中断，
  只有 `read` 漏了。
- **修复**：
  - `flash_read_impl()` 增加 `ATTR_RAMFUNC`，放入 ILM（`.fast`）；
  - 在 `rom_xpi_nor_read()` 前后使用 `flash_enter_critical()` /
    `flash_exit_critical()`，保存/恢复 `mstatus.MIE`；
  - `Interface/intf_flash.h` 的契约同步为：**擦/写/ROM 读期间关闭全局中断**。
  - 修复后符号应在 ILM：`nm` 中 `flash_read_impl`/`flash_program_impl`/
    `flash_erase_sector_impl` 均为 `0x00000xxx`，不得回到 `0x80008...`。
  - 台架验证：修复后 `cal rl save` 成功。
- **不可回退的规则**：
  1. 任何 `rom_xpi_nor_read/program/erase` 调用都必须满足：**函数在 ILM/RAM + 调用期间 MIE 关闭**；
  2. 不要在 XIP flash 中的函数里直接调用上述 ROM API；
  3. 不要在 ISR 或已关中断的临界区内调用 `app_param_store()`/flash 保存；
  4. `program`/`erase` 后按需做 D-Cache invalidate；read 的目标缓冲若有缓存别名，也要确认一致性；
  5. `g_param_store_stage=1/2` 表示尚未擦写，参数区通常仍可用；出现 `3/4` 才说明进入了擦写窗口。
- **烧录工作流陷阱**：`make build` 只生成 `build/output/demo.*`；
  `make flash` / `make flash-jlink` 读取的是 `output/HPM53M1_G6618Motor.*`。
  只执行 `make build` 后直接烧录 `output/`，可能烧到旧固件。已给
  `flash-openocd` / `flash-jlink` 增加 `artifacts` 依赖；手工烧录前也必须先
  `make artifacts`，并核对 `output/HPM53M1_G6618Motor.elf` 的时间戳和
  `flash_read_impl` 符号地址。
- **调试顺序**：`cal rl save` 类异常先读 `g_param_store_stage/slot/error`，
  再读 CPU 的 `mcause/mepc/mtval/sp`；不要只看 `portASM.S` 的 PC，
  也不要先怀疑 `cal rl` 算法或 FreeRTOS 调度。
- **`cal rl show` 是 RAM 快照，不读取 flash**：重启后它显示全 0 属于正常，
  不能据此判断 `cal rl save` 失败。验证持久化应看
  `motor.rs_ohm` / `motor.ls_h` / `software.control.current_loop.*`，
  以及启动日志 `[FOC] current tuning: restored(flash)`。

### YAML 布尔字段（2026-10-23）

- 配置中的开关统一写 `true/false`（生成器也接受 `on/off`、`yes/no`、旧 `1/0`）。
- C 结构体仍保持 `uint8_t` 0/1，参数元数据仍为 `u8`，不改变运行时 ABI/Flash 布局。
- 已覆盖：`hardware.current_sense.invert`、`software.fault.shutdown_en`、
  `software.control.current_loop.decoupling_en`、`software.control.speed_observer.enable`、
  `software.current_ff.r_en/wl_en/bemf_en`。

### 软件参数存储策略（2026-10-23）

- `config/software.yaml` 使用单字 `storage`，取值：
  - `keep`：使用 flash 已保存参数（默认）；
  - `reset`：忽略 flash；SPRM/MPRM 回 YAML 默认并写回，FCUR 清除；
  - `override`：本次启动仅 SPRM 使用 YAML 默认，不写回 flash。
- `storage` 是编译期参数；改 YAML 后必须 `make artifacts` + 重新烧录，
  **不要**用 `param set software.storage` + `param save` 代替。
- `reset` 的清理范围是 SPRM + MPRM + FCUR；FENC（电角度零点）和
  MOTN（游标机械零位）是独立机械标定记录，不在此策略内。
- `app_current_identify_clear()` / Terminal `cal rl clear` 可单独清除 FCUR。
- 参数单例必须在 `app_param_init()` 之后初始化：`app_init()` 在 0b 先调用
  `app_param_init()`，再初始化 MPRM/SPRM；`app_encoder_init()` 内的重复调用为幂等。
  CAN/UART/Terminal 也会消费 software 参数，不能用“编码器初始化后再加载参数”的顺序。
- `app_software_params_load()` 的 `storage=keep` 仍按固定记录代次 1
  兼容旧固件保存的 SPRM；`reset/override` 跳过 flash 读取。

### 电流环 R*I 自整定 A/B（2026-10-23）

- `cal rl` 的 PI 阶跃阶段是 d 轴小电流阶跃，可在同一测试点自动做
  `R*I off → R*I on` 对比；校准期通过 `app_foc_calib_trial_r_ff()` 临时覆盖，
  **不修改用户 `current_ff.r_en`**。
- 结果字段：`r_ff_step_off` / `r_ff_step_on` / `r_ff_tested` /
  `r_ff_recommended`；`cal rl` 输出 `RI A/B` 行。
- `r_ff_recommended=YES` 仅在 on 阶跃通过且 rise/overshoot/error/RMS
  未超出容差时成立；它不自动启用 R*I，仍由用户显式
  `param set software.current_ff.r_en 1` + `param save`。
- 台架可另用 `motor id`（d 轴电流、自由轴零转矩）做 2A/5A/10A
  的手动 R*I 对比；`motor id` 不得在电机旋转时使用。

### PWM 正常关断瞬态（2026-10-23，台架已验证）

- **现象**：`driver power on/off` 单独切换正常；`foc on` 后即使静止，
  `foc off` 会让 USB CDC 断连，J-Link/Ozone 保持在线，MCU 未复位。
- **定位**：不是 +12V GPIO 单独造成，也不是 `foc` 控制算法规格；
  触发点在 **PWM 停止时栅极驱动仍带电**。
- **当前软件顺序**：
  - 正常停机走 `app_3phase_inverter_disable_soft()`：
    **先关 +12V 栅极供电 → 等待 20ms → 再停 PWM**；
  - `app_foc_disable()` 正常路径调用该函数；
  - 故障/急停仍走 `app_3phase_inverter_disable()` / `emergency_stop()`，
    保持“先停 PWM/force_low → 再关 +12V”的安全顺序。
- **台架验证（2026-10-23）**：静止、`motion speed 5`、`motion speed 20`
  条件下 `foc off` 均不再导致 USB CDC 断连；`foc: state=OFF, gate power OFF`
  正常打印。后续不要回退为“先停 PWM、再关 +12V”的正常停机顺序。
- **高速边界**：`motion speed 30` 直接 `foc off` 仍会断 USB；正常停机状态机
  在电流归零后新增“轴端速度 ≤0.5 rad/s 或最多等待 2s”再执行软关断，
  Terminal `foc off` 等待上限改为 3s。
- **台架验证（2026-10-23）**：`motion speed 30` 及多次 `±30` 后 `foc off`
  全部完成，USB CDC 未再断连；最终 `omega` 已降到约 0.5 rad/s 轴端速度。
- **残留观测**：`foc off` 后 `foc` 的 `protect/pvb` 计数每次增加约 30，
  不是 USB 断连；说明关断期间仍有短暂 Vbus 采样异常，后续若要清理保护计数
  再单独查 ADC/母线窗口，不要为此回退软关断顺序。
- **若需调整**：先只调 `APP_3PHASE_GATE_OFF_SETTLE_MS`（当前 20ms）或
  `APP_FOC_STOP_SPEED_WAIT_TICKS`；若仍无效，再查 +12V/USB 地线、共模和母线回流，
  不要先改 USB 协议栈。

### 位置/速度环同频 15kHz（2026-10-24）

- **位置与速度共用 GPTMR1 CH0 的单个 15kHz 中断**；`app_motion_timer` 只绑定
  `app_motion_tick`，不再分别启动 CH0/CH1。CH1 由 `app_gptmr` 初始化但 motion timer
  不启动（预留）；CH2 通用通道仍为 10kHz。
- `app_motion_tick` 的固定顺序是 **`app_motion_position_step()` → `app_motion_step()`**：
  位置模式先产生速度给定，再由同拍速度环消费；速度/力矩模式完全跳过位置分支。
  位置反馈仍来自上一拍的 `app_motion_step()` 快照，单拍延迟为 1/15000 s。
- `APP_MOTION_POSITION_RATE_HZ`、`app_motion_timer` 预期周期、位置输入滤波和
  梯形轨迹的 `step_time_s` 均为 15kHz。
- 位置测试的仿真节拍改为 `1/15000 s`，并通过 `app_motion_tick` 驱动，按物理时长
  等比例调整 tick 数。
- 运行态以 `[MOTION] tick ...` 的 `period_max/late_count` 观测为准，不声称与 PWM 锁相。
- `late_count` 是入口间隔超过标称 110% 的次数，**不是丢拍**：当前 ADC0/FOC ISR
  优先级 3 高于 GPTMR1 的 2，FOC 重载时 motion tick 会被抢占，`late` 自然升高。
  判断是否丢拍看累计 `n` 是否约 15k/s；判断 motion 自身负载看 `max`（且 `max`
  含被高优先级 ISR 抢占的时间）。
- **后续扩展点**：多圈位置、MIT 等新 motion 指令在 `app_motion_tick` 的模式分发处
  接入；不要新增独立定时中断，也不要让速度/力矩模式承担位置解算开销。

### FOC 电角度零点默认值（2026-10-24）

- `config/motor.yaml` 的 `encoder.electrical_offset_rad` 是 **FENC 丢失时的兜底名义值**，
  不是逐台校准的替代。本装配最近验证名义值为 **3.3527 rad（192.1°）**，`direction=+1`；
  禁止再用 `0.0` 作为默认值（0 是合法角度，会造成约 180° q 轴反转且不报警）。
- `cal encoder` 只写 FENC（电角度零点/方向）；`motion cal` 只写 MOTN（游标相位/零位），
  两者不能互相替代。
- `storage=reset` 在 boot 时先重置 MPRM、随后由 `app_motor_identify_restore()` 恢复 FENC；
  `param reset motor` / `param reset` 只重置 RAM 中的 MPRM，**不会重新应用 FENC**。
  执行这类 reset 后必须重启或重新 `cal encoder apply`，否则 FOC 会以 YAML 默认值运行。
- 新装机/换编码器/重烧 flash 后，先 `cal encoder show` 确认
  `foc(runtime)`、`params`、`dir` 一致，再做闭环。仅靠速度环调参无法纠正零点/方向错误。
- `motor.encoder.electrical_offset_valid` 是“已确认标定”标志：YAML 默认 false；
  `cal encoder apply`、`cal encoder save` 或成功恢复 FENC 置 1；`param reset`、
  手动改零点/方向清 0。**当前只用于告警，不拒绝任何 FOC/param/motion 命令**；
  `foc` 状态、`cal encoder show` 和启动日志会提示 `WARN`。
- 多台量产时不要依赖单一 YAML 名义值；逐台执行 `cal encoder`，并在未来需要强制联锁时
  再把该标志接入 FOC 使能拒绝逻辑（当前明确不做）。
- `cal encoder` 的 verify 探针为 **自适应 2 A → 4 A q 轴**：每方向 40 ms 电流斜坡
  + 120 ms 满流保持，先正/反向 2 A，失败后自动升到 4 A；`probe=+fwd/-rev rad (i=..A)`
  记录两方向机械行程与最终档位。正向或反向任一方向达到 `0.05 rad` 且方向正确即通过，
  并立即停机，避免自由轴在探针电流下继续多圈旋转；90° 错位两方向都无行程、
  180° 错位两方向都反向，仍会失败。扫描每步驻留延长到 10 ms、起始静默 300 ms，
  总时长约 26 s；残差判据保持 `verify_max ≤ 15°`，不要用放宽阈值掩盖机械问题。
- trial 零点：REV 终点仅在距正/反向合并圆均值 ≤5.7° 时采用；高摩擦/摆线减速器
  的终点可能停在齿槽/弹性死点，超出窗口必须回退圆均值，避免 endpoint 偏置。
- verify 失败日志必须带上真实 `resid mean/max`、双向 `probe` 与探针档位；若在残差阶段
  就失败，`i=0.0A` 表示探针未执行，不是探针电流丢失。
- **CALIB 期间 `s_i_q_ref` 由校准端口独占**。`app_foc_run_body()` 的 DIRECT 分支
  不得用 `s_i_q_ref_cmd` 覆盖它，否则 verify 的 q 探针电流会被 1kHz 编排清零，
  日志表现为 `probe≈0`、resid 很小但始终 verify failed。该覆盖已修复，并有
  `test_foc_calib_port.c` 回归用例。

### Motion 命令整形与连续限速（2026-10-24）

- **速度/力矩目标斜坡**（`config/software.yaml`）：
  - `control.motion_input.vel_ramp_rate` [rad/s²]；0 = 直通。
  - `control.motion_input.torque_ramp_rate` [Nm/s]；0 = 直通。
  - `app_motion` 在 15kHz 速度拍用 `algo_motion_ramp()` 线性逼近目标；
    首次进入/切换模式时，速度从当前反馈起算、力矩从 0 起算。
- **位置梯形轨迹**：
  - `control.position_loop.trajectory_vel_limit` [rad/s]；
  - `control.position_loop.trajectory_accel_limit` [rad/s²]；
  - 两者均 >0 时启用，使用 `algo_ramp` 梯形模式；否则沿用原二阶输入滤波。
  - 轨迹模式由轨迹自身生成速度前馈，`input_vel_ff` 不再叠加；位置目标按
    单圈最短路径规划。
- **两套独立连续限速**（互不覆盖，各自可配置）：
  - **motion 轴端限速**：`control.limits.motion_speed_limit_rad_s` [rad/s] +
    `control.limits.motion_speed_limit_gain` [Nm/(rad/s)]；`app_motion` 在转矩层用
    `algo_motion_limit_vel()` 连续限幅，超速只允许制动。
  - **FOC 电角速度限速**：`control.limits.foc_speed_max_rad_s` [rad/s 电角] +
    `control.limits.foc_speed_limit_gain` [A/(rad/s)]；`app_foc` 在 ADC ISR 对最终
    q 轴给定做同样的连续限幅，属于 FOC 内部独立保护；CALIB 不生效。
  - 两者均为 `0` 时关闭；正常 motion 可只启用 motion 限速，也允许同时启用两套
    （各自作用在自己的物理域，不建议把两者当成同一个阈值使用）。
- **电流限幅语义**：`foc_current` 改为 **Id 优先 2-norm**：先给 d 轴预算，
  q 轴使用剩余预算；`R·I` 与 `ωL` 前馈均使用限幅后的给定电流（`ωL` 不再用反馈）。
- 改这些 YAML 字段后需 `make artifacts` + 重新烧录；它们属于 init 期消费
  （`APPLY_REBOOT`），运行期 `param set` 不立即生效。

### 电流 boost 管理（2026-10-24）

- **连续/瞬态分离**：`control.limits.i_q_continuous_a` 为连续限幅（24.3A），
  `control.limits.i_q_max_a` 为 boost/绝对硬上限（30A）；不再把 30A 作为无限时静态限幅。
- **预算**：`i_q_boost_time_s` 为 30A 满额持续时间；按
  `(I²−I_cont²)/(I_boost²−I_cont²)` 消耗 I²t 预算。预算耗尽后 `app_foc_current_get_i_limit()`
  回落到 `i_q_continuous_a`；`i_q_boost_recover_time_s` 从耗尽回满。
- **回充滞环**：仅当实测电流 ≤ `i_q_continuous_a × i_q_boost_recover_ratio`
  （默认 0.9）才回充，避免 24.3/30A 边界反复脉冲。
- `app_foc_current` 的 `i_max` 和 `app_motion` 的转矩上限均使用动态限幅；
  `i_q_max_a` 仍是 live 绝对上限，测试/调试可临时收紧。
- 当前参数：`i_q_max=30A`、`i_q_continuous=24.3A`、`i_q_boost_time=2s`、
  `recover=10s`、`recover_ratio=0.9`。30A 只用于瞬态，不改变 `motor.i_peak_10s_a`，
  也不改变 `fault.oc_trip_a`。

### Kt / flux 口径（2026-10-24）

- **唯一运行时口径**：`motor.kt_nm_per_a` 为转子转矩 / 峰值 q 轴电流 [Nm/A_peak]；
  `app_foc_current_init()` 由 `Kt/(1.5·pole_pairs)` 派生 bEMF 用的 λ，**不直接读**
  `motor.flux_linkage_wb`，避免旧 MPRM 中 Kt/λ 双源不一致。
- `config/motor.yaml` 的 `flux_linkage_wb` 由
  `kt_nm_per_a / (1.5 * pole_pairs)` 派生；生成器 `cross_checks` 会拒绝
  Kt/λ 不一致的配置。`ke_vs_per_rad` 仅作手册对照/文档，不参与 FOC。
- 当前按峰值电流口径：`kt=0.117 Nm/A` 与 `torque_peak_10s/i_peak_10s≈0.115`
  一致；对应的 λ≈0.0078 Wb，和手册 Ke 直接换算值有差异，后续以实测反电动势/
  转矩为准；改 Kt 后 `bemf_ff` 与转矩命令会同步变化。

### 位置轨迹初速度、Yd/Ydd 与惯量前馈（2026-10-24）

- **梯形轨迹必须支持有符号初速度**：`algo_ramp` 新增 `plan(target, current, velocity)`
  与 `get_velocity()` / `get_acceleration()`；`app_motion_position_step()` 在每次新
  位置命令时**优先使用上一拍规划速度** `get_velocity()`（首次进入才用测量速度兜底）。
  这比直接使用 PLL 测量速度噪声更低、命令更连续；禁止再用 `set_current()` 把轨迹
  速度清零后重新起步。
- **Yd/Ydd 消费**：位置梯形轨迹的速度由 `get_velocity()` 直接给出，不再对位置做
  差分；`get_acceleration()` 用于惯量前馈。`algo_motion_pos_filter_t` 同时输出
  `accel_setpoint_rad_s2`。
- **惯量模型与 cal inertia**：
  - `motor.inertia_kgm2` = **转子侧**惯量；`motor.load_inertia_kgm2` = **轴端**
    负载/减速器附加惯量；总轴端前馈 `J_out = inertia×reducer² + load_inertia`。
  - `cal inertia [max_current_a]` 参考 `cal rl` 采用多点状态机：固定 6 个
    `{电流, 起始速度, 截断速度}` 点位（0.6~2.0A，轴端 0.5~1.0rad/s），每个点位
    再执行 **正转 F / 反转 R 两次采样**，共 12 次；逐次做驱动/制动窗口。单次
    失败只作废该次，继续下一方向/点位。参数是**最大电流上限**（0=默认2.0A；
    上限2A并受 `i_q_max` 裁）。
  - 单次仍用 `J_total = 2τ/(α_drive−α_brake)` 抵消恒定摩擦；驱动窗口12ms，
    制动窗口在速度降到该点 stop 速度时截断，避免过零。多点结果取中位数，
    MAD/median ≤15% 且有效采样≥6（至少3点×双向）才置 valid；进度条按采样
    数和相位显示。
  - `cal inertia` 不自动生效；FOC OFF 后 `cal inertia apply` 写
    `motor.load_inertia_kgm2`，再 `cal inertia save` 写 MPRM。
  - **当前 `storage: reset` 时 `cal inertia save` 写出的 MPRM 会在下次启动被
    YAML 默认值覆盖**；要持久化，应把值写入 `config/motor.yaml`（当前 `0.064`），
    或切换 `storage: keep` 后再执行 save。
  - 轴必须自由/负载随动且无外部卡滞；测得的负载含实际联轴器/负载。总惯量
    小于转子反射时 `load_inertia` 钳0并在结果中标记 `clamped`。
- **`inertia_ff` 语义**：
  - `inertia_ff_raw_nm` = `J_out × accel`，转矩/速度限幅前的原始请求；
  - `inertia_ff_nm` = 在相同转矩/速度限幅下，最终转矩相对无前馈基线的实际增量；
  - 两者均在 `app_motion` 快照中可见，台架调参以 `inertia_ff_nm` 为准。
- **模型参数 apply**：`motor.pole_pairs/rs_ohm/ls_h/kt_nm_per_a` 以及
  `motor.encoder.electrical_offset_rad/direction` 在元数据中标记为 `foc_off`。
  `param set` 在 FOC 非 OFF 时拒绝；下一次 `foc on` 通过
  `app_foc_current_reload_model()` 重载 R/L/λ，并重建角度链。不再把这些字段
  显示为 `live` 却只改 RAM。
- **旧 MPRM 迁移**：`load_inertia_kgm2` 追加在 `app_motor_params_t` 末尾；
  `app_motor_params_load()` 使用 `APP_MOTOR_PARAMS_V1_SIZE` 前缀迁移，旧记录
  不丢其他电机参数，新字段保持 YAML 出厂默认（当前 `0.064`）。
- **电流环模型重载不重置 boost/积分器**；`app_foc_current_init()` 仍用于完整
  重初始化。`kp`/`ki` 增益仍为 live，`bandwidth_rad_s` 仅作 `cal rl` 目标。
- 回归用例：`test_algo_ramp_plan.c`、`test_algo_inertia.c`、
  `test_inertia_identify.c`、`test_motion_position_replan_velocity`、
  `test_motion_inertia_feedforward`、`test_foc_model_apply.c`、
  `test_param_terminal_guard.c`、`test_motor_params_load_inertia_migration`。
