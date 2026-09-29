# Gui 上位机软件设计：Flutter + Rust（v1）

- 日期：2026-09-29
- 状态：待评审
- 决策人：Kaiser（2026-09-29）
- 关联文档：
  - `docs/superpowers/specs/2026-09-20-param-pipeline-design.md`（参数元数据来源）
  - `docs/superpowers/specs/2026-09-21-usb-terminal-design.md`（terminal 命令与联锁语义、CAN 定位）
  - `docs/superpowers/specs/2026-09-19-fault-protection-design.md`（故障联锁来源）

## 0. 决策记录（Kaiser，2026-09-29）

| 决策点 | 结论 |
| :--- | :--- |
| 交付节奏 | A（调参 + 状态监视）→ B（实时曲线）→ C（录制/标定向导）分步交付 |
| 技术栈 | Flutter（UI）+ Rust（通讯核），经 `flutter_rust_bridge` 桥接 |
| 主通讯链路 | USB bulk |
| 备选通讯链路 | UART / USB CDC 虚拟串口 |
| CAN 链路 | 经独立 CAN 适配器小板（自制 USB 转 CAN 或无线 CAN，ESP32 / HPM 单片机）；上位机面向「适配器链路」，适配器固件是独立项目 |
| 平台 | Windows / Linux 桌面为主；Android 预留；iOS 仅网络链路（系统无自定义 USB Host） |
| 开发路径 | 方案 1：契约先行 + 设备模拟器驱动（融合纵切片交付节奏） |
| 命名 | 产品名 **Gui**（文档全称 "G6618Motor Gui"）；目录 `Gui/`；Dart 包与可执行文件 `g6618_gui`；协议 C 头 `motor_proto.h` |
| 分支策略 | `feat/host-tool`（自 `main` a8c5d2c），完成后 PR 合回 `main` |
| 示波器数据形态 | 协议定义两级流（原始流 + 降采样流），一期只实现降采样流 |
| 框架选型依据 | Qt6 / Slint / EUI-NEO 均评估否决或降级，依据见 §12 |

## 1. 背景与目标

### 1.1 背景

- 固件侧已具备：参数管线（三域 YAML → `params_generated` + `params_meta_generated`）、USB CDC Terminal（`param/motor/inv/cal/adc/pwm/enc/fault/profiler/monitor` 等 15 个命令）。
- 当前调参循环为「改 YAML → 重编译 → 烧录 → RTT 观测」，分钟级；缺少在线调参与可视化通道。
- 当前无对应硬件在手，主机侧软件需先于硬件开发（模拟器闭环）。
- 未来 FOC 落地后调参迭代密度大幅上升，需要参数面板 + 实时曲线 + 数据录制的工具支撑。

### 1.2 目标

1. **Gui A**：参数浏览/编辑/下发/落盘（元数据驱动）、状态监视、电机控制（联锁语义与固件 terminal 一致）。
2. **Gui B**：示波器——订阅式降采样流、多通道绘图、游标、CSV 导出。
3. **Gui C**：数据录制/回放、标定向导（电流零点 / 编码器零点）。
4. **无硬件开发闭环**：Gui ↔ TCP ↔ gui-sim（设备模拟器），A 阶段全部功能可在模拟器上验收。

### 1.3 非目标（v1）

- 固件侧协议服务端实现（本设计只锁契约与测试向量，见 §11）。
- FOC 参数自动整定向导、控制环辨识。
- Web 版、iOS 版、多人/网络化部署。
- CAN 适配器小板固件（独立项目，本设计只定义其对上位机呈现的字节流语义）。

## 2. 术语

| 术语 | 定义 |
| :--- | :--- |
| **Gui** | 本上位机软件（产品名，CamelCase） |
| **gui-core** | Rust 通讯核（lib crate）：传输/编解码/会话/设备模型/流 |
| **gui-sim** | Rust 设备模拟器（bin crate）：协议服务端参考实现 |
| **Transport** | 传输抽象接口：UsbBulk / Serial / Tcp / Loopback |
| **一级流** | 设备侧降采样流（订阅式，目标速率 1~5kHz 级） |
| **二级流** | 原始采样流（25kHz 全量，协议预留，v2 实现） |
| **适配器** | CAN 适配器小板：字节流 ↔ CAN 帧的桥接设备 |

## 3. 仓库布局与分支策略

