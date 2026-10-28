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
- flash 布局：末尾 12KB 由链接脚本预留（倒数第 3 扇区 = 齿槽/传动角表，倒数第 2 扇区 = 参数区，最后 1 扇区 = 自检用）
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

### DLM 冷数据分层（2026-10-25）

- **控制热路径/FOC ISR 状态、USB/CAN DMA 缓冲、FreeRTOS heap、主栈继续留在 DLM**；
  不为了腾空间把这些数据搬到 AHB SRAM。
- 已迁移到 `.ahb_sram.bss` 的冷数据：
  `s_tx_pool`（终端 TX 环）、`s_slots` / `s_load_slot` / `s_verify_slot`
  （flash 参数停机暂存）、`s_current_identify_average`（仅 `cal current` 使用）。
- `s_identify_samples` 虽只在 `cal current` 使用，但由 30kHz ISR 写入，
  仍留在 DLM，避免标定时 ISR 访问 AHB。
- `.ahb_sram` 为 `NOLOAD`，启动不会自动清零；各缓冲区必须在 init 或首次使用前
  显式初始化，且不能直接改为 DMA 缓冲。
- AHB_SRAM 为 32KB，MCAN 已占约 10KB；参数/终端/电流辨识冷数据约 26KB；
  齿槽存储的记录/加载暂存再约 3.1KB，合计约 29.8KB，保留约 3KB 余量。
  GCC 链接脚本已增加溢出 ASSERT。
- `s_trace`、`s_rms_buf` 暂留 DLM：前者可能由 FOC ISR 调试写入，后者属于
  故障 RMS 路径，不做无证据迁移。


### 齿槽/传动角表补偿（2026-10-25）

- **两张独立角表**：
  - rotor：512 点 int16，按转子机械角索引，表值为**电机侧转矩** [Nm]；
  - output：256 点 int16，按减速器输出轴角索引，表值为**输出侧转矩** [Nm]。
- **注入点**：FOC 30kHz ISR，motion 正常请求解析后、FOC 速度限幅和
  `app_foc_current` 电流/boost 限幅之前，换算为 q 轴电流：
  `iq = T_rotor / kt + T_output / (reducer_ratio × kt)`。
- **Gating**：仅 `READY/RUN + MOTION 源 + 请求有效 + motion active`；
  `motion_speed_limited` 时暂停；`cal cogging` / `cal friction` 期间 suspend；
  CALIB/vtest/stop/fault/direct 源不注入。
- `execution.requested_iq_a` 必须包含齿槽补偿，否则 `current_limited` 会误判并冻结
  速度环积分；`g_foc_current_snapshot.i_q_cogging_a` 为诊断值。
- **自整定** `cal cogging [speed_rad_s] [revs]`：
  - 不带宽参数：默认多点 `0.5/1.0/2.0/3.0 rad/s`，每点每方向 2 圈，总计约 100s；
    低速点圈时长大、采样多，等效加权低速段（齿槽低速最可见）；扫掠超时按速度点动态计算；
  - 带 `speed_rad_s`：单点快速检查，`revs=0` 时默认 4 圈；
  - 只针对电机本体 + 减速器，不装外部负载；
  - 每点正反扫，q 电流按转子角/输出角分箱，F/R 相加抵消恒定摩擦；
  - 每个速度点记录 F/R 的 q 电流均值、RMS、样本数，用于观察速度依赖
    和摩擦不对称；角表由全部速度点共同生成。
- **速度模式输出角**：正常速度/力矩模式默认不读游标 pair，`shaft_rad` 不更新；
  输出角表启用且 master=on 时通过 `app_motion_set_cogging_output_required(true)`
  让 15kHz motion 维护输出角；`cal cogging` 另用临时 output-tracking 窗口。
  标定结束/失败/中止必须恢复临时 tracking，但不得关闭输出角表所需的 required。
- **默认启用配置**（`config/software.yaml`）：
  ```yaml
  control:
    cogging:
      enable: true         # true=有有效表时 boot 自动开总开关
      rotor_enable: true   # 默认启用 rotor 表
      output_enable: true  # 默认启用；若带载表现不稳可改 false
      gain: 1.0
  ```
  boot 在 `app_cogging_store_load()` 后调用 `app_cogging_apply_defaults()`；
  `cal cogging apply` 在复制表后也重新应用同一组默认值。
  `enable=false` 时 master 保持关闭，但 rotor/output 表选择仍按 YAML 设置；
  运行期可用 `cogging on/off` 覆盖，直到下次 boot/apply。
  这些字段是 `APPLY_REBOOT` 参数；`storage: reset` 会按 YAML 重置。
