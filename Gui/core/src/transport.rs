//! 传输抽象：统一 `Tcp` 与内存回环（测试/仿真）两种字节流通道。

use std::collections::VecDeque;
use std::future::Future;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

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

/// 基于 TCP 的传输实现。
pub struct TcpTransport {
    stream: tokio::net::TcpStream,
}

impl TcpTransport {
    /// 连接远端地址（`host:port`）。
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

/// 单向回环通道的共享状态：字节队列 + 关闭标志 + 唤醒通知。
struct LoopbackChannel {
    queue: Mutex<VecDeque<u8>>,
    closed: AtomicBool,
    notify: tokio::sync::Notify,
}

impl LoopbackChannel {
    /// 创建空的单向通道。
    fn new() -> Self {
        Self {
            queue: Mutex::new(VecDeque::new()),
            closed: AtomicBool::new(false),
            notify: tokio::sync::Notify::new(),
        }
    }

    /// 标记通道关闭并唤醒挂起的读者。
    fn close(&self) {
        self.closed.store(true, Ordering::Release);
        self.notify.notify_one();
    }
}

/// 内存回环传输实现，两端互为对端；按 **字节流** 语义收发
/// （无消息边界，允许任意切分/合并，超出缓冲区的数据保留在队列中）。
pub struct LoopbackTransport {
    inbox: Arc<LoopbackChannel>,
    outbox: Arc<LoopbackChannel>,
}

/// 创建一对互为对端的回环传输。
pub fn loopback_pair() -> (LoopbackTransport, LoopbackTransport) {
    let a_to_b = Arc::new(LoopbackChannel::new());
    let b_to_a = Arc::new(LoopbackChannel::new());
    (
        LoopbackTransport {
            inbox: Arc::clone(&b_to_a),
            outbox: Arc::clone(&a_to_b),
        },
        LoopbackTransport {
            inbox: a_to_b,
            outbox: b_to_a,
        },
    )
}

impl Drop for LoopbackTransport {
    fn drop(&mut self) {
        self.inbox.close();
        self.outbox.close();
    }
}

impl Transport for LoopbackTransport {
    async fn write_all(&mut self, buf: &[u8]) -> Result<(), TransportError> {
        if self.outbox.closed.load(Ordering::Acquire) {
            return Err(TransportError::Closed);
        }
        {
            let mut queue = self
                .outbox
                .queue
                .lock()
                .unwrap_or_else(|poison| poison.into_inner());
            queue.extend(buf.iter().copied());
        }
        self.outbox.notify.notify_one();
        Ok(())
    }

    async fn read_some(&mut self, buf: &mut [u8]) -> Result<usize, TransportError> {
        // 空缓冲区视为 no-op：不阻塞、不消费队列（与 TcpTransport 的 0 语义不同，
        // 此处按 ruling 文档化为 `Ok(0)`）。
        if buf.is_empty() {
            return Ok(0);
        }
        loop {
            {
                let mut queue = self
                    .inbox
                    .queue
                    .lock()
                    .unwrap_or_else(|poison| poison.into_inner());
                if !queue.is_empty() {
                    let n = queue.len().min(buf.len());
                    for (slot, byte) in buf[..n].iter_mut().zip(queue.drain(..n)) {
                        *slot = byte;
                    }
                    return Ok(n);
                }
                // 队列已排空后才报告对端关闭。
                if self.inbox.closed.load(Ordering::Acquire) {
                    return Err(TransportError::Closed);
                }
            }
            self.inbox.notify.notified().await;
        }
    }
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
    async fn loopback_partial_reads_preserve_all_bytes_in_order() {
        let (mut a, mut b) = super::loopback_pair();
        let payload: Vec<u8> = (0..100u8).collect();
        a.write_all(&payload).await.unwrap();
        let mut got = Vec::new();
        let mut buf = [0u8; 30];
        while got.len() < payload.len() {
            let n = b.read_some(&mut buf).await.unwrap();
            assert!(n > 0 && n <= 30);
            got.extend_from_slice(&buf[..n]);
        }
        assert_eq!(got, payload);
    }

    #[tokio::test]
    async fn loopback_coalesces_writes_into_one_stream() {
        let (mut a, mut b) = super::loopback_pair();
        a.write_all(b"hello ").await.unwrap();
        a.write_all(b"world").await.unwrap();
        let mut got = Vec::new();
        let mut buf = [0u8; 11];
        while got.len() < 11 {
            let n = b.read_some(&mut buf).await.unwrap();
            got.extend_from_slice(&buf[..n]);
        }
        assert_eq!(got, b"hello world");
    }

    #[tokio::test]
    async fn loopback_drains_queued_bytes_then_reports_closed() {
        let (mut a, mut b) = super::loopback_pair();
        b.write_all(b"bye").await.unwrap();
        drop(b);
        let mut buf = [0u8; 8];
        let n = a.read_some(&mut buf).await.unwrap();
        assert_eq!(&buf[..n], b"bye");
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
