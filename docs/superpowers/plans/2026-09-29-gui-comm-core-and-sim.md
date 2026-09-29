# Gui 通讯核与设备模拟器（M0 + M1-Rust）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付 `gui/` 工作区：协议契约三件套（`motor_proto.h` + `protocol_spec.md` + `test_vectors.json`）、gui-core（codec/transport/session/params）、gui-sim（TCP 设备模拟器）及全链路集成测试——无硬件即可运行的完整 A 阶段 Rust 侧。

**Architecture:** 契约先行：`motor_proto.h` 是双端唯一事实源，gui-core 手写 Rust 镜像，用独立计算的金向量锁一致。传输抽象 `Transport`（Loopback/Tcp，UsbBulk/Serial 属 M2 计划）→ codec（SOF 重同步 + CRC）→ session（SEQ/超时/重试）→ params（元数据驱动）。gui-sim 复用 `config/*.yaml` 做协议服务端参考实现，集成测试走真实 TCP 路径。

**Tech Stack:** Rust (edition 2021, rustc ≥ 1.75)、tokio、serde/serde_yaml/serde_json。Windows PowerShell 命令行。

**Spec:** `docs/superpowers/specs/2026-09-29-gui-host-tool-design.md`（下称「spec」）

**范围边界（分期说明）：** 本计划 = spec M0 + M1 的 Rust 部分（§3/§4/§5(部分)/§6/§7(部分)/§8/§11 前四层测试）。Flutter UI（spec §9）属计划 2；`UsbBulkTransport`/`SerialTransport` 与真板接入属 M2 计划；`stream`（一级流）属 M3 计划；`frb_api` 属计划 2。

## Global Constraints

- 命名：产品 **Gui**；目录 `gui/`；协议头 `motor_proto.h`；crate 包名 `gui-core` / `gui-sim` / `gui-tests`（Rust 标识符 `gui_core` / `gui_sim` / `gui_tests`）。
- 协议 v1 硬常量：`VER=0x01`，`SOF=0xA55A`，CRC16-CCITT-FALSE（init `0xFFFF`，poly `0x1021`，无反转，结果不取反），**小端**，`MAX_PAYLOAD=512`，响应 CMD = 请求 CMD | `0x80`，响应 payload 首字节 = status。
- 错误策略（spec §10）：请求超时 **500ms/次**；**读**自动重试 2 次（共 3 发）；**写**不重试；`PARAM_COMMIT` 绝不自动重试。
- gui-core 禁止 panic：全部 `Result` 上抛（测试代码不受限）。
- 本计划只新增 `gui/` 与本计划文档；**禁改** `App/`、`Driver/`、`Interface/`、`Board/`、`config/`、`scripts/`。
- 工具链：Rust 经 rustup 安装；mingw gcc（`D:\Program Files\Compiler\mingw64\w64devkit\bin\gcc.exe`）仅用于 C 头语法检查。
- 测试入口：在 `gui/` 下 `cargo test --workspace`，必须全绿才算任务完成。
- 每个任务结束必须提交（frequent commits），提交信息用 `type(scope): 描述`（如 `feat(gui-core): ...`）。

## File Structure

```text
gui/
├── Cargo.toml                      # [workspace] members = core, sim, tests
├── README.md                       # 构建/测试/模拟器运行说明（Task 9）
├── protocol/
│   ├── motor_proto.h               # 双端契约 C 头（Task 2）
│   ├── protocol_spec.md            # 人读协议规范（Task 2）
│   └── test_vectors.json           # 金向量（Task 2，独立 C 实算的 hex）
├── core/                           # crate gui-core（lib）
│   ├── Cargo.toml
│   ├── src/
│   │   ├── lib.rs                  # 模块声明与 re-export（Task 1 起）
│   │   ├── proto.rs                # 协议常量 + payload 手工编解码（Task 2/6）
│   │   ├── codec.rs                # crc16 / encode / Decoder（Task 3）
│   │   ├── transport.rs            # Transport trait + Loopback + Tcp（Task 4）
│   │   ├── session.rs              # Session / RequestKind / SessionError（Task 5）
│   │   └── params.rs               # ParamMeta / ParamValue / 校验（Task 6）
│   └── tests/
│       └── vectors_valid.rs        # test_vectors.json 结构校验（Task 2）
├── sim/                            # crate gui-sim（lib + bin）
│   ├── Cargo.toml
│   ├── src/
│   │   ├── lib.rs                  # SimDevice + SimError（Task 7）
│   │   ├── yaml.rs                 # config/*.yaml → 参数表（Task 7）
│   │   └── main.rs                 # TCP 服务入口（Task 7）
│   └── tests/
│       └── sim_device.rs           # SimDevice 行为测试（Task 7）
└── tests/                          # crate gui-tests（集成测试）
    ├── Cargo.toml
    └── tests/
        └── tcp_roundtrip.rs        # TCP 全链路（Task 8）
```

模块依赖单向：`params`/`codec`/`proto` 互不依赖实现细节；`transport` 不依赖 `codec`；`session` 只经 `Transport` 与 `codec`；`gui-sim` 依赖 `gui-core`；`gui-tests` 依赖两者。

---

### Task 1: 工具链与工作区骨架

**Files:**
- Create: `gui/Cargo.toml`、`gui/core/Cargo.toml`、`gui/core/src/lib.rs`、`gui/sim/Cargo.toml`、`gui/sim/src/lib.rs`、`gui/sim/src/main.rs`、`gui/tests/Cargo.toml`、`gui/tests/tests/.gitkeep`
- Modify: `.gitignore`（追加 `gui/**/target/`，若顶层 `target/` 未覆盖）

**Interfaces:**
- Consumes: 无
- Produces: 可编译的 workspace；`gui_core` / `gui_sim` 空库；后续任务在此追加模块。

- [ ] **Step 1: 安装 Rust 工具链**

Run (PowerShell):
```powershell
winget install --id Rustlang.Rustup -e --accept-source-agreements --accept-package-agreements
# 重开终端后：
rustup default stable
rustc --version   # 期望 >= 1.75
cargo --version
```
若 winget 不可用：`scoop install rustup` 后同样 `rustup default stable`。若已安装（`rustc --version` 可用）跳过本步。

- [ ] **Step 2: 创建 workspace 骨架**

`gui/Cargo.toml`：
```toml
[workspace]
resolver = "2"
members = ["core", "sim", "tests"]

[workspace.package]
edition = "2021"
version = "0.1.0"
license = "BSD-3-Clause"

[workspace.dependencies]
tokio = { version = "1", features = ["rt-multi-thread", "macros", "net", "io-util", "time", "sync"] }
serde = { version = "1", features = ["derive"] }
serde_yaml = "0.9"
serde_json = "1"
```

`gui/core/Cargo.toml`：
```toml
[package]
name = "gui-core"
version.workspace = true
edition.workspace = true
license.workspace = true

[dependencies]
tokio.workspace = true

[dev-dependencies]
serde_json.workspace = true
tokio = { workspace = true, features = ["test-util", "macros", "rt-multi-thread", "net", "io-util", "time", "sync"] }
```

`gui/core/src/lib.rs`：
```rust
//! gui-core：Gui 通讯核（transport / codec / session / params）。
//! 契约源头：`gui/protocol/motor_proto.h`，本 crate 为其 Rust 镜像实现。
```

`gui/sim/Cargo.toml`：
```toml
[package]
name = "gui-sim"
version.workspace = true
edition.workspace = true
license.workspace = true

[dependencies]
gui-core = { path = "../core" }
tokio.workspace = true
serde.workspace = true
serde_yaml.workspace = true

[lib]
name = "gui_sim"
path = "src/lib.rs"

[[bin]]
name = "gui-sim"
path = "src/main.rs"
```

`gui/sim/src/lib.rs`：
```rust
//! gui-sim：设备模拟器（协议服务端参考实现）。
```

`gui/sim/src/main.rs`：
```rust
fn main() {
    println!("gui-sim: skeleton ok");
}
```

`gui/tests/Cargo.toml`：
```toml
[package]
name = "gui-tests"
version.workspace = true
edition.workspace = true
license.workspace = true
publish = false

[dependencies]
gui-core = { path = "../core" }
gui-sim = { path = "../sim" }
tokio.workspace = true
```

`gui/tests/tests/.gitkeep`：空文件。

`.gitignore` 追加（若不存在等价规则）：
```text
gui/**/target/
```

- [ ] **Step 3: 构建验证**

Run: `cargo build --workspace`（在 `gui/` 下）
Expected: 编译通过，无 error。

- [ ] **Step 4: 测试验证**

Run: `cargo test --workspace`
Expected: 0 tests（或 gui-sim 无测试），退出码 0。

- [ ] **Step 5: Commit**

```powershell
git add gui/ .gitignore
git commit -m "feat(gui): workspace 骨架（core/sim/tests crates）"
```

---

### Task 2: 协议契约三件套（motor_proto.h / protocol_spec.md / test_vectors.json）

**Files:**
- Create: `gui/protocol/motor_proto.h`、`gui/protocol/protocol_spec.md`、`gui/protocol/test_vectors.json`、`gui/core/tests/vectors_valid.rs`
- Modify: `gui/core/Cargo.toml`（dev-dep 已含 serde_json，无需改）

**Interfaces:**
- Consumes: Task 1 workspace
- Produces: `motor_proto.h` 中的全部常量与字节布局（Task 3/6/7 实现镜像）；`test_vectors.json` 结构：
  `{"crc_sanity": {"input_ascii": str, "crc": str}, "frames": [{"name": str, "hex": str, "expect": {"ver": u8, "cmd": u8, "seq": u8, "payload_hex": str}}]}`。

- [ ] **Step 1: 写 `gui/protocol/motor_proto.h`**