- `cal cogging show|apply|save|clear`；`apply` 要求 FOC OFF，复制表后按 YAML 默认值决定是否自动启用；
  `save` 只写角表到独立闪存扇区（不保存 enable/gain，后者由 YAML/SPRM 管理）；
  `clear` 清活动表并擦扇区（FOC OFF）。
- `cogging on|off|rotor on|off|output on|off|gain <0..2>|show`：默认全关；
  `cogging on` 在表选择为空时自动启用已有有效表；运动进行中禁止开启。
- 角表存储：`app_cogging_store` 使用倒数第 3 个 4KB 扇区；记录含 magic、
  version、点数、缩放、CRC，boot 加载到 DLM。`storage: reset` 不清除该表。
- 资源：活动表约 1.5KB DLM；标定 bins/结果约 12KB DLM；
  当前 DLM ~88.8%、AHB ~90.9%。`app_cogging_store` 的记录/加载暂存在
  AHB SRAM，标定完成前不启用。
- 回归：`test_algo_cogging`（含 `algo_cogging_finalize_bins`）、`test_app_cogging`、
  `test_cogging_store`、`test_foc_cogging_injection`、`test_cogging_identify_start`。

### 逆变器死区补偿（2026-10-26）

- **原理**：死区+管压降的相电压畸变 ∝ `sign(i_phase)`，幅值 `Δduty = t_eq·f_pwm`
  （duty 域自适配 Vbus）。补偿在 `foc_modulation_step_comp()` 于零序注入之后、
  逐相钳位之前按相叠加；符号由 `app_foc_current` 按相电流极性判定，
  **阈值 0.3A 带内保持上一符号**（避免过零抖动）。
- **参数**：`hardware.inverter.deadtime_comp_ns`（0=关闭，默认 0；上限 5000ns，
  折算 duty 钳 2%）。`APPLY_FOC_OFF`：`param set`（FOC OFF 时）→ 下次
  `foc on` 经 `app_foc_current_reload_model()` 折算生效，**无需重烧**。
- **覆盖范围**：正常电流环（READY/RUN）；**CALIB 全程挂起**（cal rl /
  cal encoder / motor id——辨识 q 零偏基线与 Q_CURRENT 守卫按无补偿工厂
  标定；固定符号模式的补偿矢量会注入直流 q 轴电压，150ns 实测偏移 ~0.8A
  直接触发误判，也据此推得本桥物理等效死区 ≲60ns）；
  **vtest 诊断路径不补偿**（保持原始电压响应语义）。
- **标定建议**：从 50ns 起步（守卫反推上限 ~60ns），A/B 看 `cal cogging`
  低速点 q RMS；过补偿会向齿槽表注入人工角表项。
- 观测：`foc` 命令打印 `deadtime comp: x.xxxx duty`；Ozone 直读
  `g_foc_dt_comp_duty`（.noncacheable）。
- 效果预期：低速小电流电压保真提升 → `cal cogging` 低速点 q RMS 下降、
  `cal rl` 的 R 估计更接近真实值（死区畸变原本被计入 R）。
- 回归：`test_foc_modulation`（comp 入口）、`test_foc_dt_comp`（折算/滞回/关闭语义）。

### 游标失配降级语义（2026-10-26）

- **缺陷**：静止落在游标失配区（pair 代数不一致 >0.25/50=1.8° 出轴）时，
  旧逻辑每拍 `VERNIER` 拒绝 → `feedback_valid` 永久 false → 一切 motion
  命令被拒（"feedback stale"），仅重启可恢复。速度/力矩模式根本不消费
  游标角（速度走 30kHz observer），却被它阻断——可用性缺陷。
- **修复**（`app_motion_step` 游标失配分支）：游标仅在作为**反馈/标定数据**
  （标定中、位置模式、output-tracking 窗口）时才判真失效（保持 20ms→撤销）；
  空闲态与速度/力矩态持续失配降级为 `angle_valid=0`
  （`app_cogging_set_output_angle(…,false)` → 齿槽输出表自动停注入，
  rotor 表不受影响），速度环继续走 observer。
