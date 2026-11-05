//! USB bulk 协议探针（P1a 验收工具）
//!
//! 走 WinUSB interface 0（bulk OUT 0x02 / IN 0x82），按 `Gui/protocol/motor_proto.h`
//! 的 v1 帧格式发请求并校验应答。用途：
//!   1. 验证固件 P1a 协议服务（PROTO_INFO / META / READ / STATUS / FAULT）；
//!   2. 提前验证 `nusb` 作为后续 `UsbTransport` 的可行性。
//!
//! 运行（Windows / Linux 均可，设备需绑定 WinUSB 或 usbfs 可访问）：
//!   cargo run -p usb-probe
//!
//! 退出码：0 = 全部通过；1 = 有断言失败；2 = 设备未找到 / 传输失败。

use std::error::Error;
use std::time::Duration;

use nusb::transfer::{Bulk, Buffer, In, Out};
use nusb::{list_devices, MaybeFuture};

const VID: u16 = 0x34B7;
const PID: u16 = 0x6618;
const INTERFACE: u8 = 0;
const EP_OUT: u8 = 0x02;
const EP_IN: u8 = 0x82;

const SOF0: u8 = 0xA5;
const SOF1: u8 = 0x5A;
const VER: u8 = 0x01;
const RESPONSE_BIT: u8 = 0x80;
const MAX_PAYLOAD: usize = 512;

const CMD_PROTO_INFO: u8 = 0x01;
const CMD_PARAM_META_COUNT: u8 = 0x02;
const CMD_PARAM_META_GET: u8 = 0x03;
const CMD_PARAM_READ: u8 = 0x04;
const CMD_STATUS_GET: u8 = 0x07;
const CMD_FAULT_GET: u8 = 0x09;
const CMD_STREAM_SUB: u8 = 0x0B;
const CMD_STREAM_UNSUB: u8 = 0x0C;
const CMD_STREAM_DATA: u8 = 0x8E;

/// IN 传输请求长度：必须是 MPS(512) 的整数倍；流帧最大 9 + 512 = 521，
/// 只用 512 会把尾包留在端点里，后续帧全部错位。
const MAX_TRANSFER: usize = 1024;
const SAMPLE_SIZE: usize = 72;

const ST_OK: u8 = 0x00;
const STATUS_TIMEOUT: Duration = Duration::from_millis(1000);

fn crc16_update(mut crc: u16, byte: u8) -> u16 {
    crc ^= (byte as u16) << 8;
    for _ in 0..8 {
        crc = if crc & 0x8000 != 0 {
            (crc << 1) ^ 0x1021
        } else {
            crc << 1
        };
    }
    crc
}

fn build_request(cmd: u8, seq: u8, payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(9 + payload.len());
    out.push(SOF0);
    out.push(SOF1);
    out.push(VER);
    out.push(cmd);
    out.push(seq);
    out.push((payload.len() & 0xFF) as u8);
    out.push((payload.len() >> 8) as u8);
    out.extend_from_slice(payload);

    let mut crc = 0xFFFFu16;
    for &b in &out[2..] {
        crc = crc16_update(crc, b);
    }
    out.push((crc & 0xFF) as u8);
    out.push((crc >> 8) as u8);
    out
}

/// 解析应答帧，返回 payload（首字节为 status）。
fn parse_response(raw: &[u8], expect_cmd: u8, expect_seq: u8) -> Result<&[u8], String> {
    if raw.len() < 9 {
        return Err(format!("short frame: {} bytes", raw.len()));
    }
    if raw[0] != SOF0 || raw[1] != SOF1 {
        return Err(format!("bad SOF: {:02X} {:02X}", raw[0], raw[1]));
    }
    if raw[2] != VER {
        return Err(format!("bad VER: {}", raw[2]));
    }
    if raw[3] != (expect_cmd | RESPONSE_BIT) {
        return Err(format!("bad CMD: {:02X}", raw[3]));
    }
    if raw[4] != expect_seq {
        return Err(format!("seq mismatch: {} != {}", raw[4], expect_seq));
    }
    let len = u16::from_le_bytes([raw[5], raw[6]]) as usize;
    if len > MAX_PAYLOAD || raw.len() < 7 + len + 2 {
        return Err(format!("bad LEN: {}", len));
    }

    let mut crc = 0xFFFFu16;
    for &b in &raw[2..7 + len] {
        crc = crc16_update(crc, b);
    }
    let got = u16::from_le_bytes([raw[7 + len], raw[8 + len]]);
    if crc != got {
        return Err(format!("CRC mismatch: {:04X} != {:04X}", crc, got));
    }
    Ok(&raw[7..7 + len])
}

fn f32_at(buf: &[u8], pos: usize) -> f32 {
    f32::from_le_bytes([buf[pos], buf[pos + 1], buf[pos + 2], buf[pos + 3]])
}