- 分支：`feat/host-tool` 自 `main`（a8c5d2c），上位机全部开发在此分支，完成后 PR 合回 `main`；本分支不修改固件代码。

```text
Gui/                          # 上位机根（本分支唯一新增顶层目录）
├── protocol/                 # 协议契约 = 单一事实源
│   ├── motor_proto.h         # 共享 C 头：帧格式/命令字/结构体/错误码（固件将来直接 include）
│   ├── test_vectors.json     # 金测试向量（hex 帧 + 期望结果，Rust/固件两侧共用）
│   └── protocol_spec.md      # 人读协议规范 v1（本文件 §6 的展开）
├── core/                     # gui-core（Rust lib crate）
├── sim/                      # gui-sim（Rust bin crate，依赖 core）
├── app/                      # Flutter 应用（flutter create 生成平台目录）
├── tests/                    # 跨 crate 一致性/集成测试
└── Cargo.toml                # Rust workspace（core + sim）
```

要点：

1. **协议头是双端契约**：固件侧未来 `#include "motor_proto.h"` 实现协议服务端；gui-core 手写 Rust 镜像实现，两侧用 `test_vectors.json` 金向量锁一致（不做 bindgen，避免构建复杂化）。
2. **gui-sim 走 TCP loopback**（127.0.0.1），Gui 用真实 `TcpTransport` 连接——联调路径与将来无线适配器路径一致，无特例通道。
3. Flutter / Rust 边界固定为 `flutter_rust_bridge`，UI 层永不直接触碰传输。

## 4. 技术栈

| 层 | 选型 |
| :--- | :--- |
| UI | Flutter（Material 3，深色优先）+ Dart |
| 桥接 | `flutter_rust_bridge` |
| 通讯核 | Rust + tokio（异步 I/O） |
| USB bulk | `nusb`（Windows WinUSB / Linux libusb 兼容） |
| 串口 | `serialport` / `tokio-serial` |
| 网络 | tokio TCP |
| 构建 | Flutter SDK + Rust toolchain + Android NDK（Android 构建时），均可在本机经 scoop/官方安装 |

## 5. 通讯拓扑与 Transport 抽象

| 链路 | 载体 | Transport | 定位 |
| :--- | :--- | :--- | :--- |
| USB bulk | 直连控制板 | `UsbBulkTransport` | 主链路（桌面） |
| USB CDC / UART | 虚拟串口 / 串口线 | `SerialTransport` | 备选链路（调试/降级） |
| CAN 适配器 | 串口或 TCP（有线/无线）→ 适配器 → CAN 总线 | `CanAdapterTransport`（内部落 `Serial`/`Tcp` 字节流） | 调试与无线场景 |
| 模拟器 | TCP 127.0.0.1 | `TcpTransport` | 无硬件开发闭环 |
| 测试 | 进程内 | `LoopbackTransport` | 单测 |

平台事实（设计约束）：

- Windows / Linux 桌面：USB bulk 可用；Windows 首次需 WinUSB 驱动绑定（Zadig 或 inf），属部署文档事项。
- Android：USB Host（OTG）可用，走平台通道封装进同一 `Transport` 接口（后期）。
- iOS：无自定义 USB Host，仅 `TcpTransport`（无线适配器）可达。

## 6. 协议契约 v1（要点）

完整展开见 `Gui/protocol/protocol_spec.md`（M0 产出）。要点：

### 6.1 帧格式（小端）

```text
| SOF 0xA55A | VER(1) | CMD(1) | SEQ(1) | LEN(2) | PAYLOAD(0..512) | CRC16-CCITT(2) |
```

- 请求/响应按 `SEQ` 配对（u8 回绕）；上行主动帧（流/事件）占独立命令字空间。
- 收方以 SOF 重同步；CRC/长度错误丢帧计数。

### 6.2 命令集（A 阶段）

| 命令 | 语义 |
| :--- | :--- |
| `PROTO_INFO` | 版本协商（主版本不符即拒绝连接）、能力位、设备/固件标识 |
| `PARAM_META_COUNT` / `PARAM_META_GET` | 拉取参数元数据表（name/type/unit/min/max/apply）→ UI 自动生成参数面板 |
| `PARAM_READ` / `PARAM_WRITE` / `PARAM_COMMIT` | 按名读写（live/reboot 语义）；`COMMIT` 落 flash（停机窗口联锁） |
| `STATUS_GET` | 故障态 / 相电流 / 母线 / NTC / 编码器 / 电机状态 |
| `CTRL_CMD` | motor start/stop/freq/mod、inv 逐相；联锁语义与固件 terminal 一致（故障锁存拒绝） |
| `FAULT_GET` / `FAULT_CLEAR` | 故障显示 / 清除锁存 |
| `STREAM_SUB` / `STREAM_UNSUB` / `STREAM_FRAME` | 订阅通道 + 目标速率 → 一级流；二级流命令字预留（v2） |

