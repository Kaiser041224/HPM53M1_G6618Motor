# G6618Motor Gui

G6618Motor 上位机软件（产品名 **Gui**）：面向电机控制板的参数配置与状态监视工具。
本目录当前交付 **Rust 通讯核 + 设备模拟器**（spec 的 M0 + M1-Rust 部分），
在无硬件条件下即可跑通「握手 / 元数据 / 参数读写 / 落盘 / 联锁 / 故障」全流程。

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
├── sim/               # crate gui-sim：设备模拟器（bin gui-sim + lib）
├── tests/             # crate gui-tests：跨 crate TCP 集成测试
└── Cargo.toml         # Rust workspace（成员：core / sim / tests）
```

规划中（后续计划）：`app/`（Flutter UI，经 `flutter_rust_bridge` 调用 gui-core）。

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
- **UI**：Flutter / `flutter_rust_bridge` 前端属后续计划；本目录只交付 Rust 通讯核。
- **参数取值范围**：模拟器从 YAML 加载的参数元数据 `min>max`，视为**不限制**；真实范围以固件元数据为准。
- **YAML 表达式值**：带引号的字符串（构建期表达式）在模拟器中**跳过**，不入参数表；
  这与固件参数管线 `scripts/gen_params.py` 的构建期求值不同。

## 许可证

BSD-3-Clause