```c
/**
 * @file    motor_proto.h
 * @brief   Gui <-> 电机控制板 通讯协议 v1 双端契约（线格式唯一事实源）
 * @author  Kaiser
 *
 * 字节序：全部小端。帧格式：
 *   | SOF 0xA55A | VER(1) | CMD(1) | SEQ(1) | LEN(2) | PAYLOAD(0..512) | CRC16(2) |
 * CRC16-CCITT-FALSE：init 0xFFFF, poly 0x1021, 不反转不取反，覆盖 VER..PAYLOAD。
 * 响应 CMD = 请求 CMD | 0x80；响应 PAYLOAD 首字节 = mp_status_t，其后为命令体。
 * 请求/响应以 SEQ 配对（u8 回绕）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef MOTOR_PROTO_H
#define MOTOR_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MP_SOF0            (0xA5u)
#define MP_SOF1            (0x5Au)
#define MP_VERSION         (0x01u)
#define MP_MAX_PAYLOAD     (512u)
#define MP_RESPONSE_BIT    (0x80u)

/* 命令字（请求方向）；响应 = CMD | MP_RESPONSE_BIT */
typedef enum {
    MP_CMD_PROTO_INFO       = 0x01, /* 版本协商/能力位/设备名 */
    MP_CMD_PARAM_META_COUNT = 0x02, /* 元数据条目数 */
    MP_CMD_PARAM_META_GET   = 0x03, /* 按序号取元数据 */
    MP_CMD_PARAM_READ       = 0x04, /* 按名读参数 */
    MP_CMD_PARAM_WRITE      = 0x05, /* 按名写参数 */
    MP_CMD_PARAM_COMMIT     = 0x06, /* 参数落盘（停机窗口联锁） */
    MP_CMD_STATUS_GET       = 0x07, /* 运行状态快照 */
    MP_CMD_CTRL_CMD         = 0x08, /* 电机控制（联锁同固件 terminal） */
    MP_CMD_FAULT_GET        = 0x09, /* 故障与锁存 */
    MP_CMD_FAULT_CLEAR      = 0x0A, /* 清除故障锁存 */
    MP_CMD_STREAM_SUB       = 0x0B, /* 订阅一级流（v1 预留，返回 UNSUPPORTED） */
    MP_CMD_STREAM_UNSUB     = 0x0C  /* 取消订阅（v1 预留） */
} mp_cmd_t;

/* 响应状态码（响应 payload 首字节） */
typedef enum {
    MP_STATUS_OK          = 0x00,
    MP_STATUS_BAD_CMD     = 0x01,
    MP_STATUS_BAD_PARAM   = 0x02, /* 名字不存在/类型不符 */
    MP_STATUS_BUSY        = 0x03,
    MP_STATUS_LOCKED      = 0x04, /* 联锁拒绝：故障锁存或未停机 */
    MP_STATUS_UNSUPPORTED = 0x05,
    MP_STATUS_RANGE_ERR   = 0x06  /* 数值越界 */
} mp_status_t;

/* 参数类型 / 生效语义（PARAM_META_* 响应体） */
typedef enum { MP_TYPE_F32 = 0, MP_TYPE_U32 = 1, MP_TYPE_I32 = 2, MP_TYPE_BOOL = 3 } mp_type_t;
typedef enum { MP_APPLY_LIVE = 0, MP_APPLY_REBOOT = 1 } mp_apply_t;

/* CTRL_CMD opcode（请求体首字节，后跟 f32 value） */
typedef enum {
    MP_CTRL_START    = 0x01,
    MP_CTRL_STOP     = 0x02,
    MP_CTRL_SET_FREQ = 0x03,
    MP_CTRL_SET_MOD  = 0x04
} mp_ctrl_opcode_t;

/* 帧头布局（CRC 覆盖范围 = hdr[0..7] + PAYLOAD，即 VER..PAYLOAD） */
typedef struct {
    uint8_t  ver;      /* MP_VERSION */
    uint8_t  cmd;
    uint8_t  seq;
    uint16_t len;      /* PAYLOAD 字节数，0..MP_MAX_PAYLOAD */
} mp_hdr_t;            /* 线上 5 字节，逐字段小端序列化，勿直接 memcpy */

/* --- 命令体布局（响应体在首部含 mp_status_t = uint8_t）--- */

/* PROTO_INFO resp: status | proto_major u8 | proto_minor u8 | caps u32 | name_len u8 | name[name_len] */
/* PARAM_META_COUNT resp: status | count u16 */
/* PARAM_META_GET req: index u16
 * resp: status | index u16 | type u8 | apply u8 | min f32 | max f32 | name_len u8 | name | unit_len u8 | unit */
/* PARAM_READ req: name_len u8 | name; resp: status | type u8 | value u32（原始位型，按 type 重解释） */
/* PARAM_WRITE req: name_len u8 | name | type u8 | value u32; resp: status */
/* PARAM_COMMIT req: 空; resp: status */
/* STATUS_GET resp: status | fault_flags u32 | motor_running u8 | motor_state u8 | freq_hz f32 | mod_pct f32
 *                | i_u f32 | i_v f32 | i_w f32 | vbus f32 | ntc_temp f32 | enc_rotor_deg f32 | enc_output_deg f32 */
/* CTRL_CMD req: opcode u8 | value f32; resp: status */
/* FAULT_GET resp: status | fault_flags u32 | latched_flags u32 */
/* FAULT_CLEAR req: 空; resp: status */

/* motor_state 取值 */
#define MP_MOTOR_STOP (0x00u)
#define MP_MOTOR_RUN  (0x01u)

#ifdef __cplusplus
}
#endif

#endif /* MOTOR_PROTO_H */
```

- [ ] **Step 2: 写 `gui/protocol/protocol_spec.md`**

内容要求（逐节写全，不得留 TBD）：
1. 帧格式表（含 `test_vectors.json` 4 个金帧的逐步字节注解）；
2. 命令表：每个命令的 req/resp payload 字段表（名称/类型/字节序/取值范围），与 `motor_proto.h` 注释一致；
3. 状态码表（7 个，含 LOCKED 的两种触发条件：故障锁存、未停机窗口）；
4. 传输映射（bulk/CDC/UART 直载；CAN 适配器分片承载）；
5. 版本协商：`proto_major` 不等 → 主机拒绝连接并报错；`caps` 位定义 v1 仅 bit0=参数写、bit1=控制、bit2=一级流（预留恒 0）。

- [ ] **Step 3: 写 `gui/protocol/test_vectors.json`（下列 hex 为独立 C 程序实算结果，原样使用）**

```json
{
  "crc_sanity": { "input_ascii": "123456789", "crc": "29B1" },
  "frames": [
    {
      "name": "proto_info_req_seq0",
      "hex": "A55A0101000000CDE9",
      "expect": { "ver": 1, "cmd": 1, "seq": 0, "payload_hex": "" }
    },
    {
      "name": "proto_info_resp_sim",
      "hex": "A55A0181000A000100030000000353494D2DC4",
      "expect": { "ver": 1, "cmd": 129, "seq": 0, "payload_hex": "0100030000000353494D" }
    },
    {
      "name": "param_read_req_motor_pole_pairs",
      "hex": "A55A0104011100106D6F746F722E706F6C655F70616972733540",
      "expect": { "ver": 1, "cmd": 4, "seq": 1, "payload_hex": "106D6F746F722E706F6C655F7061697273" }
    },
    {
      "name": "param_read_resp_bad_param_seq1",
      "hex": "A55A018401010002A081",
      "expect": { "ver": 1, "cmd": 132, "seq": 1, "payload_hex": "02" }
    }
  ]
}
```

向量出处（勿手改 hex）：frame1 CRC 覆盖 `01 01 00 00 00` → `0xCDE9`；frame2 覆盖 `01 81 00 0A 00` + `01 00 03 00 00 00 03 53 49 4D`（proto v1.0，caps=3，name "SIM"）→ `0xC42D`；frame3 覆盖 `01 04 01 11 00` + `10 "motor.pole_pairs"` → `0x4035`；frame4 覆盖 `01 84 01 01 00 02` → `0x81A0`。CRC 字段小端放置。

- [ ] **Step 4: 写 `gui/core/tests/vectors_valid.rs`（JSON 结构校验）**

```rust
use serde_json::Value;

#[test]
fn test_vectors_json_is_well_formed() {
    let raw = std::fs::read_to_string(
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../protocol/test_vectors.json"),
    )
    .expect("test_vectors.json 可读");
    let v: Value = serde_json::from_str(&raw).expect("合法 JSON");
    assert_eq!(v["crc_sanity"]["crc"], "29B1");
    let frames = v["frames"].as_array().expect("frames 数组");
    assert_eq!(frames.len(), 4);
    for f in frames {
        let hex = f["hex"].as_str().unwrap();
        assert!(hex.len() >= 20 && hex.len() % 2 == 0, "hex 长度合法");
        let payload_hex = f["expect"]["payload_hex"].as_str().unwrap();
        let frame_bytes = hex.len() / 2;
        // 帧长 = 2(SOF)+5(hdr)+payload+2(CRC)
        assert_eq!(frame_bytes, 2 + 5 + payload_hex.len() / 2 + 2);
    }
}
```

- [ ] **Step 5: C 头语法检查**

Run (PowerShell):
```powershell
& "D:\Program Files\Compiler\mingw64\w64devkit\bin\gcc.exe" -std=c17 -fsyntax-only -x c gui/protocol/motor_proto.h
```
Expected: 无输出、退出码 0。

- [ ] **Step 6: 测试验证**

Run: `cargo test --workspace`
Expected: `test_vectors_json_is_well_formed` PASS。

- [ ] **Step 7: Commit**

```powershell
git add gui/protocol gui/core/tests
git commit -m "feat(gui): 协议契约 v1（motor_proto.h + protocol_spec.md + 金向量）"
```

---

### Task 3: gui-core codec（CRC / 帧编解码 / SOF 重同步）

**Files:**
- Create: `gui/core/src/codec.rs`
- Modify: `gui/core/src/lib.rs`（声明 `pub mod codec;`）

