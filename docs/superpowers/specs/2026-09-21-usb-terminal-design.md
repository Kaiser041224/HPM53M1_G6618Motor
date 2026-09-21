# USB Terminal 终端设计：CherrySH + App/Comm 通讯层

- 日期：2026-09-21
- 状态：待评审
- 决策人：Kaiser（2026-09-21）
- 关联文档：
  - `docs/superpowers/specs/2026-09-20-param-pipeline-design.md`（参数元数据来源）
  - `docs/superpowers/specs/2026-09-19-fault-protection-design.md`（故障联锁来源）
  - `docs/superpowers/specs/2026-09-19-adc-sampling-design.md`（ADC/触发延时来源）

## 0. 决策记录（Kaiser，2026-09-21）

| 决策项 | 结论 |
| :--- | :--- |
| 终端接口 | USB CDC 作为 Terminal 终端；UART 暂时保留当前设计（单字符命令） |
| 交互形态 | L2：行式 REPL + 补全/历史 + `monitor` 实时仪表盘 + 长命令中止 |
| 核心组件 | CherrySH（SDK middleware）；允许使用 cherryrb 作为组件 |
| 文件结构 | 新增 `App/Comm/` 层（与 Control 平级）；terminal/can/uart 在 Comm 层内分类分层 |
| CAN 定位 | 未来主控制链路（接收控制指令 + 回传电机状态）；协议设计入 Comm 层（独立 spec） |
| flash 参数持久化 | 放 v2（本设计只留接口与纪律） |
| 中断优先级 | 同意改动：本次 `USB0: 2 → 1`；ADC0/PWM1 提升随 FOC 落地 |
| 命令集 | 按本设计 §5 执行；扩展规范就位，便于后续增加 |
| 输出门控 | 用 DTR（使用者自行开启，或使用带 DTR 的串口工具）；不新增 `is_configured` 接口 |
| FOC 约束 | Terminal 不得破坏控制环优先级与时序（§4 为硬约束） |

## 1. 背景与目标

### 1.1 背景

- 现有 USB CDC 仅做自检（回显 + 1Hz 心跳），另有 20 个单字符命令（`app_debug_cmd.c`，UART/USB 共用）——无帮助、无补全、不可发现。
- FOC 落地后，调参需大量迭代；当前"改 YAML → 重编译 → 烧录 → 观测"为分钟级循环，需要在线调参通道。
- CAN 将成为主控制链路；未来通讯相关设计（CAN 协议、可能的 UART 协议）需要在架构上有明确归属。
- 参数管线（三域 YAML → 生成 → 类型化结构）已就位，但缺少运行时访问通道。

### 1.2 目标

1. USB 终端达到 L2 交互形态：行编辑、TAB 补全、历史、彩色输出、`monitor` 仪表盘、长命令 Ctrl-C/任意键中止；
2. 引入 `App/Comm/` 通讯层，terminal/can/uart 分类分层，依赖方向明确；
3. CherrySH 集成符合工程风格与约束（C17、分层、Doxygen、命名规范）；
4. 参数在线调优：`param list/get/set/reset`（元数据驱动，含类型校验与生效语义标注）；
5. 现有单字符命令能力全量迁移为可发现命令；UART 侧保持现状不受影响；
6. **FOC 控制环全程高优先级与稳定性不被破坏**（硬约束，§4）。

### 1.3 非目标（本轮不做）

- `param save/load` flash 持久化（v2；本设计只定义接口与纪律）；
- 全屏 curses 式 TUI（L3）；上机位自动化测试脚本；
- CAN 协议实现（独立 spec，本设计只定 Comm 层位置与约定）；
- UART 迁移到 Terminal 核心（保持现状）；
- 登录/多用户/环境变量/文件系统等 CherrySH 高级特性（不启用）。

## 2. 总体架构

### 2.1 分层与依赖

```text
Application ──> Comm ──> Control / Platform ──> Interface ──> Driver ──> Board
```

- **Comm 层职责**：通讯接口与协议——对外交互（终端会话、命令层、协议帧）。
- **依赖方向**：`Comm → Control / Platform / Interface`；允许调用 `Debug` 层 dump 函数（单向，便于复用既有自检输出）；**禁止** Debug/Control/Platform 反向依赖 Comm。
- Comm 层不直接包含 `hpm_*.h`（同 App 层约束）。

### 2.2 Comm 层组织（分类分层）

Comm 层按"通讯接口/协议"划分子系统，文件前缀区分；本次实现 `terminal`，`can` 与 `uart` 预留：

| 子系统 | 文件前缀 | 状态 | 说明 |
| :--- | :--- | :--- | :--- |
| Terminal | `app_terminal_*` | 本次 | USB CDC 终端会话 + 命令层 + monitor |
| CAN 协议 | `app_can_proto_*` | 预留（独立 spec） | 主控制链路：控制指令接收 + 电机状态回传 |
| UART | — | 现状保留 | `app_debug_uart` + 单字符命令暂不动；未来可接入 Terminal 核心 |

**通讯子系统统一约定**（新增子系统按此模板）：

1. 生命周期：`app_<subsys>_init()`（boot 调用）+ `app_<subsys>_run_once()`（1kHz 慢任务调用）；
2. ISR 零协议逻辑：中断只做收发搬运（驱动内部环形缓冲），解析/状态机在 1kHz 层；
3. 输入输出均经 `Interface` 契约（如 `intf_usb_cdc`、`intf_can`），不直接操作寄存器；
4. 处理有界：单次 `run_once` 预算 ≤ 200 µs（正常态几 µs）；长操作走 job 框架（§4.2）；
5. 不阻塞控制环：禁止等待枚举、禁止同步等待主机、禁止在 1kHz 层做 flash 操作。

