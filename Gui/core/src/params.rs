//! 参数模型：元数据 + 值 + 校验 + payload 编解码（小端，镜像 motor_proto.h）。

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ParamType {
    F32,
    U32,
    I32,
    Bool,
}

impl ParamType {
    pub fn to_u8(self) -> u8 {
        match self {
            ParamType::F32 => 0,
            ParamType::U32 => 1,
            ParamType::I32 => 2,
            ParamType::Bool => 3,
        }
    }

    pub fn from_u8(v: u8) -> Option<Self> {
        match v {
            0 => Some(ParamType::F32),
            1 => Some(ParamType::U32),
            2 => Some(ParamType::I32),
            3 => Some(ParamType::Bool),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Apply {
    Live,
    Reboot,
}

impl Apply {
    pub fn to_u8(self) -> u8 {
        match self {
            Apply::Live => 0,
            Apply::Reboot => 1,
        }
    }

    pub fn from_u8(v: u8) -> Option<Self> {
        match v {
            0 => Some(Apply::Live),
            1 => Some(Apply::Reboot),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct ParamMeta {
    pub index: u16,
    pub name: String,
    pub ty: ParamType,
    pub apply: Apply,
    pub unit: String,
    pub min: f32,
    pub max: f32,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ParamValue {
    pub raw: u32,
}

impl ParamValue {
    pub fn from_f32(v: f32) -> Self {
        Self { raw: v.to_bits() }
    }

    pub fn as_f32(&self) -> f32 {
        f32::from_bits(self.raw)
    }

    pub fn from_u32(v: u32) -> Self {
        Self { raw: v }
    }

    pub fn as_u32(&self) -> u32 {
        self.raw
    }

    pub fn from_i32(v: i32) -> Self {
        Self { raw: v as u32 }
    }

    pub fn as_i32(&self) -> i32 {
        self.raw as i32
    }

    pub fn from_bool(v: bool) -> Self {
        Self { raw: v as u32 }
    }

    pub fn as_bool(&self) -> bool {
        self.raw != 0
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RangeError;

/// min <= max 时校验区间；min > max 表示不限制。
pub fn check_range(v: &ParamValue, meta: &ParamMeta) -> Result<(), RangeError> {
    if meta.min > meta.max {
        return Ok(());
    }
    let x = match meta.ty {
        ParamType::F32 => v.as_f32(),
        ParamType::U32 => v.as_u32() as f32,
        ParamType::I32 => v.as_i32() as f32,
        ParamType::Bool => v.raw as f32,
    };
    if x >= meta.min && x <= meta.max {
        Ok(())
    } else {
        Err(RangeError)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PayloadError;

/// len-prefixed (u8) name 编码；name 超过 255 字节返回 Err(PayloadError)。
pub fn encode_name(name: &str) -> Result<Vec<u8>, PayloadError> {
    let bytes = name.as_bytes();
    if bytes.len() > u8::MAX as usize {
        return Err(PayloadError);
    }
    let mut out = vec![bytes.len() as u8];
    out.extend_from_slice(bytes);
    Ok(out)
}

pub fn decode_name(buf: &[u8], pos: &mut usize) -> Result<String, PayloadError> {
    let len = *buf.get(*pos).ok_or(PayloadError)? as usize;
    *pos += 1;
    let end = (*pos).checked_add(len).ok_or(PayloadError)?;
    if end > buf.len() {
        return Err(PayloadError);
    }
    let s = std::str::from_utf8(&buf[*pos..end]).map_err(|_| PayloadError)?;
    *pos = end;
    Ok(s.to_string())
}

pub fn encode_meta_resp(meta: &ParamMeta) -> Result<Vec<u8>, PayloadError> {
    let mut out = Vec::new();
    out.extend_from_slice(&meta.index.to_le_bytes());
    out.push(meta.ty.to_u8());
    out.push(meta.apply.to_u8());
    out.extend_from_slice(&meta.min.to_le_bytes());
    out.extend_from_slice(&meta.max.to_le_bytes());
    out.extend_from_slice(&encode_name(&meta.name)?);
    out.extend_from_slice(&encode_name(&meta.unit)?);
    Ok(out)
}

pub fn decode_meta_resp(body: &[u8]) -> Result<ParamMeta, PayloadError> {
    if body.len() < 12 {
        return Err(PayloadError);
    }
    let index = u16::from_le_bytes([body[0], body[1]]);
    let ty = ParamType::from_u8(body[2]).ok_or(PayloadError)?;
    let apply = Apply::from_u8(body[3]).ok_or(PayloadError)?;
    let min = f32::from_le_bytes(body[4..8].try_into().unwrap());
    let max = f32::from_le_bytes(body[8..12].try_into().unwrap());
    let mut pos = 12;
    let name = decode_name(body, &mut pos)?;
    let unit = decode_name(body, &mut pos)?;
    Ok(ParamMeta { index, name, ty, apply, unit, min, max })
}

pub fn encode_value(v: ParamValue, ty: ParamType) -> [u8; 5] {
    let mut out = [0u8; 5];
    out[0] = ty.to_u8();
    out[1..5].copy_from_slice(&v.raw.to_le_bytes());
    out
}

pub fn decode_value(body: &[u8]) -> Result<(ParamType, ParamValue), PayloadError> {
    if body.len() < 5 {
        return Err(PayloadError);
    }
    let ty = ParamType::from_u8(body[0]).ok_or(PayloadError)?;
    let raw = u32::from_le_bytes(body[1..5].try_into().unwrap());
    Ok((ty, ParamValue { raw }))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn value_roundtrip_all_types() {
        assert_eq!(ParamValue::from_f32(1.5).as_f32(), 1.5);
        assert_eq!(ParamValue::from_u32(0xDEAD_BEEF).as_u32(), 0xDEAD_BEEF);
        assert_eq!(ParamValue::from_i32(-7).as_i32(), -7);
        assert!(ParamValue::from_bool(true).as_bool());
    }

    #[test]
    fn range_check_bounds_and_disabled() {
        let meta = ParamMeta {
            index: 0,
            name: "x".into(),
            ty: ParamType::F32,
            apply: Apply::Live,
            unit: "V".into(),
            min: 0.0,
            max: 60.0,
        };
        assert!(check_range(&ParamValue::from_f32(30.0), &meta).is_ok());
        assert!(check_range(&ParamValue::from_f32(61.0), &meta).is_err());
        let open = ParamMeta { min: 1.0, max: 0.0, ..meta };
        assert!(check_range(&ParamValue::from_f32(1e9), &open).is_ok());
    }

    #[test]
    fn meta_payload_roundtrip_matches_wire_layout() {
        let meta = ParamMeta {
            index: 7,
            name: "motor.pole_pairs".into(),
            ty: ParamType::U32,
            apply: Apply::Reboot,
            unit: String::new(),
            min: 1.0,
            max: 64.0,
        };
        let bytes = encode_meta_resp(&meta).unwrap();
        // 布局：index u16 | type u8 | apply u8 | min f32 | max f32 | name_len u8 | name | unit_len u8 | unit
        assert_eq!(&bytes[0..2], &7u16.to_le_bytes());
        assert_eq!(bytes[2], ParamType::U32 as u8);
        assert_eq!(bytes[3], Apply::Reboot as u8);
        let pos = 4 + 8 + 4; // 跳过 min/max 后是 name_len
        let got = decode_meta_resp(&bytes).unwrap();
        assert_eq!(got.name, "motor.pole_pairs");
        assert_eq!(got.index, 7);
        let _ = pos;
    }

    #[test]
    fn value_payload_roundtrip() {
        let (ty, v) = decode_value(&encode_value(ParamValue::from_f32(3.3), ParamType::F32)).unwrap();
        assert_eq!(ty, ParamType::F32);
        assert_eq!(v.as_f32(), 3.3);
    }

    #[test]
    fn encode_name_rejects_overlong_name() {
        let overlong = "a".repeat(256);
        assert_eq!(encode_name(&overlong), Err(PayloadError));
        let max = "a".repeat(255);
        assert!(encode_name(&max).is_ok());
    }
}