- 观测：`motion show` 新增 `angle=%u`（游标角有效性，与 `valid=`（速度反馈）
  分离）；`vernier=` 计数仍每失配拍递增。
- 回归：`test_motion_vernier_degradation`（降级不阻断命令/不进坏拍统计/
  位置模式仍硬撤销）；`test_motion` 中"apply 后失配"用例同步为新语义。
- **遗留疑点（待台架诊断）**：本次会话游标 pair 大面积失配（59s 扫掠期间
  ~87% 拍失配、静止后持续失配），但前一日同流程正常。判定路径：
  ① `motion cal`（FOC OFF、静止）重推相位——若新相位与 flash MOTN 的
  0.000129 差 >0.005 turn，则是相位问题（`motion apply`+`motion zero save` 修复）；
  ② 若相位一致仍失配，是游标 pair 传感器/机械（装配、磁环、温度漂移）问题，
  用 `enc` debug 看 pair 统计（output fail/valid 计数）分段定位。

### 母线（vbus）采样与电流同源同步（2026-10-26）

- **机理**：vbus 原由 GPTMR0 CH2 1kHz **自由相位**链触发，与 PWM 载波无相位关系。
  PWM 实际频率是整数截断值（`reload = clk/f - 1`，本板 ≈30001.87Hz），与
  1000.000Hz 不成整比 → 采样点相对载波每 ms 前移 0.062µs，约 535ms 扫满一个开关
  周期（≈1.9Hz 拍频）→ 开关纹波被读成"母线波动"，并污染 `cal rl` 的电压重建
  （Rs 辨识用 `v_bus × duty` 反推绕组电压）。
- **修法**：ADC1 SEQ 改由 **PWM1_CH10REF 与 ADC0 PMT 同源触发**（TRGM 扇出，每个
  输出独立选源），序列重排 `[V_VBUS, NTC0, NTC1, CANID, V_VBUS]`。**ADC0 的 PMT
  队列不动**——每触发 ≤4 槽是硬件上限（`ADC_SOC_MAX_TRIG_CH_LEN=4`）且"首槽=队尾
  副本"规则必须保留，vbus 无法进入同一队列。
- **vbus 必须在序列首项**：触发后 ≈1.15µs 完成，早于 ADC0 PMT（4 槽 ≈4.6µs），
  故 PMT ISR 每拍都能读到本帧值；相位与电流完全相同，最大陈旧 1 帧（pwm 周期）。
- **保护节拍红线**：ADC1 SEQ 完成回调按 `pwm_freq/1000` **分频**后驱动
  `app_fault_tick()` → fault tick 仍 1kHz、去抖 5 次 = 5ms，逐位不变。ADC1 的 SEQ
  中断一直是使能的（spec §9.2 "无 ISR" 的表述有误，已更正）。
- **快通道口径**：`app_adc` 在 PMT ISR 内锁存 `PRD_RESULT[VBUS]`
  （`intf_adc_get_prd_result`，直读寄存器，ILM 常驻）+ 用 `intf_adc_get_seq_frames`
  判新鲜（帧计数未推进 = 链停摆 → 保持旧值并计 `stale`，不采信结果寄存器）。
  `app_analog_signal_vbus_fast()` 做换算 + 一阶 IIR（**截止 2kHz、采样率必须取
  `inverter.pwm_freq_hz`**；沿用 `APP_ANALOG_PROCESS_RATE_HZ=1kHz` 会把 α 从
  0.295 抬到 0.926）。FOC ISR 优先快通道，`foc_finite` 失败才回退 1kHz 缓存值。
- **ILM 纪律**：PMT ISR 调用链上的函数必须在 `.fast`（本批：`adc_get_prd_result`、
  `intf_adc_get_prd_result`、`adc_get_seq_frames`、`intf_adc_get_seq_frames`、
  `app_adc_get_vbus_fast(…sequence)`、`app_analog_signal_vbus_fast`、`adc_slow_adapter`）。
  改完用 `nm` 核对地址应为 `0x0000xxxx`；不要经 `intf_adc_read`（模式分支 + SDK
  包装会回到 XIP）。
- **`app_io_step` 顺序**：`app_adc_slow_process()` 必须在 `app_analog_signal_process()`
  **之前**（原顺序使慢通道物理量恒定落后 1 拍）。
