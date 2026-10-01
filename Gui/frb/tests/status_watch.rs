//! 1 kHz 状态采样 + 批量流集成测试（gui-sim 内嵌）。

use std::sync::{Arc, Mutex as StdMutex};
use std::time::Duration;

use gui_frb::api::{self, StatusBatchDto};

/// 全局单会话 / 单采样任务，测试间串行避免互相打断。
static SERIAL: tokio::sync::Mutex<()> = tokio::sync::Mutex::const_new(());

async fn serial() -> tokio::sync::MutexGuard<'static, ()> {
    SERIAL.lock().await
}

/// 内嵌 gui-sim，返回监听地址。
async fn spawn_sim() -> String {
    let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../config");
    let dev = Arc::new(tokio::sync::Mutex::new(
        gui_sim::SimDevice::from_config_dir(&dir).unwrap(),
    ));
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap().to_string();
    tokio::spawn(async move {
        let _ = gui_sim::serve(listener, dev).await;
    });
    addr
}

#[derive(Default)]
struct Collector {
    batches: StdMutex<Vec<StatusBatchDto>>,
}

impl Collector {
    fn emit(&self, batch: StatusBatchDto) -> bool {
        self.batches.lock().unwrap().push(batch);
        true
    }
    fn snapshot(&self) -> Vec<StatusBatchDto> {
        self.batches.lock().unwrap().clone()
    }
    fn sample_count(&self) -> usize {
        self.batches.lock().unwrap().iter().map(|b| b.samples.len()).sum()
    }
}

#[tokio::test]
async fn watch_collects_batches_and_signals_advance() {
    let _guard = serial().await;
    let addr = spawn_sim().await;
    api::connect(addr).await.unwrap();

    let collector = Arc::new(Collector::default());
    let c = collector.clone();
    gui_frb::watch::start_with(1000, move |batch| c.emit(batch)).unwrap();
    tokio::time::sleep(Duration::from_millis(120)).await;
    gui_frb::watch::stop().unwrap();

    let batches = collector.snapshot();
    assert!(
        collector.sample_count() >= 50,
        "expected >=50 samples in ~100ms, got {}",
        collector.sample_count()
    );

    for pair in batches.windows(2) {
        assert!(pair[1].seq > pair[0].seq, "batch seq must strictly increase");
    }

    let angles: Vec<f32> = batches
        .iter()
        .flat_map(|b| b.samples.iter().map(|s| s.enc_rotor_deg))
        .collect();
    assert!(
        angles.windows(2).any(|w| w[0] != w[1]),
        "synthetic encoder signal must advance between ticks"
    );

    api::disconnect().await.unwrap();
}

#[tokio::test]
async fn watch_pushes_every_sample_as_its_own_batch() {
    let _guard = serial().await;
    let addr = spawn_sim().await;
    api::connect(addr).await.unwrap();

    let collector = Arc::new(Collector::default());
    let c = collector.clone();
    gui_frb::watch::start_with(1000, move |batch| c.emit(batch)).unwrap();
    tokio::time::sleep(Duration::from_millis(120)).await;
    gui_frb::watch::stop().unwrap();

    let batches = collector.snapshot();
    assert!(
        batches.len() >= 50,
        "expected >=50 per-sample batches in ~120ms, got {}",
        batches.len()
    );
    for batch in &batches {
        assert_eq!(
            batch.samples.len(),
            1,
            "1 kHz feed must push one sample per batch"
        );
    }
    for pair in batches.windows(2) {
        assert_eq!(
            pair[1].seq,
            pair[0].seq + 1,
            "each per-sample batch bumps seq by exactly one"
        );
    }

    api::disconnect().await.unwrap();
}

#[tokio::test]
async fn stop_halts_batches() {
    let _guard = serial().await;
    let addr = spawn_sim().await;
    api::connect(addr).await.unwrap();

    let collector = Arc::new(Collector::default());
    let c = collector.clone();
    gui_frb::watch::start_with(1000, move |batch| c.emit(batch)).unwrap();
    tokio::time::sleep(Duration::from_millis(80)).await;
    gui_frb::watch::stop().unwrap();

    tokio::time::sleep(Duration::from_millis(30)).await;
    let settled = collector.snapshot().len();
    tokio::time::sleep(Duration::from_millis(60)).await;
    assert_eq!(collector.snapshot().len(), settled, "no batches after stop");
    assert!(!gui_frb::watch::is_active());

    api::disconnect().await.unwrap();
}

#[test]
fn rate_is_clamped_to_bounds() {
    assert_eq!(gui_frb::watch::clamp_rate(0), 1);
    assert_eq!(gui_frb::watch::clamp_rate(99999), 5000);
    assert_eq!(gui_frb::watch::clamp_rate(1000), 1000);
}

#[tokio::test]
async fn extreme_rates_do_not_panic() {
    let _guard = serial().await;
    let addr = spawn_sim().await;
    api::connect(addr).await.unwrap();

    let collector = Arc::new(Collector::default());
    for rate in [0u32, 99999u32] {
        let c = collector.clone();
        gui_frb::watch::start_with(rate, move |batch| c.emit(batch)).unwrap();
        tokio::time::sleep(Duration::from_millis(40)).await;
        gui_frb::watch::stop().unwrap();
    }
    assert!(!gui_frb::watch::is_active());

    api::disconnect().await.unwrap();
}

#[tokio::test]
async fn disconnect_stops_watch() {
    let _guard = serial().await;
    let addr = spawn_sim().await;
    api::connect(addr).await.unwrap();

    let collector = Arc::new(Collector::default());
    let c = collector.clone();
    gui_frb::watch::start_with(1000, move |batch| c.emit(batch)).unwrap();
    tokio::time::sleep(Duration::from_millis(60)).await;

    api::disconnect().await.unwrap();
    assert!(!gui_frb::watch::is_active(), "disconnect must stop watch");

    tokio::time::sleep(Duration::from_millis(20)).await;
    let settled = collector.snapshot().len();
    tokio::time::sleep(Duration::from_millis(60)).await;
    assert_eq!(collector.snapshot().len(), settled);
}

#[tokio::test]
async fn sink_close_stops_watch() {
    let _guard = serial().await;
    let addr = spawn_sim().await;
    api::connect(addr).await.unwrap();

    gui_frb::watch::start_with(1000, |_batch| false).unwrap();
    tokio::time::sleep(Duration::from_millis(50)).await;
    assert!(
        !gui_frb::watch::is_active(),
        "watch must self-stop when the sink is closed"
    );

    api::disconnect().await.unwrap();
}

#[tokio::test]
async fn repeated_start_restarts_single_task() {
    let _guard = serial().await;
    let addr = spawn_sim().await;
    api::connect(addr).await.unwrap();

    let first = Arc::new(Collector::default());
    let c1 = first.clone();
    gui_frb::watch::start_with(1000, move |batch| c1.emit(batch)).unwrap();
    tokio::time::sleep(Duration::from_millis(50)).await;

    let second = Arc::new(Collector::default());
    let c2 = second.clone();
    gui_frb::watch::start_with(1000, move |batch| c2.emit(batch)).unwrap();
    tokio::time::sleep(Duration::from_millis(20)).await;
    let first_after_restart = first.snapshot().len();
    tokio::time::sleep(Duration::from_millis(60)).await;

    assert_eq!(
        first.snapshot().len(),
        first_after_restart,
        "old watch must be stopped by restart"
    );
    assert!(second.snapshot().len() > first_after_restart);
    assert!(gui_frb::watch::is_active());

    gui_frb::watch::stop().unwrap();
    api::disconnect().await.unwrap();
}