**Interfaces:**
- Consumes: `test_vectors.json`（Task 2）
- Produces（后续任务依赖的精确签名）:
  - `gui_core::codec::crc16_ccitt_false(data: &[u8]) -> u16`
  - `gui_core::codec::Frame { pub ver: u8, pub cmd: u8, pub seq: u8, pub payload: Vec<u8> }`
  - `gui_core::codec::encode(frame: &Frame) -> Vec<u8>`
  - `gui_core::codec::Decoder::new() -> Decoder`、`Decoder::feed(&mut self, data: &[u8])`、`Decoder::next_frame(&mut self) -> Option<Frame>`、`Decoder::crc_errors() -> u64`、`Decoder::resyncs() -> u64`

- [ ] **Step 1: 写失败测试 `gui/core/src/codec.rs`（测试置于同文件 `#[cfg(test)] mod tests`）**

```rust
//! 帧编解码：SOF 重同步 + CRC16-CCITT-FALSE。

pub const SOF: [u8; 2] = [0xA5, 0x5A];
pub const VER: u8 = 0x01;
pub const MAX_PAYLOAD: usize = 512;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Frame {
    pub ver: u8,
    pub cmd: u8,
    pub seq: u8,
    pub payload: Vec<u8>,
}

pub fn crc16_ccitt_false(data: &[u8]) -> u16 {
    let mut crc: u16 = 0xFFFF;
    for &b in data {
        crc ^= (b as u16) << 8;
        for _ in 0..8 {
            crc = if crc & 0x8000 != 0 { (crc << 1) ^ 0x1021 } else { crc << 1 };
        }
    }
    crc
}

pub fn encode(frame: &Frame) -> Vec<u8> {
    assert!(frame.payload.len() <= MAX_PAYLOAD, "payload 超长");
    let mut body = Vec::with_capacity(5 + frame.payload.len());
    body.push(frame.ver);
    body.push(frame.cmd);
    body.push(frame.seq);
    body.extend_from_slice(&(frame.payload.len() as u16).to_le_bytes());
    body.extend_from_slice(&frame.payload);
    let crc = crc16_ccitt_false(&body);
    let mut out = Vec::with_capacity(2 + body.len() + 2);
    out.extend_from_slice(&SOF);
    out.extend_from_slice(&body);
    out.extend_from_slice(&crc.to_le_bytes());
    out
}

#[derive(Default)]
pub struct Decoder {
    buf: Vec<u8>,
    crc_errors: u64,
    resyncs: u64,
}

impl Decoder {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn feed(&mut self, data: &[u8]) {
        self.buf.extend_from_slice(data);
    }

    pub fn crc_errors(&self) -> u64 {
        self.crc_errors
    }

    pub fn resyncs(&self) -> u64 {
        self.resyncs
    }

    /// 取下一完整帧；坏帧丢弃并计数，可继续调用直到 None。
    pub fn next_frame(&mut self) -> Option<Frame> {
        loop {
            // 定位 SOF
            let sof_pos = self.buf.windows(2).position(|w| w == SOF);
            let Some(start) = sof_pos else {
                if self.buf.len() >= 2 {
                    self.resyncs += self.buf.len() as u64 - 1;
                    let last = *self.buf.last().unwrap();
                    self.buf.clear();
                    self.buf.push(last); // 保留尾字节，可能是半个 SOF
                }
                return None;
            };
            if start > 0 {
                self.resyncs += start as u64;
                self.buf.drain(..start);
            }
            if self.buf.len() < 2 + 5 {
                return None; // 头不完整
            }
            let len = u16::from_le_bytes([self.buf[5], self.buf[6]]) as usize;
            if len > MAX_PAYLOAD {
                // 非法长度：跳过本次 SOF 继续寻址
                self.resyncs += 1;
                self.buf.drain(..2);
                continue;
            }
            let total = 2 + 5 + len + 2;
            if self.buf.len() < total {
                return None; // 等更多字节
            }
            let body_end = 2 + 5 + len;
            let crc_calc = crc16_ccitt_false(&self.buf[2..body_end]);
            let crc_wire = u16::from_le_bytes([self.buf[body_end], self.buf[body_end + 1]]);
            if crc_calc != crc_wire {
                self.crc_errors += 1;
                self.buf.drain(..total);
                continue;
            }
            let frame = Frame {
                ver: self.buf[2],
                cmd: self.buf[3],
                seq: self.buf[4],
                payload: self.buf[7..body_end].to_vec(),
            };
            self.buf.drain(..total);
            return Some(frame);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(s: &str) -> Vec<u8> {
        (0..s.len() / 2)
            .map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap())
            .collect()
    }

    #[test]
    fn crc_matches_standard_vector() {
        assert_eq!(crc16_ccitt_false(b"123456789"), 0x29B1);
    }

    #[test]
    fn decodes_golden_frames_from_json() {
        let raw = std::fs::read_to_string(
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../protocol/test_vectors.json"),
        )
        .unwrap();
        let v: serde_json::Value = serde_json::from_str(&raw).unwrap();
        for f in v["frames"].as_array().unwrap() {
            let mut dec = Decoder::new();
            dec.feed(&hex(f["hex"].as_str().unwrap()));
            let frame = dec.next_frame().expect("金帧必须可解出");
            assert_eq!(frame.ver, f["expect"]["ver"].as_u64().unwrap() as u8);
            assert_eq!(frame.cmd, f["expect"]["cmd"].as_u64().unwrap() as u8);
            assert_eq!(frame.seq, f["expect"]["seq"].as_u64().unwrap() as u8);
            assert_eq!(frame.payload, hex(f["expect"]["payload_hex"].as_str().unwrap()));
            assert_eq!(dec.next_frame(), None);
        }
    }

    #[test]
    fn encode_decode_roundtrip() {
        let f = Frame { ver: VER, cmd: 0x07, seq: 200, payload: vec![1, 2, 3, 4] };
        let mut dec = Decoder::new();
        dec.feed(&encode(&f));
        assert_eq!(dec.next_frame(), Some(f));
    }

    #[test]
    fn resyncs_after_garbage_and_half_sof() {
        let f = Frame { ver: VER, cmd: 0x01, seq: 0, payload: vec![] };
        let wire = encode(&f);
        let mut dec = Decoder::new();
        dec.feed(&[0xDE, 0xAD, 0xA5]); // 垃圾 + 半个 SOF
        dec.feed(&wire);
        assert_eq!(dec.next_frame(), Some(f));
        assert!(dec.resyncs() > 0);
        assert_eq!(dec.crc_errors(), 0);
    }

    #[test]
    fn drops_bad_crc_frame_and_continues() {
        let bad = Frame { ver: VER, cmd: 0x01, seq: 9, payload: vec![0xFF] };
        let mut wire = encode(&bad);
        let n = wire.len();
        wire[n - 1] ^= 0xFF; // 破坏 CRC
        let good = Frame { ver: VER, cmd: 0x01, seq: 10, payload: vec![] };
        let mut dec = Decoder::new();
        dec.feed(&wire);
        dec.feed(&encode(&good));
        assert_eq!(dec.next_frame(), Some(good));
        assert_eq!(dec.crc_errors(), 1);
    }

    #[test]
    fn incremental_feed_yields_frame() {
        let f = Frame { ver: VER, cmd: 0x04, seq: 1, payload: vec![0x10] };
        let wire = encode(&f);
        let mut dec = Decoder::new();
        for chunk in wire.chunks(3) {
            dec.feed(chunk);
        }
        assert_eq!(dec.next_frame(), Some(f));
    }
}
```

- [ ] **Step 2: 运行测试确认失败（编译错误即失败）**

Run: `cargo test -p gui-core`
Expected: FAIL（`serde_json` dev-dep 已有；若报 `serde_json` 未定义则先在 `gui/core/Cargo.toml` `[dev-dependencies]` 确认 `serde_json.workspace = `true`）。

- [ ] **Step 3: 若编译错误系 `lib.rs` 未声明模块，补 `gui/core/src/lib.rs`：**

```rust
//! gui-core：Gui 通讯核（transport / codec / session / params）。
//! 契约源头：`gui/protocol/motor_proto.h`，本 crate 为其 Rust 镜像实现。
pub mod codec;
```

- [ ] **Step 4: 运行测试确认通过**

Run: `cargo test -p gui-core`
Expected: 6 tests PASS。

- [ ] **Step 5: Commit**

```powershell
git add gui/core
git commit -m "feat(gui-core): 帧编解码 codec（CRC16-CCITT + SOF 重同步，金向量测试）"
```

---

### Task 4: transport（trait + Loopback + Tcp）

**Files:**
- Create: `gui/core/src/transport.rs`
- Modify: `gui/core/src/lib.rs`（追加 `pub mod transport;`）

**Interfaces:**
- Consumes: 无（独立模块）
- Produces:
  - `gui_core::transport::TransportError { Io(std::io::Error), Closed }`
  - `pub trait Transport: Send { fn write_all<'a>(&'a mut self, buf: &'a [u8]) -> impl Future<Output = Result<(), TransportError>> + Send + 'a; fn read_some<'a>(&'a mut self, buf: &'a mut [u8]) -> impl Future<Output = Result<usize, TransportError>> + Send + 'a; }`
  - `gui_core::transport::TcpTransport`：`TcpTransport::connect(addr: &str) -> Result<TcpTransport, TransportError>`，实现 `Transport`
  - `gui_core::transport::LoopbackTransport`：`loopback_pair() -> (LoopbackTransport, LoopbackTransport)`，两两互为对端；`write_all` 的字节出现在对端 `read_some`；对端 drop 后 `read_some` 返回 `Err(Closed)`

- [ ] **Step 1: 写失败测试（`transport.rs` 尾部 `#[cfg(test)] mod tests`）**

