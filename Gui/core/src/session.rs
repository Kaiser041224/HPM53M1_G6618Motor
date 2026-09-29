//! 会话层：SEQ 配对、超时/重试、响应状态剥离。

use crate::codec::{encode, Decoder, Frame};
use crate::transport::{Transport, TransportError};

const RESPONSE_BIT: u8 = 0x80;
const REQUEST_TIMEOUT_MS: u64 = 500;
const READ_RETRIES: u32 = 2;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RequestKind {
    Read,
    Write,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConnState {
    Disconnected,
    Ready,
}

#[derive(Debug, Clone, Copy, Default)]
pub struct SessionStats {
    pub crc_errors: u64,
    pub resyncs: u64,
    pub attempts: u64,
}

#[derive(Debug)]
pub enum SessionError {
    Timeout,
    Status(u8),
    Transport(TransportError),
    Closed,
    /// payload 长度超过 `crate::codec::MAX_PAYLOAD`。
    BadPayload,
}

pub struct Session<T: Transport> {
    transport: T,
    decoder: Decoder,
    next_seq: u8,
    stats: SessionStats,
}

impl<T: Transport> Session<T> {
    pub fn new(transport: T) -> Self {
        Self {
            transport,
            decoder: Decoder::new(),
            next_seq: 0,
            stats: SessionStats::default(),
        }
    }

    pub fn stats(&self) -> SessionStats {
        SessionStats {
            crc_errors: self.decoder.crc_errors(),
            resyncs: self.decoder.resyncs(),
            ..self.stats
        }
    }

    pub fn transport_mut(&mut self) -> &mut T {
        &mut self.transport
    }

    /// 发起请求并等待匹配 SEQ 的响应。
    /// 读：失败（超时/传输错）重试至多 READ_RETRIES 次；写：不重试。
    /// status != OK 返回 Err(Status)，不重试。
    pub async fn request(
        &mut self,
        cmd: u8,
        payload: &[u8],
        kind: RequestKind,
    ) -> Result<Vec<u8>, SessionError> {
        if payload.len() > crate::codec::MAX_PAYLOAD {
            return Err(SessionError::BadPayload);
        }
        let max_attempts = match kind {
            RequestKind::Read => READ_RETRIES + 1,
            RequestKind::Write => 1,
        };
        let mut last_err = SessionError::Timeout;
        for _ in 0..max_attempts {
            self.stats.attempts += 1;
            let seq = self.next_seq;
            self.next_seq = self.next_seq.wrapping_add(1);
            let frame = Frame {
                ver: crate::codec::VER,
                cmd,
                seq,
                payload: payload.to_vec(),
            };
            let wire = match encode(&frame) {
                Ok(w) => w,
                Err(_) => return Err(SessionError::BadPayload),
            };
            if let Err(e) = self.transport.write_all(&wire).await {
                last_err = SessionError::Transport(e);
                continue;
            }
            match self.wait_response(seq, cmd).await {
                Ok(body) => return Ok(body),
                Err(e) => {
                    let retryable = matches!(e, SessionError::Timeout);
                    last_err = e;
                    if !retryable {
                        return Err(last_err);
                    }
                }
            }
        }
        Err(last_err)
    }

    async fn wait_response(&mut self, seq: u8, cmd: u8) -> Result<Vec<u8>, SessionError> {
        let deadline =
            std::time::Instant::now() + std::time::Duration::from_millis(REQUEST_TIMEOUT_MS);
        loop {
            let remain = deadline.saturating_duration_since(std::time::Instant::now());
            if remain.is_zero() {
                return Err(SessionError::Timeout);
            }
            let mut buf = [0u8; 2048];
            let n = match tokio::time::timeout(remain, self.transport.read_some(&mut buf)).await {
                Ok(Ok(n)) => n,
                Ok(Err(TransportError::Closed)) => return Err(SessionError::Closed),
                Ok(Err(e)) => return Err(SessionError::Transport(e)),
                Err(_) => return Err(SessionError::Timeout),
            };
            self.decoder.feed(&buf[..n]);
            while let Some(f) = self.decoder.next_frame() {
                if f.seq == seq && f.cmd == (cmd | RESPONSE_BIT) {
                    if f.payload.is_empty() {
                        return Err(SessionError::Status(0xFF)); // 协议违例，视为失败
                    }
                    let status = f.payload[0];
                    return if status == 0 {
                        Ok(f.payload[1..].to_vec())
                    } else {
                        Err(SessionError::Status(status))
                    };
                }
                // 非匹配帧（含预流/事件）：丢弃，计数在 decoder 内
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::codec::{encode, Frame, Decoder};
    use crate::transport::{loopback_pair, Transport};

    /// 假设备：对每个请求回显 status=OK 与 payload+1；可配置「不回」模拟超时。
    fn spawn_fake_device(
        mut t: crate::transport::LoopbackTransport,
        silence: bool,
    ) -> tokio::task::JoinHandle<()> {
        tokio::spawn(async move {
            if silence {
                // 吞掉一切请求但保持连接
                let mut buf = [0u8; 2048];
                loop {
                    if t.read_some(&mut buf).await.is_err() {
                        break;
                    }
                }
                return;
            }
            let mut dec = Decoder::new();
            let mut buf = [0u8; 2048];
            loop {
                let n = match t.read_some(&mut buf).await {
                    Ok(n) => n,
                    Err(_) => break,
                };
                dec.feed(&buf[..n]);
                while let Some(req) = dec.next_frame() {
                    let mut body = vec![0u8]; // status OK
                    body.extend_from_slice(&req.payload);
                    let resp = Frame {
                        ver: crate::codec::VER,
                        cmd: req.cmd | 0x80,
                        seq: req.seq,
                        payload: body,
                    };
                    if t.write_all(&encode(&resp).unwrap()).await.is_err() {
                        break;
                    }
                }
            }
        })
    }

    #[tokio::test]
    async fn request_returns_body_on_ok() {
        let (client, server) = loopback_pair();
        let h = spawn_fake_device(server, false);
        let mut s = Session::new(client);
        let body = s.request(0x04, b"abc", RequestKind::Read).await.unwrap();
        assert_eq!(body, b"abc");
        drop(s); // 关闭客户端，令假设备的读循环收到 Closed 后退出
        h.await.unwrap();
    }

    #[tokio::test]
    async fn read_retries_then_gives_up_on_timeout() {
        let (client, server) = loopback_pair();
        let h = spawn_fake_device(server, true);
        let mut s = Session::new(client);
        let err = s.request(0x04, b"x", RequestKind::Read).await.unwrap_err();
        assert!(matches!(err, SessionError::Timeout));
        assert_eq!(s.stats().attempts, 3); // 1 + 2 重试
        drop(s);
        h.await.unwrap();
    }

    #[tokio::test]
    async fn write_does_not_retry() {
        let (client, server) = loopback_pair();
        let h = spawn_fake_device(server, true);
        let mut s = Session::new(client);
        let err = s.request(0x05, b"x", RequestKind::Write).await.unwrap_err();
        assert!(matches!(err, SessionError::Timeout));
        assert_eq!(s.stats().attempts, 1);
        drop(s);
        h.await.unwrap();
    }

    #[tokio::test]
    async fn non_ok_status_becomes_error_and_is_not_retried() {
        let (client, mut server) = loopback_pair();
        let h = tokio::spawn(async move {
            let mut dec = Decoder::new();
            let mut buf = [0u8; 2048];
            let n = server.read_some(&mut buf).await.unwrap();
            dec.feed(&buf[..n]);
            let req = dec.next_frame().unwrap();
            let resp = Frame {
                ver: crate::codec::VER,
                cmd: req.cmd | 0x80,
                seq: req.seq,
                payload: vec![0x04], // MP_STATUS_LOCKED
            };
            server.write_all(&encode(&resp).unwrap()).await.unwrap();
        });
        let mut s = Session::new(client);
        let err = s.request(0x06, b"", RequestKind::Write).await.unwrap_err();
        assert!(matches!(err, SessionError::Status(0x04)));
        assert_eq!(s.stats().attempts, 1);
        h.await.unwrap();
    }

    #[tokio::test]
    async fn seq_wraps_after_256() {
        let (client, server) = loopback_pair();
        let h = spawn_fake_device(server, false);
        let mut s = Session::new(client);
        for _ in 0..300 {
            s.request(0x07, b"", RequestKind::Read).await.unwrap();
        }
        drop(s);
        h.await.unwrap();
    }

    #[tokio::test]
    async fn oversized_payload_returns_bad_payload_without_attempts() {
        let (client, server) = loopback_pair();
        let h = spawn_fake_device(server, true);
        let mut s = Session::new(client);
        let payload = vec![0u8; crate::codec::MAX_PAYLOAD + 1];
        let err = s
            .request(0x04, &payload, RequestKind::Read)
            .await
            .unwrap_err();
        assert!(matches!(err, SessionError::BadPayload));
        assert_eq!(s.stats().attempts, 0);
        drop(s);
        h.await.unwrap();
    }
}
