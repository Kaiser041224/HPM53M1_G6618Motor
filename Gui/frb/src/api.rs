//! frb 导出面：Gui 客户端 API（全局单会话 + tokio runtime）。
use gui_core::params::{
    decode_meta_resp, decode_value, encode_name, encode_value, ParamMeta, ParamType, ParamValue,
};
use gui_core::proto::{cmd, ctrl};
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

pub(crate) fn runtime() -> &'static tokio::runtime::Runtime {
    RUNTIME.get_or_init(|| tokio::runtime::Runtime::new().expect("tokio runtime"))
}
fn client() -> &'static Mutex<Option<Session<TcpTransport>>> {
    CLIENT.get_or_init(|| Mutex::new(None))
}

#[derive(Debug, Clone)]
pub struct FrbError {
    pub code: u32,
    pub message: String,
}
impl std::fmt::Display for FrbError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "code={} {}", self.code, self.message)
    }
}
impl std::error::Error for FrbError {}

fn map_err(e: SessionError) -> FrbError {
    match e {
        SessionError::Status(s) => FrbError {
            code: s as u32,
            message: format!("device status {s}"),
        },
        SessionError::Timeout => FrbError {
            code: 0x100,
            message: "request timeout".into(),
        },
        SessionError::Transport(err) => {
            CONNECTED.store(false, Ordering::SeqCst);
            FrbError {
                code: 0x101,
                message: format!("transport: {err}"),
            }
        }
        SessionError::Closed => {
            CONNECTED.store(false, Ordering::SeqCst);
            FrbError {
                code: 0x102,
                message: "connection closed".into(),
            }
        }
        SessionError::BadPayload => FrbError {
            code: 0x1FF,
            message: "payload too long".into(),
        },
    }
}

/// 在自有 tokio runtime 上执行（frb 线程无 reactor 上下文）。
async fn on_tokio<F, T>(fut: F) -> Result<Result<T, FrbError>, FrbError>
where
    F: Future<Output = Result<T, FrbError>> + Send + 'static,
    T: Send + 'static,
{
    runtime()
        .spawn(fut)
        .await
        .map_err(|e| FrbError {
            code: 0x1FF,
            message: e.to_string(),
        })
}

/// 请求便捷封装：锁定单会话 → request → 剥 status（Session 已剥）。
async fn request(cmd: u8, payload: Vec<u8>, kind: RequestKind) -> Result<Vec<u8>, FrbError> {
    let mut guard = client().lock().await;
    let Some(sess) = guard.as_mut() else {
        return Err(FrbError {
            code: 0x102,
            message: "not connected".into(),
        });
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
            let Some(sess) = guard.as_mut() else {
                *guard = None;
                CONNECTED.store(false, Ordering::SeqCst);
                return Err(FrbError {
                    code: 0x1FF,
                    message: "internal: session missing".into(),
                });
            };
            sess.request(cmd::PROTO_INFO, &[], RequestKind::Read)
                .await
                .map_err(map_err)?
        };
        if body.is_empty() || body[0] != 1 {
            *guard = None;
            CONNECTED.store(false, Ordering::SeqCst);
            return Err(FrbError {
                code: 0x1FF,
                message: "proto major mismatch".into(),
            });
        }
        Ok(())
    })
    .await?
}

pub async fn disconnect() -> Result<(), FrbError> {
    on_tokio(async move {
        // 先隐式停止采样任务，避免断连后野任务继续打请求。
        crate::watch::stop()?;
        *client().lock().await = None;
        CONNECTED.store(false, Ordering::SeqCst);
        Ok(())
    })
    .await?
}

pub fn is_connected() -> bool {
    CONNECTED.load(Ordering::SeqCst)
}

/// 解析 PROTO_INFO 应答体：major u8 | minor u8 | caps u32 | name_len u8 | name。
/// 头部长度或 name 越界均返回本地 `0x1FF`，绝不 panic。
fn parse_proto_info_body(body: &[u8]) -> Result<(u8, u8, u32, String), FrbError> {
    if body.len() < 7 {
        return Err(FrbError {
            code: 0x1FF,
            message: "short PROTO_INFO".into(),
        });
    }
    let caps = u32::from_le_bytes(body[2..6].try_into().unwrap());
    let n = body[6] as usize;
    let end = 7usize.checked_add(n).ok_or_else(|| FrbError {
        code: 0x1FF,
        message: "short PROTO_INFO name".into(),
    })?;
    if end > body.len() {
        return Err(FrbError {
            code: 0x1FF,
            message: "short PROTO_INFO name".into(),
        });
    }
    let name = String::from_utf8_lossy(&body[7..end]).into_owned();
    Ok((body[0], body[1], caps, name))
}

