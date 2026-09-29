# Gui Flutter A 阶段 UI 实现计划（计划 2）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付可直接运行的 Windows 图形界面（`Gui/app`，产品名 Gui）：连接管理、元数据驱动参数面板、Dashboard（状态/电机控制/联锁），经 flutter_rust_bridge 驱动既有的 gui-core/gui-sim，`flutter build windows --release` 产出可执行文件。

**Architecture:** UI（Flutter/Dart）→ frb 薄绑定（`lib/bridge/`，唯一触碰 Rust 处）→ `gui-frb` crate（全局单会话 + tokio runtime，DTO 化导出）→ `gui-core`（Session/params/codec，零改动行为）→ TCP → `gui-sim`（无硬件联调）或真板（M2）。参数面板由 `PARAM_META_*` 元数据自动生成，零硬编码参数名。

**Tech Stack:** Flutter stable（Windows desktop）、Dart、flutter_rust_bridge 2.x、Rust（gui-core 复用）、VS Build Tools（MSVC，Flutter Windows 构建官方前置）。

**Spec:** `docs/superpowers/specs/2026-09-29-gui-host-tool-design.md`（§9 UI 结构、§10 数据流/错误、A 阶段范围）
**计划 1 交付物（已入库）：** `Gui/protocol/`（契约+金向量）、`Gui/core/`（codec/transport/session/params/proto）、`Gui/sim/`（设备模拟器）、`Gui/tests/`（TCP 集成测试）；42 tests 绿。

**范围边界：** 只做 A 阶段（连接/参数/Dashboard）；示波器（B）与录制/向导（C）不实现；UsbBulk/Serial transport UI 选项保留占位（禁用态）；Linux/macOS 构建不在本期验收（代码不写死平台即可）。

## Global Constraints

- 命名：产品 **Gui**；Dart 包名 `g6618_gui`；应用目录 `Gui/app`；frb crate `gui-frb`（标识符 gui_frb）。
- **UI 永不直接触碰传输/协议**：Dart 只调 `lib/bridge/` 生成物；协议细节（帧/命令字/状态码）只存在于 Rust 侧。
- **参数面板零硬编码**：UI 完全由 `ParamMetaDto` 列表生成；固件加参数上位机零改动。
- 错误呈现：`FrbError.code` = 设备状态码（2=BAD_PARAM 3=BUSY 4=LOCKED 5=UNSUPPORTED 6=RANGE_ERR）或本地码（0x100=Timeout 0x101=Transport 0x102=Disconnected 0x1FF=Internal）；LOCKED 必须显示原因提示（故障锁存→引导 FAULT_CLEAR；未停机→引导 STOP）。
- 联锁语义与协议一致（`Gui/protocol/protocol_spec.md`）：START 在故障锁存时 LOCKED；PARAM_COMMIT 在「运行中或故障锁存」时 LOCKED。
- 主题：Material 3，**深色**为默认；主题集中在 `lib/ui/theme.dart` 单处定义。
- 状态栏常驻：连接态 / 重连计数 / CRC 错误计数 / 请求失败计数（数据源 `Session::stats` 经 frb 暴露）。
- 既有 `Gui/core`、`Gui/sim`、`Gui/tests`、`Gui/protocol` **行为不得改变**（可加 `frb_api` 之外的新文件；发现缺陷报 controller 裁定，禁自行改）。
- 固件目录（`App/` `Driver/` `Interface/` `Board/` `config/` `scripts/`）禁改。
- 回归门：`Gui/` 下 `cargo test --workspace` 全绿 + `flutter test` 全绿 + `flutter build windows --release` 成功。
- 每任务禁跑 git（控制器提交）；报告文件路径见各任务派发指令。

## File Structure

```text
Gui/
├── frb/                            # 新 crate gui-frb（cdylib，frb 生成物的目标）
│   ├── Cargo.toml
│   ├── src/
│   │   ├── lib.rs                  # pub mod api; + flutter_rust_bridge 集成入口
│   │   └── api.rs                  # frb 导出面（DTO + 全局单会话 + tokio runtime）
│   └── tests/
│       └── api_roundtrip.rs        # 对 gui-sim 的 API 级集成测试
└── app/                            # Flutter 工程（flutter create 生成 windows/ 等平台目录）
    ├── pubspec.yaml                # name: g6618_gui
    ├── flutter_rust_bridge.yaml    # codegen 配置（rust_root: ../frb）
    ├── lib/
    │   ├── main.dart               # 入口：主题 + 壳（NavigationRail + 状态栏）
    │   ├── bridge/                 # frb 生成物（frb_codegen 输出，不手改）
    │   ├── state/
    │   │   └── app_state.dart      # ChangeNotifier：连接/参数表/状态快照/计数器
    │   └── ui/
    │       ├── theme.dart          # Material 3 深色主题
    │       ├── shell.dart          # 导航壳 + 底部 StatusBar
    │       ├── status_bar.dart
    │       ├── connect_page.dart   # 连接管理
    │       ├── param_page.dart     # 参数面板（元数据驱动）
    │       └── dashboard_page.dart # 状态 + 电机控制
    └── test/
        ├── param_page_test.dart
        └── dashboard_page_test.dart
```

依赖方向单向：`ui/ → state/ → bridge/ → gui-frb → gui-core`。

---

### Task 1: 工具链（VS Build Tools + Flutter SDK + frb codegen）

**Files:**
- Create: `Gui/app/`（本任务只到 `flutter doctor` 通过 + 空工程可构建；工程内容属 Task 3）— 实际创建 `Gui/app/.gitkeep` 占位即可
- Modify: 无

**Interfaces:**
- Consumes: 无
- Produces: 可用工具链（后续任务依赖的命令）：`flutter`（PATH 或绝对路径）、`flutter_rust_bridge_codegen`、MSVC `cl.exe`/`link.exe`（VS Build Tools）。

- [ ] **Step 1: 安装 VS Build Tools（MSVC，Flutter Windows 构建强制前置）**

Run (PowerShell, 管理员):
```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive --norestart" --accept-source-agreements --accept-package-agreements
```
完成后验证（新终端）：
```powershell
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property displayName
```
Expected: 输出 VS Build Tools 2022 名称行。若 winget 失败，报 BLOCKED（此为 Flutter Windows 构建硬前置，不可绕过）。

- [ ] **Step 2: 安装 Flutter SDK（git clone stable）**

```powershell
git clone https://github.com/flutter/flutter.git -b stable D:\Tools\flutter
[Environment]::SetEnvironmentVariable('Path', [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User') + ';D:\Tools\flutter\bin', 'User')
# 新终端后：
flutter --version        # 记录版本进报告
flutter config --no-analytics
flutter doctor           # Windows 桌面工具链 OK（除 Android/iary 可忽略）
```
Expected: `flutter doctor` 中「Visual Studio」一项 ✓。

- [ ] **Step 3: 安装 flutter_rust_bridge_codegen**

