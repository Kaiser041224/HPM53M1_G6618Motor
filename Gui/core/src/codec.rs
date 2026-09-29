//! 帧编解码：SOF 重同步 + CRC16-CCITT-FALSE。

pub const SOF: [u8; 2] = [0xA5, 0x5A];
pub const VER: u8 = 0x01;
pub const MAX_PAYLOAD: usize = 512;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Frame {
    pub ver: u8,
    pub cmd: u8,
    pub seq: u8,
    pub payload: Vec<u8>,
}

pub fn crc16_ccitt_false(data: &[u8]) -> u16 {
    let mut crc: u16 = 0xFFFF;
    for &b in data {
        crc ^= (b as u16) << 8;
        for _ in 0..8 {
            crc = if crc & 0x8000 != 0 { (crc << 1) ^ 0x1021 } else { crc << 1 };
        }
    }
    crc
}

pub fn encode(frame: &Frame) -> Vec<u8> {
    assert!(frame.payload.len() <= MAX_PAYLOAD, "payload 超长");
    let mut body = Vec::with_capacity(5 + frame.payload.len());
    body.push(frame.ver);
    body.push(frame.cmd);
    body.push(frame.seq);
    body.extend_from_slice(&(frame.payload.len() as u16).to_le_bytes());
    body.extend_from_slice(&frame.payload);
    let crc = crc16_ccitt_false(&body);
    let mut out = Vec::with_capacity(2 + body.len() + 2);
    out.extend_from_slice(&SOF);
    out.extend_from_slice(&body);
    out.extend_from_slice(&crc.to_le_bytes());
    out
}

#[derive(Default)]
pub struct Decoder {
    buf: Vec<u8>,
    crc_errors: u64,
    resyncs: u64,
}

impl Decoder {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn feed(&mut self, data: &[u8]) {
        self.buf.extend_from_slice(data);
    }

    pub fn crc_errors(&self) -> u64 {
        self.crc_errors
    }

    pub fn resyncs(&self) -> u64 {
        self.resyncs
    }

    /// 取下一完整帧；坏帧丢弃并计数，可继续调用直到 None。
    pub fn next_frame(&mut self) -> Option<Frame> {
        loop {
            // 定位 SOF
            let sof_pos = self.buf.windows(2).position(|w| w == SOF);
            let Some(start) = sof_pos else {
                if self.buf.len() >= 2 {
                    self.resyncs += self.buf.len() as u64 - 1;
                    let last = *self.buf.last().unwrap();
                    self.buf.clear();
                    self.buf.push(last); // 保留尾字节，可能是半个 SOF
                }
                return None;
            };
            if start > 0 {
                self.resyncs += start as u64;
                self.buf.drain(..start);
            }
            if self.buf.len() < 2 + 5 {
                return None; // 头不完整
            }
            let len = u16::from_le_bytes([self.buf[5], self.buf[6]]) as usize;
            if len > MAX_PAYLOAD {
                // 非法长度：跳过本次 SOF 继续寻址
                self.resyncs += 1;
                self.buf.drain(..2);
                continue;
            }
            let total = 2 + 5 + len + 2;
            if self.buf.len() < total {
                return None; // 等更多字节
            }
            let body_end = 2 + 5 + len;
            let crc_calc = crc16_ccitt_false(&self.buf[2..body_end]);
            let crc_wire = u16::from_le_bytes([self.buf[body_end], self.buf[body_end + 1]]);
            if crc_calc != crc_wire {
                self.crc_errors += 1;
                self.buf.drain(..total);
                continue;
            }
            let frame = Frame {
                ver: self.buf[2],
                cmd: self.buf[3],
                seq: self.buf[4],
                payload: self.buf[7..body_end].to_vec(),
            };
            self.buf.drain(..total);
            return Some(frame);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(s: &str) -> Vec<u8> {
        (0..s.len() / 2)
            .map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap())
            .collect()
    }

    #[test]
    fn crc_matches_standard_vector() {
        assert_eq!(crc16_ccitt_false(b"123456789"), 0x29B1);
    }

    #[test]
    fn decodes_golden_frames_from_json() {
        let raw = std::fs::read_to_string(
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../protocol/test_vectors.json"),
        )
        .unwrap();
        let v: serde_json::Value = serde_json::from_str(&raw).unwrap();
        for f in v["frames"].as_array().unwrap() {
            let mut dec = Decoder::new();
            dec.feed(&hex(f["hex"].as_str().unwrap()));
            let frame = dec.next_frame().expect("金帧必须可解出");
            assert_eq!(frame.ver, f["expect"]["ver"].as_u64().unwrap() as u8);
            assert_eq!(frame.cmd, f["expect"]["cmd"].as_u64().unwrap() as u8);
            assert_eq!(frame.seq, f["expect"]["seq"].as_u64().unwrap() as u8);
            assert_eq!(frame.payload, hex(f["expect"]["payload_hex"].as_str().unwrap()));
            assert_eq!(dec.next_frame(), None);
        }
    }

    #[test]
    fn encode_decode_roundtrip() {
        let f = Frame { ver: VER, cmd: 0x07, seq: 200, payload: vec![1, 2, 3, 4] };
        let mut dec = Decoder::new();
        dec.feed(&encode(&f));
        assert_eq!(dec.next_frame(), Some(f));
    }

    #[test]
    fn resyncs_after_garbage_and_half_sof() {
        let f = Frame { ver: VER, cmd: 0x01, seq: 0, payload: vec![] };
        let wire = encode(&f);
        let mut dec = Decoder::new();
        dec.feed(&[0xDE, 0xAD, 0xA5]); // 垃圾 + 半个 SOF
        dec.feed(&wire);
        assert_eq!(dec.next_frame(), Some(f));
        assert!(dec.resyncs() > 0);
        assert_eq!(dec.crc_errors(), 0);
    }

    #[test]
    fn drops_bad_crc_frame_and_continues() {
        let bad = Frame { ver: VER, cmd: 0x01, seq: 9, payload: vec![0xFF] };
        let mut wire = encode(&bad);
        let n = wire.len();
        wire[n - 1] ^= 0xFF; // 破坏 CRC
        let good = Frame { ver: VER, cmd: 0x01, seq: 10, payload: vec![] };
        let mut dec = Decoder::new();
        dec.feed(&wire);
        dec.feed(&encode(&good));
        assert_eq!(dec.next_frame(), Some(good));
        assert_eq!(dec.crc_errors(), 1);
    }

    #[test]
    fn incremental_feed_yields_frame() {
        let f = Frame { ver: VER, cmd: 0x04, seq: 1, payload: vec![0x10] };
        let wire = encode(&f);
        let mut dec = Decoder::new();
        for chunk in wire.chunks(3) {
            dec.feed(chunk);
        }
        assert_eq!(dec.next_frame(), Some(f));
    }
}