- **驱动卫生**：抢占队列清理循环上界必须覆盖 `0..ADC_SOC_MAX_TRIG_CH_NUM(11)`
  （原 `t < ADC_PMT_MAX_TRIG(11)` 漏掉索引 11，复位残留 QUEUE_EN 会随共享 PTRGI
  启动垃圾队列）。
- **测试纪律**：`scripts/tests/adc/`、`scripts/tests/analog/` 为新增主机套件；
  pass 2 用 `-O2 -ffast-math` 编译 → **测试里禁止用 `isnan()/isfinite()`**（该档隐含
  `-ffinite-math-only`，会被常量折叠为 false）；改用位模式判定（同 `foc_finite`）。
- 回归：`test_adc_*`（队列形态/触发扇出/分频/快通道锁存与陈旧）、
  `test_analog_vbus_fast_*`（换算/滤波采样率/路径隔离）、`test_foc_vbus_fast_source`。
- **快通道供 duty 已定案（2026-10-26 配对 A/B，同固件同热态 3+3 轮）**：
  开关 1 → `cal rl` **887/1445/1044Hz 全 PASS**、RI A/B recommend=YES、raw 超调 22.3~26.9%；
  开关 0 → **754/754/754Hz 全 NOT PASSED**、raw 超调 28.6~31.1%。故快通道为
  **唯一正式来源**：原 A/B 开关 `g_foc_vbus_fast_en` 已于 2026-10-27 收尾**移除**，
  改为固定采用快通道 + 仅在 NAN 时回退 1kHz 缓存；`g_foc_vbus_src_used`（1=快/0=回退）
  只做观测，终端 `foc` 的 `vbus src=` 行据此显示（为 0 即代表数据链异常）。
  机理：`duty = v_cmd/v_bus`，慢值在阶跃期间给的是阶跃前母线 → 缩放系统性错误。
  **教训**：首轮单臂观测（快通道硬连线）的 754Hz 曾被误归因为"快通道供 duty"；
  无对照组的单臂观测不足以归因，必须配对 A/B。
- **ADC1 SEQ 配置冻结（2026-10-27）**：速率 **30kHz 由"与 ADC0 同一触发沿"结构性决定**
  （= `hardware.inverter.pwm_freq_hz` = 30000），不可独立设定；5 槽 · 唯一触发源
  PWM1_CH10REF · 槽序 `[V_VBUS, NTC0, NTC1, CANID, V_VBUS]`（首末同通道是 S/H 首槽
  污染自吸收的必需项）。`app_adc.c` 有 `_Static_assert` 守住槽数；改速率或槽序
  必须重跑 §9.5.2 配对 A/B 与 `cal rl`。
- **cal rl 剩余差异：结案（vbus 通路已排除，2026-10-26）**：相对改动前会话 `Rs_eff`
  0.162→0.1705（+5.3%）、`Ld` 121.5→108µH（−10%）、落点 1228~2000→887~1445Hz；
  而 Rs 散布由 ±4.3% 收到 ±0.7%。**排除项**：热漂移（台架确认电机室温）；vbus 读数偏置
  （万用表与外部电源表头一致）；**vbus 通路本身**（配对两臂 Rs/Ld 一致到 +0.83%/+0.50%）。
  **分工结论**：vbus 通路只影响**瞬态电压重建**（落点差 1~2 档、raw 超调差 3~5 点），
  **不影响 Rs/Ld 慢测量**；故 Rs/Ld 的会话间差异与 vbus 无关。剩余候选=「两臂共有的
  ADC1 30kHz 活动」与「会话间工况差异（停机位/接线/接触电阻）」；**非阻塞**。
  判别实验 = 下一项的方案 A（见下），复测后若不回归即结案。
- **采样链精简方案**（`docs/superpowers/plans/2026-10-26-sampling-chain-slimming.md`）：
  需求=快环路只留 FOC/motion。硬约束：每触发 ≤4 槽 + 一实例一模式一速率 → vbus 无法进
  ADC0 队列，且**只要 vbus 要 30kHz 同步，同实例的 NTC/CANID 就会同样高频转换**（软件
  只低频读，现状已如此）。方案 A=ADC1 `seq_cb=NULL`（30kHz ISR 消失）+ fault tick 移入
  1kHz 任务（**唯一触碰保护红线处，需显式确认**）；方案 B3=SEQ 只留 VBUS + NTC/CANID 走
  BUS_RESULT oneshot（需先做寄存器可行性实验）。