```powershell
& "$env:USERPROFILE\.cargo\bin\cargo.exe" install flutter_rust_bridge_codegen --locked
flutter_rust_bridge_codegen --version
```
记录版本；Task 2 的 `flutter_rust_bridge` crate 依赖版本必须与 codegen 主.次版本一致（见 Task 2 Step 1）。

- [ ] **Step 4: 验证 Windows 构建链路（空工程冒烟）**

```powershell
cd D:\Codes\HPM53M1_G6618Motor\Gui
flutter create --platforms=windows --project-name g6618_gui app
cd app
flutter build windows --debug
```
Expected: `build\windows\x64\debug\` 产出 exe（默认计数器 demo 无需运行验证）。此步通过 = 本机具备「可直接运行编译产物」的全链路。

- [ ] **Step 5: Commit（由控制器执行；实现者禁 git）**

提交内容：`Gui/app/`（flutter create 产物，**删除 `build/` 后提交**；`Gui/app/build/` 加入 `.gitignore`）、`Gui/app/.gitkeep` 不需要（app 已有内容）。

---

### Task 2: gui-frb API 层（Rust 侧）

**Files:**
- Create: `Gui/frb/Cargo.toml`、`Gui/frb/src/lib.rs`、`Gui/frb/src/api.rs`、`Gui/frb/tests/api_roundtrip.rs`
- Modify: `Gui/Cargo.toml`（workspace members += "frb"）

**Interfaces:**
- Consumes（计划 1 已定稿签名）: `gui_core::session::{Session, SessionError, RequestKind, SessionStats}`；`gui_core::transport::TcpTransport`（`connect(&str) -> Result<TcpTransport, TransportError>`）；`gui_core::params::{ParamMeta, ParamValue, ParamType, Apply, encode_name, decode_meta_resp, encode_meta_resp, decode_value, encode_value, PayloadError}`；`gui_core::proto::{cmd, ctrl, status, RESPONSE_BIT}`；`gui_core::codec::{Frame, encode, MAX_PAYLOAD}`。
- Produces（Task 3~6 的 Dart 侧依赖，精确签名）:

```rust
pub struct ParamMetaDto { pub index: u16, pub name: String, pub ty: u8, pub apply: u8, pub unit: String, pub min: f32, pub max: f32 }
pub struct ParamValueDto { pub ty: u8, pub raw: u32 }
pub struct StatusDto { pub fault_flags: u32, pub motor_running: bool, pub motor_state: u8, pub freq_hz: f32, pub mod_pct: f32, pub i_u: f32, pub i_v: f32, pub i_w: f32, pub vbus: f32, pub ntc_temp: f32, pub enc_rotor_deg: f32, pub enc_output_deg: f32 }
pub struct StatsDto { pub crc_errors: u64, pub resyncs: u64, pub attempts: u64, pub reconnects: u64 }
pub struct FrbError { pub code: u32, pub message: String }

pub async fn connect(addr: String) -> Result<(), FrbError>;
pub async fn disconnect() -> Result<(), FrbError>;
pub fn is_connected() -> bool;
pub async fn proto_info() -> Result<(u8, u8, u32, String), FrbError>;
pub async fn param_meta_count() -> Result<u16, FrbError>;
pub async fn param_meta_get(index: u16) -> Result<ParamMetaDto, FrbError>;
pub async fn param_read(name: String) -> Result<ParamValueDto, FrbError>;
pub async fn param_write(name: String, ty: u8, raw: u32) -> Result<(), FrbError>;
pub async fn param_commit() -> Result<(), FrbError>;
pub async fn status_get() -> Result<StatusDto, FrbError>;
pub async fn ctrl_start() -> Result<(), FrbError>;
pub async fn ctrl_stop() -> Result<(), FrbError>;
pub async fn ctrl_set_freq(freq_hz: f32) -> Result<(), FrbError>;
pub async fn ctrl_set_mod(mod_pct: f32) -> Result<(), FrbError>;
pub async fn fault_get() -> Result<(u32, u32), FrbError>;
pub async fn fault_clear() -> Result<(), FrbError>;
pub async fn stats_get() -> Result<StatsDto, FrbError>;
```

**FrbError.code 词表（Global Constraints 的错误呈现依此）：** 设备 status 码原样（2..6）；本地 `0x100`=Timeout、`0x101`=Transport、`0x102`=Disconnected、`0x1FF`=Internal。`SessionError::Status(s)` → `code=s`；`Timeout` → 0x100；`Transport(_)|Closed` → 0x101/0x102 且内部置 disconnected。

- [ ] **Step 1: 建 crate 与依赖**

`Gui/frb/Cargo.toml`：
```toml
[package]
name = "gui-frb"
version.workspace = true
edition.workspace = true
license.workspace = true

[lib]
crate-type = ["cdylib", "staticlib"]

[dependencies]
gui-core = { path = "../core" }
tokio = { workspace = true, features = ["rt-multi-thread", "macros", "net", "io-util", "time", "sync"] }
flutter_rust_bridge = "2"
```
`Gui/Cargo.toml` members 改为 `members = ["core", "sim", "tests", "frb"]`。
**版本一致性**：`cargo tree -p gui-frb -i flutter_rust_bridge` 查实际解析版本（记入报告）；codegen 版本主.次必须一致，不一致则 `cargo install flutter_rust_bridge_codegen --version <解析版本>` 重装。

- [ ] **Step 2: 写失败测试 `Gui/frb/tests/api_roundtrip.rs`**

```rust
//! API 级集成测试：起 gui-sim → gui-frb 全流程。
use std::sync::Arc;