### 2.3 文件布局

```text
App/Comm/
  terminal/                     # USB CDC 终端（模块目录）
    Inc/
      app_terminal.h            # init / 1kHz run_once / 输出 / 刷新
      app_terminal_job.h        # 长命令 job 框架
      app_terminal_cmd.h        # 命令公共工具：参数解析/输出/安全联锁/注册约定
      app_terminal_monitor.h    # 常驻状态区（monitor）
    Src/
      app_terminal.c            # CherrySH 实例、sput/sget、TX 环、回调链、DTR/欢迎界面
      app_terminal_job.c
      app_terminal_cmd.c
      app_terminal_cmd_sys.c    # ver / status / usb / reboot / clear
      app_terminal_cmd_diag.c   # adc / pwm / enc / fault / profiler
      app_terminal_cmd_param.c  # param list|get|set|reset
      app_terminal_cmd_motor.c  # motor / inv / cal
      app_terminal_monitor.c    # 常驻状态区绘制 + monitor 命令
  can/                          # CAN 协议（预留：电机控制报文 / 反馈报文，独立 spec）
    Inc/  Src/
config/
  csh_config.h               # CherrySH 工程配置（SDK 模板派生，Apache-2.0 保留归属）
build/generated/
  params_meta_generated.{c,h}  # 参数元数据表（gen_params.py 生成，不提交）
```

### 2.4 通道分工（固定）

| 通道 | 用途 | 说明 |
| :--- | :--- | :--- |
| USB CDC | Terminal 交互终端 | 本设计新增；独占 |
| RTT | 高频 trace（`app_debug_printf`） | 不变；Terminal 不向 RTT 输出 |
| UART0 | 现状单字符命令 + 周期输出 | 不变（按决策） |

## 3. CherrySH 集成

### 3.1 组件与编译方式

| 项 | 值 |
| :--- | :--- |
| CherrySH | `$HPM_SDK_BASE/middleware/cherrysh`（Apache-2.0，Egahp/HPMicro） |
| cherryrb | `$HPM_SDK_BASE/middleware/cherryrb`（Apache-2.0，Egahp） |
| 编译源 | `chry_shell.c`（内部 include CherryRL）+ `builtin/help.c` + `chry_ringbuffer.c` |
| 不编译 | `builtin/login.c`、`builtin/shsize.c`、SDK port（见下） |

**不使用 SDK port 的原因**：`CONFIG_CHERRYSH=1` 会使 middleware 编入 `port/hpm/barebone_usb/src/shell.c`，其 CDC 描述符、`usbd_cdc_acm_bulk_out`、`cdc_acm_init` 等符号与现有 `Driver/hpm_impl/drv_usb_cdc.c` 重复定义。故按 SEGGER_RTT 同款方式直接引用源文件（`sdk_app_inc` / `sdk_app_src`），**不设置 `CONFIG_CHERRYSH`**，自研移植层接现有 `intf_usb_cdc`。

**配置头**：`csh.h` 无条件 `#include "csh_config.h"`，工程提供 `config/csh_config.h`（`config/` 已在 include 路径）。

**许可**：两组件均 Apache-2.0，SDK 内引用、源文件不改动；`config/csh_config.h` 由 SDK 模板派生，保留 Egahp/HPMicro 归属并注明工程修改。

### 3.2 工程配置（`config/csh_config.h`）

| 配置项 | 值 | 理由 |
| :--- | :--- | :--- |
| `CONFIG_CSH_NOBLOCK` | 1 | 非阻塞 repl（bare-metal 单循环） |
| `CONFIG_CSH_LNBUFF_STATIC` | 1 | 与 NOBLOCK 强制搭配（编译期 #error 校验） |
| `CONFIG_CSH_LNBUFF_SIZE` | 256 | 命令行最大长度 |
| `CONFIG_CSH_HISTORY` / `_BUF_SIZE` | 1 / 256 | 历史（↑/↓） |
| `CONFIG_CSH_COMPLETION` / `_MAX_COMPLETION` | 1 / 40 | TAB 补全 |
| `CONFIG_CSH_PROMPTEDIT` / `_BUF_SIZE` | 1 / 128 | 彩色提示符 |
| `CONFIG_CSH_MAXLEN_PATH` | 0 | const path，省 128B RAM（无文件系统需求） |
| `CONFIG_CSH_MAX_USER` | 1 | 无登录 |
| `CONFIG_CSH_MAX_ARG` | 8 | 参数上限 |
| `CONFIG_CSH_SYMTAB` | 1 | 命令表走链接段 |
| `CONFIG_CSH_USER_CALLBACK` | 1 | Ctrl-C 等用户事件钩子 |
| `CONFIG_CSH_PRINT_BUFFER_SIZE` | 512 | `csh_printf` 缓冲 |
| `CONFIG_CSH_XTERM` | 0 | 固定：`NOBLOCK=1` 时 XTERM 不可启用（CherryRL 编译期 `#error` 互斥），键位按 VT100 序列实测 |
| `CONFIG_CSH_NEWLINE` | `"\r\n"` | 串口终端约定 |
| `CONFIG_CSH_DEBUG` | 0 | 关闭参数检查（release） |

### 3.3 移植层（sput / sget / TX 环 / flush）

**关键事实**：release 配置（`CONFIG_CSH_DEBUG=0`）下 CherryRL 的 `chry_readline_put` 宏**忽略 `sput` 返回值**——因此 sput 必须保证"**要么全收，要么有界丢弃**"，不能依赖返回值重试。