- **终端观测（勿再误传）**：`profiler dump`（逐 ISR 频率/耗时/outliers，快环路盘点）、
  `adc diag`（SEQ 帧率 + `vbus_fast` 活性）、`adc dump`、`adc delay <ns>`（运行期改触发延时）、
  `foc`（含 `vbus src=… fast/slow/Δ/seq/stale`）。`p`/`k`/`d`/`n` 是 **UART0** 单字符调试
  命令，USB 终端用上面的等价命令——**给台架指令前先查 `CSH_CMD_EXPORT_ALIAS` 清单**。

### 标定链鲁棒性（2026-10-26）

- **output 表零点解耦**：齿槽 output 表索引/查询改为物理输出角
  （`shaft_raw_rad`，不含方向与用户零点；identify 分箱同源）。re-zero /
  方向翻转不再失效表相位。本装配 direction=+1 且 zero invalid，切换为
  等价变换，flash 旧表不受影响。
- **motion cal 静止门**：相位采样前先过 20ms 收敛窗（逐拍位移
  <0.0002 turn），3s 不收敛判 `REASON_CALIBRATION` 失败。背景：相位对
  刚性旋转不变，但轴爬行（软停末段）经减速器回差污染 1000 样本平均
  相位（实测差 0.0041 turn → 解算余量 0.25→0.21）。
- **游标余量观测**：`motion show` 新增 `margin=`（|estimate−nearest| 的
  LPF，τ≈13ms）。健康 <0.1；接近 0.25 提示 pair 一致性边际
  （相位漂移/装配变化，重跑 motion cal）。`algo_motion_vernier` 增加
  可空残差输出参数（旧调用补 NULL）。
- **cal rl PASS 保留**：新 run 失败不再清空最近一轮 PASS（RAM 保留，
  重启清空）；`cal rl apply` 自动回落并显式提示；`cal rl show` 打印
  "retained PASS" 行（含代龄）；`cal rl clear` 同时丢弃 RAM 保留。

### RL 自整定带宽阶梯细化（2026-10-26）

- 阶梯 0.7×3 → **0.85×6**（2000→1700→1445→1228→1044→887→754Hz），
  "最高可通过带宽"定位精度 ±30%→±15%；超时 60→90s。
- **PI 阶段捕获无效样本重试 ×2**：瞬态保护拍（编码器坏帧/vbus 抖动）
  不再单拍否决整轮（此前两轮在 2000/1400Hz 首试即死于
  `detail=5 capture-invalid`）。
- **实测上限（2026-10-26 台架）**：细阶梯下 2kHz 满档可通过
  （超调 13~15%，容差 20%），阶梯落点由当轮 Ld 质量决定
  （半周期差 18.5% 的离群轮下探 1228Hz，干净轮 retry=0 满档）。
  此前 686Hz 落点=粗阶梯跳过+捕获毛刺瞬死的复合结果。
  曾按 50µs 环延迟推"上限 1.2~1.6kHz"——实测推翻，延迟估计偏保守。
  Ld 离散（114~137µH）是落点波动主因；候选改进：把半周期差
  纳入 Ld 接受度，离群轮重测 Ld 而非降带宽。
- 回归：`test_current_identify_retained`（保留/回落/clear 语义全程）、
  `test_motion_cal_settle`、`test_motion_vernier_residual`；
  `test_current_identify` 的阶梯与 -4/-5 断言同步。

- **`cal cogging save` 的内容是 1596B 记录，必须按 256B page 分块 program**：
  一次性提交大块会跨越 NOR page，已在台架表现为 `cal cogging save` 卡死；
  `app_cogging_store_save()` 现在与 `app_param_store()` 一样按 256B 提交。
- **D-Cache invalidate 必须按 32B cache line 对齐**：
  `l1c_dc_invalidate(addr, len)` 要求 addr/len 均为 32B 对齐；flash 记录
  1596B 或最后一块 60B 直接传入会触发 SDK assert。assert 经 newlib `_write`
  走到未初始化的 debug console UART，最终表现为卡在 `uart_flush()`。
  `drv_flash.c` 已统一将 invalidate 区间向下/向上取整到 cache line。

### 保护链 v3：分层保护与动作层（2026-10-27，台架已验证）

