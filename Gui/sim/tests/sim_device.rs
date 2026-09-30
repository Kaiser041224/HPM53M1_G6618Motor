//! gui-sim SimDevice 行为测试：YAML 加载、协议应答、联锁语义。

use gui_core::codec::Frame;
use gui_core::params::{self, ParamType, ParamValue};
use gui_core::proto::{cmd, ctrl, status};
use gui_sim::SimDevice;

/// 串行化对 cwd 下 `sim_params.json` 的访问：PARAM_COMMIT 写盘与测试清理删除
/// 在并行测试线程间存在竞态（写失败 → BUSY → 断言失败），须全程持锁。
static COMMIT_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

fn req(cmd: u8, payload: &[u8]) -> Frame {
    Frame { ver: gui_core::proto::VER, cmd, seq: 0, payload: payload.to_vec() }
}

fn name(n: &str) -> Vec<u8> {
    params::encode_name(n).expect("合法参数名")
}

fn config_dir() -> std::path::PathBuf {
    std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../config")
}

fn load() -> SimDevice {
    SimDevice::from_config_dir(&config_dir()).expect("config/*.yaml 应可加载")
}

#[test]
fn loads_yaml_params_with_domain_prefix() {
    let dev = load();
    let all = dev.params();
    assert!(all.contains_key("motor.pole_pairs"), "motor.yaml 键应展平为 motor.<path>");
    assert!(all.contains_key("hardware.inverter.pwm_freq_hz"));
    // software.fault.oc_trip_a 是引号表达式（v1 跳过），改验数值键
    assert!(all.contains_key("software.fault.vbus_ov_v"));
    // 引号表达式不入表
    assert!(!all.contains_key("software.fault.oc_trip_a"));
}

#[test]
fn proto_info_reports_v1_and_name() {
    let mut dev = load();
    let resp = dev.handle_frame(&req(cmd::PROTO_INFO, &[]));
    assert_eq!(resp.cmd, cmd::PROTO_INFO | gui_core::proto::RESPONSE_BIT);
    assert_eq!(resp.payload[0], status::OK);
    assert_eq!(resp.payload[1], 1); // proto_major
    assert_eq!(resp.payload[2], 0); // proto_minor
    assert_eq!(&resp.payload[3..7], &3u32.to_le_bytes()); // caps
    assert_eq!(resp.payload[7], 7); // name_len
    assert_eq!(&resp.payload[8..], b"gui-sim");
}

#[test]
fn param_read_write_roundtrip_and_range() {
    let mut dev = load();
    // 读 motor.pole_pairs
    let resp = dev.handle_frame(&req(cmd::PARAM_READ, &name("motor.pole_pairs")));
    assert_eq!(resp.payload[0], status::OK);
    // 写 software.fault.vbus_ov_v（F32，写 60.0 合法）再读回
    let mut w = name("software.fault.vbus_ov_v");
    w.extend_from_slice(&params::encode_value(ParamValue::from_f32(60.0), ParamType::F32));
    let resp = dev.handle_frame(&req(cmd::PARAM_WRITE, &w));
    assert_eq!(resp.payload[0], status::OK);
    let resp = dev.handle_frame(&req(cmd::PARAM_READ, &name("software.fault.vbus_ov_v")));
    let (_ty, v) = params::decode_value(&resp.payload[1..]).unwrap();
    assert_eq!(v.as_f32(), 60.0);
}

#[test]
fn param_write_type_mismatch_is_bad_param() {
    let mut dev = load();
    let mut w = name("software.fault.vbus_ov_v");
    w.extend_from_slice(&params::encode_value(ParamValue::from_u32(60), ParamType::U32));
    let resp = dev.handle_frame(&req(cmd::PARAM_WRITE, &w));
    assert_eq!(resp.payload[0], status::BAD_PARAM);
}

#[test]
fn unknown_param_is_bad_param() {
    let mut dev = load();
    let resp = dev.handle_frame(&req(cmd::PARAM_READ, &name("nope.none")));
    assert_eq!(resp.payload[0], status::BAD_PARAM);
}

#[test]
fn param_meta_count_and_get() {
    let mut dev = load();
    let resp = dev.handle_frame(&req(cmd::PARAM_META_COUNT, &[]));
    assert_eq!(resp.payload[0], status::OK);
    let count = u16::from_le_bytes([resp.payload[1], resp.payload[2]]) as usize;
    assert_eq!(count, dev.params().len());
    assert!(count > 0);
    // index 0 可取
    let resp = dev.handle_frame(&req(cmd::PARAM_META_GET, &0u16.to_le_bytes()));
    assert_eq!(resp.payload[0], status::OK);
    let meta = params::decode_meta_resp(&resp.payload[1..]).unwrap();
    assert_eq!(meta.index, 0);
    // 越界 → RANGE_ERR（线契约 §3.3：meta 序号越界）
    let resp = dev.handle_frame(&req(cmd::PARAM_META_GET, &(count as u16).to_le_bytes()));
    assert_eq!(resp.payload[0], status::RANGE_ERR);
}