#[test]
fn api_roundtrip_against_sim() {
    // gui-sim 作为库内嵌：临时端口起 serve
    let rt = tokio::runtime::Runtime::new().unwrap();
    let addr = rt.block_on(async {
        let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../config");
        let dev = Arc::new(tokio::sync::Mutex::new(
            gui_sim::SimDevice::from_config_dir(&dir).unwrap(),
        ));
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap().to_string();
        let d = dev.clone();
        std::thread::spawn(move || {
            let rt = tokio::runtime::Runtime::new().unwrap();
            rt.block_on(async move { let _ = gui_sim::serve(listener, d).await; });
        });
        addr
    });

    rt.block_on(async {
        gui_frb::api::connect(addr.clone()).await.unwrap();
        let (major, _minor, caps, name) = gui_frb::api::proto_info().await.unwrap();
        assert_eq!(major, 1);
        assert_eq!(caps & 0x3, 0x3);
        assert_eq!(name, "gui-sim");

        let count = gui_frb::api::param_meta_count().await.unwrap();
        assert!(count > 10);
        let meta = gui_frb::api::param_meta_get(0).await.unwrap();
        assert!(!meta.name.is_empty());

        // 读-写-读
        let v0 = gui_frb::api::param_read("software.fault.vbus_ov_v".into()).await.unwrap();
        gui_frb::api::param_write("software.fault.vbus_ov_v".into(), v0.ty, v0.raw).await.unwrap();
        let v1 = gui_frb::api::param_read("software.fault.vbus_ov_v".into()).await.unwrap();
        assert_eq!(v0.raw, v1.raw);

        // 联锁：START(无故障)=OK → COMMIT=LOCKED(4) → STOP → COMMIT=OK
        gui_frb::api::ctrl_start().await.unwrap();
        let err = gui_frb::api::param_commit().await.unwrap_err();
        assert_eq!(err.code, 4);
        gui_frb::api::ctrl_stop().await.unwrap();
        gui_frb::api::param_commit().await.unwrap();

        // 故障注入不可经 API → 用 fault_get/clear 路径验证可读
        let (flags, latched) = gui_frb::api::fault_get().await.unwrap();
        assert_eq!(flags | latched, 0);
        gui_frb::api::disconnect().await.unwrap();
        assert!(!gui_frb::api::is_connected());
    });
}
```
依赖：`Gui/frb/Cargo.toml` `[dev-dependencies] gui-sim = { path = "../sim" } tokio = { workspace = true, features = ["full"] }`。
Run: `cargo test -p gui-frb` → Expected: FAIL（api 未定义）。

- [ ] **Step 3: 实现 `api.rs`（完整）**

```rust
//! frb 导出面：Gui 客户端 API（全局单会话 + tokio runtime）。
use flutter_rust_bridge::frb;
use gui_core::codec::encode;
use gui_core::params::{
    decode_meta_resp, decode_value, encode_name, encode_value, Apply, ParamMeta, ParamType,
    ParamValue,
};
use gui_core::proto::{cmd, ctrl, status};
use gui_core::session::{RequestKind, Session, SessionError};
use gui_core::transport::TcpTransport;
use std::future::Future;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::OnceLock;
use tokio::sync::Mutex;

static RUNTIME: OnceLock<tokio::runtime::Runtime> = OnceLock::new();
static CLIENT: OnceLock<Mutex<Option<Session<TcpTransport>>>> = OnceLock::new();
static CONNECTED: AtomicBool = AtomicBool::new(false);
static RECONNECTS: AtomicU64 = AtomicU64::new(0);

fn runtime() -> &'static tokio::runtime::Runtime {
    RUNTIME.get_or_init(|| tokio::runtime::Runtime::new().expect("tokio runtime"))
}
fn client() -> &'static Mutex<Option<Session<TcpTransport>>> {
    CLIENT.get_or_init(|| Mutex::new(None))
}

#[derive(Debug, Clone)]
pub struct FrbError { pub code: u32, pub message: String }
impl std::fmt::Display for FrbError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "code={} {}", self.code, self.message)
    }
}
impl std::error::Error for FrbError {}

fn map_err(e: SessionError) -> FrbError {
    match e {
        SessionError::Status(s) => FrbError { code: s as u32, message: format!("device status {s}") },
        SessionError::Timeout => FrbError { code: 0x100, message: "request timeout".into() },
        SessionError::Transport(err) => {
            CONNECTED.store(false, Ordering::SeqCst);
            FrbError { code: 0x101, message: format!("transport: {err}") }
        }
        SessionError::Closed => {
            CONNECTED.store(false, Ordering::SeqCst);
            FrbError { code: 0x102, message: "connection closed".into() }
        }
        SessionError::BadPayload => FrbError { code: 0x1FF, message: "payload too long".into() },
    }
}

/// 在自有 tokio runtime 上执行（frb 线程无 reactor 上下文）。
async fn on_tokio<F, T>(fut: F) -> Result<T, FrbError>
where
    F: Future<Output = Result<T, FrbError>> + Send + 'static,
    T: Send + 'static,
{
    runtime()
        .spawn(fut)
        .await
        .map_err(|e| FrbError { code: 0x1FF, message: e.to_string() })?
}

/// 请求便捷封装：锁定单会话 → request → 剥 status（Session 已剥）。
async fn request(cmd: u8, payload: Vec<u8>, kind: RequestKind) -> Result<Vec<u8>, FrbError> {
    let mut guard = client().lock().await;
    let Some(sess) = guard.as_mut() else {
        return Err(FrbError { code: 0x102, message: "not connected".into() });
    };
    sess.request(cmd, &payload, kind).await.map_err(map_err)
}

// ---- 连接 ----

pub async fn connect(addr: String) -> Result<(), FrbError> {
    on_tokio(async move {
        let t = TcpTransport::connect(&addr).await.map_err(|e| FrbError {
            code: 0x101,
            message: format!("connect {addr}: {e}"),
        })?;
        let mut guard = client().lock().await;
        *guard = Some(Session::new(t));
        CONNECTED.store(true, Ordering::SeqCst);
        // 握手：版本不符即拒绝（协议规则）
        let body = {
            let Some(sess) = guard.as_mut() else { unreachable!() };
            sess.request(cmd::PROTO_INFO, &[], RequestKind::Read).await.map_err(map_err)?
        };
        if body.is_empty() || body[0] != 1 {
            *guard = None;
            CONNECTED.store(false, Ordering::SeqCst);
            return Err(FrbError { code: 0x1FF, message: "proto major mismatch".into() });
        }
        Ok(())
    })
    .await?
}

pub async fn disconnect() -> Result<(), FrbError> {
    on_tokio(async move {
        *client().lock().await = None;
        CONNECTED.store(false, Ordering::SeqCst);
        Ok(())
    })
    .await?
}

pub fn is_connected() -> bool {
    CONNECTED.load(Ordering::SeqCst)
}

pub async fn proto_info() -> Result<(u8, u8, u32, String), FrbError> {
    on_tokio(async move {
        let body = request(cmd::PROTO_INFO, vec![], RequestKind::Read).await?;
        // body: major u8 | minor u8 | caps u32 | name_len u8 | name
        if body.len() < 7 {
            return Err(FrbError { code: 0x1FF, message: "short PROTO_INFO".into() });
        }
        let caps = u32::from_le_bytes(body[2..6].try_into().unwrap());
        let n = body[6] as usize;
        let name = String::from_utf8_lossy(&body[7..7 + n]).into_owned();
        Ok((body[0], body[1], caps, name))
    })
    .await?
}

// ---- 参数 ----

pub async fn param_meta_count() -> Result<u16, FrbError> {
    on_tokio(async move {
        let body = request(cmd::PARAM_META_COUNT, vec![], RequestKind::Read).await?;
        if body.len() < 2 {
            return Err(FrbError { code: 0x1FF, message: "short META_COUNT".into() });
        }
        Ok(u16::from_le_bytes([body[0], body[1]]))
    })
    .await?
}

#[derive(Debug, Clone)]
pub struct ParamMetaDto {
    pub index: u16,
    pub name: String,
    pub ty: u8,
    pub apply: u8,
    pub unit: String,
    pub min: f32,
    pub max: f32,
}

