//! gui-core：Gui 通讯核（transport / codec / session / params）。
//! 契约源头：`Gui/protocol/motor_proto.h`，本 crate 为其 Rust 镜像实现。

pub mod proto;
pub mod codec;
pub mod transport;
pub mod session;
pub mod params;