```c
/* sput：输出回调（CherrySH → TX 环） */
uint16_t app_terminal_sput(chry_readline_t *rl, const void *data, uint16_t size) {
    if (!app_usb_is_dtr()) {
        return size;                 /* 主机未打开端口（DTR 未置位）：直接丢弃，不累积陈旧输出 */
    }
    uint32_t written = chry_ringbuffer_write(&s_tx_ring, (void *)data, size);
    if (written < size) {
        app_terminal_tx_flush_bounded();          /* 有界 flush（预算 ≤ 500 µs） */
        written += chry_ringbuffer_write(&s_tx_ring, (uint8_t *)data + written, size - written);
    }
    if (written < size) {
        s_tx_drop_bytes += (size - written);   /* 极端情况有界丢弃 + 计数 */
    }
    return size;                     /* 恒返回 size：调用方不重试、不挂死 */
}

/* sget：输入回调（驱动 RX 环 → CherrySH） */
uint16_t app_terminal_sget(chry_readline_t *rl, void *data, uint16_t size) {
    int n = app_usb_read((uint8_t *)data, size);
    if (n > 0 && app_terminal_job_is_active()) {
        app_terminal_job_abort();       /* 任意新输入中止前台 job */
    }
    return (n > 0) ? (uint16_t)n : 0;
}
```

- **TX 环**：cherryrb，容量 2 KB（`s_tx_pool[2048]`）；丢弃字节数 `s_tx_drop_bytes` 可在 `usb` 命令中查询。
- **flush**（1kHz）：`chry_ringbuffer_linear_read_setup` → `app_usb_write_timeout(..., 0)`（单次 ≤ 512B，端点忙返回 -1）→ `linear_read_done`；失败下拍重试。HS 下单拍可推 512B，2KB 环最多 4 拍清空。
- **输出门控用 DTR**（决策：使用者自行开启 DTR 或使用带 DTR 的串口工具）：DTR 未置位时直接丢弃输出，不累积陈旧数据。
- **重连处理**：DTR 上升沿 → 清 TX 环 + **欢迎界面** + 提示符重绘。欢迎界面（v4，ANSI 艺术字版）：
  - 主标题：**ANSI Shadow** 艺术字（pyfiglet `ansi_shadow`）——`Hajimi Dynamics` **单行不拆分**（110 列 × 6 行）；
  - 副标题：**ANSI Regular** 艺术字（pyfiglet `ansi_regular`）——`G6618`（38 列 × 5 行，尺寸明显小于主标题，形成层级）；
  - 作者/组织：`Kaiser @ Alliance HardwareGroup`；提示行：版本 + `help`；
  - 边框：UTF-8 单线制表符（╭─╮│╰╯），内宽 112，总宽 **114 列**，共 **17 行**（高度紧凑）；
  - 配色：**蒸汽波 256 色**（主标题纵向渐变 粉213→洋红207→紫141→靛99→蓝63→青51；副标题青45→青51→紫141→洋红207→粉213；边框横向 6 段渐变；作者薰衣草 141、提示薄荷 121）；
  - 兼容性：块字符（█ ╗ ═ 等）与制表符均为单列宽，需终端 **UTF-8** 编码；宽度按终端自适应——终端窄于 114 列时会折行（本版按"不压缩横向"决策，不做拆分）。
- **初始化不阻塞**：`app_terminal_init()` 不等枚举、不等 DTR（明确否决 SDK port 的 `while (!usb_device_is_configured)` 阻塞等待）；欢迎界面在 DTR 上升沿输出。

### 3.4 回调链（补全 / 用户回调）

CherrySH 初始化时设置了内部回调；`chry_readline_t` 结构体完全公开（`cplt.acb` / `ucb` 字段可见），初始化后可**链式接管**：

| 回调 | 接管方式 | 行为 |
| :--- | :--- | :--- |
| 补全 `cplt.acb` | 替换为 `app_terminal_completion`（自研上下文补全；`$` 变量前缀回退原回调） | ① 命令位（第 1 token）→ 命令名候选（**空前缀 = 全部**）；② 子命令位（第 2 token）→ 子命令表（motor/inv/adc/enc/fault/pwm/profiler/cal/reboot/param）；③ 参数位（第 3 token）→ `param get/set/reset` 参数名（元数据表）、`enc zero` → rotor/output、`param list` → 域；④ **重复 TAB 循环选择**：首次 TAB 列候选（内置公共前缀/列显），再次 TAB 用下一候选替换当前词（`chry_readline_edit_backspace/insert`），回车即执行；会话以（词起点/词/行长）失效检测 |
| 用户事件 `ucb` | 保存原回调，替换为 `app_terminal_user_cb` | `CHRY_READLINE_EXEC_SIGINT`（Ctrl-C）→ 优先级：**前台 job 中止**（如 `cal current`）→ **关闭 monitor 常驻状态区**（提示 `monitor: OFF (Ctrl-C)`）→ 无则交原回调；其余事件委托原回调 |

补全能力边界（CherryRL 无内建菜单选择）：循环选择为"词替换"式反馈（当前选中项直接显示在命令行中），不做列表重绘高亮（需自行维护终端光标记账）。

补全上下文读取：`rl->ln.buff->pbuf`（行缓冲）+ `rl->ln.buff->size`（行长）+ `rl->ln.curoff`（光标偏移）。

### 3.5 命令注册与链接段

- 注册宏：`CSH_CMD_EXPORT_FULL(func, name, usage, help)` → 放入 `FSymTab` 段；
- 链接脚本已具备 `KEEP(*(FSymTab))` 与 `__fsymtab_start/end`（已核实 `linkers/gcc/user_linker.ld`），**零链接脚本改动**；
- `help` 命令由 `builtin/help.c` 提供，自动列出 FSymTab 全部命令（含 usage/help）。

## 4. 执行模型与 FOC 隔离（硬约束）