```rust
use std::future::Future;

#[derive(Debug)]
pub enum TransportError {
    Io(std::io::Error),
    Closed,
}

impl std::fmt::Display for TransportError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            TransportError::Io(e) => write!(f, "io: {e}"),
            TransportError::Closed => write!(f, "closed"),
        }
    }
}

impl std::error::Error for TransportError {}

pub trait Transport: Send {
    fn write_all<'a>(
        &'a mut self,
        buf: &'a [u8],
    ) -> impl Future<Output = Result<(), TransportError>> + Send + 'a;
    fn read_some<'a>(
        &'a mut self,
        buf: &'a mut [u8],
    ) -> impl Future<Output = Result<usize, TransportError>> + Send + 'a;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn loopback_pair_carries_bytes_both_ways() {
        let (mut a, mut b) = super::loopback_pair();
        a.write_all(&[1, 2, 3]).await.unwrap();
        b.write_all(&[9]).await.unwrap();
        let mut buf = [0u8; 8];
        let n = b.read_some(&mut buf).await.unwrap();
        assert_eq!(&buf[..n], &[1, 2, 3]);
        let n = a.read_some(&mut buf).await.unwrap();
        assert_eq!(&buf[..n], &[9]);
    }

    #[tokio::test]
    async fn loopback_reports_closed_after_peer_drop() {
        let (mut a, b) = super::loopback_pair();
        drop(b);
        let mut buf = [0u8; 4];
        assert!(matches!(a.read_some(&mut buf).await, Err(TransportError::Closed)));
    }

    #[tokio::test]
    async fn tcp_transport_roundtrip_with_local_listener() {
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        let server = tokio::spawn(async move {
            let (mut sock, _) = listener.accept().await.unwrap();
            use tokio::io::{AsyncReadExt, AsyncWriteExt};
            let mut buf = [0u8; 16];
            let n = sock.read(&mut buf).await.unwrap();
            sock.write_all(&buf[..n]).await.unwrap(); // echo
        });
        let mut t = super::TcpTransport::connect(&addr.to_string()).await.unwrap();
        t.write_all(b"ping").await.unwrap();
        let mut buf = [0u8; 16];
        let n = t.read_some(&mut buf).await.unwrap();
        assert_eq!(&buf[..n], b"ping");
        server.await.unwrap();
    }
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cargo test -p gui-core`
Expected: FAIL（`loopback_pair` / `TcpTransport` 未定义）。

- [ ] **Step 3: 最小实现（追加到 `transport.rs`）**

```rust
pub struct TcpTransport {
    stream: tokio::net::TcpStream,
}

impl TcpTransport {
    pub async fn connect(addr: &str) -> Result<Self, TransportError> {
        let stream = tokio::net::TcpStream::connect(addr)
            .await
            .map_err(TransportError::Io)?;
        Ok(Self { stream })
    }
}

impl Transport for TcpTransport {
    async fn write_all(&mut self, buf: &[u8]) -> Result<(), TransportError> {
        use tokio::io::AsyncWriteExt;
        self.stream.write_all(buf).await.map_err(TransportError::Io)
    }
    async fn read_some(&mut self, buf: &mut [u8]) -> Result<usize, TransportError> {
        use tokio::io::AsyncReadExt;
        match self.stream.read(buf).await.map_err(TransportError::Io)? {
            0 => Err(TransportError::Closed),
            n => Ok(n),
        }
    }
}

pub struct LoopbackTransport {
    tx: tokio::sync::mpsc::UnboundedSender<Vec<u8>>,
    rx: tokio::sync::mpsc::UnboundedReceiver<Vec<u8>>,
}

pub fn loopback_pair() -> (LoopbackTransport, LoopbackTransport) {
    let (tx_a, rx_b) = tokio::sync::mpsc::unbounded_channel();
    let (tx_b, rx_a) = tokio::sync::mpsc::unbounded_channel();
    (
        LoopbackTransport { tx: tx_a, rx: rx_a },
        LoopbackTransport { tx: tx_b, rx: rx_b },
    )
}

impl Transport for LoopbackTransport {
    async fn write_all(&mut self, buf: &[u8]) -> Result<(), TransportError> {
        self.tx.send(buf.to_vec()).map_err(|_| TransportError::Closed)
    }
    async fn read_some(&mut self, buf: &mut [u8]) -> Result<usize, TransportError> {
        match self.rx.recv().await {
            Some(data) => {
                let n = data.len().min(buf.len());
                buf[..n].copy_from_slice(&data[..n]);
                Ok(n)
            }
            None => Err(TransportError::Closed),
        }
    }
}
```

注意：trait 中 `async fn` 与 RPITIT 混用问题——实现侧直接写 `async fn`（Rust 1.75+ 允许实现用 `async fn` 匹配 RPITIT 声明）。若编译器报签名不匹配，把 trait 与 impl 两侧统一为 `async fn` 简写形式（去掉 `impl Future` 包装）。

- [ ] **Step 4: 运行确认通过**

Run: `cargo test -p gui-core`
Expected: Task 3 全部 + 本任务 3 个测试 PASS。

- [ ] **Step 5: Commit**

```powershell
git add gui/core
git commit -m "feat(gui-core): Transport 抽象（Loopback/Tcp）"
```

---

### Task 5: session（SEQ / 超时 / 重试 / 状态机）

**Files:**
- Create: `gui/core/src/session.rs`
- Modify: `gui/core/src/lib.rs`（追加 `pub mod session;`）

**Interfaces:**
- Consumes: `codec::{Frame, encode, Decoder}`、`transport::{Transport, TransportError}`
- Produces:
  - `gui_core::session::RequestKind { Read, Write }`
  - `gui_core::session::SessionError { Timeout, Status(u8), Transport(TransportError), Closed }`
  - `gui_core::session::ConnState { Disconnected, Ready }`（v1 状态集；Reconnecting/Handshake 属计划 2/M2）
  - `gui_core::session::Session<T: Transport>::new(t: T) -> Session<T>`
  - `Session::request(&mut self, cmd: u8, payload: &[u8], kind: RequestKind) -> Result<Vec<u8>, SessionError>`——返回值为**响应体**（已剥去 status 字节）；status != OK 时返回 `Err(Status(code))`
  - `Session::transport_mut(&mut self) -> &mut T`
  - `Session::stats(&self) -> SessionStats { pub crc_errors: u64, pub resyncs: u64, pub attempts: u64 }`

**协议常量（与 `motor_proto.h` 镜像）：** 本任务在 `session.rs` 顶部定义私有 `const RESPONSE_BIT: u8 = 0x80;`、`const REQUEST_TIMEOUT_MS: u64 = 500;`、`const READ_RETRIES: u32 = 2;`。完整常量镜像属 Task 6 的 `proto.rs`（本任务不提前建，避免重复）。

- [ ] **Step 1: 写失败测试（`session.rs` 尾部 `#[cfg(test)]`，用 Loopback + 脚本化假设备）**

```rust
#[cfg(test)]
mod tests {
    use super::*;
    use crate::codec::{encode, Frame, Decoder};
    use crate::transport::{loopback_pair, Transport};

    /// 假设备：对每个请求回显 status=OK 与 payload+1；可配置「不回」模拟超时。
    fn spawn_fake_device(
        mut t: crate::transport::LoopbackTransport,
        silence: bool,
    ) -> tokio::task::JoinHandle<()> {
        tokio::spawn(async move {
            if silence {
                // 吞掉一切请求但保持连接
                let mut buf = [0u8; 2048];
                loop {
                    if t.read_some(&mut buf).await.is_err() {
                        break;
                    }
                }
                return;
            }
            let mut dec = Decoder::new();
            let mut buf = [0u8; 2048];
            loop {
                let n = match t.read_some(&mut buf).await {
                    Ok(n) => n,
                    Err(_) => break,
                };
                dec.feed(&buf[..n]);
                while let Some(req) = dec.next_frame() {
                    let mut body = vec![0u8]; // status OK
                    body.extend_from_slice(&req.payload);
                    let resp = Frame {
                        ver: crate::codec::VER,
                        cmd: req.cmd | 0x80,
                        seq: req.seq,
                        payload: body,
                    };
                    if t.write_all(&encode(&resp)).await.is_err() {
                        break;
                    }
                }
            }
        })
    }

    #[tokio::test]
    async fn request_returns_body_on_ok() {
        let (client, server) = loopback_pair();
        let h = spawn_fake_device(server, false);
        let mut s = Session::new(client);
        let body = s.request(0x04, b"abc", RequestKind::Read).await.unwrap();
        assert_eq!(body, b"abc");
        h.await.unwrap();
    }

    #[tokio::test]
    async fn read_retries_then_gives_up_on_timeout() {
        let (client, server) = loopback_pair();
        let h = spawn_fake_device(server, true);
        let mut s = Session::new(client);
        let err = s.request(0x04, b"x", RequestKind::Read).await.unwrap_err();
        assert!(matches!(err, SessionError::Timeout));
        assert_eq!(s.stats().attempts, 3); // 1 + 2 重试
        drop(s);
        h.await.unwrap();
    }

    #[tokio::test]
    async fn write_does_not_retry() {
        let (client, server) = loopback_pair();
        let h = spawn_fake_device(server, true);
        let mut s = Session::new(client);
        let err = s.request(0x05, b"x", RequestKind::Write).await.unwrap_err();
        assert!(matches!(err, SessionError::Timeout));
        assert_eq!(s.stats().attempts, 1);
        drop(s);
        h.await.unwrap();
    }

    #[tokio::test]
    async fn non_ok_status_becomes_error_and_is_not_retried() {
        let (client, mut server) = loopback_pair();
        let h = tokio::spawn(async move {
            let mut dec = Decoder::new();
            let mut buf = [0u8; 2048];
            let n = server.read_some(&mut buf).await.unwrap();
            dec.feed(&buf[..n]);
            let req = dec.next_frame().unwrap();
            let resp = Frame {
                ver: crate::codec::VER,
                cmd: req.cmd | 0x80,
                seq: req.seq,
                payload: vec![0x04], // MP_STATUS_LOCKED
            };
            server.write_all(&encode(&resp)).await.unwrap();
        });
        let mut s = Session::new(client);
        let err = s.request(0x06, b"", RequestKind::Write).await.unwrap_err();
        assert!(matches!(err, SessionError::Status(0x04)));
        assert_eq!(s.stats().attempts, 1);
        h.await.unwrap();
    }

    #[tokio::test]
    async fn seq_wraps_after_256() {
        let (client, server) = loopback_pair();
        let h = spawn_fake_device(server, false);
        let mut s = Session::new(client);
        for _ in 0..300 {
            s.request(0x07, b"", RequestKind::Read).await.unwrap();
        }
        drop(s);
        h.await.unwrap();
    }
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cargo test -p gui-core`
Expected: FAIL（`Session` 未定义）。注意 `seq_wraps_after_256` 用 300 次请求（每次 2 帧）压测 SEQ 回绕；假设备按回显 `req.seq` 应答，天然验证配对逻辑对回绕的容忍。

