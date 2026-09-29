//! 传输抽象：统一 `Tcp` 与内存回环（测试/仿真）两种字节流通道。

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

/// 内存回环传输实现，两端互为对端。
pub struct LoopbackTransport {
    tx: tokio::sync::mpsc::UnboundedSender<Vec<u8>>,
    rx: tokio::sync::mpsc::UnboundedReceiver<Vec<u8>>,
}

/// 创建一对互为对端的回环传输。
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