### 4.1 运行位置与预算

- `app_terminal_run_once()` 位于 **1kHz 慢任务**（`app_logic.c` 主循环第 2 步），替换 `app_debug_usb_run_once()`；在控制路径（编码器/模拟量/故障/电机）之后执行。
- 顺序：`task_exec`（执行已解析命令）→ `task_repl`（读取输入）→ job tick → TX flush → DTR 边沿处理。
- 单次预算 ≤ 200 µs；命令执行在同一上下文，命令实现必须有界（大输出经 TX 环吸收）。
- ISR 零 terminal 逻辑（USB IRQ 仅填驱动环形缓冲，现有行为不变）。

### 4.2 job 框架（长命令）

```c
typedef struct {
    const char *name;              /**< job 名（提示用） */
    void (*tick)(uint32_t now_ms); /**< 1kHz 周期回调 */
    void (*abort)(void);           /**< 中止回调（可 NULL） */
    bool active;                   /**< 运行标志（框架维护） */
} app_terminal_job_t;

int  app_terminal_job_start(const app_terminal_job_t *job); /* 已有 job 先中止 */
void app_terminal_job_tick(uint32_t now_ms);             /* 1kHz 驱动 */
void app_terminal_job_abort(void);
bool app_terminal_job_is_active(void);
```

- 单前台 job；中止触发源：Ctrl-C（§3.4）、sget 收到新输入（§3.3，触发键被吞掉不落入命令行）、新 job 启动。
- 已接入 job：`monitor`（2Hz 刷新）、`cal current`（256 帧标定状态机，非阻塞）；后续标定/整定流程复用。
- 命令函数本体必须快速返回（禁止阻塞等待）；长流程一律 job 化。

### 4.3 中断优先级

PLIC 语义：数字越大优先级越高（见 `drv_adc.c` 注释）。

| 中断源 | 现状 | 目标 | 时机 |
| :--- | :--- | :--- | :--- |
| ADC0（电流环触发） | 2 | 3 | FOC 落地 |
| PWM1（逆变桥 reload） | 1 | 3 | FOC 落地 |
| GPTMR | 3 | 2 | FOC 落地（让位电流环） |
| PWM0（调试） | 2 | 2 | 不变 |
| **USB0（Terminal）** | 2 | **1** | **本次** |
| ADC1 / MCAN / UART / GPIO | 1 | 1 | 不变 |

**硬约束**：任何 Terminal/通讯相关中断优先级必须低于控制类（ADC0/PWM1）；本次先降 USB0 消除"USB 与 ADC0 同级"的隐患。

### 4.4 flash 操作纪律

- `param save/load` 为 v2；本次先立纪律：
  1. flash 操作仅允许在**电机停止**窗口（命令前置检查，违反则拒绝并提示）；
  2. flash 操作会阻塞 XIP 取指（`rom_xpi_nor_erase/program` 轮询），必须在输出中提示预计停顿；
  3. 控制 ISR 与热路径保持 ILM 部署（现有 `.fast` 机制不变）。

### 4.5 验收

- 主循环 late% 相对基线（当前 ~4%）劣化 ≤ 0.5 个百分点；
- IrqProfiler 快照无新增异常（无长 ISR、无优先级反转）；
- 4KB 粘贴压力测试不挂死（丢字节计数可观测）；
- `monitor` 2Hz 运行 10 分钟，控制环 late% 无劣化。

## 5. 命令集 v1

### 5.1 命令清单

| 命令 | 用法 | 说明 |
| :--- | :--- | :--- |
| `help` | `help [cmd]` | CherrySH builtin，列出命令/用法 |
| `ver` | `ver` | 固件版本、构建信息、板名 |
| `status` | `status` | 单次系统快照（状态/故障/电流/母线/转速/温度） |
| `monitor` | `monitor` | 实时仪表盘（job，§7） |
| `param` | `param list [域]` / `param get <name>` / `param set <name> <value>` / `param reset [name\|域]` | §6 |
| `fault` | `fault show` / `fault clear` | `app_debug_fault_dump/clear` |
| `adc` | `adc dump` / `adc diag` / `adc delay <ns>` | `app_debug_adc_*` / `app_adc_set_trigger_delay_ns` |
| `pwm` | `pwm dump` | `app_debug_hrpwm` |
| `enc` | `enc info` / `enc zero <rotor\|output>` / `enc clear` | `app_encoder_*` |
| `motor` | `motor [status]` / `motor start\|stop` / `motor freq [<hz>\|+\|-]` / `motor mod [<pct>\|+\|-]` | `app_debug_motor_*`（V/F 自检；无参=查询，数值=直接设置，±=步进） |
| `inv` | `inv <u\|v\|w\|all\|off>` | `app_debug_inverter_set_output` |
| `cal` | `cal current` | `app_analog_signal_calibrate_{start,step}`（job 驱动，非阻塞） |
| `profiler` | `profiler dump` | IrqProfiler |
| `usb` | `usb` | DTR/枚举状态、TX 丢弃计数、terminal 统计 |
| `history` | — | **暂缓**：↑/↓ 已提供历史导航；列表需按 CherryRL 历史环内部格式实现（后续按需） |
| `reboot` | `reboot confirm` | 复位（联锁 + 确认；`intf_sys_reset`） |

### 5.2 单字符命令映射（UART 侧保留，USB 侧升级）