### 6.3 错误码（响应 payload 内嵌）

`OK / BAD_CMD / BAD_PARAM / BUSY / LOCKED(联锁) / UNSUPPORTED`

### 6.4 传输映射

- bulk / CDC / UART：帧直载字节流，SOF 重同步。
- CAN 适配器：适配器将字节流分片过 CAN（适配器帧协议另立小 spec），对上位机呈现为字节流——**一套协议跑全部链路**。

## 7. gui-core 设计

单向依赖（上层不回头）：

```text
transport   trait Transport{open/read/write/close}，tokio 异步
            实现：UsbBulk(nusb) / Serial / Tcp / Loopback(test)
codec       帧编解码 + SOF 重同步 + CRC16 校验（纯函数，重测试）
session     SEQ 配对、请求超时(500ms)/重试策略、连接状态机、重连
device      参数注册表（元数据驱动）、类型化 get/set、状态/故障/电机控制模型
stream      订阅管理、降采样帧 → 环形缓冲、时间戳、丢帧检测
frb_api     flutter_rust_bridge 导出面（connect/device/streams → Dart Stream）
```

- Rust 核内部全 `Result` 上抛，无 panic；frb 边界统一错误类型 → Dart 异常。
- 流帧不占请求队列：示波器高负载不影响参数读写。

## 8. gui-sim 设备模拟器

- **参数同源**：gui-sim 用 Rust 直接解析 `config/{motor,hardware,software}.yaml`，校验规则与 `scripts/gen_params.py` 的 SCHEMA 同构（M1 以一致性测试锁定），参数表与真机同源——gui-sim 是协议服务端的参考实现，不是假数据。
- 监听 TCP 127.0.0.1，与真设备同一条 `TcpTransport` 路径；Gui 联调与 CI 测试全部走它。
- 合成信号源：正弦相电流 / 斜坡转速 / 母线波动，驱动 `STREAM_FRAME`（一级流）。
- 故障注入：过流锁存、编码器丢失、BUSY/LOCKED 场景，用于验证 UI 联锁与故障显示。
- 协议合规以 `test_vectors.json` 回归。

## 9. Gui（Flutter）UI 结构

```text
app/lib/
├── bridge/          # frb 生成的绑定 + 薄封装（唯一触碰 Rust 的地方）
├── state/           # 连接状态 / 参数注册表 / 状态流 / 故障（轻量状态管理）
└── ui/
    ├── connect/     # 连接管理：transport 选择(bulk/serial/tcp/can-adapter)、端口扫描、握手状态
    ├── param/       # A：参数面板——元数据自动生成（域树/搜索/类型与范围校验/
    │                #     live·reboot 徽标 / COMMIT 落盘二次确认 + 停机联锁提示）
    ├── dashboard/   # A：状态监视——故障横幅、电流/母线/NTC/编码器、电机控制面板（联锁同固件）
    ├── scope/       # B：示波器——订阅通道、一级流绘图、游标、CSV 导出
    └── tools/       # C：录制回放、标定向导
```

- **参数面板零硬编码**：UI 完全由设备元数据表生成，固件加参数上位机零改动。
- 状态栏常驻可观测性：连接态 / 重连计数 / CRC 错误计数 / 流丢帧计数。
- 主题单处定义：Material 3 深色优先（示波器场景）。

## 10. 数据流与错误处理

数据流（单向依赖，流与请求分离）：

```text
下行: UI 事件 → state → frb_api → session(SEQ/超时) → codec → transport
上行: transport → codec(CRC 门禁) → session ─┬─ 响应 → device 模型 → UI
                                             └─ 流帧 → stream 环形缓冲 → UI(scope)
```

连接状态机：