- [ ] **Step 3: 最小实现**

```rust
//! 会话层：SEQ 配对、超时/重试、响应状态剥离。

use crate::codec::{encode, Decoder, Frame};
use crate::transport::{Transport, TransportError};

const RESPONSE_BIT: u8 = 0x80;
const REQUEST_TIMEOUT_MS: u64 = 500;
const READ_RETRIES: u32 = 2;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RequestKind {
    Read,
    Write,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConnState {
    Disconnected,
    Ready,
}

#[derive(Debug, Clone, Copy, Default)]
pub struct SessionStats {
    pub crc_errors: u64,
    pub resyncs: u64,
    pub attempts: u64,
}

#[derive(Debug)]
pub enum SessionError {
    Timeout,
    Status(u8),
    Transport(TransportError),
    Closed,
}

pub struct Session<T: Transport> {
    transport: T,
    decoder: Decoder,
    next_seq: u8,
    stats: SessionStats,
}

impl<T: Transport> Session<T> {
    pub fn new(transport: T) -> Self {
        Self {
            transport,
            decoder: Decoder::new(),
            next_seq: 0,
            stats: SessionStats::default(),
        }
    }

    pub fn stats(&self) -> SessionStats {
        SessionStats {
            crc_errors: self.decoder.crc_errors(),
            resyncs: self.decoder.resyncs(),
            ..self.stats
        }
    }

    pub fn transport_mut(&mut self) -> &mut T {
        &mut self.transport
    }

    /// 发起请求并等待匹配 SEQ 的响应。
    /// 读：失败（超时/传输错）重试至多 READ_RETRIES 次；写：不重试。
    /// status != OK 返回 Err(Status)，不重试。
    pub async fn request(
        &mut self,
        cmd: u8,
        payload: &[u8],
        kind: RequestKind,
    ) -> Result<Vec<u8>, SessionError> {
        let max_attempts = match kind {
            RequestKind::Read => READ_RETRIES + 1,
            RequestKind::Write => 1,
        };
        let mut last_err = SessionError::Timeout;
        for _ in 0..max_attempts {
            self.stats.attempts += 1;
            let seq = self.next_seq;
            self.next_seq = self.next_seq.wrapping_add(1);
            let frame = Frame {
                ver: crate::codec::VER,
                cmd,
                seq,
                payload: payload.to_vec(),
            };
            if let Err(e) = self.transport.write_all(&encode(&frame)).await {
                last_err = SessionError::Transport(e);
                continue;
            }
            match self.wait_response(seq, cmd).await {
                Ok(body) => return Ok(body),
                Err(e) => {
                    let retryable = matches!(e, SessionError::Timeout);
                    last_err = e;
                    if !retryable {
                        return Err(last_err);
                    }
                }
            }
        }
        Err(last_err)
    }

    async fn wait_response(&mut self, seq: u8, cmd: u8) -> Result<Vec<u8>, SessionError> {
        let deadline =
            std::time::Instant::now() + std::time::Duration::from_millis(REQUEST_TIMEOUT_MS);
        loop {
            let remain = deadline.saturating_duration_since(std::time::Instant::now());
            if remain.is_zero() {
                return Err(SessionError::Timeout);
            }
            let mut buf = [0u8; 2048];
            let n = match tokio::time::timeout(remain, self.transport.read_some(&mut buf)).await {
                Ok(Ok(n)) => n,
                Ok(Err(TransportError::Closed)) => return Err(SessionError::Closed),
                Ok(Err(e)) => return Err(SessionError::Transport(e)),
                Err(_) => return Err(SessionError::Timeout),
            };
            self.decoder.feed(&buf[..n]);
            while let Some(f) = self.decoder.next_frame() {
                if f.seq == seq && f.cmd == (cmd | RESPONSE_BIT) {
                    if f.payload.is_empty() {
                        return Err(SessionError::Status(0xFF)); // 协议违例，视为失败
                    }
                    let status = f.payload[0];
                    return if status == 0 {
                        Ok(f.payload[1..].to_vec())
                    } else {
                        Err(SessionError::Status(status))
                    };
                }
                // 非匹配帧（含预流/事件）：丢弃，计数在 decoder 内
            }
        }
    }
}
```

- [ ] **Step 4: 运行确认通过**

Run: `cargo test -p gui-core`
Expected: 全部 PASS（含 `seq_wraps_after_256`）。

- [ ] **Step 5: Commit**

```powershell
git add gui/core
git commit -m "feat(gui-core): session 层（SEQ 配对/500ms 超时/读重试 2 次）"
```

---

### Task 6: params 模型 + proto 常量镜像（payload 编解码）

**Files:**
- Create: `gui/core/src/params.rs`、`gui/core/src/proto.rs`
- Modify: `gui/core/src/lib.rs`（追加 `pub mod params; pub mod proto;`）

**Interfaces:**
- Consumes: `motor_proto.h` 常量（Task 2）
- Produces（Task 7/8 依赖）:
  - `gui_core::proto`：常量 `VER, SOF0, SOF1, MAX_PAYLOAD, RESPONSE_BIT`；`cmd::{PROTO_INFO=0x01, PARAM_META_COUNT=0x02, PARAM_META_GET=0x03, PARAM_READ=0x04, PARAM_WRITE=0x05, PARAM_COMMIT=0x06, STATUS_GET=0x07, CTRL_CMD=0x08, FAULT_GET=0x09, FAULT_CLEAR=0x0A, STREAM_SUB=0x0B, STREAM_UNSUB=0x0C}`；`status::{OK=0, BAD_CMD=1, BAD_PARAM=2, BUSY=3, LOCKED=4, UNSUPPORTED=5, RANGE_ERR=6}`；`ctrl::{START=1, STOP=2, SET_FREQ=3, SET_MOD=4}`
  - `gui_core::params::{ParamType{F32,U32,I32,Bool}, Apply{Live,Reboot}}`，各含 `to_u8()/from_u8(u8) -> Option<...>`
  - `gui_core::params::ParamMeta { pub index: u16, pub name: String, pub ty: ParamType, pub apply: Apply, pub unit: String, pub min: f32, pub max: f32 }`
  - `gui_core::params::ParamValue { pub raw: u32 }`，方法 `from_f32(f32)/as_f32(&self)->f32/from_u32/u32/from_i32/i32/from_bool/as_bool`（`raw` 为位型存储：f32 用 `to_bits`）
  - `gui_core::params::check_range(&ParamValue, &ParamMeta) -> Result<(), RangeError>`——`min<=max` 时校验区间；`min>max` 表示无限制（恒 Ok）
  - payload 字节布局函数（全部小端）：
    `encode_name(name:&str)->Vec<u8>`（len-prefixed u8）、`decode_name(buf:&[u8], pos:&mut usize)->Result<String, PayloadError>`
    `encode_meta_resp(meta:&ParamMeta)->Vec<u8>`、`decode_meta_resp(body:&[u8])->Result<ParamMeta, PayloadError>`（body 已剥 status）
    `encode_value(v:ParamValue, ty:ParamType)->[u8;5]`（type u8 + raw u32 LE）、`decode_value(body:&[u8])->Result<(ParamType,ParamValue), PayloadError>`

- [ ] **Step 1: 写失败测试（params.rs 尾部 `#[cfg(test)]`）**

```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn value_roundtrip_all_types() {
        assert_eq!(ParamValue::from_f32(1.5).as_f32(), 1.5);
        assert_eq!(ParamValue::from_u32(0xDEAD_BEEF).as_u32(), 0xDEAD_BEEF);
        assert_eq!(ParamValue::from_i32(-7).as_i32(), -7);
        assert!(ParamValue::from_bool(true).as_bool());
    }

    #[test]
    fn range_check_bounds_and_disabled() {
        let meta = ParamMeta {
            index: 0,
            name: "x".into(),
            ty: ParamType::F32,
            apply: Apply::Live,
            unit: "V".into(),
            min: 0.0,
            max: 60.0,
        };
        assert!(check_range(&ParamValue::from_f32(30.0), &meta).is_ok());
        assert!(check_range(&ParamValue::from_f32(61.0), &meta).is_err());
        let open = ParamMeta { min: 1.0, max: 0.0, ..meta };
        assert!(check_range(&ParamValue::from_f32(1e9), &open).is_ok());
    }

    #[test]
    fn meta_payload_roundtrip_matches_wire_layout() {
        let meta = ParamMeta {
            index: 7,
            name: "motor.pole_pairs".into(),
            ty: ParamType::U32,
            apply: Apply::Reboot,
            unit: String::new(),
            min: 1.0,
            max: 64.0,
        };
        let bytes = encode_meta_resp(&meta);
        // 布局：index u16 | type u8 | apply u8 | min f32 | max f32 | name_len u8 | name | unit_len u8 | unit
        assert_eq!(&bytes[0..2], &7u16.to_le_bytes());
        assert_eq!(bytes[2], ParamType::U32 as u8);
        assert_eq!(bytes[3], Apply::Reboot as u8);
        let mut pos = 4 + 8 + 4; // 跳过 min/max 后是 name_len
        let got = decode_meta_resp(&bytes).unwrap();
        assert_eq!(got.name, "motor.pole_pairs");
        assert_eq!(got.index, 7);
        let _ = pos;
    }

    #[test]
    fn value_payload_roundtrip() {
        let (ty, v) = decode_value(&encode_value(ParamValue::from_f32(3.3), ParamType::F32)).unwrap();
        assert_eq!(ty, ParamType::F32);
        assert_eq!(v.as_f32(), 3.3);
    }
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cargo test -p gui-core`
Expected: FAIL（类型未定义）。