| 现有（单字符） | 新命令 | 后端 |
| :--- | :--- | :--- |
| `z` / `o` / `c` / `i` | `enc zero rotor` / `enc zero output` / `enc clear` / `enc info` | `app_encoder_*` |
| `1` / `2` / `3` / `a` / `0` | `inv u` / `inv v` / `inv w` / `inv all` / `inv off` | `app_debug_inverter_set_output` |
| `r` / `+` / `-` / `m` / `M` | `motor start` / `motor stop` / `motor freq ±` / `motor mod ±` | `app_debug_motor_*` |
| `d` / `p` | `adc dump` / `adc diag` | `app_debug_adc_*` |
| `k` | `adc delay <ns>` | `app_adc_set_trigger_delay_ns` |
| `n` | `cal current` | `app_analog_signal_calibrate_offsets` |
| `f` / `F` | `fault show` / `fault clear` | `app_debug_fault_*` |

### 5.3 命令实现规范（为后续增加做准备）

```c
/* 模板：新命令 */
static int cmd_xxx(int argc, char **argv) {
    chry_shell_t *csh = app_terminal_cmd_ctx(argc, argv);   /* CherrySH 约定：末位存实例指针 */
    if (argc < 2) {
        return app_terminal_cmd_usage(csh, "xxx <arg>");    /* 统一 usage 输出 */
    }
    ...
    return 0;                                            /* 0 = 成功 */
}
CSH_CMD_EXPORT_ALIAS(cmd_xxx, xxx, );
```

- **实现事实**：本 SDK 的 CherrySH 版本无 `CSH_CMD_EXPORT_FULL`（无 usage/help 元数据）——`help` 仅列出命令名；每个命令在缺参/非法参数时自行打印 usage；
- 约定：输出用 `csh_printf(csh, ...)`；参数解析用 `app_terminal_cmd` 工具（整数/浮点/枚举）；
- 安全操作前置检查走 `app_terminal_cmd` 联锁 helper（§8）；
- 新命令按域归入 `app_terminal_cmd_{sys,diag,param,motor}.c`，跨域则新建同前缀文件并登记本文档。

## 6. 参数元数据与 param 命令

### 6.1 生成器扩展（`params_meta_generated.{c,h}`）

`gen_params.py` 增加元数据表生成；生效方式在生成器内以 `APPLY_REBOOT` 集合声明（与 `HEX_FIELDS` 同模式——类型/范围等结构属性同样在 SCHEMA 中维护，YAML 保持不变；缺省 `live`）。

```c
typedef enum { PARAM_META_DOMAIN_MOTOR = 0, PARAM_META_DOMAIN_HARDWARE, PARAM_META_DOMAIN_SOFTWARE } param_meta_domain_t;
typedef enum { PARAM_META_TYPE_U8 = 0, PARAM_META_TYPE_U16, PARAM_META_TYPE_U32, PARAM_META_TYPE_F32 } param_meta_type_t;
typedef enum { PARAM_META_APPLY_LIVE = 0, PARAM_META_APPLY_REBOOT } param_meta_apply_t;

typedef struct {
    const char *name;   /**< 点分路径（嵌套展开），如 "control.current_loop.kp" */
    uint16_t offset;    /**< 所属域结构体内偏移（offsetof） */
    uint8_t type;       /**< param_meta_type_t */
    uint8_t domain;     /**< param_meta_domain_t */
    uint8_t apply;      /**< param_meta_apply_t */
    const char *unit;   /**< 单位（可为 NULL） */
} param_meta_t;

extern const param_meta_t g_params_meta[];
extern const uint32_t g_params_meta_count;
```

- 嵌套结构展开为点分路径（如 `encoder.rotor_ratio`）；表达式字段不生成（只生成存储字段）；
- 自测扩展（`scripts/test_gen_params.py`）：元数据数量/名称/偏移/类型与生成结构体一致性校验用例。

### 6.2 单例 RAM 实例与消费者迁移

每域模块新增（三域同构）：

```c
void app_motor_params_init(void);                          /* boot 调一次：load 工厂值 */
const app_motor_params_t *app_motor_params_current(void);  /* 消费者只读 */
app_motor_params_t *app_motor_params_mutable(void);        /* 仅 Terminal 调试路径写入 */
```

消费者迁移（机械改动，逐处核对读取时机）：

| 消费者 | 迁移 |
| :--- | :--- |
| `app_logic`（启动摘要、节拍派生） | `_current()` |
| `app_adc` / `app_analog_signal` | `_current()` |
| `app_fault`（阈值/WDOG） | `_current()` |
| `app_can`（波特率/ID） | `_current()` |
| `app_debug_motor` / `_inverter` / `_hrpwm` / `_encoder` / `_adc` / `_can` | `_current()` |

原则：**读取时机不变**——原先 boot 缓存一次的（如 PWM 频率）保持 boot 读取（apply=reboot）；每拍/每次使用读取的改指针读取（apply=live）。

### 6.3 apply 语义

| 值 | 含义 | Terminal 行为 |
| :--- | :--- | :--- |
| `live` | 消费者经 `_current()` 实时读取，`set` 立即生效 | 输出 `OK` |
| `reboot` | 消费者 boot 缓存，`set` 仅改 RAM | 输出 `OK (reboot 生效)` |

审计结果（2026-09-21，`gen_params.py` 的 `APPLY_REBOOT` 集合，共 20 项）：
- `reboot`：`hardware.{adc.sample_cycle, adc.trigger_delay_ns, inverter.*, current_sense.{shunt_ohm,amp_gain,bias_v}, vbus_sense.{divider_*,}, canid_dip.levels}`、`software.can.*`、`software.fault.*`；
- `live`（31 项）：`hardware.{current_sense.a_per_volt, vbus_sense.v_per_volt, ntc.*}`（逐次使用读取，支持在线调整）、`motor.*` 与 `software.control.*`（FOC 后续消费者，经 `_current()` 读取）。

### 6.4 param 命令行为

