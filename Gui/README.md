# G6618Motor Gui

G6618Motor 上位机软件（产品名 **Gui**）：面向电机控制板的参数配置与状态监视工具。
本目录交付 **Rust 通讯核 + 设备模拟器**（spec 的 M0 + M1-Rust 部分），
在无硬件条件下即可跑通「握手 / 元数据 / 参数读写 / 落盘 / 联锁 / 故障」全流程；
并在此基础上交付 **A 阶段 Flutter 桌面界面**（`app/`，经 `flutter_rust_bridge` 调用通讯核）。

- 设计文档：`docs/superpowers/specs/2026-09-29-gui-host-tool-design.md`
- 实施计划：`docs/superpowers/plans/2026-09-29-gui-comm-core-and-sim.md`

## 目录结构

```text
Gui/
├── protocol/          # 协议契约（单一事实源，固件与上位机共用）
│   ├── motor_proto.h      # 双端契约 C 头：帧格式/命令字/结构体/错误码
│   ├── protocol_spec.md   # 人读协议规范 v1（字段表/状态码/传输映射）
│   └── test_vectors.json  # 金测试向量（hex 帧 + 期望值）
├── core/              # crate gui-core：通讯核（proto/codec/transport/session/params）
├── frb/               # crate gui-frb：flutter_rust_bridge 绑定层（面向上层 Dart）
├── sim/               # crate gui-sim：设备模拟器（bin gui-sim + lib）
├── tests/             # crate gui-tests：跨 crate TCP 集成测试
├── app/               # Flutter 桌面界面（g6618_gui），经 flutter_rust_bridge 调用 gui-frb
└── Cargo.toml         # Rust workspace（成员：core / frb / sim / tests）
```

## 环境要求

- **Rust**：经 rustup 安装的 stable 工具链，rustc/cargo **≥ 1.75**（workspace 使用 edition 2021
  与 trait 内 `impl Future` 返回位置类型）。
- **本机（无 MSVC link.exe）**：使用 GNU 工具链，在 `Gui/` 下设置一次覆盖即可：

  ```powershell
  rustup override set stable-x86_64-pc-windows-gnu
  ```

  仓库内不保存该设置，每个工作副本执行一次。

- **链接器遮蔽问题（重要）**：若 `w64devkit` 的 `gcc` 在 `PATH` 中，会遮蔽 rustup 自带的
  链接器（其缺少 `libgcc_eh`），导致 GNU 目标链接失败。需在**用户级**
  `%USERPROFILE%\.cargo\config.toml` 显式指定 rustup 工具链自带的链接器：

  ```toml
  [target.x86_64-pc-windows-gnu]
  linker = "<rustup 工具链>/lib/rustlib/x86_64-pc-windows-gnu/bin/self-contained/x86_64-w64-mingw32-gcc.exe"
  ```

  这是**机器本地配置**，不写入仓库（`Gui/.cargo/` 已在 `.gitignore` 中忽略）。

- **有 MSVC 的机器**：直接 `rustup default stable`（msvc）即可构建，无需上述覆盖与链接器配置。

## 构建与测试

在 `Gui/` 目录下执行：

```powershell
cargo build --workspace     # 编译 core / sim / tests
cargo test  --workspace     # 全量回归：协议金向量 / codec / session / sim / TCP 集成
```

## 运行设备模拟器

模拟器在 `127.0.0.1` 上以 TCP 提供协议服务端，Gui 通过真实 `TcpTransport` 连接，
联调路径与将来「无线适配器」链路一致：

```powershell
cargo run -p gui-sim -- --port 9000 --config ../config
```

- `--port`：监听端口，默认 `9000`；
- `--config`：参数 YAML 目录，默认 `../config`（即仓库根 `config/`，与真机参数同源）。

> `PARAM_COMMIT` 会把全部参数写入**进程当前工作目录**下的 `sim_params.json`
> （从 `Gui/` 运行时即 `Gui/sim_params.json`）。

## 图形界面（A 阶段）

Flutter 桌面端界面位于 `Gui/app/`，经 `flutter_rust_bridge` 调用 `gui-frb`（复用 `gui-core`
通讯核）。当前实现 **连接管理 / 参数面板 / Dashboard** 三页 + 常驻状态栏。

### 构建

在 `Gui/app/` 目录下依次执行：

```powershell
flutter pub get                              # 拉取依赖
flutter_rust_bridge_codegen generate         # 仅当 gui/frb 的 Rust API 变更后才需要
flutter build windows --release              # 生成 release 产物
```

