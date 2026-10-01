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
    // v1.3 增量（additive，独立命令字）：按名单/全部恢复出厂默认值。
    pub const PARAM_RESET: u8 = 0x0D;
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
    // v1.1 增量（additive，镜像 `motor_proto.h` 的 mp_ctrl_opcode_t；
    // 收敛 sim/frb 各自的本地 `ctrl_v11`，消除镜像漂移）。
    pub const SET_OUTPUT_ZERO_NOW: u8 = 0x05;
    pub const SET_OUTPUT_ZERO_ABS: u8 = 0x06;
    pub const SET_MODE: u8 = 0x07;
    // v1.2 增量（additive）：设定值写入，联锁同 v1.1（停机窗口）。
    pub const SET_TORQUE: u8 = 0x08;
    pub const SET_SPEED: u8 = 0x09;
    pub const SET_ANGLE: u8 = 0x0A;
}

#[cfg(test)]
mod tests {
    use super::*;

    /// v1.1 ctrl opcode 与 `motor_proto.h` / sim / frb 三处镜像一致。
    #[test]
    fn ctrl_v11_opcodes_match_contract() {
        assert_eq!(ctrl::SET_OUTPUT_ZERO_NOW, 0x05);
        assert_eq!(ctrl::SET_OUTPUT_ZERO_ABS, 0x06);
        assert_eq!(ctrl::SET_MODE, 0x07);
    }

    /// v1.2 设定值 opcode 与 `motor_proto.h` / sim / frb 三处镜像一致。
    #[test]
    fn ctrl_v12_opcodes_match_contract() {
        assert_eq!(ctrl::SET_TORQUE, 0x08);
        assert_eq!(ctrl::SET_SPEED, 0x09);
        assert_eq!(ctrl::SET_ANGLE, 0x0A);
    }

    /// v1.3 命令字与 `motor_proto.h` / sim / frb 三处镜像一致。
    #[test]
    fn cmd_v13_param_reset_matches_contract() {
        assert_eq!(cmd::PARAM_RESET, 0x0D);
    }
}
