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

        // v1.1：出轴零点 NOW → 读角归零（停机窗口）
        let mut pre = 0f32;
        for _ in 0..10 {
            pre = gui_frb::api::status_get().await.unwrap().enc_output_deg;
        }
        gui_frb::api::ctrl_set_output_zero_now().await.unwrap();
        let post = gui_frb::api::status_get().await.unwrap().enc_output_deg;
        assert!(pre > 5.0, "pre={pre}");
        assert!((post - 0.9).abs() < 1e-2, "post={post}");

        // v1.2：设定值存取（停机窗口）+ 速度联动 + 运行中联锁拒绝
        gui_frb::api::ctrl_set_torque(-0.5).await.unwrap();
        gui_frb::api::ctrl_set_speed(1200.0).await.unwrap();
        gui_frb::api::ctrl_set_angle(45.0).await.unwrap();
        // 停机：越界/非有限值 → RANGE_ERR(6)
        assert_eq!(gui_frb::api::ctrl_set_speed(20001.0).await.unwrap_err().code, 6);
        assert_eq!(gui_frb::api::ctrl_set_torque(f32::NAN).await.unwrap_err().code, 6);
        assert_eq!(gui_frb::api::ctrl_set_angle(f32::INFINITY).await.unwrap_err().code, 6);
        // 模式=速度(2)：停机设模式/设定，启动后联动生效 → 转子角 7.2 °/tick
        gui_frb::api::ctrl_set_mode(2).await.unwrap();
        gui_frb::api::ctrl_start().await.unwrap();
        let a = gui_frb::api::status_get().await.unwrap().enc_rotor_deg;
        let b = gui_frb::api::status_get().await.unwrap().enc_rotor_deg;
        assert!(((b - a).rem_euclid(360.0) - 7.2).abs() < 1e-2, "a={a} b={b}");
        // 运行中改设定 → LOCKED(4)
        assert_eq!(gui_frb::api::ctrl_set_speed(600.0).await.unwrap_err().code, 4);
        assert_eq!(gui_frb::api::ctrl_set_torque(1.0).await.unwrap_err().code, 4);
        assert_eq!(gui_frb::api::ctrl_set_angle(0.0).await.unwrap_err().code, 4);
        gui_frb::api::ctrl_stop().await.unwrap();

        gui_frb::api::disconnect().await.unwrap();
        assert!(!gui_frb::api::is_connected());
    });
}
