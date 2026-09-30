//! 1 kHz 状态采样 + 批量流内部实现。
//!
//! 本模块不参与 frb codegen（`rust_input` 仅声明 `crate::api`），
//! 以便用闭包作为批次出口进行可测试的采样；公开的 frb API 在 `crate::api`
//! 中以 [`crate::api::start_status_watch`] / [`crate::api::stop_status_watch`] 暴露。

use crate::api::{FrbError, StatusBatchDto, StatusDto};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};
use tokio::time::MissedTickBehavior;

const RATE_MIN: u32 = 1;
const RATE_MAX: u32 = 5000;
const BATCH_MAX_SAMPLES: usize = 16;
const BATCH_WINDOW: Duration = Duration::from_millis(20);
const MAX_CONSECUTIVE_FAILURES: u32 = 3;

struct WatchHandle {
    generation: u64,
    task: tokio::task::JoinHandle<()>,
}

/// Windows 上 tokio 定时器受系统计时器粒度（默认约 15.6 ms）约束，
/// 无法达到 1 kHz；一次性把进程计时器精度提升到 1 ms。
#[cfg(windows)]
fn ensure_high_resolution_timer() {
    use std::sync::Once;
    #[link(name = "winmm")]
    extern "system" {
        fn timeBeginPeriod(period: u32) -> u32;
    }
    static ONCE: Once = Once::new();
    ONCE.call_once(|| unsafe {
        let _ = timeBeginPeriod(1);
    });
}

#[cfg(not(windows))]
fn ensure_high_resolution_timer() {}

fn slot() -> &'static Mutex<Option<WatchHandle>> {
    static SLOT: OnceLock<Mutex<Option<WatchHandle>>> = OnceLock::new();
    SLOT.get_or_init(|| Mutex::new(None))
}

static GENERATION: AtomicU64 = AtomicU64::new(0);

fn lock_slot() -> std::sync::MutexGuard<'static, Option<WatchHandle>> {
    slot().lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

/// 采样频率钳位到 `1..=5000` Hz。
pub fn clamp_rate(rate_hz: u32) -> u32 {
    rate_hz.clamp(RATE_MIN, RATE_MAX)
}

/// 采样任务是否存活（测试与内部使用）。
pub fn is_active() -> bool {
    lock_slot().is_some()
}

/// 启动采样任务；重复调用 = 停旧启新。
///
/// `emit` 返回 `false` 表示下游已关闭，采样任务自行结束。
pub fn start_with<F>(rate_hz: u32, emit: F) -> Result<(), FrbError>
where
    F: Fn(StatusBatchDto) -> bool + Send + 'static,
{
    ensure_high_resolution_timer();
    let rate_hz = clamp_rate(rate_hz);
    let mut guard = lock_slot();
    if let Some(old) = guard.take() {
        old.task.abort();
    }
    let generation = GENERATION.fetch_add(1, Ordering::SeqCst);
    let task = crate::api::runtime().spawn(run_watch(rate_hz, generation, emit));
    *guard = Some(WatchHandle { generation, task });
    Ok(())
}

/// 停止采样任务（幂等）。
pub fn stop() -> Result<(), FrbError> {
    let mut guard = lock_slot();
    if let Some(old) = guard.take() {
        old.task.abort();
    }
    Ok(())
}

/// 仅当当前句柄仍属于 `generation` 时清除（避免旧任务清掉新任务）。
fn clear_if_current(generation: u64) {
    let mut guard = lock_slot();
    if guard.as_ref().map(|h| h.generation) == Some(generation) {
        *guard = None;
    }
}

async fn run_watch<F>(rate_hz: u32, generation: u64, emit: F)
where
    F: Fn(StatusBatchDto) -> bool + Send + 'static,
{
    let period = Duration::from_secs_f64(1.0 / f64::from(rate_hz));
    let mut interval = tokio::time::interval(period);
    interval.set_missed_tick_behavior(MissedTickBehavior::Skip);

    let mut samples: Vec<StatusDto> = Vec::with_capacity(BATCH_MAX_SAMPLES);
    let mut seq: u64 = 0;
    let mut batch_started: Option<Instant> = None;
    let mut consecutive_failures: u32 = 0;

    loop {
        interval.tick().await;
        match crate::api::status_sample().await {
            Ok(sample) => {
                consecutive_failures = 0;
                let started = *batch_started.get_or_insert_with(Instant::now);
                samples.push(sample);
                if samples.len() >= BATCH_MAX_SAMPLES || started.elapsed() >= BATCH_WINDOW {
                    let batch = StatusBatchDto {
                        samples: std::mem::take(&mut samples),
                        seq,
                    };
                    seq = seq.wrapping_add(1);
                    batch_started = None;
                    if !emit(batch) {
                        break;
                    }
                }
            }
            Err(_) => {
                consecutive_failures += 1;
                if consecutive_failures >= MAX_CONSECUTIVE_FAILURES {
                    break;
                }
            }
        }
    }

    clear_if_current(generation);
}
