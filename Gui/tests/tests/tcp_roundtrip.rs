//! TCP 全链路集成测试：握手 / 元数据 / 读-写-读 / 联锁 / 故障。
//!
//! 客户端使用 `gui_core::session::Session` + `TcpTransport`，服务端使用
//! `gui_sim::SimDevice` + `gui_sim::serve`，走真实 TCP 回环（字节流语义）。

use gui_core::proto::{cmd, ctrl, status};
use gui_core::session::{RequestKind, Session};
use gui_core::transport::TcpTransport;
use gui_sim::SimDevice;
use std::sync::Arc;

/// 在回环地址的临时端口启动模拟器，返回 `host:port` 与设备状态句柄。
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
    let name = gui_core::params::encode_name("software.fault.vbus_ov_v").unwrap();
    let body = s.request(cmd::PARAM_READ, &name, RequestKind::Read).await.unwrap();
    let (ty, v0) = gui_core::params::decode_value(&body).unwrap();
    let mut w = name.clone();
    w.extend_from_slice(&gui_core::params::encode_value(
        gui_core::params::ParamValue::from_f32(v0.as_f32() + 1.0),
        ty,
    ));
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

    // STOP 解锁后 COMMIT=OK
    let mut stop = vec![ctrl::STOP];
    stop.extend_from_slice(&0f32.to_le_bytes());
    s.request(cmd::CTRL_CMD, &stop, RequestKind::Write).await.unwrap();
    s.request(cmd::PARAM_COMMIT, &[], RequestKind::Write).await.unwrap();

    // 未知参数 → BAD_PARAM
    let err = s
        .request(cmd::PARAM_READ, &gui_core::params::encode_name("x.y").unwrap(), RequestKind::Read)
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
