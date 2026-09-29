//! config/*.yaml → 参数表（名字 = `<域>.<路径>`）。
//!
//! 标量类型推断：整数 → U32（负数 → I32），浮点 → F32，`true/false` → Bool。
//! 带引号字符串视为构建期表达式，v1 不支持求值，整条跳过不入表
//! （与 `gen_params` 的构建期求值不同，属已知限制）。

use gui_core::params::{Apply, ParamMeta, ParamType, ParamValue};
use std::collections::BTreeMap;

/// 递归展平 YAML，把标量叶子写入 `out`；`index` 为连续分配的参数序号。
pub fn flatten_yaml(
    domain: &str,
    value: &serde_yaml::Value,
    prefix: &str,
    out: &mut BTreeMap<String, (ParamMeta, ParamValue)>,
    index: &mut u16,
) {
    match value {
        serde_yaml::Value::Mapping(map) => {
            for (k, v) in map {
                let key = k.as_str().unwrap_or_default().to_string();
                let path =
                    if prefix.is_empty() { key } else { format!("{prefix}.{key}") };
                flatten_yaml(domain, v, &path, out, index);
            }
        }
        serde_yaml::Value::String(_) => {
            /* 带引号字符串 = 构建期表达式，v1 跳过 */
        }
        serde_yaml::Value::Number(n) => {
            let name = format!("{domain}.{prefix}");
            let (ty, val) = if let Some(i) = n.as_i64() {
                if i < 0 {
                    (ParamType::I32, ParamValue::from_i32(i as i32))
                } else {
                    (ParamType::U32, ParamValue::from_u32(i as u32))
                }
            } else {
                (ParamType::F32, ParamValue::from_f32(n.as_f64().unwrap_or(0.0) as f32))
            };
            out.insert(name.clone(), (meta_of(*index, &name, ty), val));
            *index += 1;
        }
        serde_yaml::Value::Bool(b) => {
            let name = format!("{domain}.{prefix}");
            out.insert(
                name.clone(),
                (meta_of(*index, &name, ParamType::Bool), ParamValue::from_bool(*b)),
            );
            *index += 1;
        }
        _ => {}
    }
}

/// 构造参数元数据；`min>max` 表示不限制（v1 简化，真范围以固件元数据为准）。
fn meta_of(index: u16, name: &str, ty: ParamType) -> ParamMeta {
    let apply = if name.starts_with("software.fault.") || name.starts_with("software.can.") {
        Apply::Reboot
    } else {
        Apply::Live
    };
    ParamMeta {
        index,
        name: name.to_string(),
        ty,
        apply,
        unit: String::new(),
        min: 1.0,
        max: 0.0,
    }
}
