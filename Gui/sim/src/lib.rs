//! gui-sim：设备模拟器（协议服务端参考实现）。
//!
//! 参数表与仓库 `config/*.yaml` 同源；控制联锁语义与固件 terminal 保持一致。
//! 网络层每条连接一个任务：字节流解码 → `handle_frame` → 编码写回。

pub mod yaml;

use gui_core::codec::{encode, Decoder, Frame};
use gui_core::params::{self, ParamMeta, ParamType, ParamValue};
use gui_core::proto::{cmd, ctrl, status};
use std::collections::BTreeMap;
use std::path::Path;
use std::sync::Arc;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;
use tokio::sync::Mutex;

/// 模拟器错误：配置加载或运行时 I/O。
#[derive(Debug)]
pub enum SimError {
    /// 配置读取/解析失败（含文件路径上下文）。
    Yaml(String),
    /// 运行时 I/O 失败。
    Io(std::io::Error),
}

impl std::fmt::Display for SimError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SimError::Yaml(s) => write!(f, "yaml error: {s}"),
            SimError::Io(e) => write!(f, "io error: {e}"),
        }
    }
}

impl std::error::Error for SimError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            SimError::Io(e) => Some(e),
            SimError::Yaml(_) => None,
        }
    }
}

impl From<std::io::Error> for SimError {
    fn from(e: std::io::Error) -> Self {
        SimError::Io(e)
    }
}

/// 单个参数条目（元数据 + 当前值）。
pub struct ParamEntry {
    /// 参数元数据（序号、类型、生效语义、范围）。
    pub meta: ParamMeta,
    /// 参数当前值（原始位型）。
    pub value: ParamValue,
}

/// 设备模拟器状态。
pub struct SimDevice {
    params: BTreeMap<String, ParamEntry>,
    by_index: Vec<String>,
    fault_flags: u32,
    latched_flags: u32,
    motor_running: bool,
    freq_hz: f32,
    mod_pct: f32,
}

impl SimDevice {
    /// 从目录读取 `motor.yaml` / `hardware.yaml` / `software.yaml` 构建参数表。
    pub fn from_config_dir(dir: &Path) -> Result<Self, SimError> {
        let mut raw: BTreeMap<String, (ParamMeta, ParamValue)> = BTreeMap::new();
        let mut index: u16 = 0;
        for domain in ["motor", "hardware", "software"] {
            let path = dir.join(format!("{domain}.yaml"));
            let text = std::fs::read_to_string(&path)
                .map_err(|e| SimError::Yaml(format!("read {}: {e}", path.display())))?;
            let doc: serde_yaml::Value = serde_yaml::from_str(&text)
                .map_err(|e| SimError::Yaml(format!("parse {}: {e}", path.display())))?;
            yaml::flatten_yaml(domain, &doc, "", &mut raw, &mut index);
        }
        let params: BTreeMap<String, ParamEntry> = raw
            .into_iter()
            .map(|(name, (meta, value))| (name, ParamEntry { meta, value }))
            .collect();
        let mut by_index = vec![String::new(); params.len()];
        for (name, entry) in &params {
            if let Some(slot) = by_index.get_mut(entry.meta.index as usize) {
                *slot = name.clone();
            }
        }
        Ok(Self {
            params,
            by_index,
            fault_flags: 0,
            latched_flags: 0,
            motor_running: false,
            freq_hz: 0.0,
            mod_pct: 0.0,
        })
    }

    /// 同步处理一帧请求，返回 `cmd|0x80`、同 `seq` 的应答帧（payload 首字节 = 状态）。
    pub fn handle_frame(&mut self, req: &Frame) -> Frame {
        let (status, body) = self.dispatch(req.cmd, &req.payload);
        let mut payload = Vec::with_capacity(1 + body.len());
        payload.push(status);
        payload.extend_from_slice(&body);
        Frame {
            ver: gui_core::proto::VER,
            cmd: req.cmd | gui_core::proto::RESPONSE_BIT,
            seq: req.seq,
            payload,
        }
    }

    /// 注入故障位（同时置入实时与锁存）。
    pub fn inject_fault(&mut self, flags: u32) {
        self.fault_flags |= flags;
        self.latched_flags |= flags;
    }

    /// 清除实时故障与锁存。
    pub fn clear_faults(&mut self) {
        self.fault_flags = 0;
        self.latched_flags = 0;
    }