```text
$ param list hardware
hardware.current_sense.shunt_ohm      0.002000   Ω
hardware.current_sense.amp_gain       7.500000
...
$ param get control.current_loop.kp
control.current_loop.kp = 0.000000        [live]
$ param set control.current_loop.kp 0.35
OK  control.current_loop.kp = 0.350000 (RAM)
$ param reset control.current_loop.kp
OK  control.current_loop.kp 恢复工厂值 0.000000
```

- 解析：整数 `strtoul`（拒绝负号）、浮点 `strtof`；**范围校验**用元数据 `min`/`max`（由 SCHEMA 范围生成，`±INFINITY` = 无界；F32 额外要求 `isfinite`）；非法/越界不写入；
- `param list` 输出含 `apply` 标记；`param get` 显示单位与标记；
- 安全参数警告见 §8；`param set` 只写 RAM，持久化 v2。

### 6.5 v2 预留（flash 持久化）

- `app_param` 每域一个 key（`MOTP`/`HWPP`/`SWPP`），存结构体整块 + **布局版本守卫**（生成器输出结构体版本常量，不匹配则忽略并提示）；
- `load()` 合并语义：工厂默认 + flash 覆盖（字段级）；
- 接口预留：`app_terminal_cmd_param.c` 内命令位已留 `save`/`load` 子命令，v2 接入即可。

## 7. monitor 常驻状态区（L2）

- 形态：**列表式状态区**（一值一行，标签 + 管道符 `|` + 颜色），位于提示符**上方**；**显示期间 REPL 可正常输入/执行命令**（非 job，不吞输入、不阻塞）。
- 命令：`monitor`（切换）/ `monitor on|off`；**Ctrl-C 关闭**（job 优先：若 `cal current` 在跑，第一次 Ctrl-C 中止标定，再次 Ctrl-C 关闭状态区；行非空时 Ctrl-C 仅清行，再按一次生效）。
- **关键约束**：CherryRL 的 `chry_readline_edit_refresh` 结尾会执行 `\033[0J`（擦除光标下方）——因此状态区必须画在提示符**上方**，且不能依赖"下方区域"。
- 绘制路径（仅相对光标移动，无需终端位置查询；用输出流换行计数区分）：
  - **空闲**（自上次绘制后无换行输出）：原位更新 —— `\033[<N>A` 上移到状态区首行 → `\r\033[J` 擦除（含旧提示符行）→ 重绘 N 行 → `refresh`（提示符回到状态区下方）；
  - **有输出/首帧**：插入 —— `\r\033[<N>L` 在提示符上方插入 N 行（原提示符行下移）→ 重绘 → `refresh`；命令输出自然累积在状态区上方，屏幕正常滚动。
  - 停用：`\033[<N>A` + `\r\033[<N>M` 删除状态区 → `refresh`。
- 换行计数：`app_terminal_output()` 统计输出流中的 `'\n'`（`app_terminal_get_line_feeds()`）；键盘回显/提示符重绘无换行（不影响原位更新），命令输出/回车/横幅有换行（触发插入路径）。
- 刷新：2 Hz（1kHz 慢任务驱动）；数据源：`app_fault_*`（状态/码/位名）、`app_analog_signal`、`app_encoder_read_deg`、`g_enc_loop_late_us`。
- 布局（12 行；标签青色，故障行整行底色，LATCH 非零红字，LATE 非零黄字）：

```text
 FAULT  | OK                       ← 绿底（WARN=黄底 / FAULT=红底加亮）
 LATCH  | none                     ← 非零时红字
 I_U    |     0.312 A
 I_V    |    -0.185 A
 I_W    |    -1.266 A
 V_BUS  |     23.77 V
 NTC0   | 6291481.5 ohm
 NTC1   | 1000000.0 ohm
 ENC_R  |     123.45 deg
 ENC_O  |     -45.67 deg
 PARAM  | valid
 LATE   |      0 us                 ← 非零黄字
```

- 异步输出约定：job 等异步消息以 `\r\n` 起头（避免粘在提示符行）；`cal current` 已按此约定。

- 所需 getter 清单在实施期核对，缺失的按最小面在 Platform 层补齐（不读 Debug 层内部变量）。

## 8. 安全联锁

| 命令/操作 | 前置条件 | 违反行为 |
| :--- | :--- | :--- |
| `motor` / `inv` / `cal` | 无故障锁存 | 拒绝 + 提示 |
| `inv` / `motor` 驱动类 | 先停旋转（沿用现有语义） | 内部先 stop |
| `enc zero` / `enc clear`（flash 写） | 电机停止 | 拒绝 |
| `reboot` | 电机停止 + 二次确认 | 取消 |
| `param set`（安全参数） | — | 警告输出（仍执行） |

安全参数清单：`fault.oc_trip_a`、`fault.vbus_ov_v`、`fault.vbus_uv_v`、`hardware.inverter.pwm_freq_hz`、`hardware.inverter.deadtime_ns`、`control.limits.duty_max`、`control.limits.i_q_max_a`。

## 9. 迁移与退役

| 项 | 处理 |
| :--- | :--- |
| `App/Debug/{Inc,Src}/app_debug_usb.*` | **删除**（回显/心跳自检被 Terminal 取代；DTR 状态并入 `usb` 命令） |
| `app_logic.c` 初始化 | `app_debug_usb_init()` → `app_usb_init()`（Platform）+ `app_terminal_init()`（Comm）；参数域 `_init()` 前置 |
| `app_logic.c` 1kHz 慢任务 | `app_debug_usb_run_once()` → `app_terminal_run_once()` |
| `app_debug_cmd.*` / `app_debug_uart.*` | 保留（UART 现状） |
| `CMakeLists.txt` | 新增 Comm 源清单；引用 SDK cherrysh/cherryrb 源；`config/` 已在 include 路径 |
| `AGENTS.md` 项目附加约定 | 新增 Comm 层定义/依赖方向/通讯子系统约定/第三方豁免（SDK CherrySH、cherryrb 源文件保持原样） |