pub async fn param_meta_get(index: u16) -> Result<ParamMetaDto, FrbError> {
    on_tokio(async move {
        let body = request(cmd::PARAM_META_GET, index.to_le_bytes().to_vec(), RequestKind::Read).await?;
        let m: ParamMeta = decode_meta_resp(&body)
            .map_err(|_| FrbError { code: 0x1FF, message: "bad META resp".into() })?;
        Ok(ParamMetaDto {
            index: m.index,
            name: m.name,
            ty: m.ty.to_u8(),
            apply: m.apply.to_u8(),
            unit: m.unit,
            min: m.min,
            max: m.max,
        })
    })
    .await?
}

#[derive(Debug, Clone)]
pub struct ParamValueDto { pub ty: u8, pub raw: u32 }

pub async fn param_read(name: String) -> Result<ParamValueDto, FrbError> {
    on_tokio(async move {
        let payload = encode_name(&name).map_err(|_| FrbError { code: 0x1FF, message: "name too long".into() })?;
        let body = request(cmd::PARAM_READ, payload, RequestKind::Read).await?;
        let (ty, v) = decode_value(&body)
            .map_err(|_| FrbError { code: 0x1FF, message: "bad VALUE resp".into() })?;
        Ok(ParamValueDto { ty: ty.to_u8(), raw: v.raw })
    })
    .await?
}

pub async fn param_write(name: String, ty: u8, raw: u32) -> Result<(), FrbError> {
    on_tokio(async move {
        let mut payload =
            encode_name(&name).map_err(|_| FrbError { code: 0x1FF, message: "name too long".into() })?;
        let ty = ParamType::from_u8(ty)
            .ok_or_else(|| FrbError { code: 0x1FF, message: "bad type".into() })?;
        payload.extend_from_slice(&encode_value(ParamValue { raw }, ty));
        request(cmd::PARAM_WRITE, payload, RequestKind::Write).await?;
        Ok(())
    })
    .await?
}

pub async fn param_commit() -> Result<(), FrbError> {
    on_tokio(async move {
        request(cmd::PARAM_COMMIT, vec![], RequestKind::Write).await?;
        Ok(())
    })
    .await?
}

// ---- 状态 / 控制 / 故障 ----

#[derive(Debug, Clone)]
pub struct StatusDto {
    pub fault_flags: u32,
    pub motor_running: bool,
    pub motor_state: u8,
    pub freq_hz: f32,
    pub mod_pct: f32,
    pub i_u: f32,
    pub i_v: f32,
    pub i_w: f32,
    pub vbus: f32,
    pub ntc_temp: f32,
    pub enc_rotor_deg: f32,
    pub enc_output_deg: f32,
}

fn f32_at(b: &[u8], o: usize) -> f32 {
    f32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}

pub async fn status_get() -> Result<StatusDto, FrbError> {
    on_tokio(async move {
        let b = request(cmd::STATUS_GET, vec![], RequestKind::Read).await?;
        if b.len() < 42 {
            return Err(FrbError { code: 0x1FF, message: "short STATUS".into() });
        }
        Ok(StatusDto {
            fault_flags: u32::from_le_bytes(b[0..4].try_into().unwrap()),
            motor_running: b[4] != 0,
            motor_state: b[5],
            freq_hz: f32_at(&b, 6),
            mod_pct: f32_at(&b, 10),
            i_u: f32_at(&b, 14),
            i_v: f32_at(&b, 18),
            i_w: f32_at(&b, 22),
            vbus: f32_at(&b, 26),
            ntc_temp: f32_at(&b, 30),
            enc_rotor_deg: f32_at(&b, 34),
            enc_output_deg: f32_at(&b, 38),
        })
    })
    .await?
}

async fn ctrl_op(opcode: u8, value: f32) -> Result<(), FrbError> {
    let mut payload = vec![opcode];
    payload.extend_from_slice(&value.to_le_bytes());
    request(cmd::CTRL_CMD, payload, RequestKind::Write).await?;
    Ok(())
}

pub async fn ctrl_start() -> Result<(), FrbError> {
    on_tokio(ctrl_op(ctrl::START, 0.0)).await?
}
pub async fn ctrl_stop() -> Result<(), FrbError> {
    on_tokio(ctrl_op(ctrl::STOP, 0.0)).await?
}
pub async fn ctrl_set_freq(freq_hz: f32) -> Result<(), FrbError> {
    on_tokio(ctrl_op(ctrl::SET_FREQ, freq_hz)).await?
}
pub async fn ctrl_set_mod(mod_pct: f32) -> Result<(), FrbError> {
    on_tokio(ctrl_op(ctrl::SET_MOD, mod_pct)).await?
}

pub async fn fault_get() -> Result<(u32, u32), FrbError> {
    on_tokio(async move {
        let b = request(cmd::FAULT_GET, vec![], RequestKind::Read).await?;
        if b.len() < 8 {
            return Err(FrbError { code: 0x1FF, message: "short FAULT".into() });
        }
        Ok((
            u32::from_le_bytes(b[0..4].try_into().unwrap()),
            u32::from_le_bytes(b[4..8].try_into().unwrap()),
        ))
    })
    .await?
}

pub async fn fault_clear() -> Result<(), FrbError> {
    on_tokio(async move {
        request(cmd::FAULT_CLEAR, vec![], RequestKind::Write).await?;
        Ok(())
    })
    .await?
}

#[derive(Debug, Clone)]
pub struct StatsDto {
    pub crc_errors: u64,
    pub resyncs: u64,
    pub attempts: u64,
    pub reconnects: u64,
}

pub async fn stats_get() -> Result<StatsDto, FrbError> {
    on_tokio(async move {
        let mut guard = client().lock().await;
        let Some(sess) = guard.as_mut() else {
            return Ok(StatsDto {
                crc_errors: 0,
                resyncs: 0,
                attempts: 0,
                reconnects: RECONNECTS.load(Ordering::SeqCst),
            });
        };
        let s = sess.stats();
        Ok(StatsDto {
            crc_errors: s.crc_errors,
            resyncs: s.resyncs,
            attempts: s.attempts,
            reconnects: RECONNECTS.load(Ordering::SeqCst),
        })
    })
    .await?
}
```

`lib.rs`：
```rust
//! gui-frb：Flutter ↔ gui-core 桥接层。
pub mod api;
```

注意两点（实现时按需微调，语义不变）：
1. `#[frb]` 属性仅在需要控制导出名时加；frb v2 默认导出全部 pub fn/struct——确保 `lib.rs` 不导出内部工具项（`on_tokio`/`request`/`map_err` 等用 `pub(crate)` 或移入非导出模块）。
2. `ParamType`/`Apply` 仅取 `to_u8/from_u8`，DTO 用 u8 传枚举，Dart 侧 switch。

- [ ] **Step 4: 运行确认通过**