- **判据按物理时间尺度分层，不按代码归属**：L1 = ADC0 PMT 的**硬件 WDOG**（30kHz，
  TRIP 同拍）+ 电流环快跳 `i_trip_a`；L2 = 相电流 RMS（1kHz，DERATE）；L3 = 母线
  OV/UV（1kHz，读 30kHz 快通道值；一级 STOP / 二级 40V TRIP / UV STOP）；
  健康项 = PMT 帧停滞（TRIP）/ 编码器增量（WARN）。
- **ADC1 无周期中断**：`seq_cb = NULL`。实测其 30kHz 入口占 **1.5~1.6% CPU**，且 XIP
  慢链风暴（10.9µs）落在距下一拍仅 5.9µs 的窗口内。慢保护节拍改由 **1kHz io 任务**
  承载；`app_foc_wait_off()` 的 3s 自旋内加**保护泵**（否则减速/软关断窗口内停摆）。
  **flash 保存窗口仍无泵**（任务自身被阻塞），已记账。
- **动作总开关**：`config/software.yaml` 的 `fault.actions_enable`（**默认 false = 仅
  警告**）。运行期 `fault protect on|off`（RAM 覆盖）。原编译期宏
  `app_protect_policy.h` **已删除**。`fault.shutdown_en` 已退役并移除。
- **policy 表即开关**：`fault policy show` 分 `design`/`live` 两列；
  `fault policy <code> <warn|derate|stop|trip>` 运行期改表；`fault inject <code> on|off`
  台架注入（不经物理阈值）。**代码名大小写不敏感**（`ci_equal`）。
- **动作语义（不可回退）**：
  - `state = FAULT` **仅当生效动作为 TRIP**。STOP 若也置 FAULT，会被 FOC 门控立即急停，
    违背"受控停机"本意。
  - STOP 以**边沿请求**（`app_fault_take_stop_request`）交 `app_foc_run_once` →
    `app_foc_request_fault_stop()` 复用既有软停机。OFF-但桥使能 / CALIB / vtest 返回 −1
    → 调用方 `app_fault_note_stop_rejected()` 升 TRIP。**OFF 且桥已断返回 0（不算失败）**，
    否则空闲期一次越限会让此后所有 STOP 永久变 TRIP。
  - 使能门禁（`app_terminal_cmd_require_no_fault` 与 `app_foc_enable`）**只拦"带动作码"
    （STOP/TRIP）的锁存**；纯 WARN/DERATE 不阻塞（曾误用 `get_latched()` → 注入
    `ENC_ERR` 也会拒绝 `foc on`）。
- **L1 WDOG 契约**：状态评估在 `adc_generic_isr` 中**前移到 PMT/SEQ 分支之前**（否则同帧
  先跑完 FOC、后处理 WDOG → 停机要等下一拍）。**上电静默期（`settle_ticks < 50`）内忽略
  命中**——实测启动瞬态下 PMT 可能整帧为 0/全 1，否则每次开机假锁存三路 OC_FAST，
  `actions on` 时直接进 FAULT 并拒绝 `foc on`。命中后驱动自动关该通道中断，由
  `app_fault_wdog_service()`（1kHz）在 `wdog_rearm_ms` 且 raw 回窗后重臂；达
  `wdog_rearm_max` 升 TRIP 锁存。`fault show` 打印 `wdog rearm: U/V/W`。
- **vbus 快通道新鲜度**：ADC1 无中断，且 `ADC16.SEQ_WR_ADDR` **只随 DMA 写推进**
  （ADC1 非 DMA）→ **任何 SEQ 侧计数器都不可用**。改用**结果合法性**判定（全 0/全 1
  视为未写入 → 不采信 + 计 `stale`）。快通道健康看 `fault` 的 `vbus src=FAST(30k)`
  与 `stale` 增量，不看 ADC1 的 frames/seq_wr。
- **观测契约**：
  - `adc diag` 的 `rate`/`isr_cpu` **带自校验**：占用率 >100% 或 ADC0 速率偏离 pwm_freq
    ±20% 即判本窗无效并打印 `n/a`。根因：`intf_clock_get_cycle()` 是 **32 位**，
    480MHz 下每 **8.94s 回绕** → 间隔超一个回绕期会少算整数倍周期。
    **必须连续两次 `adc diag` 且间隔 <8.9s**。
  - ADC1 行 `irq/pmt/rate` 与 `SEQ frames/seq_wr` 恒 0 属**预期**（无中断 + 非 DMA）；
    ADC1 的 `INT_STS` 由 `app_adc_slow_process()` 每 1kHz 清一次（`INT_EN=0`，不影响中断）。