    /// 参数表（按名字典序）。
    pub fn params(&self) -> &BTreeMap<String, ParamEntry> {
        &self.params
    }

    fn dispatch(&mut self, cmd: u8, payload: &[u8]) -> (u8, Vec<u8>) {
        match cmd {
            cmd::PROTO_INFO => (status::OK, proto_info_body()),
            cmd::PARAM_META_COUNT => {
                (status::OK, (self.params.len() as u16).to_le_bytes().to_vec())
            }
            cmd::PARAM_META_GET => self.meta_get(payload),
            cmd::PARAM_READ => self.param_read(payload),
            cmd::PARAM_WRITE => self.param_write(payload),
            cmd::PARAM_COMMIT => self.param_commit(),
            cmd::STATUS_GET => (status::OK, self.status_body()),
            cmd::CTRL_CMD => self.ctrl_cmd(payload),
            cmd::FAULT_GET => {
                let mut body = Vec::with_capacity(8);
                body.extend_from_slice(&self.fault_flags.to_le_bytes());
                body.extend_from_slice(&self.latched_flags.to_le_bytes());
                (status::OK, body)
            }
            cmd::FAULT_CLEAR => {
                self.clear_faults();
                (status::OK, Vec::new())
            }
            cmd::STREAM_SUB | cmd::STREAM_UNSUB => (status::UNSUPPORTED, Vec::new()),
            _ => (status::BAD_CMD, Vec::new()),
        }
    }

    fn meta_get(&self, payload: &[u8]) -> (u8, Vec<u8>) {
        if payload.len() < 2 {
            return (status::BAD_PARAM, Vec::new());
        }
        let index = u16::from_le_bytes([payload[0], payload[1]]);
        let entry = self
            .by_index
            .get(index as usize)
            .and_then(|name| self.params.get(name));
        match entry {
            Some(e) => match params::encode_meta_resp(&e.meta) {
                Ok(body) => (status::OK, body),
                Err(_) => (status::BAD_PARAM, Vec::new()),
            },
            None => (status::BAD_PARAM, Vec::new()),
        }
    }

    fn param_read(&self, payload: &[u8]) -> (u8, Vec<u8>) {
        let mut pos = 0;
        let name = match params::decode_name(payload, &mut pos) {
            Ok(n) => n,
            Err(_) => return (status::BAD_PARAM, Vec::new()),
        };
        match self.params.get(&name) {
            Some(e) => (status::OK, params::encode_value(e.value, e.meta.ty).to_vec()),
            None => (status::BAD_PARAM, Vec::new()),
        }
    }

    fn param_write(&mut self, payload: &[u8]) -> (u8, Vec<u8>) {
        let mut pos = 0;
        let name = match params::decode_name(payload, &mut pos) {
            Ok(n) => n,
            Err(_) => return (status::BAD_PARAM, Vec::new()),
        };
        if payload.len() < pos + 5 {
            return (status::BAD_PARAM, Vec::new());
        }
        let ty = match ParamType::from_u8(payload[pos]) {
            Some(t) => t,
            None => return (status::BAD_PARAM, Vec::new()),
        };
        let raw = u32::from_le_bytes([
            payload[pos + 1],
            payload[pos + 2],
            payload[pos + 3],
            payload[pos + 4],
        ]);
        let value = ParamValue { raw };
        match self.params.get_mut(&name) {
            Some(entry) => {
                if ty != entry.meta.ty {
                    return (status::BAD_PARAM, Vec::new());
                }
                if params::check_range(&value, &entry.meta).is_err() {
                    return (status::RANGE_ERR, Vec::new());
                }
                entry.value = value;
                (status::OK, Vec::new())
            }
            None => (status::BAD_PARAM, Vec::new()),
        }
    }

    /// 停机窗口才允许落盘；运行中返回 LOCKED。
    fn param_commit(&mut self) -> (u8, Vec<u8>) {
        if self.motor_running {
            return (status::LOCKED, Vec::new());
        }
        match self.save_params() {
            Ok(()) => (status::OK, Vec::new()),
            Err(_) => (status::BUSY, Vec::new()),
        }
    }