Run: `cargo test -p gui-frb` 然后 `cargo test --workspace`（`Gui/` 下）。
Expected: api_roundtrip PASS；workspace 全绿（42+1 tests），零警告。注意 `cargo test --workspace` 会因 cdylib 目标构建 gui-frb，属正常。

- [ ] **Step 5: Commit**（控制器执行）`feat(gui-frb): frb API 层（DTO/单会话/tokio runtime + sim 集成测试）`

---

### Task 3: Flutter 工程 + frb 集成 + 首个可运行 exe

**Files:**
- Modify: `Gui/app/pubspec.yaml`（+ `flutter_rust_bridge: ^2.x` 依赖，版本与 Task 2 记录一致）、`Gui/app/lib/main.dart`（重写为壳骨架）
- Create: `Gui/app/flutter_rust_bridge.yaml`、`Gui/app/lib/ui/theme.dart`、`Gui/app/lib/state/app_state.dart`（本任务只到 connect/proto_info 可用）
- 生成物: `Gui/app/lib/bridge/`（`flutter_rust_bridge_codegen generate` 输出）
- Modify: `.gitignore`（+ `Gui/app/build/`、`Gui/app/.dart_tool/`、`Gui/app/windows/flutter/ephemeral/`）

**Interfaces:**
- Consumes: Task 2 全部 frb API（codegen 后 Dart 侧同名方法，位于 `lib/bridge/frb/api.dart`）
- Produces: `AppState extends ChangeNotifier`（后续任务扩展）：
  ```dart
  class AppState extends ChangeNotifier {
    bool connected; String lastError; String deviceName;
    Future<void> connect(String addr); Future<void> disconnect();
  }
  ```
  `theme.dart` 提供 `ThemeData buildGuiTheme()`（Material 3，`ColorScheme.dark()` 为基础，seed `Color(0xFF4DB6AC)`）。

- [ ] **Step 1: frb codegen 配置与执行**

`Gui/app/flutter_rust_bridge.yaml`：
```yaml
rust_input: crate::api
rust_root: ../frb
dart_output: lib/bridge/frb
c_output: windows/flutter/generated_rust_bridge.h   # windows 平台接线用
dart_entrypoint_class_name: RustLib
```
Run（在 `Gui/app` 下）：
```powershell
flutter_rust_bridge_codegen generate
flutter pub add flutter_rust_bridge
```
Expected: `lib/bridge/frb/` 生成 `api.dart`、`frb.dart` 等；`pubspec.yaml` 出现依赖。若 codegen 报版本不匹配，按 Task 2 Step 1 的版本一致性规则处理。

- [ ] **Step 2: 写壳 `main.dart` + `theme.dart` + `app_state.dart`**

`lib/ui/theme.dart`：
```dart
import 'package:flutter/material.dart';

ThemeData buildGuiTheme() {
  final scheme = ColorScheme.fromSeed(
    seedColor: const Color(0xFF4DB6AC),
    brightness: Brightness.dark,
  );
  return ThemeData(
    useMaterial3: true,
    colorScheme: scheme,
    scaffoldBackgroundColor: scheme.surface,
  );
}
```

`lib/state/app_state.dart`（本任务范围：连接 + 设备名）：
```dart
import 'package:flutter/foundation.dart';
import '../bridge/frb/api.dart' as frb;

class AppState extends ChangeNotifier {
  bool connected = false;
  String lastError = '';
  String deviceName = '';

  Future<bool> connect(String addr) async {
    try {
      await frb.connect(addr: addr);
      final info = await frb.protoInfo();
      deviceName = info.item4;
      connected = true;
      lastError = '';
      notifyListeners();
      return true;
    } on Exception catch (e) {
      lastError = e.toString();
      connected = false;
      notifyListeners();
      return false;
    }
  }

  Future<void> disconnect() async {
    await frb.disconnect();
    connected = false;
    deviceName = '';
    notifyListeners();
  }
}
```
（frb 生成的方法名/记录形态以实际 codegen 输出为准：`protoInfo()` 返回 record 时用 `info.$4`，返回类时用字段——**以生成物为准适配，语义不变**。）

`lib/main.dart`：
```dart
import 'package:flutter/material.dart';
import 'state/app_state.dart';
import 'ui/theme.dart';
import 'ui/shell.dart';

void main() {
  runApp(const GuiApp());
}

class GuiApp extends StatelessWidget {
  const GuiApp({super.key});
  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: AppState(),
      builder: (context, _) => MaterialApp(
        title: 'Gui',
        debugShowCheckedModeBanner: false,
        theme: buildGuiTheme(),
        home: const Shell(),
      ),
    );
  }
}
```
（`Shell` 属 Task 4；本任务先以 `home: const Scaffold(body: Center(child: Text('Gui')))` 占位，Task 4 替换。）

- [ ] **Step 3: 构建可运行产物**

Run: `flutter build windows --debug`（在 `Gui/app`）。
Expected: `build\windows\x64\debug\g6618_gui.exe` 产出并可双击启动（显示 Gui 占位页）。

- [ ] **Step 4: Commit**（控制器）`feat(gui-app): Flutter 工程 + frb 集成（可运行 exe 骨架）`

---

### Task 4: 连接管理页 + 壳 + 状态栏

**Files:**
- Create: `Gui/app/lib/ui/shell.dart`、`Gui/app/lib/ui/connect_page.dart`、`Gui/app/lib/ui/status_bar.dart`
- Modify: `Gui/app/lib/main.dart`（home → `Shell`）、`Gui/app/lib/state/app_state.dart`（扩展：状态轮询计数器、reconnects、crcErrors）

**Interfaces:**
- Consumes: `AppState.connect/disconnect`、frb `statsGet()`
- Produces（Task 5/6 依赖）：
  ```dart
  class AppState extends ChangeNotifier {
    bool connected; String lastError; String deviceName;
    int reconnects; int crcErrors; int requestFailures;
    Timer? statusTimer;          // 500ms 周期 statsGet()（连通时）
    Future<bool> connect(String addr); Future<void> disconnect();
  }
  class Shell extends StatefulWidget   // NavigationRail: 0=连接 1=参数 2=Dashboard + 底部 StatusBar
  ```

- [ ] **Step 1: `status_bar.dart`（完整）**

```dart
import 'package:flutter/material.dart';

class StatusBar extends StatelessWidget {
  const StatusBar({super.key, required this.state});
  final dynamic state; // AppState（避免循环 import 用 dynamic，或把本文件并入 shell.dart）

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    return Container(
      height: 28,
      color: scheme.surfaceContainerHighest,
      padding: const EdgeInsets.symmetric(horizontal: 12),
      child: Row(children: [
        Icon(state.connected ? Icons.link : Icons.link_off,
            size: 14, color: state.connected ? Colors.greenAccent : scheme.error),
        const SizedBox(width: 8),
        Text(state.connected ? '已连接 ${state.deviceName}' : '未连接',
            style: const TextStyle(fontSize: 12)),
        const Spacer(),
        Text('reconnect ${state.reconnects}   crc ${state.crcErrors}   fail ${state.requestFailures}',
            style: TextStyle(fontSize: 12, color: scheme.outline)),
      ]),
    );
  }
}
```
（实现时若嫌 dynamic 不雅，可将 `AppState` 移至 `state/app_state.dart` 并正常 import——**推荐后者**，`status_bar.dart` 直接 `import '../state/app_state.dart';` 用 `final AppState state;`。）