fn u32_at(buf: &[u8], pos: usize) -> u32 {
    u32::from_le_bytes([buf[pos], buf[pos + 1], buf[pos + 2], buf[pos + 3]])
}

fn u16_at(buf: &[u8], pos: usize) -> u16 {
    u16::from_le_bytes([buf[pos], buf[pos + 1]])
}

struct Link {
    ep_out: nusb::Endpoint<Bulk, Out>,
    ep_in: nusb::Endpoint<Bulk, In>,
    seq: u8,
}

impl Link {
    fn transact(&mut self, cmd: u8, payload: &[u8]) -> Result<Vec<u8>, String> {
        let seq = self.seq;
        self.seq = self.seq.wrapping_add(1);

        let frame = build_request(cmd, seq, payload);
        let completion = self
            .ep_out
            .transfer_blocking(Buffer::from(frame), STATUS_TIMEOUT);
        completion
            .status
            .map_err(|e| format!("OUT transfer failed: {e:?}"))?;

        let completion = self
            .ep_in
            .transfer_blocking(Buffer::new(MAX_TRANSFER), STATUS_TIMEOUT);
        completion
            .status
            .map_err(|e| format!("IN transfer failed: {e:?}"))?;

        let raw = &completion.buffer[..completion.actual_len];
        let payload = parse_response(raw, cmd, seq)?;
        Ok(payload.to_vec())
    }
}