#[test]
fn ctrl_interlocks_follow_terminal_semantics() {
    let _g = COMMIT_LOCK.lock().unwrap_or_else(|p| p.into_inner());
    let mut dev = load();
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
    // 运行中 START 再次 OK（幂等）
    let _ = std::fs::remove_file("sim_params.json"); // 清理 COMMIT 产物
}

#[test]
fn param_commit_locked_by_latch_even_when_stopped() {
    let _g = COMMIT_LOCK.lock().unwrap_or_else(|p| p.into_inner());
    let mut dev = load();
    // 注入故障（同时置入锁存）
    dev.inject_fault(0x1);
    // 停机：motor_running=false，但故障锁存仍在
    let mut stop = vec![ctrl::STOP];
    stop.extend_from_slice(&0f32.to_le_bytes());
    assert_eq!(dev.handle_frame(&req(cmd::CTRL_CMD, &stop)).payload[0], status::OK);
    // 停机但故障锁存 → PARAM_COMMIT 仍 LOCKED（契约 §3.6）
    let resp = dev.handle_frame(&req(cmd::PARAM_COMMIT, &[]));
    assert_eq!(resp.payload[0], status::LOCKED);
    // 清除故障锁存后 → PARAM_COMMIT OK
    dev.handle_frame(&req(cmd::FAULT_CLEAR, &[]));
    let resp = dev.handle_frame(&req(cmd::PARAM_COMMIT, &[]));
    assert_eq!(resp.payload[0], status::OK);
    let _ = std::fs::remove_file("sim_params.json"); // 清理 COMMIT 产物
}

/// 从 STATUS_GET 应答 payload 中读取偏移 `off` 处的 f32 LE 字段。
fn f32_at(payload: &[u8], off: usize) -> f32 {
    f32::from_le_bytes([payload[off], payload[off + 1], payload[off + 2], payload[off + 3]])
}

fn to_hex(bytes: &[u8]) -> String {
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push_str(&format!("{b:02X}"));
    }
    s
}

#[test]
fn proto_info_response_encodes_to_golden_vector() {
    let raw = std::fs::read_to_string(
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../protocol/test_vectors.json"),
    )
    .expect("test_vectors.json 可读");
    let v: serde_json::Value = serde_json::from_str(&raw).expect("合法 JSON");
    let golden = v["frames"]
        .as_array()
        .unwrap()
        .iter()
        .find(|f| f["name"] == "proto_info_resp_sim")
        .expect("存在 proto_info_resp_sim 金帧")["hex"]
        .as_str()
        .unwrap()
        .to_string();
    // 编码侧闭环：sim 真实应答 → 编码 → 与金向量 hex 逐字符相等
    let mut dev = load();
    let resp = dev.handle_frame(&req(cmd::PROTO_INFO, &[]));
    let wire = gui_core::codec::encode(&resp).unwrap();
    assert_eq!(to_hex(&wire), golden);
}

#[test]
fn ctrl_set_range_errors_and_unknown_opcode() {
    let mut dev = load();
    let mut p = vec![ctrl::SET_FREQ];
    p.extend_from_slice(&1001f32.to_le_bytes());
    assert_eq!(dev.handle_frame(&req(cmd::CTRL_CMD, &p)).payload[0], status::RANGE_ERR);
    let mut p = vec![ctrl::SET_FREQ];
    p.extend_from_slice(&1000f32.to_le_bytes());
    assert_eq!(dev.handle_frame(&req(cmd::CTRL_CMD, &p)).payload[0], status::OK);
    let mut p = vec![ctrl::SET_MOD];
    p.extend_from_slice(&101f32.to_le_bytes());
    assert_eq!(dev.handle_frame(&req(cmd::CTRL_CMD, &p)).payload[0], status::RANGE_ERR);
    let mut p = vec![ctrl::SET_MOD];
    p.extend_from_slice(&100f32.to_le_bytes());
    assert_eq!(dev.handle_frame(&req(cmd::CTRL_CMD, &p)).payload[0], status::OK);
    let p = vec![0x7F, 0, 0, 0, 0];
    assert_eq!(dev.handle_frame(&req(cmd::CTRL_CMD, &p)).payload[0], status::BAD_CMD);
}