- [ ] **Step 2: `connect_page.dart`（完整）**

```dart
import 'package:flutter/material.dart';

class ConnectPage extends StatefulWidget {
  const ConnectPage({super.key, required this.state});
  final AppState state;
  @override
  State<ConnectPage> createState() => _ConnectPageState();
}

class _ConnectPageState extends State<ConnectPage> {
  final _addrCtrl = TextEditingController(text: '127.0.0.1:9000');
  bool _busy = false;

  @override
  Widget build(BuildContext context) {
    final s = widget.state;
    return Padding(
      padding: const EdgeInsets.all(24),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Text('连接管理', style: Theme.of(context).textTheme.titleLarge),
        const SizedBox(height: 16),
        SegmentedButton<String>(segments: const [
          ButtonSegment(value: 'tcp', label: Text('TCP / 无线适配器')),
          ButtonSegment(value: 'usb', label: Text('USB bulk（M2）'), enabled: false),
          ButtonSegment(value: 'serial', label: Text('UART / CDC（M2）'), enabled: false),
        ], selected: const {'tcp'}, onSelectionChanged: (_) {}),
        const SizedBox(height: 16),
        Row(children: [
          SizedBox(width: 280, child: TextField(
            controller: _addrCtrl,
            decoration: const InputDecoration(
              labelText: '地址', hintText: '127.0.0.1:9000', border: OutlineInputBorder()),
          )),
          const SizedBox(width: 12),
          FilledButton.icon(
            onPressed: _busy ? null : () async {
              setState(() => _busy = true);
              final ok = await s.connect(_addrCtrl.text.trim());
              if (mounted) {
                setState(() => _busy = false);
                ScaffoldMessenger.of(context).showSnackBar(SnackBar(
                  content: Text(ok ? '已连接 ${s.deviceName}' : '连接失败：${s.lastError}')));
              }
            },
            icon: const Icon(Icons.link), label: const Text('连接')),
          const SizedBox(width: 8),
          OutlinedButton.icon(
            onPressed: _busy || !s.connected ? null : () => s.disconnect(),
            icon: const Icon(Icons.link_off), label: const Text('断开')),
        ]),
      ]),
    );
  }
}
```

- [ ] **Step 3: `shell.dart`（完整）**

```dart
import 'package:flutter/material.dart';
import '../state/app_state.dart';
import 'connect_page.dart';
import 'dashboard_page.dart';   // Task 6 前先建占位（Scaffold(body: Text('Dashboard（Task 6）')))
import 'param_page.dart';       // Task 5 前先建占位
import 'status_bar.dart';

class Shell extends StatefulWidget {
  const Shell({super.key, required this.state});
  final AppState state;
  @override
  State<Shell> createState() => _ShellState();
}

class _ShellState extends State<Shell> {
  int _index = 0;
  @override
  Widget build(BuildContext context) {
    final pages = [
      ConnectPage(state: widget.state),
      ParamPage(state: widget.state),
      DashboardPage(state: widget.state),
    ];
    return Scaffold(
      body: Row(children: [
        NavigationRail(
          selectedIndex: _index,
          onDestinationSelected: (i) => setState(() => _index = i),
          labelType: NavigationRailLabelType.all,
          destinations: const [
            NavigationRailDestination(icon: Icon(Icons.cable), label: Text('连接')),
            NavigationRailDestination(icon: Icon(Icons.tune), label: Text('参数')),
            NavigationRailDestination(icon: Icon(Icons.speed), label: Text('Dashboard')),
          ],
        ),
        const VerticalDivider(width: 1),
        Expanded(child: pages[_index]),
      ]),
      bottomNavigationBar: StatusBar(state: widget.state),
    );
  }
}
```
`main.dart` 改为持有单一 `AppState`（`late final AppState _state = AppState();`，`ListenableBuilder(listenable: _state, ...)`，`home: Shell(state: _state)`）。

- [ ] **Step 4: 状态轮询（app_state.dart 扩展）**

连接成功后启动 `Timer.periodic(const Duration(milliseconds: 500), ...)` 调 `frb.statsGet()` 更新 `reconnects/crcErrors`（`attempts` 差值计入 requestFailures 可选）；disconnect 时 cancel。错误吞掉但计数 `requestFailures++`。

- [ ] **Step 5: 验证 + Commit**

Run: `flutter build windows --debug` 成功；手动起 `cargo run -p gui-sim`，运行 exe → 连接 `127.0.0.1:9000` → 提示「已连接 gui-sim」，状态栏出现计数。（实现者可只验构建，人工联调由控制器/Kaiser。）
Commit: `feat(gui-app): 连接管理页 + 导航壳 + 状态栏`

---

### Task 5: 参数面板页（元数据驱动）

**Files:**
- Create: `Gui/app/lib/ui/param_page.dart`（重写占位）
- Modify: `Gui/app/lib/state/app_state.dart`（参数表：`List<ParamMetaDto> metas`、`Map<String, ParamValueDto> values`、`dirty` 集合、`loadMetas()/readAll()/writeParam()/commit()`）
- Test: `Gui/app/test/param_page_test.dart`

**Interfaces:**
- Consumes: frb `paramMetaCount/paramMetaGet/paramRead/paramWrite/paramCommit`
- Produces: `ParamPage(state)`；`AppState` 参数 API（Task 6 不用，但测试用）：
  ```dart
  Future<void> loadMetas();                     // 遍历 metaGet(0..n-1)
  Future<bool> writeParam(String name, double valueAsDouble, int raw, int ty);
  Future<bool> commit();                        // LOCKED→false 且 lastError 带原因
  ```

- [ ] **Step 1: 写失败测试 `test/param_page_test.dart`**