## 10. 资源与风险

| 项 | 估算 | 余量 |
| :--- | :--- | :--- |
| Flash | +8~15 KB（CherrySH + cherryrb + Comm + 命令） | 余 843 KB |
| RAM | ~3 KB（TX 环 2KB + 行缓冲 256B + 历史 256B + 提示符 128B + 实例） | 余 62.8 KB |

| 风险 | 缓解 | 验证 |
| :--- | :--- | :--- |
| sput 短写导致输出截断（release 下无返回值检查） | 环 + 有界 flush + 丢弃计数；输出量控制 | 4KB 粘贴压力 + `usb` 计数 |
| CherrySH 命令执行占用 1kHz 槽 | 命令有界化 + job 框架 + 单次预算 | late% 对比 + IrqProfiler |
| MobaXterm 键序列差异（Home/End/箭头） | VT100/xterm 序列兼容性实测；XTERM 与 NOBLOCK 编译期互斥，不可作备选 | §11 第 3 项实测 |
| `_current()` 迁移面广 | 机械迁移 + 编译/启动摘要/台架回归 | 全量回归 |
| 补全回调链在 CherrySH 版本升级后失效 | 字段可见性为当前版本事实；升级 SDK 时回归 | 补全实测 |
| 断线/重连输出错乱 | DTR 上升沿清环 + banner | §11 第 10 项 |

## 11. 验收标准

1. MobaXterm 打开端口 → 出现提示符；`help` 列出命令（含 usage）；
2. TAB 补全：命令名可用；`param get/set` 参数位补全参数名；
3. 键位：↑/↓ 历史、←/→/Home/End 行内编辑正常（XTERM 与 NOBLOCK 互斥，固定 VT100 模式）；
4. `param list/get/set/reset` 行为正确（类型校验、apply 标记、安全参数警告）；
5. `monitor` 2 Hz 刷新，Ctrl-C 与任意键均可退出；
6. `motor`/`inv`/`cal`/`enc`/`fault`/`adc`/`pwm` 命令与单字符命令行为一致（同后端）；
7. 4 KB 粘贴不挂死，`usb` 显示丢弃计数（正常为 0）；
8. UART 单字符命令与周期输出不受影响；
9. 主循环 late% ≤ 基线 + 0.5pp；IrqProfiler 无新增异常；
10. 断线重连：重新打开端口后 banner/提示符正常、无陈旧输出；
11. 故障锁存时驱动类命令被拒绝；
12. 停机联锁：电机旋转中 `enc zero` / `reboot` 被拒绝。

## 12. 实施顺序

| 步 | 内容 | 验证点 |
| :--- | :--- | :--- |
| 1 | CMake + `config/csh_config.h` + 核心编译 + 最小 sput/sget（直写 USB）+ 提示符 | 终端出现提示符、可回显 |
| 2 | TX 环 + 有界 flush + DTR banner + 回调链（补全/用户回调） | 粘贴压力、补全/历史/键位 |
| 3 | job 框架 + `app_terminal_cmd` 工具 | Ctrl-C 中止演示命令 |
| 4 | sys/diag 命令（ver/status/usb/reboot/history/adc/pwm/enc/fault/profiler） | 命令行为与单字符一致 |
| 5 | 参数元数据生成 + `param` 命令 + `_current()` 迁移 | 自测 13+、`param` 全流程、启动摘要回归 |
| 6 | motor/inv/cal + 安全联锁 | 联锁用例 |
| 7 | monitor + 台架验收（late%/IrqProfiler/断线重连）+ 文档与 AGENTS.md | §11 全项 |

## 13. 变更记录