- [ ] **Step 3: 实现 `params.rs` 与 `proto.rs`**

`params.rs`：
```rust
//! 参数模型：元数据 + 值 + 校验 + payload 编解码（小端，镜像 motor_proto.h）。

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ParamType {
    F32,
    U32,
    I32,
    Bool,
}

impl ParamType {
    pub fn to_u8(self) -> u8 {
        match self {
            ParamType::F32 => 0,
            ParamType::U32 => 1,
            ParamType::I32 => 2,
            ParamType::Bool => 3,
        }
    }
    pub fn from_u8(v: u8) -> Option<Self> {
        match v {
            0 => Some(ParamType::F32),
            1 => Some(ParamType::U32),
            2 => Some(ParamType::I32),
            3 => Some(ParamType::Bool),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Apply {
    Live,
    Reboot,
}

impl Apply {
    pub fn to_u8(self) -> u8 {
        match self {
            Apply::Live => 0,
            Apply::Reboot => 1,
        }
    }
    pub fn from_u8(v: u8) -> Option<Self> {
        match v {
            0 => Some(Apply::Live),
            1 => Some(Apply::Reboot),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct ParamMeta {
    pub index: u16,
    pub name: String,
    pub ty: ParamType,
    pub apply: Apply,
    pub unit: String,
    pub min: f32,
    pub max: f32,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ParamValue {
    pub raw: u32,
}

impl ParamValue {
    pub fn from_f32(v: f32) -> Self {
        Self { raw: v.to_bits() }
    }
    pub fn as_f32(&self) -> f32 {
        f32::from_bits(self.raw)
    }
    pub fn from_u32(v: u32) -> Self {
        Self { raw: v }
    }
    pub fn as_u32(&self) -> u32 {
        self.raw
    }
    pub fn from_i32(v: i32) -> Self {
        Self { raw: v as u32 }
    }
    pub fn as_i32(&self) -> i32 {
        self.raw as i32
    }
    pub fn from_bool(v: bool) -> Self {
        Self { raw: v as u32 }
    }
    pub fn as_bool(&self) -> bool {
        self.raw != 0
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RangeError;

/// min <= max 时校验区间；min > max 表示不限制。
pub fn check_range(v: &ParamValue, meta: &ParamMeta) -> Result<(), RangeError> {
    if meta.min > meta.max {
        return Ok(());
    }
    let x = match meta.ty {
        ParamType::F32 => v.as_f32(),
        ParamType::U32 => v.as_u32() as f32,
        ParamType::I32 => v.as_i32() as f32,
        ParamType::Bool => v.raw as f32,
    };
    if x >= meta.min && x <= meta.max {
        Ok(())
    } else {
        Err(RangeError)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PayloadError;

pub fn encode_name(name: &str) -> Vec<u8> {
    let bytes = name.as_bytes();
    assert!(bytes.len() <= 255);
    let mut out = vec![bytes.len() as u8];
    out.extend_from_slice(bytes);
    out
}

pub fn decode_name(buf: &[u8], pos: &mut usize) -> Result<String, PayloadError> {
    let len = *buf.get(*pos).ok_or(PayloadError)? as usize;
    *pos += 1;
    let end = pos.checked_add(len).ok_or(PayloadError)?.min(buf.len());
    if buf.len() < *pos + len {
        return Err(PayloadError);
    }
    let s = std::str::from_utf8(&buf[*pos..*pos + len]).map_err(|_| PayloadError)?;
    *pos += len;
    let _ = end;
    Ok(s.to_string())
}

pub fn encode_meta_resp(meta: &ParamMeta) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(&meta.index.to_le_bytes());
    out.push(meta.ty.to_u8());
    out.push(meta.apply.to_u8());
    out.extend_from_slice(&meta.min.to_le_bytes());
    out.extend_from_slice(&meta.max.to_le_bytes());
    out.extend_from_slice(&encode_name(&meta.name));
    out.extend_from_slice(&encode_name(&meta.unit));
    out
}

pub fn decode_meta_resp(body: &[u8]) -> Result<ParamMeta, PayloadError> {
    if body.len() < 12 {
        return Err(PayloadError);
    }
    let index = u16::from_le_bytes([body[0], body[1]]);
    let ty = ParamType::from_u8(body[2]).ok_or(PayloadError)?;
    let apply = Apply::from_u8(body[3]).ok_or(PayloadError)?;
    let min = f32::from_le_bytes(body[4..8].try_into().unwrap());
    let max = f32::from_le_bytes(body[8..12].try_into().unwrap());
    let mut pos = 12;
    let name = decode_name(body, &mut pos)?;
    let unit = decode_name(body, &mut pos)?;
    Ok(ParamMeta { index, name, ty, apply, unit, min, max })
}

pub fn encode_value(v: ParamValue, ty: ParamType) -> [u8; 5] {
    let mut out = [0u8; 5];
    out[0] = ty.to_u8();
    out[1..5].copy_from_slice(&v.raw.to_le_bytes());
    out
}

pub fn decode_value(body: &[u8]) -> Result<(ParamType, ParamValue), PayloadError> {
    if body.len() < 5 {
        return Err(PayloadError);
    }
    let ty = ParamType::from_u8(body[0]).ok_or(PayloadError)?;
    let raw = u32::from_le_bytes(body[1..5].try_into().unwrap());
    Ok((ty, ParamValue { raw }))
}
```

`proto.rs`：
```rust
//! motor_proto.h 的 Rust 常量镜像。改动必须双侧同步并补金向量。

pub const VER: u8 = 0x01;
pub const SOF0: u8 = 0xA5;
pub const SOF1: u8 = 0x5A;
pub const MAX_PAYLOAD: usize = 512;
pub const RESPONSE_BIT: u8 = 0x80;

pub mod cmd {
    pub const PROTO_INFO: u8 = 0x01;
    pub const PARAM_META_COUNT: u8 = 0x02;
    pub const PARAM_META_GET: u8 = 0x03;
    pub const PARAM_READ: u8 = 0x04;
    pub const PARAM_WRITE: u8 = 0x05;
    pub const PARAM_COMMIT: u8 = 0x06;
    pub const STATUS_GET: u8 = 0x07;
    pub const CTRL_CMD: u8 = 0x08;
    pub const FAULT_GET: u8 = 0x09;
    pub const FAULT_CLEAR: u8 = 0x0A;
    pub const STREAM_SUB: u8 = 0x0B;
    pub const STREAM_UNSUB: u8 = 0x0C;
}

pub mod status {
    pub const OK: u8 = 0x00;
    pub const BAD_CMD: u8 = 0x01;
    pub const BAD_PARAM: u8 = 0x02;
    pub const BUSY: u8 = 0x03;
    pub const LOCKED: u8 = 0x04;
    pub const UNSUPPORTED: u8 = 0x05;
    pub const RANGE_ERR: u8 = 0x06;
}

pub mod ctrl {
    pub const START: u8 = 0x01;
    pub const STOP: u8 = 0x02;
    pub const SET_FREQ: u8 = 0x03;
    pub const SET_MOD: u8 = 0x04;
}
```

- [ ] **Step 4: 运行确认通过**

Run: `cargo test -p gui-core`
Expected: 全部 PASS。

- [ ] **Step 5: Commit**

```powershell
git add gui/core
git commit -m "feat(gui-core): params 模型 + proto 常量镜像（payload 编解码）"
```

---

### Task 7: gui-sim（SimDevice + YAML 加载 + TCP 服务）

**Files:**
- Create: `gui/sim/src/lib.rs`（改写）、`gui/sim/src/yaml.rs`、`gui/sim/src/main.rs`（改写）、`gui/sim/tests/sim_device.rs`

**Interfaces:**
- Consumes: `gui_core::codec::{Frame, encode, Decoder}`、`gui_core::proto`、`gui_core::params`
- Produces（Task 8 依赖）:
  - `gui_sim::SimError { Yaml(String), Io(std::io::Error) }`
  - `gui_sim::SimDevice::from_config_dir(dir: &std::path::Path) -> Result<SimDevice, SimError>`
  - `SimDevice::handle_frame(&mut self, req: &Frame) -> Frame`——同步纯函数式应答（响应 `cmd = req.cmd | 0x80`，`seq = req.seq`，payload 首字节 status）
  - `SimDevice::inject_fault(&mut self, flags: u32)`、`SimDevice::clear_faults(&mut self)`、`SimDevice::params(&self) -> &BTreeMap<String, ParamEntry>`
  - `gui_sim::ParamEntry { pub meta: gui_core::params::ParamMeta, pub value: gui_core::params::ParamValue }`
  - `gui_sim::serve(listener: tokio::net::TcpListener, dev: std::sync::Arc<tokio::sync::Mutex<SimDevice>>) -> Result<(), SimError>`——每连接一任务，读帧→`handle_frame`→写回
  - bin `gui-sim`：`gui-sim [--port 9000] [--config ../config]`，默认 `127.0.0.1:9000` 与仓库 `config/` 目录