- **台架已验证（2026-10-27）**：① 硬件 WDOG **确实触发**（此前自 2026-09-19 起从未验证）：
  `cnt F_U/F_V/F_W` 各 1、快照带回越窗原始码；② `inject → policy → FAULT → force_low +
  关 +12V` 全链成立，`driver power` 显示 `gate power: OFF` 为物理证据；③ **TRIP 在 READY
  与带载下均不掉 USB、不复位**（曾误判为副作用，实为手动重启，已更正）；④ DERATE
  `i_limit 30.00 → 15.00 → 30.00`（条件解除自动恢复）；⑤ WARN 锁存不阻塞 `foc on`。
- **遗留**：① `ADC1 INT_STS` bit23 语义待识别（建议查 `SEQ_DMA_CFG` 是否残留 DMA 使能）；
  ② 弱磁（`motion speed 40+` 实测 `use=100% sat=1`、`i_d` 被耦合推到 −4A）；③ NTC1 恒为
  开路替代值（`adc diag ch1=65535`，硬件待查）；④ `vbus fast−slow` 差值 ±0.55V
  （固定相位纹波，不阻塞）；⑤ EWDG 未做（CPU 挂死无兜底，G4 开放）。

### 多圈/增量位置与 MIT（2026-10-27，台架已验证）

- **多圈位置 P 项必须用连续误差**：新增 `algo_motion_position_cont()`（算术差），
  `app_motion_position_step()` 按 `s_position_multi` 选择。
  单圈 `algo_motion_position()` 内含 `foc_wrap_pm_pi`，多圈误用会把整圈误差折成 0 →
  **"与目标差整圈的位置即静止"**。台架实测：`pos_multi 400` 停在 40°、
  `pos_multi 720` 停在 0°，`diag` 显示 `now=40/cur=tgt=400` 而 `vel_cmd≈0`。
  回归：`test_algo_position_p`（差 2π / 1.5 圈的两种语义对比）。
- **多圈/增量强制要求梯形轨迹**：`trajectory_vel_limit` 与 `trajectory_accel_limit`
  均 > 0，否则 `APP_MOTION_CMD_TRAJ_DISABLED(-11)`。二者是 `APPLY_REBOOT`：
  运行期 `param set` 后必须 `param save` + `reboot`（`storage: keep` 会优先用 flash）。
- **行程保护不可关闭**：`max_step_deg`（单次 3600°）、`max_travel_deg`（绝对值 36000°）。
  `travel_min/max_deg` 为可选软限位（0/0 = 关闭，用户决定不强制）。
- **连续角只在同一上电周期内可靠**；断电后由游标恢复单圈，绝对圈数无保证（需 homing）。
  `motion cal/apply` 使累积器失效重基准；`motion zero` 同步归零。
- **MIT 的 `p_des` 是绝对连续坐标**，不是"原地"：`motion mit 0 ...` 在轴于 120° 时
  会因 `pos_err_max_deg(90)` 被拒。原地施加阻抗用 `motion mit_here v kp kd t_ff`。
- **多圈/MIT 的实时路径必须在 ILM**：`algo_motion_position_cont`、`algo_mit_step`
  （含 `mit_finite`/`mit_approach`）由 15kHz `app_motion_step` 每拍调用。布局守卫
  已纳入这 5 个符号（28 项）；新增热路径函数必须同时加 `FOC_ATTR_RAMFUNC` 与守卫项，
  且**不要在 ILM 函数里调用 libm**（`fabsf` 已改为显式比较）。
- **MIT 限幅链顺序**（algo_mit）：增益上限校验 → 增益斜坡（Kd 按 Kp/Kd 上限比例）
  → 位置误差钳制（运行中钳制、入口拒绝）→ 前馈占比 → 力矩上限（含 boost/DERATE）。
- **CAN 1kHz 双向回报只支持 1~2 节点**（1 Mbps，标准帧≈125 bit；4 节点满载）。
- **通信看门狗喂狗在 CAN RX ISR**（`app_can_set_rx_isr_hook`），不依赖 1kHz 任务——
  否则 `foc off` 的 3s 自旋会把"有意静默"判成"通信丢失"。首帧才 arm；
  CALIB/停机窗口挂起；出厂 `actions_enable=false` 时只告警不停机。