#[test]
fn fault_get_and_clear_roundtrip() {
    let mut dev = load();
    let resp = dev.handle_frame(&req(cmd::FAULT_GET, &[]));
    assert_eq!(resp.payload[0], status::OK);
    assert_eq!(&resp.payload[1..5], &0u32.to_le_bytes());
    assert_eq!(&resp.payload[5..9], &0u32.to_le_bytes());
    dev.inject_fault(0x3);
    let resp = dev.handle_frame(&req(cmd::FAULT_GET, &[]));
    assert_eq!(&resp.payload[1..5], &3u32.to_le_bytes());
    assert_eq!(&resp.payload[5..9], &3u32.to_le_bytes());
    let resp = dev.handle_frame(&req(cmd::FAULT_CLEAR, &[]));
    assert_eq!(resp.payload[0], status::OK);
    let resp = dev.handle_frame(&req(cmd::FAULT_GET, &[]));
    assert_eq!(&resp.payload[1..5], &0u32.to_le_bytes());
    assert_eq!(&resp.payload[5..9], &0u32.to_le_bytes());
}

#[test]
fn status_get_layout_and_motor_state() {
    let mut dev = load();
    dev.inject_fault(0x2);
    let resp = dev.handle_frame(&req(cmd::STATUS_GET, &[]));
    assert_eq!(resp.payload[0], status::OK);
    // status | fault_flags u32 | motor_running u8 | motor_state u8 | 9×f32
    assert_eq!(resp.payload.len(), 1 + 4 + 1 + 1 + 9 * 4);
    assert_eq!(&resp.payload[1..5], &2u32.to_le_bytes());
    assert_eq!(resp.payload[5], 0); // motor_running
    assert_eq!(resp.payload[6], 0); // MP_MOTOR_STOP
    // START 后 running/state 置位
    dev.clear_faults();
    let mut p = vec![ctrl::START];
    p.extend_from_slice(&0f32.to_le_bytes());
    assert_eq!(dev.handle_frame(&req(cmd::CTRL_CMD, &p)).payload[0], status::OK);
    let resp = dev.handle_frame(&req(cmd::STATUS_GET, &[]));
    assert_eq!(resp.payload[5], 1);
    assert_eq!(resp.payload[6], 1); // MP_MOTOR_RUN
    // 合成信号为 tick 确定函数，仅校验物理范围而非精确值
    let i_u = f32_at(&resp.payload, 15);
    let vbus = f32_at(&resp.payload, 27);
    let ntc = f32_at(&resp.payload, 31);
    assert!((-10.0..=10.0).contains(&i_u), "i_u={i_u}");
    assert!((46.0..=50.0).contains(&vbus), "vbus={vbus}");
    assert!((55.0..=65.0).contains(&ntc), "ntc={ntc}");
}

#[test]
fn status_get_advances_tick_between_calls() {
    let mut dev = load();
    let a = dev.handle_frame(&req(cmd::STATUS_GET, &[]));
    let b = dev.handle_frame(&req(cmd::STATUS_GET, &[]));
    // 每次 STATUS_GET 自增 tick，合成信号应随之前进
    assert_ne!(a.payload, b.payload);
}

#[test]
fn status_currents_are_symmetric_over_ticks() {
    let mut dev = load();
    let mut max_abs = 0f32;
    for _ in 0..20 {
        let resp = dev.handle_frame(&req(cmd::STATUS_GET, &[]));
        let i_u = f32_at(&resp.payload, 15);
        let i_v = f32_at(&resp.payload, 19);
        let i_w = f32_at(&resp.payload, 23);
        assert!((i_u + i_v + i_w).abs() < 1e-3, "sum={}", i_u + i_v + i_w);
        assert!((-10.0..=10.0).contains(&i_u));
        max_abs = max_abs.max(i_u.abs());
    }
    assert!(max_abs > 1.0, "tick 推进后合成电流应为非平凡正弦");
}

#[test]
fn status_encoder_angles_stay_in_one_revolution() {
    let mut dev = load();
    let mut prev = 0f32;
    let mut advanced = false;
    for _ in 0..100 {
        let resp = dev.handle_frame(&req(cmd::STATUS_GET, &[]));
        let rotor = f32_at(&resp.payload, 35);
        let output = f32_at(&resp.payload, 39);
        assert!((0.0..360.0).contains(&rotor), "rotor={rotor}");
        assert!((0.0..360.0).contains(&output), "output={output}");
        if rotor > prev {
            advanced = true;
        }
        prev = rotor;
    }
    assert!(advanced, "enc_rotor_deg 应随 tick 前进");
}

#[test]
fn unknown_command_is_bad_cmd() {
    let mut dev = load();
    let resp = dev.handle_frame(&req(0x7E, &[]));
    assert_eq!(resp.cmd, 0x7E | gui_core::proto::RESPONSE_BIT);
    assert_eq!(resp.payload[0], status::BAD_CMD);
}

#[test]
fn stream_cmds_are_unsupported_in_v1() {
    let mut dev = load();
    let resp = dev.handle_frame(&req(cmd::STREAM_SUB, &[0, 0]));
    assert_eq!(resp.payload[0], status::UNSUPPORTED);
    let resp = dev.handle_frame(&req(cmd::STREAM_UNSUB, &[]));
    assert_eq!(resp.payload[0], status::UNSUPPORTED);
}