```dart
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/ui/param_page.dart';

void main() {
  testWidgets('参数面板由元数据自动生成：域分组 + 搜索过滤', (tester) async {
    final state = AppState.test(metas: [
      makeMeta(0, 'motor.pole_pairs', ty: 1, unit: '', min: 1, max: 64),
      makeMeta(1, 'software.fault.vbus_ov_v', ty: 0, unit: 'V', min: 0, max: 80),
    ]);
    await tester.pumpWidget(MaterialApp(home: Scaffold(body: ParamPage(state: state))));
    expect(find.text('motor.pole_pairs'), findsOneWidget);
    // 搜索过滤
    await tester.enterText(find.byType(TextField).first, 'vbus');
    await tester.pump();
    expect(find.text('motor.pole_pairs'), findsNothing);
    expect(find.text('software.fault.vbus_ov_v'), findsOneWidget);
  });

  testWidgets('范围越界输入被拒绝并提示', (tester) async {
    final state = AppState.test(metas: [
      makeMeta(0, 'software.fault.vbus_ov_v', ty: 0, unit: 'V', min: 0, max: 80),
    ]);
    await tester.pumpWidget(MaterialApp(home: Scaffold(body: ParamPage(state: state))));
    await tester.tap(find.text('software.fault.vbus_ov_v'));
    await tester.pumpAndSettle();
    await tester.enterText(find.byType(TextField).last, '120');
    await tester.tap(find.text('写入'));
    await tester.pumpAndSettle();
    expect(find.textContaining('范围'), findsOneWidget);
  });
}
```
（`AppState.test` / `makeMeta` 为测试构造器：在 `app_state.dart` 增 `@visibleForTesting AppState.test({required List<ParamMetaDto> metas})`；`makeMeta` 为测试文件内辅助函数构造 `ParamMetaDto`。测试驱动的 API 形态如上，实现者可微调名字但语义不变。）
Run: `flutter test test/param_page_test.dart` → FAIL。

- [ ] **Step 2: 实现 `param_page.dart`（核心结构）**

页面结构（自上而下）：
1. 顶部工具条：`TextField`（搜索，hint「按名称过滤」）+ `OutlinedButton('读全部')` + `FilledButton('COMMIT 落盘')`（带二次确认 `AlertDialog`：「将参数写入设备 flash。须在停机且无故障锁存时执行。」）。
2. 主体：`ListView` 按**域名前缀**（`motor.`/`hardware.`/`software.`，取首段）分组，`ExpansionTile(标题=域)` 内为参数行。
3. 参数行（`ListTile`）：`title: Text(meta.name)`，`subtitle: Text('${typeName(meta.ty)} · ${meta.min}..${meta.max} ${meta.unit}')`，trailing：值文本 + `apply==1 ? 徽标 'reboot' : 徽标 'live'`（`Chip(label: Text(...), visualDensity: compact)`）；dirty 行 `title` 加粗并在 trailing 加 `Icons.edit` 小图标。
4. 点击行 → 编辑 `AlertDialog`：`TextField`（初值为当前值，键盘按 ty：int→`number`）+ 「写入」「取消」；写入前校验：ty==1(u32)/2(i32) 时整数解析，ty==0(f32) 浮点解析，ty==3(bool) 只收 0/1；**范围校验**（`min<=max` 时入界，否则拒绝并 `Text('超出范围 min..max')`）；调 `state.writeParam`，返回 false 时对话框内显示 `state.lastError`（LOCKED(4) 显示「设备联锁拒绝：先 STOP / FAULT_CLEAR」；6 显示「设备判定越界」）。
5. 空态：metas 为空且未加载 → 居中 `Text('连接设备后自动加载参数表')`；加载中 `CircularProgressIndicator`。

value ↔ double 显示规则：ty==0 `f32` 位型经 `dart:typed_data`（`ByteData(4)..setUint32(0, raw, Endian.little)..getFloat32(0)`）转 double 显示；ty==1/2 显示整数；ty==3 显示 true/false。写入反向同理。

- [ ] **Step 3: app_state.dart 参数部分（核心）**

```dart
List<ParamMetaDto> metas = [];
final Map<String, ParamValueDto> values = {};
final Set<String> dirty = {};
bool loadingParams = false;

Future<void> loadMetas() async {
  loadingParams = true; notifyListeners();
  try {
    final n = await frb.paramMetaCount();
    final list = <ParamMetaDto>[];
    for (var i = 0; i < n; i++) {
      list.add(await frb.paramMetaGet(i));
    }
    metas = list;
    for (final m in list) {
      values[m.name] = await frb.paramRead(m.name);
    }
  } on Exception catch (e) {
    lastError = e.toString();
  }
  loadingParams = false; notifyListeners();
}

Future<bool> writeParam(String name, int ty, int raw) async {
  try {
    await frb.paramWrite(name: name, ty: ty, raw: raw);
    values[name] = ParamValueDto(ty: ty, raw: raw);
    dirty.add(name);
    notifyListeners();
    return true;
  } on Exception catch (e) {
    lastError = e.toString();
    notifyListeners();
    return false;
  }
}

Future<bool> commit() async {
  try {
    await frb.paramCommit();
    dirty.clear();
    notifyListeners();
    return true;
  } on Exception catch (e) {
    lastError = e.toString();
    notifyListeners();
    return false;
  }
}
```

- [ ] **Step 4: 测试通过 + 构建 + Commit**

Run: `flutter test`（全部 widget 测试绿）+ `flutter build windows --debug` 成功。
Commit: `feat(gui-app): 参数面板（元数据驱动/校验/COMMIT 联锁提示）`

---

### Task 6: Dashboard 页（状态 + 电机控制 + 联锁）

**Files:**
- Create: `Gui/app/lib/ui/dashboard_page.dart`（重写占位）
- Modify: `Gui/app/lib/state/app_state.dart`（`StatusDto? status`、500ms `statusGet()` 轮询并入既有 Timer、`ctrlStart/Stop/setFreq/setMod/faultClear`）
- Test: `Gui/app/test/dashboard_page_test.dart`

**Interfaces:**
- Consumes: frb `statusGet/ctrlStart/ctrlStop/ctrlSetFreq/ctrlSetMod/faultGet/faultClear`
- Produces: `DashboardPage(state)`

- [ ] **Step 1: 写失败测试 `test/dashboard_page_test.dart`**

```dart
void main() {
  testWidgets('故障锁存时 START 显示联锁提示', (tester) async {
    final state = AppState.test(status: makeStatus(faultFlags: 0x1), metas: []);
    state.onCtrl = (op) async => throw FrbLikeError(code: 4); // 测试注入：LOCKED
    await tester.pumpWidget(MaterialApp(home: Scaffold(body: DashboardPage(state: state))));
    await tester.tap(find.text('启动'));
    await tester.pumpAndSettle();
    expect(find.textContaining('联锁'), findsOneWidget);
    expect(find.textContaining('FAULT_CLEAR'), findsOneWidget);
  });

  testWidgets('状态卡显示母线电压与转速', (tester) async {
    final state = AppState.test(status: makeStatus(vbus: 48.2, freq: 100), metas: []);
    await tester.pumpWidget(MaterialApp(home: Scaffold(body: DashboardPage(state: state))));
    expect(find.textContaining('48.2'), findsOneWidget);
    expect(find.textContaining('100.0'), findsOneWidget);
  });
}
```
（测试钩子：`AppState.test` 注入 `status` 与 `Future<void> Function(String op) onCtrl` 回调覆盖默认 frb 调用——实现者在 `AppState` 中以可注入字段实现，语义不变。`makeStatus` 为测试辅助。）

