//! motor_proto.h 的 Rust 常量镜像。改动必须双侧同步并补金向量。

pub const VER: u8 = 0x01;
pub const SOF0: u8 = 0xA5;
pub const SOF1: u8 = 0x5A;
pub const MAX_PAYLOAD: usize = 512;
pub const RESPONSE_BIT: u8 = 0x80;

pub mod cmd {
    pub const PROTO_INFO: u8 = 0x01;
    pub const PARAM_META_COUNT: u8 = 0x02;
    pub const PARAM_META_GET: u8 = 0x03;
    pub const PARAM_READ: u8 = 0x04;
    pub const PARAM_WRITE: u8 = 0x05;
    pub const PARAM_COMMIT: u8 = 0x06;
    pub const STATUS_GET: u8 = 0x07;
    pub const CTRL_CMD: u8 = 0x08;
    pub const FAULT_GET: u8 = 0x09;
    pub const FAULT_CLEAR: u8 = 0x0A;
    pub const STREAM_SUB: u8 = 0x0B;
    pub const STREAM_UNSUB: u8 = 0x0C;
}

pub mod status {
    pub const OK: u8 = 0x00;
    pub const BAD_CMD: u8 = 0x01;
    pub const BAD_PARAM: u8 = 0x02;
    pub const BUSY: u8 = 0x03;
    pub const LOCKED: u8 = 0x04;
    pub const UNSUPPORTED: u8 = 0x05;
    pub const RANGE_ERR: u8 = 0x06;
}

pub mod ctrl {
    pub const START: u8 = 0x01;
    pub const STOP: u8 = 0x02;
    pub const SET_FREQ: u8 = 0x03;
    pub const SET_MOD: u8 = 0x04;
}