**行为规约（写入测试）：**
- `PROTO_INFO`：resp body = `proto_major=1, proto_minor=0, caps=0x0000_0003(u32 LE), name_len=7, "gui-sim"`（caps bit0=参数写、bit1=控制）。
- `PARAM_META_COUNT`：body = count u16（参数总数）；`PARAM_META_GET`：req body = index u16；越界 → `BAD_PARAM`。
- `PARAM_READ`：req body = name len-prefixed；未知名 → `BAD_PARAM`；resp body = `encode_value(value, ty)`。
- `PARAM_WRITE`：req body = name len-prefixed | type u8 | value u32 LE；未知名/类型不符 → `BAD_PARAM`；`check_range` 失败 → `RANGE_ERR`。
- `PARAM_COMMIT`：若 `motor_running` → `LOCKED`；否则把全部参数写 `sim_params.json`（cwd 下）→ `OK`。
- `STATUS_GET`：body 按 `motor_proto.h` STATUS 布局；注入的 `fault_flags` OR 进返回值。
- `CTRL_CMD`：`START`：若 `latched_flags != 0` → `LOCKED`，否则 `motor_running=true`；`STOP`：`motor_running=false`；`SET_FREQ`：range 0.0..=1000.0 越界 → `RANGE_ERR`；`SET_MOD`：range 0.0..=100.0。未知 opcode → `BAD_CMD`。
- `FAULT_GET`：body = fault_flags u32 | latched_flags u32；`FAULT_CLEAR`：`latched_flags=0, fault_flags=0`。
- `STREAM_SUB` / `STREAM_UNSUB`：→ `UNSUPPORTED`（一级流属 M3）。
- 未知命令字 → `BAD_CMD`。
- YAML 加载（`yaml.rs`）：读 `motor.yaml`/`hardware.yaml`/`software.yaml`，递归展平 map，参数名 = `<域>.<路径>`（域=文件名去扩展名，路径用 `.` 连接）；标量推断类型：整数→`U32`（负数→`I32`），浮点→`F32`，`true/false`→`Bool`，带引号字符串（表达式）→ 跳过不入表（v1 不支持表达式求值，与 gen_params 的构建期求值不同，记入 README 限制）；`min>max`（不限制）；`apply`：名字含 `software.fault.` 或 `software.can.` 前缀 → `Reboot`，其余 `Live`。

- [ ] **Step 1: 写失败测试 `gui/sim/tests/sim_device.rs`**

```rust
use gui_core::codec::{Frame, Decoder};
use gui_core::proto::{cmd, ctrl, status};
use gui_sim::SimDevice;

fn req(cmd: u8, payload: &[u8]) -> Frame {
    Frame { ver: gui_core::proto::VER, cmd, seq: 0, payload: payload.to_vec() }
}

fn config_dir() -> std::path::PathBuf {
    std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../config")
}

#[test]
fn loads_yaml_params_with_domain_prefix() {
    let dev = SimDevice::from_config_dir(&config_dir()).unwrap();
    let params = dev.params();
    assert!(params.contains_key("motor.pole_pairs"), "motor.yaml 键应展平为 motor.<path>");
    assert!(params.contains_key("hardware.inverter.pwm_freq_hz"));
    assert!(params.contains_key("software.fault.oc_trip_a"));
}

#[test]
fn proto_info_reports_v1_and_name() {
    let mut dev = SimDevice::from_config_dir(&config_dir()).unwrap();
    let resp = dev.handle_frame(&req(cmd::PROTO_INFO, &[]));
    assert_eq!(resp.cmd, cmd::PROTO_INFO | gui_core::proto::RESPONSE_BIT);
    assert_eq!(resp.payload[0], status::OK);
    assert_eq!(resp.payload[1], 1); // proto_major
    assert_eq!(&resp.payload[3..7], &3u32.to_le_bytes()); // caps（[0]=status,[1]=major,[2]=minor,[3..7]=caps）
}

#[test]
fn param_read_write_roundtrip_and_range() {
    let mut dev = SimDevice::from_config_dir(&config_dir()).unwrap();
    // 读 motor.pole_pairs
    let mut payload = gui_core::params::encode_name("motor.pole_pairs");
    let resp = dev.handle_frame(&req(cmd::PARAM_READ, &payload));
    assert_eq!(resp.payload[0], status::OK);
    // 写 software.fault.vbus_ov_v（F32，写 60.0 合法）再读回
    let mut w = gui_core::params::encode_name("software.fault.vbus_ov_v");
    w.extend_from_slice(&gui_core::params::encode_value(
        gui_core::params::ParamValue::from_f32(60.0),
        gui_core::params::ParamType::F32,
    ));
    let resp = dev.handle_frame(&req(cmd::PARAM_WRITE, &w));
    assert_eq!(resp.payload[0], status::OK);
    let resp = dev.handle_frame(&req(cmd::PARAM_READ, &gui_core::params::encode_name("software.fault.vbus_ov_v")));
    let (_ty, v) = gui_core::params::decode_value(&resp.payload[1..]).unwrap();
    assert_eq!(v.as_f32(), 60.0);
}

#[test]
fn unknown_param_is_bad_param() {
    let mut dev = SimDevice::from_config_dir(&config_dir()).unwrap();
    let resp = dev.handle_frame(&req(cmd::PARAM_READ, &gui_core::params::encode_name("nope.none")));
    assert_eq!(resp.payload[0], status::BAD_PARAM);
}

#[test]
fn ctrl_interlocks_follow_terminal_semantics() {
    let mut dev = SimDevice::from_config_dir(&config_dir()).unwrap();
    // 故障锁存时 START → LOCKED
    dev.inject_fault(0x1);
    let mut p = vec![ctrl::START];
    p.extend_from_slice(&0f32.to_le_bytes());
    let resp = dev.handle_frame(&req(cmd::CTRL_CMD, &p));
    assert_eq!(resp.payload[0], status::LOCKED);
    // 清故障后 START → OK；COMMIT 运行中 → LOCKED
    dev.handle_frame(&req(cmd::FAULT_CLEAR, &[]));
    let resp = dev.handle_frame(&req(cmd::CTRL_CMD, &p));
    assert_eq!(resp.payload[0], status::OK);
    let resp = dev.handle_frame(&req(cmd::PARAM_COMMIT, &[]));
    assert_eq!(resp.payload[0], status::LOCKED);
    // STOP 后 COMMIT → OK
    let mut stop = vec![ctrl::STOP];
    stop.extend_from_slice(&0f32.to_le_bytes());
    dev.handle_frame(&req(cmd::CTRL_CMD, &stop));
    let resp = dev.handle_frame(&req(cmd::PARAM_COMMIT, &[]));
    assert_eq!(resp.payload[0], status::OK);
}

#[test]
fn stream_cmds_are_unsupported_in_v1() {
    let mut dev = SimDevice::from_config_dir(&config_dir()).unwrap();
    let resp = dev.handle_frame(&req(cmd::STREAM_SUB, &[0, 0]));
    assert_eq!(resp.payload[0], status::UNSUPPORTED);
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cargo test -p gui-sim`
Expected: FAIL（`SimDevice` 未定义）。

- [ ] **Step 3: 实现 `yaml.rs` / `lib.rs` / `main.rs`**

`yaml.rs` 核心（完整实现按下述规约）：
```rust
//! config/*.yaml → 参数表（名字 = <域>.<路径>）。
use gui_core::params::{Apply, ParamMeta, ParamType, ParamValue};
use std::collections::BTreeMap;

pub fn flatten_yaml(
    domain: &str,
    value: &serde_yaml::Value,
    prefix: &str,
    out: &mut BTreeMap<String, (ParamMeta, ParamValue)>,
    index: &mut u16,
) {
    match value {
        serde_yaml::Value::Mapping(map) => {
            for (k, v) in map {
                let key = k.as_str().unwrap_or_default().to_string();
                let path = if prefix.is_empty() { key } else { format!("{prefix}.{key}") };
                flatten_yaml(domain, v, &path, out, index);
            }
        }
        serde_yaml::Value::String(_) => {
            /* 带引号字符串 = 构建期表达式，v1 跳过 */
        }
        serde_yaml::Value::Number(n) => {
            let name = format!("{domain}.{prefix}");
            let (ty, val) = if let Some(i) = n.as_i64() {
                if i < 0 {
                    (ParamType::I32, ParamValue::from_i32(i as i32))
                } else {
                    (ParamType::U32, ParamValue::from_u32(i as u32))
                }
            } else {
                (ParamType::F32, ParamValue::from_f32(n.as_f64().unwrap_or(0.0) as f32))
            };
            out.insert(name.clone(), (meta_of(*index, &name, ty), val));
            *index += 1;
        }
        serde_yaml::Value::Bool(b) => {
            let name = format!("{domain}.{prefix}");
            out.insert(name.clone(), (meta_of(*index, &name, ParamType::Bool), ParamValue::from_bool(*b)));
            *index += 1;
        }
        _ => {}
    }
}

fn meta_of(index: u16, name: &str, ty: ParamType) -> ParamMeta {
    let apply = if name.starts_with("software.fault.") || name.starts_with("software.can.") {
        Apply::Reboot
    } else {
        Apply::Live
    };
    ParamMeta {
        index,
        name: name.to_string(),
        ty,
        apply,
        unit: String::new(),
        min: 1.0,
        max: 0.0, // min>max = 不限制（v1 简化，真范围以固件元数据为准）
    }
}
```