| 日期 | 变更 | 说明 |
| :--- | :--- | :--- |
| 2026-09-21 | 初稿 | 决策记录见 §0 |
| 2026-09-21 | 实施修订 | ① 输出门控改回 DTR（决策）；② 本 CherrySH 版本无 `CSH_CMD_EXPORT_FULL`，改用 `CSH_CMD_EXPORT_ALIAS` + 命令自述 usage；③ XTERM 与 NOBLOCK 编译期互斥（固定 0）；④ `history` 命令暂缓；⑤ 参数元数据名称采用域限定全名（`<域>.<路径>`）；⑥ `apply` 审计定稿（20 reboot / 31 live）；⑦ 新增 `intf_sys_reset`（reboot 命令）、`app_debug_set_writer`（dump 旁路）、`app_debug_motor_is_running`（联锁） |
| 2026-09-21 | 评审修复 | ① `cal current` 改 job 驱动（`app_analog_signal_calibrate_{start,step,cancel}`，消除 10~50ms 阻塞）；② `enc zero/clear` 增加 flash 停顿提示；③ `inv off` 解除故障联锁（安全优先：off 始终可用）；④ `param set` 增加元数据 min/max 范围校验 + F32 `isfinite`；⑤ reboot 参数提示改为"boot 期消费、不持久化"；⑥ `hash[0]=""`（消除 NULL 读取）；⑦ flush 预算改为**单拍全局 200µs**（原为单次调用 500µs）；⑧ 参数补全支持空前缀（列全部）；⑨ job 活动时 sget 吞掉触发键；⑩ `parse_u32` 拒绝负号 |
| 2026-09-21 | 台架修复（首轮） | **现象**：提示符/回显正常但所有命令 "command not found"。**根因**：CherrySH 命令查找经 `PATH` 环境变量（VSymTab）解析路径，移植层未导出 PATH（变量表为空 → `PATH = NULL` → 全部解析失败）；SDK port 样例正是靠 `CSH_RVAR_EXPORT(ENV_PATH, PATH, ...)` 工作。**修复**：`app_terminal.c` 导出只读变量 `PATH="/sbin:/bin"`（/bin = 工程命令，/sbin = builtin help）+ init 期空表防御告警（RTT）；顺带新增 `clear` 命令（16 命令） |
| 2026-09-21 | 台架 UX（二轮） | ① 补全支持**空前缀列出全部**：CherrySH 原回调对空前缀返回 0（不列出），改为自实现命令名补全（首个 token，含空前缀）+ 参数名补全已支持空前缀；② `motor` 语法简化：`motor [status]` 无参查询、`motor freq <hz>` / `motor mod <pct>` 直接设置（`+`/`-` 仍为步进）、`motor help` 打印用法；③ 新增 `app_debug_motor_get_state/set_freq/set_mod`（Platform→Debug 层） |
| 2026-09-21 | 台架 UX（三轮） | ① **子命令补全表**（命令→子命令、命令+子命令→参数），解决参数位误出命令名（`motor s<TAB>` 曾补出 `status`）与子命令无补全；② **重复 TAB 循环选择**：首次 TAB 列候选、再次 TAB 逐项替换当前词、回车执行（会话含词/行长失效检测）；③ 参数位不再回退原回调（仅 `$` 变量前缀回退） |
| 2026-09-21 | 台架 UX（四轮） | ① `enc info` 十六进制计数 → **十进制角度**（`read_deg`，含 counts/zero）；`enc zero` 输出同步改十进制；② monitor 重排：**故障状态独立成行 + 颜色高亮**（OK 绿底 / WARN 黄底 / FAULT 红底加亮，含触发位名与 LATCHED），物理量行随其后；③ 新增 `app_terminal_cmd_fault_codes_text()`（位图→位名，`app_fault.h` 位定义驱动）；④ `status` 故障行附位名 |
| 2026-09-21 | 台架 UX（五轮） | monitor 重构为**常驻状态区**：① 非 job，显示期间 REPL 可正常使用（输入不再中止显示）；② 列表化布局（12 行、一值一行、标签青色 + 管道符 + 状态底色）；③ 状态区画在提示符**上方**（CherryRL refresh 会擦除光标下方），空闲原位更新 / 有输出插入重绘（输出流换行计数区分）；④ 命令改 `monitor [on\|off]`（切换，off 时删除状态区）；⑤ 异步输出约定：job 消息以 `\r\n` 起头（避免粘在提示符行） |
| 2026-09-21 | 台架 UX（六轮） | 连接欢迎界面：DTR 上升沿输出 **ASCII 艺术字 banner**（pyfiglet "small"：Hajimi Dynamics 主标题 60 列 / G6618 副标题 / Kaiser @ Alliance HardwareGroup / 提示行；青色主标题、白色副标题、绿色提示；逐行写入 TX 环） |
| 2026-09-21 | 台架 UX（七轮） | 欢迎界面升级：① **alligator 艺术字**（Hajimi，73 列）+ `D Y N A M I C S` 字距展开（alligator 原生 "Dynamics"=89 列超宽，拆分以保证字形正确）；② **UTF-8 边框**（╭─╮│╰╯，内宽 74）；③ **蒸汽波 256 色配色**（标题纵向渐变 + 边框横向 6 段渐变 + 霓虹强调色）；④ 逐行 ≤ 78 列 |
| 2026-09-21 | 台架 UX（八轮） | 欢迎界面换用 **3D-ASCII** 风格（pyfiglet `3d-ascii`）：主标题 Hajimi / Dyna / mics 三行（"Dynamics" 91 列 → 按可读性拆分）、副标题 G6618（反序渐变）；配色方案不变（蒸汽波 256 色）；无空行紧凑排版（约 33 行）；单行 ≤ 78 列 |
| 2026-09-21 | 台架 UX（九轮） | 欢迎界面换用 **ANSI 艺术字**：主标题 `ansi_shadow`（Hajimi Dynamics **单行 110 列**，不拆分）+ 副标题 `ansi_regular`（G6618，38×5，尺寸层级分明）；边框内宽 112 / 总宽 114 列、共 17 行；配色不变（蒸汽波 256 色）；决策：不压缩横向尺寸，按终端自适应 |
| 2026-09-21 | 提交前评审修复 | ① TX 环 2KB → **8KB**（欢迎界面实测 4096B，避免冷重连截断与丢字计数）；② `cal current` 启动顺序改为 **先 job_start 再 calibrate_start**（避免旧 job 的 cancel 误杀新标定）；③ 补全会话支持**公共前缀扩展**后的继续循环；④ `parse_float` 拒绝 nan/inf；⑤ `param list` 域过滤加 `.` 边界；⑥ 文档漂移修正（monitor 位于提示符上方/补全能力描述/AGENTS.md 契约表述） |
| 2026-09-21 | 模块化改名 | Comm 层按模块分子目录（`terminal/` + `can/` 预留）；模块术语 shell → **terminal**（文件/符号 `app_shell_*` → `app_terminal_*`、`APP_SHELL_*` → `APP_TERMINAL_*`、`s_shell` → `s_csh`、`s_shell_job` → `s_active_job`）；文档同步改名 `2026-09-21-usb-terminal-design.md`；CherrySH/csh_*/chry_shell 库标识符保持不变 |
| 2026-09-21 | 台架 UX（十轮） | **Ctrl-C 关闭 monitor 状态区**：用户回调优先级 job → monitor（提示 `monitor: OFF (Ctrl-C)`）；`monitor on` 提示同步更新 |