- [ ] **Step 2: 实现 `dashboard_page.dart`（核心结构）**

布局：`Row` 两列（左 2/3 状态区，右 1/3 控制区）。
- **故障横幅**（`status.faultFlags != 0 || latched != 0`）：顶部满宽 `MaterialBanner` 风格容器（`colorScheme.errorContainer`），文本 `故障 flags=0x.. latched=0x..` + `TextButton('FAULT_CLEAR')`。
- **状态卡网格**（`Wrap` + `Card`，每卡 `标题 + 大字号值 + 单位`）：母线电压 V、NTC ℃、三相电流 A×3、转子角/出轴角 °、频率 Hz、调制度 %。值格式：f32 toStringAsFixed(1)。
- **控制区**：`ListTile('电机状态', trailing: running ? 'RUN' : 'STOP')`；`FilledButton('启动')` / `OutlinedButton('停止')`；频率/调制度 `Slider`+`TextField` 组合（freq 0..1000，mod 0..100，松手即 `ctrlSetFreq/ctrlSetMod`）。
- **联锁提示**：任何控制调用返回 code==4 → `SnackBar`：「联锁拒绝：若故障锁存请先 FAULT_CLEAR；若运行中请先停止」；code==6 → 「设备判定越界」；0x100 → 「请求超时」；0x102 → 「连接已断开」。
- 未连接空态：`Text('未连接设备')`。

- [ ] **Step 3: app_state.dart 控制部分（核心）**

```dart
StatusDto? status;
int latchedFlags = 0;

Future<void> refreshStatus() async {
  try {
    status = await frb.statusGet();
    final f = await frb.faultGet();
    latchedFlags = f.$2;   // 以生成物实际返回形态为准
  } on Exception catch (e) {
    requestFailures++;
    lastError = e.toString();
  }
  notifyListeners();
}

Future<bool> _ctrl(Future<void> Function() op) async {
  try { await op(); return true; }
  on Exception catch (e) { lastError = e.toString(); notifyListeners(); return false; }
}
Future<bool> ctrlStart() => _ctrl(frb.ctrlStart);
Future<bool> ctrlStop() => _ctrl(frb.ctrlStop);
Future<bool> ctrlSetFreq(double hz) => _ctrl(() => frb.ctrlSetFreq(hz));
Future<bool> ctrlSetMod(double pct) => _ctrl(() => frb.ctrlSetMod(pct));
Future<bool> faultClear() => _ctrl(frb.faultClear);
```
（`_ctrl` 失败后按 `lastError` 中的 code 决定 SnackBar 文案；提取 code 的方式：frb 抛出的异常 `toString()` 含 `code=4`，用正则或包装类型解析——**推荐在 bridge 薄封装层把异常转为 `GuiException(code, message)`**，UI 只看 code。）

- [ ] **Step 4: 测试通过 + 构建 + Commit**

Run: `flutter test` 全绿 + `flutter build windows --debug` 成功。
Commit: `feat(gui-app): Dashboard（状态卡/电机控制/联锁提示）`

---

### Task 7: release 构建产物 + README + widget 测试收口

**Files:**
- Modify: `Gui/README.md`（追加「图形界面」章节）
- Modify: `Gui/app/test/`（若 Task 5/6 测试有缺口则补）

**Interfaces:**
- Consumes: Task 3~6 全部
- Produces: `build\windows\x64\release\g6618_gui.exe`（可双击运行）

- [ ] **Step 1: release 构建**

Run: `flutter build windows --release`（在 `Gui/app`）。
Expected: `build\windows\x64\release\g6618_gui.exe` + `.dll` 产物完整。**记录产物路径与体积进报告**。

- [ ] **Step 2: README 增补（内容清单，写全不写占位）**

1. 「图形界面（A 阶段）」：构建命令（`flutter pub get` / `flutter_rust_bridge_codegen generate` / `flutter build windows --release`）、产物路径、首次运行方式（先起 `cargo run -p gui-sim`，exe 中连接 `127.0.0.1:9000`）。
2. 「界面功能」：连接管理（TCP 今期可用，USB bulk/UART 为 M2 禁用项）、参数面板（元数据驱动、live/reboot 徽标、COMMIT 联锁）、Dashboard（状态卡/控制/联锁提示）。
3. 「工具链」：追加 Flutter stable + VS Build Tools + flutter_rust_bridge_codegen 三项与版本记录。
4. 更新「已知限制」：Windows 优先；Linux/macOS 未验收；移动端未做。

- [ ] **Step 3: 全量回归**

Run: `Gui/` 下 `cargo test --workspace` 全绿；`Gui/app` 下 `flutter test` 全绿。
Commit: `docs(gui-app): README 图形界面章节 + release 产物说明`

---

### Task 8: 收尾（回归 + 评审 + 合入准备）

**Files:** 无新文件（流程任务）

- [ ] **Step 1: 全链路人工验证清单（写入报告）**

1. `cargo run -p gui-sim -- --port 9000 --config ../config` 启动；
2. 运行 release exe → 连接 `127.0.0.1:9000` → 状态栏「已连接 gui-sim」；
3. 参数页自动出参数表（motor./hardware./software. 三组）；改 `software.fault.vbus_ov_v` 越界值 → 被拒；合法值 → dirty 标记；COMMIT（停机）→ 成功；启动电机后 COMMIT → 联锁提示；
4. Dashboard：状态卡刷新（500ms）；启动/停止；故障横幅（模拟器无故障注入 API，此路径以 widget 测试为准）；
5. 断开 → 状态栏计数正确。

- [ ] **Step 2: 全分支评审（控制器派发）+ 修复波（如有）**

- [ ] **Step 3: 合入选项交 Kaiser 决定（merge / PR / keep）**

---

## Self-Review 记录（计划完成后已执行）

1. **Spec 覆盖**：spec §9 connect/param/dashboard → Task 4/5/6；§9 bridge/state 分层 → Task 3；§10 错误呈现（LOCKED 显示原因/计数器）→ Global Constraints + Task 4/6；§8 gui-sim 联调 → Task 2 测试 + Task 8 清单；§11 widget 测试层 → Task 5/6/7；M1 验收（模拟器上参数全流程）→ Task 8 清单。示波器/录制/USB bulk UI 禁用态标注与范围边界一致。
2. **占位符扫描**：无 TBD/TODO；Task 3「以生成物为准适配」是对 codegen 输出形态（record vs class）的显式规则而非空缺；所有测试含实际断言代码。
3. **类型一致性**：`ParamMetaDto/ParamValueDto/StatusDto/StatsDto/FrbError` 与 frb API 签名在 Task 2 定义、Task 5/6 引用一致（`ty/raw u8/u32`、`code u32` 词表统一）；`AppState` 方法族（connect/disconnect/loadMetas/writeParam/commit/refreshStatus/ctrl*）在 Task 3/4/5/6 递进定义无冲突。