`lib.rs` 核心结构：
```rust
pub struct ParamEntry {
    pub meta: gui_core::params::ParamMeta,
    pub value: gui_core::params::ParamValue,
}

pub struct SimDevice {
    params: std::collections::BTreeMap<String, ParamEntry>,
    by_index: Vec<String>,
    fault_flags: u32,
    latched_flags: u32,
    motor_running: bool,
    freq_hz: f32,
    mod_pct: f32,
}

impl SimDevice {
    pub fn from_config_dir(dir: &std::path::Path) -> Result<Self, SimError> { /* 逐域读 yaml.rs 展平 */ }
    pub fn handle_frame(&mut self, req: &Frame) -> Frame {
        let (status, body) = self.dispatch(req.cmd, &req.payload);
        let mut payload = vec![status];
        payload.extend_from_slice(&body);
        Frame { ver: gui_core::proto::VER, cmd: req.cmd | gui_core::proto::RESPONSE_BIT, seq: req.seq, payload }
    }
    fn dispatch(&mut self, cmd: u8, payload: &[u8]) -> (u8, Vec<u8>) { /* 按行为规约 switch */ }
    pub fn inject_fault(&mut self, flags: u32) { self.fault_flags |= flags; self.latched_flags |= flags; }
    pub fn clear_faults(&mut self) { self.fault_flags = 0; self.latched_flags = 0; }
    pub fn params(&self) -> &std::collections::BTreeMap<String, ParamEntry> { &self.params }
}

pub async fn serve(listener: tokio::net::TcpListener, dev: std::sync::Arc<tokio::sync::Mutex<SimDevice>>) -> Result<(), SimError> {
    loop {
        let (mut sock, _) = listener.accept().await.map_err(SimError::Io)?;
        let dev = dev.clone();
        tokio::spawn(async move {
            use tokio::io::{AsyncReadExt, AsyncWriteExt};
            let mut dec = Decoder::new();
            let mut buf = [0u8; 2048];
            loop {
                let n = match sock.read(&mut buf).await { Ok(0) | Err(_) => break, Ok(n) => n };
                dec.feed(&buf[..n]);
                while let Some(req) = dec.next_frame() {
                    let resp = { let mut d = dev.lock().await; d.handle_frame(&req) };
                    if sock.write_all(&encode(&resp)).await.is_err() { break; }
                }
            }
        });
    }
}
```
`main.rs`：解析 `--port`（默认 9000）与 `--config`（默认 `../config`），`Arc<Mutex<SimDevice>>` + `serve`，启动打印监听地址。

- [ ] **Step 4: 运行确认通过**

Run: `cargo test -p gui-sim`
Expected: 6 tests PASS。注意 `loads_yaml_params_with_domain_prefix` 依赖仓库 `config/*.yaml`：`motor.pole_pairs` 等键名以实际 YAML 为准，若键名不符**修正测试期望值**（以 `config/motor.yaml` 实际键为准，不改 config/）。

- [ ] **Step 5: Commit**

```powershell
git add gui/sim
git commit -m "feat(gui-sim): 设备模拟器（YAML 参数同源 + 联锁语义 + TCP 服务）"
```

---

### Task 8: 集成测试（TCP 全链路）

**Files:**
- Create: `gui/tests/tests/tcp_roundtrip.rs`

**Interfaces:**
- Consumes: `gui_core::session::{Session, RequestKind}`、`gui_core::transport::TcpTransport`、`gui_sim::{SimDevice, serve}`
- Produces: 无（仅测试）

- [ ] **Step 1: 写失败测试**

```rust
use gui_core::proto::{cmd, ctrl, status};
use gui_core::session::{RequestKind, Session};
use gui_core::transport::TcpTransport;
use gui_sim::SimDevice;
use std::sync::Arc;

async fn start_sim() -> (String, Arc<tokio::sync::Mutex<SimDevice>>) {
    let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../config");
    let dev = Arc::new(tokio::sync::Mutex::new(SimDevice::from_config_dir(&dir).unwrap()));
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap().to_string();
    let d = dev.clone();
    tokio::spawn(async move {
        let _ = gui_sim::serve(listener, d).await;
    });
    (addr, dev)
}

#[tokio::test]
async fn handshake_meta_read_write_commit_over_tcp() {
    let (addr, _dev) = start_sim().await;
    let t = TcpTransport::connect(&addr).await.unwrap();
    let mut s = Session::new(t);

    // 握手
    let body = s.request(cmd::PROTO_INFO, &[], RequestKind::Read).await.unwrap();
    assert_eq!(body[0], 1); // proto_major

    // 元数据拉取
    let body = s.request(cmd::PARAM_META_COUNT, &[], RequestKind::Read).await.unwrap();
    let count = u16::from_le_bytes([body[0], body[1]]);
    assert!(count > 10);
    let idx = 0u16.to_le_bytes();
    let body = s.request(cmd::PARAM_META_GET, &idx, RequestKind::Read).await.unwrap();
    let meta = gui_core::params::decode_meta_resp(&body).unwrap();
    assert!(!meta.name.is_empty());

    // 读-写-读
    let name = gui_core::params::encode_name("software.fault.vbus_ov_v");
    let body = s.request(cmd::PARAM_READ, &name, RequestKind::Read).await.unwrap();
    let (ty, v0) = gui_core::params::decode_value(&body).unwrap();
    let mut w = name.clone();
    w.extend_from_slice(&gui_core::params::encode_value(
        gui_core::params::ParamValue::from_f32(v0.as_f32() + 1.0), ty));
    s.request(cmd::PARAM_WRITE, &w, RequestKind::Write).await.unwrap();
    let body = s.request(cmd::PARAM_READ, &name, RequestKind::Read).await.unwrap();
    let (_, v1) = gui_core::params::decode_value(&body).unwrap();
    assert_eq!(v1.as_f32(), v0.as_f32() + 1.0);

    // COMMIT（停机 OK）
    s.request(cmd::PARAM_COMMIT, &[], RequestKind::Write).await.unwrap();
}

#[tokio::test]
async fn locked_and_range_errors_surface_as_status() {
    let (addr, dev) = start_sim().await;
    let t = TcpTransport::connect(&addr).await.unwrap();
    let mut s = Session::new(t);

    // 注入故障 → START 返回 LOCKED
    dev.lock().await.inject_fault(0x1);
    let mut p = vec![ctrl::START];
    p.extend_from_slice(&0f32.to_le_bytes());
    let err = s.request(cmd::CTRL_CMD, &p, RequestKind::Write).await.unwrap_err();
    assert!(matches!(err, gui_core::session::SessionError::Status(x) if x == status::LOCKED));

    // 清故障后 START OK，随后 COMMIT → LOCKED（未停机）
    s.request(cmd::FAULT_CLEAR, &[], RequestKind::Write).await.unwrap();
    s.request(cmd::CTRL_CMD, &p, RequestKind::Write).await.unwrap();
    let err = s.request(cmd::PARAM_COMMIT, &[], RequestKind::Write).await.unwrap_err();
    assert!(matches!(err, gui_core::session::SessionError::Status(x) if x == status::LOCKED));

    // 未知参数 → BAD_PARAM
    let err = s
        .request(cmd::PARAM_READ, &gui_core::params::encode_name("x.y"), RequestKind::Read)
        .await
        .unwrap_err();
    assert!(matches!(err, gui_core::session::SessionError::Status(x) if x == status::BAD_PARAM));
}

#[tokio::test]
async fn fault_get_reflects_injection() {
    let (addr, dev) = start_sim().await;
    let t = TcpTransport::connect(&addr).await.unwrap();
    let mut s = Session::new(t);
    dev.lock().await.inject_fault(0x5);
    let body = s.request(cmd::FAULT_GET, &[], RequestKind::Read).await.unwrap();
    let flags = u32::from_le_bytes(body[0..4].try_into().unwrap());
    assert_eq!(flags & 0x5, 0x5);
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cargo test -p gui-tests`
Expected: FAIL（编译错误或断言失败）。

- [ ] **Step 3: 修正实现直到通过（仅改 gui/core、gui/sim，不改协议）**

Run: `cargo test --workspace`
Expected: 全部 PASS。

- [ ] **Step 4: Commit**

```powershell
git add gui/tests
git commit -m "test(gui): TCP 全链路集成测试（握手/元数据/读写/联锁/故障）"
```

---

### Task 9: 文档收尾

**Files:**
- Create: `gui/README.md`

**Interfaces:**
- Consumes: 全部前序任务
- Produces: 面向使用者的构建/测试/运行说明

- [ ] **Step 1: 写 `gui/README.md`**

必含内容：
1. 定位：Gui 上位机的 Rust 通讯核 + 设备模拟器（spec 链接）；
2. 构建与测试：`cargo build --workspace` / `cargo test --workspace`（在 `gui/` 下）；
3. 运行模拟器：`cargo run -p gui-sim -- --port 9000 --config ../config`；
4. 协议入口：`protocol/motor_proto.h`（双端契约）、`protocol/protocol_spec.md`、`protocol/test_vectors.json`；
5. 已知限制（如实记录）：v1 不含一级流（M3）、UsbBulk/Serial（M2）、frb（计划 2）；YAML 表达式值跳过（与 `gen_params.py` 的构建期求值不同）；参数 min/max 不限制（真范围以固件元数据为准）。

- [ ] **Step 2: 全量回归**

Run: `cargo test --workspace`
Expected: 全部 PASS。

- [ ] **Step 3: Commit**

```powershell
git add gui/README.md
git commit -m "docs(gui): README（构建/测试/模拟器运行/已知限制）"
```

---

## Self-Review 记录（计划完成后已执行）

1. **Spec 覆盖**：spec §3 布局→Task 1/2；§4 技术栈→Task 1；§5 Transport（Loopback/Tcp 部分）→Task 4（UsbBulk/Serial 明确标注属 M2，头部「范围边界」已声明）；§6 协议→Task 2/3/6（STREAM 预留为 UNSUPPORTED，与「一级流 v1 不实现」一致）；§7 core→Task 3/4/5/6（stream/frb 明确划出）；§8 sim→Task 7/8；§10 重试策略→Task 5 测试锁定；§11 前四层测试→Task 3/5/7/8。计划 2 承接 spec §9（Flutter）。
2. **占位符扫描**：无 TBD/TODO；所有测试含实际断言代码；yaml.rs match 语法为合法 Rust。
3. **类型一致性**：`RequestKind::Read/Write`、`SessionError::{Timeout,Status,Transport,Closed}`、`ParamValue{raw:u32}`、`SimDevice::handle_frame(&mut self, &Frame) -> Frame`、`encode_meta_resp/decode_meta_resp`、`encode_name/decode_name`、`encode_value/decode_value` 在 Task 6 定义、Task 7/8 引用，签名一致。`proto::VER` 与 `codec::VER` 均为 `0x01`（两处常量同值；codec 不依赖 proto，允许重复常量）。