    /// 把全部参数写到 cwd 下的 `sim_params.json`。
    fn save_params(&self) -> Result<(), SimError> {
        let mut out = String::from("{\n");
        for (i, (name, entry)) in self.params.iter().enumerate() {
            if i > 0 {
                out.push_str(",\n");
            }
            out.push_str("  ");
            out.push_str(&json_string(name));
            out.push_str(": ");
            match entry.meta.ty {
                ParamType::F32 => out.push_str(&entry.value.as_f32().to_string()),
                ParamType::U32 => out.push_str(&entry.value.as_u32().to_string()),
                ParamType::I32 => out.push_str(&entry.value.as_i32().to_string()),
                ParamType::Bool => {
                    out.push_str(if entry.value.as_bool() { "true" } else { "false" })
                }
            }
        }
        out.push_str("\n}\n");
        std::fs::write("sim_params.json", out).map_err(SimError::Io)
    }

    fn ctrl_cmd(&mut self, payload: &[u8]) -> (u8, Vec<u8>) {
        let Some(&opcode) = payload.first() else {
            return (status::BAD_CMD, Vec::new());
        };
        let value = if payload.len() >= 5 {
            f32::from_le_bytes([payload[1], payload[2], payload[3], payload[4]])
        } else {
            0.0
        };
        match opcode {
            ctrl::START => {
                if self.latched_flags != 0 {
                    return (status::LOCKED, Vec::new());
                }
                self.motor_running = true;
                (status::OK, Vec::new())
            }
            ctrl::STOP => {
                self.motor_running = false;
                (status::OK, Vec::new())
            }
            ctrl::SET_FREQ => {
                if !(0.0..=1000.0).contains(&value) {
                    return (status::RANGE_ERR, Vec::new());
                }
                self.freq_hz = value;
                (status::OK, Vec::new())
            }
            ctrl::SET_MOD => {
                if !(0.0..=100.0).contains(&value) {
                    return (status::RANGE_ERR, Vec::new());
                }
                self.mod_pct = value;
                (status::OK, Vec::new())
            }
            _ => (status::BAD_CMD, Vec::new()),
        }
    }

    /// STATUS_GET 体：`fault_flags | motor_running | motor_state | freq | mod | 7×0f32`。
    fn status_body(&self) -> Vec<u8> {
        let mut body = Vec::with_capacity(42);
        body.extend_from_slice(&self.fault_flags.to_le_bytes());
        body.push(self.motor_running as u8);
        body.push(if self.motor_running { 0x01 } else { 0x00 });
        body.extend_from_slice(&self.freq_hz.to_le_bytes());
        body.extend_from_slice(&self.mod_pct.to_le_bytes());
        for _ in 0..7 {
            body.extend_from_slice(&0f32.to_le_bytes());
        }
        body
    }
}

/// PROTO_INFO 体：major=1, minor=0, caps(u32), name_len=7, "gui-sim"。
fn proto_info_body() -> Vec<u8> {
    let mut body = Vec::with_capacity(11);
    body.push(1);
    body.push(0);
    body.extend_from_slice(&0x0000_0003u32.to_le_bytes());
    body.push(7);
    body.extend_from_slice(b"gui-sim");
    body
}

/// 最小 JSON 字符串转义（参数名仅含字母/数字/`.`/`_`，防御性处理）。
fn json_string(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// TCP 服务：每条连接一个任务，读帧 → `handle_frame` → 写回。
pub async fn serve(listener: TcpListener, dev: Arc<Mutex<SimDevice>>) -> Result<(), SimError> {
    loop {
        let (mut sock, _) = listener.accept().await.map_err(SimError::Io)?;
        let dev = dev.clone();
        tokio::spawn(async move {
            let mut dec = Decoder::new();
            let mut buf = [0u8; 2048];
            loop {
                let n = match sock.read(&mut buf).await {
                    Ok(0) | Err(_) => break,
                    Ok(n) => n,
                };
                dec.feed(&buf[..n]);
                while let Some(req) = dec.next_frame() {
                    let resp = {
                        let mut guard = dev.lock().await;
                        guard.handle_frame(&req)
                    };
                    let wire = match encode(&resp) {
                        Ok(w) => w,
                        // 响应无法编码（协议违例）：结束该连接，不 panic
                        Err(_) => break,
                    };
                    if sock.write_all(&wire).await.is_err() {
                        break;
                    }
                }
            }
        });
    }
}