```text
Disconnected → Connecting → Handshake(PROTO_INFO) → Ready
                    │              │(主版本不符/超时 → 明确报错，不重试)
                    └→ Reconnecting（退避 0.5s→5s，恢复后自动重拉元数据 + 恢复订阅）
Ready → Degraded（流中断但请求可通，状态栏显式标注）
```

错误处理规则：

| 错误 | 处理 |
| :--- | :--- |
| CRC / 长度错 | 丢帧 + SOF 重同步，计数器 +1，状态栏常驻显示 |
| 请求超时 | **读**自动重试 2 次（幂等）；**写不重试**，UI 明示失败由人决定；`PARAM_COMMIT` 落 flash **绝不自动重试** |
| `LOCKED` 联锁拒绝 | 非错误：UI 显示原因（故障锁存 / 未停机），引导用户先 `FAULT_CLEAR` |
| 重连恢复 | 元数据重拉、订阅恢复；本地若有未下发的参数编辑（dirty）提示复核，不静默重发 |
| Rust 核内部 | 全 `Result` 上抛，无 panic；frb 边界统一错误类型 |

## 11. 测试策略与里程碑

测试金字塔（无硬件可跑完前四层）：

1. **协议金向量**：`test_vectors.json` 驱动 codec 单测（编码/解码/坏帧/重同步）。
2. **session 状态机**：超时/重试/SEQ 回绕/重连，`LoopbackTransport` 驱动。
3. **gui-sim 集成测试**：TCP 全链路——参数读写、COMMIT、CTRL 联锁拒绝、故障注入恢复、流订阅与丢帧。
4. **Flutter widget 测试**：元数据 → 参数面板生成、类型/范围校验、联锁提示。
5. **真板验收**（M2 后）：同一套集成测试换 transport 重跑。

里程碑：

| 里程碑 | 内容 | 验收 |
| :--- | :--- | :--- |
| **M0** | 分支 + `Gui/` 骨架 + `motor_proto.h` + `protocol_spec.md` + 测试向量 | spec 评审通过 |
| **M1** | gui-core + gui-sim + Gui 参数面板（**A 阶段**） | 模拟器上参数全流程 + 联锁/故障用例通过 |
| **M2** | dashboard 状态/电机控制 + 真板 bulk 接入 | 真板联调（依赖固件侧协议服务端） |
| **M3** | 示波器（**B 阶段**，一级流） | 模拟器流 + 真板流 |
| **M4** | 录制/标定向导（**C 阶段**）+ 移动端评估 | 按需 |

**对端依赖声明**：M2 依赖固件侧「USB bulk 端点 + `motor_proto` 服务端」，属固件后续工作（可能落 `feat/foc-v1` 或独立分支），本分支只锁协议头与测试向量。

## 12. 风险与决策依据

### 12.1 风险

| 风险 | 影响 | 缓解 |
| :--- | :--- | :--- |
| 固件侧协议服务端未实现 | M2 阻塞 | 契约先行 + gui-sim 保证 A 阶段不受阻；协议缺口在 M2 集中修一轮 |
| B 阶段示波器性能（尤其移动端） | 移动端体验 | 一级流设备侧降采样；桌面先行，移动端按「看板 + 调参」定位 |
| Windows USB bulk 驱动绑定 | 首次部署 | 部署文档写清 WinUSB 绑定（Zadig/inf）；CDC 备链路兜底 |
| 无线链路（CAN 适配器）带宽/时延 | 调参体验 | 一级流对无线优化；适配器协议独立分期 |
| Flutter 图表生态不足以支撑 scope | B 阶段工作量 | 自绘管线（环形缓冲 + 降采样 + CustomPainter），工作量计入 M3 |

### 12.2 框架选型依据（2026-09-29）

| 候选 | 否决/降级原因 |
| :--- | :--- |
| Qt6 + C++ | 可行但开发效率约 1/2~1/3；LGPL 合规成本；Kaiser 偏好现代 UI 与移动体验 |
| Slint + C++ | iOS 的 C++ 后端未支持（官方文档）；组件与图表生态单薄，移动需求下出局 |
| EUI-NEO + C++ | Apache-2.0 + serial 模块契合，但 0.x 版本 + 单人维护，长周期工具风险高（v0.5.0 有 API 破坏性变更先例）；留作 A 阶段试水备选，不入主线 |
| Flutter + Rust | 移动端一等公民 + UI 上限最高；硬件 I/O 弱项由 Rust 通讯核承担，短板全部绕开 → **采纳** |