`flutter_rust_bridge_codegen generate` 依据 `Gui/frb/src/api.rs` 重新生成
`Gui/app/lib/bridge/frb/` 下的绑定；**只改 UI 时无需执行**。

### 产物

```text
Gui/app/build/windows/x64/runner/Release/g6618_gui.exe
```

实测 Release / x64：`g6618_gui.exe` **90,624 字节（约 88.5 KiB）**；同目录下还需
`flutter_windows.dll`（21,274,112 B）、`gui_frb.dll`（3,886,341 B）与 `data/`
（含 `app.so` 5,620,616 B）。**分发时须整目录拷贝，不可只拷 exe。**

### 运行

1. 先启动设备模拟器（在 `Gui/` 下）：

   ```powershell
   cargo run -p gui-sim -- --port 9000 --config ../config
   ```

2. 运行 `Release\g6618_gui.exe`，在「连接管理」页填入地址 `127.0.0.1:9000` 后连接。

### 界面功能

- **连接管理**：TCP 链路今期可用；`USB bulk` / `UART` 为 M2 禁用项（列表可见但不可选）。
- **参数面板**：由设备元数据驱动自动生成（按域分组 + 搜索过滤）；每项带 `live` / `reboot`
  生效徽标；写入仅标记 dirty，须 `COMMIT` 落盘（联锁：故障锁存或运行中拒绝提交）。
- **Dashboard**：状态卡（母线电压 / 三相电流 / 频率 / 温度 / 编码器角度等）+ 控制
  （启动 / 停止 / 频率 / 调制）+ 联锁提示文案。

### 工具链版本（实测）

| 组件 | 版本 |
| :--- | :--- |
| Flutter | 3.47.5（stable） |
| Dart | 3.13.4 |
| VS Build Tools | 17.14.41（含 Windows 10/11 SDK，提供 `cl.exe` / `link.exe`） |
| flutter_rust_bridge（含 `_codegen`） | 2.13.0 |
| Rust | stable（`x86_64-pc-windows-gnu`，cargo 1.98.1） |

### 图形界面测试

在 `Gui/app/` 下：

```powershell
flutter test      # widget / AppState 单测
flutter analyze   # 静态检查（应无 issue）
```

## 协议契约入口

| 文件 | 作用 |
| :--- | :--- |
| `protocol/motor_proto.h` | **双端契约**：帧格式、命令字、状态码、类型/生效语义；固件将来直接 `#include`，gui-core 为其 Rust 镜像实现 |
| `protocol/protocol_spec.md` | 人读规范：字段逐字节说明、示例帧注解、传输映射、版本协商 |
| `protocol/test_vectors.json` | 金测试向量：独立 C 程序实算的 hex 帧，Rust 与固件两侧共用 |

帧格式（小端）：

```text
| SOF 0xA55A | VER(1) | CMD(1) | SEQ(1) | LEN(2) | PAYLOAD(0..512) | CRC16-CCITT-FALSE(2) |
```

## 已知限制

- **一级流（示波器）**：v1 不实现，`STREAM_SUB` / `STREAM_UNSUB` 返回 `UNSUPPORTED`（属 M3）。
- **传输链路**：当前仅 `LoopbackTransport` / `TcpTransport`；`UsbBulkTransport` / `SerialTransport`
  与真板接入属 M2，依赖固件侧协议服务端。
- **传输多态（M2 注意）**：`Transport` 使用 RPITIT（trait 内 `impl Future`），**非对象安全**，
  不能用 `Box<dyn Transport>`。M2 运行时多链路选择需以 `AnyTransport` 枚举包装或等价方案实现。
- **UI 平台**：**Windows x64 优先**（当前唯一验收平台）；Linux / macOS **未验收**（`flutter create`
  仅生成 `windows/` 平台工程，扩展需 `flutter create --platforms=linux,macos .`）；iOS / Android 移动端**未做**。
- **UI 传输链路**：连接管理页今期仅 **TCP** 可用；**USB bulk / UART 为 M2 禁用项**（依赖固件侧协议服务端）。
- **参数取值范围**：模拟器从 YAML 加载的参数元数据 `min>max`，视为**不限制**；真实范围以固件元数据为准。
- **YAML 表达式值**：带引号的字符串（构建期表达式）在模拟器中**跳过**，不入参数表；
  这与固件参数管线 `scripts/gen_params.py` 的构建期求值不同。

## 许可证

BSD-3-Clause