pub async fn proto_info() -> Result<(u8, u8, u32, String), FrbError> {
    on_tokio(async move {
        let body = request(cmd::PROTO_INFO, vec![], RequestKind::Read).await?;
        parse_proto_info_body(&body)
    })
    .await?
}

// ---- 参数 ----

pub async fn param_meta_count() -> Result<u16, FrbError> {
    on_tokio(async move {
        let body = request(cmd::PARAM_META_COUNT, vec![], RequestKind::Read).await?;
        if body.len() < 2 {
            return Err(FrbError {
                code: 0x1FF,
                message: "short META_COUNT".into(),
            });
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
        let body = request(
            cmd::PARAM_META_GET,
            index.to_le_bytes().to_vec(),
            RequestKind::Read,
        )
        .await?;
        let m: ParamMeta = decode_meta_resp(&body).map_err(|_| FrbError {
            code: 0x1FF,
            message: "bad META resp".into(),
        })?;
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
pub struct ParamValueDto {
    pub ty: u8,
    pub raw: u32,
}

pub async fn param_read(name: String) -> Result<ParamValueDto, FrbError> {
    on_tokio(async move {
        let payload = encode_name(&name).map_err(|_| FrbError {
            code: 0x1FF,
            message: "name too long".into(),
        })?;
        let body = request(cmd::PARAM_READ, payload, RequestKind::Read).await?;
        let (ty, v) = decode_value(&body).map_err(|_| FrbError {
            code: 0x1FF,
            message: "bad VALUE resp".into(),
        })?;
        Ok(ParamValueDto {
            ty: ty.to_u8(),
            raw: v.raw,
        })
    })
    .await?
}

pub async fn param_write(name: String, ty: u8, raw: u32) -> Result<(), FrbError> {
    on_tokio(async move {
        let mut payload = encode_name(&name).map_err(|_| FrbError {
            code: 0x1FF,
            message: "name too long".into(),
        })?;
        let ty = ParamType::from_u8(ty).ok_or_else(|| FrbError {
            code: 0x1FF,
            message: "bad type".into(),
        })?;
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

fn parse_status(b: &[u8]) -> Result<StatusDto, FrbError> {
    if b.len() < 42 {
        return Err(FrbError {
            code: 0x1FF,
            message: "short STATUS".into(),
        });
    }
    Ok(StatusDto {
        fault_flags: u32::from_le_bytes(b[0..4].try_into().unwrap()),
        motor_running: b[4] != 0,
        motor_state: b[5],
        freq_hz: f32_at(b, 6),
        mod_pct: f32_at(b, 10),
        i_u: f32_at(b, 14),
        i_v: f32_at(b, 18),
        i_w: f32_at(b, 22),
        vbus: f32_at(b, 26),
        ntc_temp: f32_at(b, 30),
        enc_rotor_deg: f32_at(b, 34),
        enc_output_deg: f32_at(b, 38),
    })
}

/// 单次 STATUS_GET 采样（复用全局单会话；供采样任务与 `status_get` 共用）。
pub(crate) async fn status_sample() -> Result<StatusDto, FrbError> {
    let b = request(cmd::STATUS_GET, vec![], RequestKind::Read).await?;
    parse_status(&b)
}

pub async fn status_get() -> Result<StatusDto, FrbError> {
    on_tokio(status_sample()).await?
}

/// 一批状态样本：`samples` 保序，`seq` 每批递增。
#[derive(Debug, Clone)]
pub struct StatusBatchDto {
    pub samples: Vec<StatusDto>,
    pub seq: u64,
}

/// 启动 1 kHz 状态采样任务（`rate_hz` 钳位 1..=5000），
/// 每 20 ms 或 16 个样本（先到为准）向 `sink` 推送一批。
/// 重复调用 = 停旧启新；断连或连续失败 3 次自动停并结束流。
pub fn start_status_watch(
    rate_hz: u32,
    sink: crate::frb_generated::StreamSink<StatusBatchDto>,
) -> Result<(), FrbError> {
    crate::watch::start_with(rate_hz, move |batch| sink.add(batch).is_ok())
}

/// 停止状态采样任务（幂等）；`disconnect()` 亦隐式调用。
pub fn stop_status_watch() -> Result<(), FrbError> {
    crate::watch::stop()
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

/// v1.1：把当前出轴角设为零点（停机窗口联锁；运行中/故障锁存 → `LOCKED`）。
pub async fn ctrl_set_output_zero_now() -> Result<(), FrbError> {
    on_tokio(ctrl_op(ctrl::SET_OUTPUT_ZERO_NOW, 0.0)).await?
}

/// v1.1：把指定角度（度）设为出轴零点（非法非有限值 → `RANGE_ERR`）。
pub async fn ctrl_set_output_zero_abs(deg: f32) -> Result<(), FrbError> {
    on_tokio(ctrl_op(ctrl::SET_OUTPUT_ZERO_ABS, deg)).await?
}

/// v1.1：设置控制模式（1=力矩 / 2=速度 / 3=角度；非法 → `BAD_CMD`）。
/// `value` 以 f32 位型低字节承载 u8 模式。
pub async fn ctrl_set_mode(mode: u8) -> Result<(), FrbError> {
    on_tokio(ctrl_op(ctrl::SET_MODE, f32::from_bits(mode as u32))).await?
}

/// v1.2：设置力矩设定（N·m；负值允许；非有限值 → `RANGE_ERR`；运行中 → `LOCKED`）。
pub async fn ctrl_set_torque(nm: f32) -> Result<(), FrbError> {
    on_tokio(ctrl_op(ctrl::SET_TORQUE, nm)).await?
}

/// v1.2：设置转速设定（rpm；负值允许；`|value| > 20000` → `RANGE_ERR`；运行中 → `LOCKED`）。
pub async fn ctrl_set_speed(rpm: f32) -> Result<(), FrbError> {
    on_tokio(ctrl_op(ctrl::SET_SPEED, rpm)).await?
}

/// v1.2：设置角度设定（°；负值允许；非有限值 → `RANGE_ERR`；运行中 → `LOCKED`）。
pub async fn ctrl_set_angle(deg: f32) -> Result<(), FrbError> {
    on_tokio(ctrl_op(ctrl::SET_ANGLE, deg)).await?
}

pub async fn fault_get() -> Result<(u32, u32), FrbError> {
    on_tokio(async move {
        let b = request(cmd::FAULT_GET, vec![], RequestKind::Read).await?;
        if b.len() < 8 {
            return Err(FrbError {
                code: 0x1FF,
                message: "short FAULT".into(),
            });
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_proto_info_body_accepts_valid() {
        let mut body = vec![1u8, 0];
        body.extend_from_slice(&0x0000_0003u32.to_le_bytes());
        body.push(7);
        body.extend_from_slice(b"gui-sim");
        let (major, minor, caps, name) = parse_proto_info_body(&body).unwrap();
        assert_eq!(major, 1);
        assert_eq!(minor, 0);
        assert_eq!(caps, 0x0000_0003);
        assert_eq!(name, "gui-sim");
    }

    #[test]
    fn parse_proto_info_body_accepts_empty_name() {
        let mut body = vec![1u8, 0];
        body.extend_from_slice(&0x0000_0003u32.to_le_bytes());
        body.push(0);
        let (major, minor, caps, name) = parse_proto_info_body(&body).unwrap();
        assert_eq!((major, minor, caps), (1, 0, 0x0000_0003));
        assert!(name.is_empty());
    }

    #[test]
    fn parse_proto_info_body_rejects_truncated_header() {
        let err = parse_proto_info_body(&[1, 0, 3]).unwrap_err();
        assert_eq!(err.code, 0x1FF);
        assert_eq!(err.message, "short PROTO_INFO");
    }

    #[test]
    fn parse_proto_info_body_rejects_name_len_overrun() {
        // 头部合法但 name_len 声称 9 字节、实际仅 3 字节：旧实现会 panic。
        let mut body = vec![1u8, 0];
        body.extend_from_slice(&0x0000_0003u32.to_le_bytes());
        body.push(9);
        body.extend_from_slice(b"gui");
        let err = parse_proto_info_body(&body).unwrap_err();
        assert_eq!(err.code, 0x1FF);
        assert_eq!(err.message, "short PROTO_INFO name");
    }
}