fn main() -> Result<(), Box<dyn Error>> {
    let info = list_devices()
        .wait()?
        .find(|d| d.vendor_id() == VID && d.product_id() == PID)
        .ok_or("device not found (VID 34B7 / PID 6618)")?;

    /* Windows 不缓存 manufacturer 字符串：manufacturer_string() 恒 None，
     * 必须用 get_manufacturer_string() 主动读描述符。 */
    let manufacturer = info
        .get_manufacturer_string()
        .wait()
        .ok()
        .flatten()
        .unwrap_or_else(|| "<no manufacturer>".to_string());
    let product = info.product_string().unwrap_or("<no product string>");
    let serial = info.serial_number().unwrap_or("<no serial>");
    println!(
        "device: {manufacturer} / {product}\n        VID={VID:04X} PID={PID:04X} serial={serial}"
    );
    /* 直接读接口字符串描述符：与 Windows 缓存的 FriendlyName 无关，
     * 用于判断固件是否真的改了 iInterface（改名字后必须以此为准）。 */
    if let Some(intf) = info.interfaces().find(|i| i.interface_number() == INTERFACE) {
        println!(
            "        interface0=\"{}\"",
            intf.interface_string().unwrap_or("<no interface string>")
        );
    }

    let device = info.open().wait()?;
    let interface = device.claim_interface(INTERFACE).wait()?;
    let mut link = Link {
        ep_out: interface.endpoint::<Bulk, Out>(EP_OUT)?,
        ep_in: interface.endpoint::<Bulk, In>(EP_IN)?,
        seq: 0,
    };

    let mut failures = 0u32;

    // 1. PROTO_INFO
    match link.transact(CMD_PROTO_INFO, &[]) {
        Ok(r) if r[0] == ST_OK && r.len() >= 8 => {
            let major = r[1];
            let minor = r[2];
            let caps = u32_at(&r, 3);
            let name_len = r[7] as usize;
            let name = String::from_utf8_lossy(&r[8..(8 + name_len).min(r.len())]).to_string();
            println!("[OK] PROTO_INFO: proto={major}.{minor} caps=0x{caps:08X} name=\"{name}\"");
        }
        Ok(r) => {
            failures += 1;
            println!("[FAIL] PROTO_INFO: status={} len={}", r[0], r.len());
        }
        Err(e) => {
            failures += 1;
            println!("[FAIL] PROTO_INFO: {e}");
        }
    }

    // 2. PARAM_META_COUNT
    let mut meta_count = 0u16;
    match link.transact(CMD_PARAM_META_COUNT, &[]) {
        Ok(r) if r[0] == ST_OK && r.len() >= 3 => {
            meta_count = u16_at(&r, 1);
            println!("[OK] PARAM_META_COUNT: {meta_count}");
        }
        Ok(r) => {
            failures += 1;
            println!("[FAIL] PARAM_META_COUNT: status={}", r[0]);
        }
        Err(e) => {
            failures += 1;
            println!("[FAIL] PARAM_META_COUNT: {e}");
        }
    }

    // 3. PARAM_META_GET(0)
    if meta_count > 0 {
        match link.transact(CMD_PARAM_META_GET, &0u16.to_le_bytes()) {
            Ok(r) if r[0] == ST_OK && r.len() >= 14 => {
                let index = u16_at(&r, 1);
                let ty = r[3];
                let apply = r[4];
                let min = f32_at(&r, 5);
                let max = f32_at(&r, 9);
                let name_len = r[13] as usize;
                let name =
                    String::from_utf8_lossy(&r[14..(14 + name_len).min(r.len())]).to_string();
                let unit_pos = 14 + name_len;
                let unit = if unit_pos < r.len() {
                    let unit_len = r[unit_pos] as usize;
                    String::from_utf8_lossy(
                        &r[(unit_pos + 1)..(unit_pos + 1 + unit_len).min(r.len())],
                    )
                    .to_string()
                } else {
                    String::new()
                };
                println!(
                    "[OK] META[{index}]: name={name} type={ty} apply={apply} min={min} max={max} unit=\"{unit}\""
                );
            }
            Ok(r) => {
                failures += 1;
                println!("[FAIL] PARAM_META_GET: status={} len={}", r[0], r.len());
            }
            Err(e) => {
                failures += 1;
                println!("[FAIL] PARAM_META_GET: {e}");
            }
        }
    }

    // 4. PARAM_READ("motor.pole_pairs")
    let name = "motor.pole_pairs";
    let mut req = Vec::with_capacity(1 + name.len());
    req.push(name.len() as u8);
    req.extend_from_slice(name.as_bytes());
    match link.transact(CMD_PARAM_READ, &req) {
        Ok(r) if r[0] == ST_OK && r.len() >= 6 => {
            println!("[OK] PARAM_READ {name}: type={} value={}", r[1], u32_at(&r, 2));
        }
        Ok(r) => {
            failures += 1;
            println!("[FAIL] PARAM_READ {name}: status={}", r[0]);
        }
        Err(e) => {
            failures += 1;
            println!("[FAIL] PARAM_READ {name}: {e}");
        }
    }

    // 5. FAULT_GET
    match link.transact(CMD_FAULT_GET, &[]) {
        Ok(r) if r[0] == ST_OK && r.len() >= 9 => {
            println!(
                "[OK] FAULT_GET: codes=0x{:08X} latched=0x{:08X}",
                u32_at(&r, 1),
                u32_at(&r, 5)
            );
        }
        Ok(r) => {
            failures += 1;
            println!("[FAIL] FAULT_GET: status={}", r[0]);
        }
        Err(e) => {
            failures += 1;
            println!("[FAIL] FAULT_GET: {e}");
        }
    }

    // 6. STATUS_GET × 20（约 5 Hz，验证持续收发）
    println!("-- STATUS_GET x20 (~5 Hz) --");
    for i in 0..20 {
        match link.transact(CMD_STATUS_GET, &[]) {
            Ok(r) if r[0] == ST_OK && r.len() >= 43 => {
                println!(
                    "[{i:02}] fault=0x{:08X} run={} state={} freq={:.2}Hz mod={:.1}% \
iu={:.2}A iv={:.2}A iw={:.2}A vbus={:.2}V ntc={:.1}C rotor={:.1}deg output={:.1}deg",
                    u32_at(&r, 1),
                    r[5],
                    r[6],
                    f32_at(&r, 7),
                    f32_at(&r, 11),
                    f32_at(&r, 15),
                    f32_at(&r, 19),
                    f32_at(&r, 23),
                    f32_at(&r, 27),
                    f32_at(&r, 31),
                    f32_at(&r, 35),
                    f32_at(&r, 39),
                );
            }
            Ok(r) => {
                failures += 1;
                println!("[FAIL] STATUS_GET: status={} len={}", r[0], r.len());
                break;
            }
            Err(e) => {
                failures += 1;
                println!("[FAIL] STATUS_GET: {e}");
                break;
            }
        }
        std::thread::sleep(Duration::from_millis(200));
    }

    // 7. 遥测流：STREAM_SUB(1kHz, batch=7)，采集 3 秒
    println!("-- STREAM_SUB rate=1000Hz batch=7 (3s) --");
    let mut subscribed = false;
    {
        let mut req = Vec::new();
        req.extend_from_slice(&1000u16.to_le_bytes());
        req.extend_from_slice(&7u16.to_le_bytes());
        match link.transact(CMD_STREAM_SUB, &req) {
            Ok(r) if r[0] == ST_OK => {
                subscribed = true;
                println!("[OK] STREAM_SUB");
            }
            Ok(r) => {
                failures += 1;
                println!("[FAIL] STREAM_SUB: status={}", r[0]);
            }
            Err(e) => {
                failures += 1;
                println!("[FAIL] STREAM_SUB: {e}");
            }
        }
    }

    if subscribed {
        let started = std::time::Instant::now();
        let deadline = started + Duration::from_secs(3);
        let mut frames = 0u64;
        let mut samples = 0u64;
        let mut parse_errors = 0u64;
        let mut printed = false;

        while std::time::Instant::now() < deadline {
            let completion = link
                .ep_in
                .transfer_blocking(Buffer::new(MAX_TRANSFER), Duration::from_millis(500));
            if completion.status.is_err() {
                break;
            }
            let raw = &completion.buffer[..completion.actual_len];
            if raw.len() < 9 || raw[0] != SOF0 || raw[1] != SOF1 || raw[2] != VER {
                parse_errors += 1;
                continue;
            }
            if raw[3] != CMD_STREAM_DATA {
                continue; /* SUB 的应答残余 */
            }
            let len = u16::from_le_bytes([raw[5], raw[6]]) as usize;
            if len > MAX_PAYLOAD || raw.len() < 7 + len + 2 {
                parse_errors += 1;
                continue;
            }
            let mut crc = 0xFFFFu16;
            for &b in &raw[2..7 + len] {
                crc = crc16_update(crc, b);
            }
            let got = u16::from_le_bytes([raw[7 + len], raw[8 + len]]);
            if crc != got {
                parse_errors += 1;
                continue;
            }
            let payload = &raw[7..7 + len];
            if payload.len() < 8 {
                parse_errors += 1;
                continue;
            }
            let count = u16::from_le_bytes([payload[4], payload[5]]) as usize;
            let size = payload[6] as usize;
            if size != SAMPLE_SIZE || payload.len() != 8 + count * size || count == 0 {
                parse_errors += 1;
                continue;
            }
            frames += 1;
            samples += count as u64;

            if !printed {
                printed = true;
                let s = &payload[8..8 + size];
                println!(
                    "  sample: t={}us iu={:.2} iv={:.2} iw={:.2} vbus={:.2} ntc0={:.1} \
ntc1={:.1} rotor_raw={} rotor_deg={:.1} output_raw={} output_deg={:.1} id={:.2} \
iq={:.2} speed={:.3} torque={:.3} pos={:.3} fault=0x{:08X}",
                    u32_at(s, 0),
                    f32_at(s, 4),
                    f32_at(s, 8),
                    f32_at(s, 12),
                    f32_at(s, 16),
                    f32_at(s, 20),
                    f32_at(s, 24),
                    u16_at(s, 68),
                    f32_at(s, 36),
                    u16_at(s, 70),
                    f32_at(s, 40),
                    f32_at(s, 44),
                    f32_at(s, 48),
                    f32_at(s, 52),
                    f32_at(s, 56),
                    f32_at(s, 60),
                    u32_at(s, 64)
                );
            }
        }

        let elapsed = started.elapsed().as_secs_f64();
        let rate = if elapsed > 0.0 { samples as f64 / elapsed } else { 0.0 };
        println!(
            "[{}] stream: {frames} frames / {samples} samples in {elapsed:.2}s \
(~{rate:.0} sample/s), parse_errors={parse_errors}",
            if samples > 0 && parse_errors == 0 { "OK" } else { "FAIL" }
        );
        if samples == 0 || parse_errors != 0 {
            failures += 1;
        }

        let _ = link.transact(CMD_STREAM_UNSUB, &[]);
    }

    if failures == 0 {
        println!("== ALL PASS ==");
        Ok(())
    } else {
        println!("== {failures} FAILURE(S) ==");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(s: &str) -> Vec<u8> {
        (0..s.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
            .collect()
    }

    /// 金向量（Gui/protocol/test_vectors.json）：编码结果必须逐字节一致。
    #[test]
    fn request_frames_match_golden_vectors() {
        assert_eq!(build_request(CMD_PROTO_INFO, 0, &[]), hex("A55A0101000000E9CD"));

        let mut payload = vec![16u8];
        payload.extend_from_slice(b"motor.pole_pairs");
        assert_eq!(
            build_request(CMD_PARAM_READ, 1, &payload),
            hex("A55A0104011100106D6F746F722E706F6C655F70616972733540")
        );
    }

    /// CRC16-CCITT-FALSE 标准校验值："123456789" -> 0x29B1。
    #[test]
    fn crc_sanity() {
        let mut crc = 0xFFFFu16;
        for b in b"123456789" {
            crc = crc16_update(crc, *b);
        }
        assert_eq!(crc, 0x29B1);
    }

    /// 应答解析：模拟 gui-sim 的 PROTO_INFO 应答。
    #[test]
    fn parses_golden_response() {
        let raw = hex("A55A0181000F0000010003000000076775692D73696DCF5D");
        let payload = parse_response(&raw, CMD_PROTO_INFO, 0).unwrap();
        assert_eq!(payload[0], ST_OK);
        assert_eq!(payload[1], 1);
        assert_eq!(u32_at(payload, 3), 3);
        assert_eq!(&payload[8..15], b"gui-sim");
    }
}
