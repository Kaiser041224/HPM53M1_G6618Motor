/// 参数编解码、校验与共享控件。
///
/// 提取自 `param_zone.dart` —— 参数页与各控制/配置板块共用同一套
/// 解析、校验、编辑与「生效时机」徽标，避免多份实现各自漂移。
///
/// 本文件只依赖 [AppState] 的公开接口；参数**名称一律由调用方给出、
/// 元数据一律来自设备**（`AppState.metas`），不在此硬编码任何参数表。
library;

import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../state/app_state.dart';
import '../theme/gui_glow.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import 'gui_button.dart';
import 'gui_text_field.dart';

/// 参数表统一阅读字号 [logical px]：名称 / 类型 / 范围 / 徽标 / 值同档，
/// 层级只靠颜色与字重区分，避免同一列内大小混排。
const double kParamRowFontSize = 15;

/// Human-readable name of a parameter wire type.
String typeName(int ty) => switch (ty) {
  0 => 'f32',
  1 => 'u32',
  2 => 'i32',
  3 => 'bool',
  _ => 'ty$ty',
};

/// Reinterprets a 32-bit [raw] little-endian bit pattern as an IEEE-754 single.
double f32FromRaw(int raw) =>
    (ByteData(4)..setInt32(0, raw, Endian.little)).getFloat32(0, Endian.little);

/// Encodes an IEEE-754 single into its 32-bit little-endian [raw] bit pattern.
int f32ToRaw(double value) => (ByteData(
  4,
)..setFloat32(0, value, Endian.little)).getInt32(0, Endian.little);

/// Formats [raw] for display according to its wire type.
String formatValue(int ty, int raw) => switch (ty) {
  0 => f32FromRaw(raw).toStringAsFixed(2),
  3 => raw == 1 ? 'true' : 'false',
  _ => raw.toString(),
};

/// Short numeric rendering used inside range hints.
String numText(double v) => v == v.roundToDouble()
    ? v.toStringAsFixed(v.abs() >= 1000 ? 0 : 1)
    : v.toStringAsFixed(2);

/// 参数的量程提示文本（空量程返回空串）。
String rangeText(ParamMetaDto meta) =>
    meta.min <= meta.max ? '${numText(meta.min)}..${numText(meta.max)}' : '';

/// Namespace a parameter belongs to: its name minus the leaf segment. The
/// group is derived from metadata, never hard-coded (`motor.i_rated_a` ->
/// `motor`, `software.fault.vbus_ov_v` -> `software.fault`).
String groupKeyOf(String name) {
  final dot = name.lastIndexOf('.');
  return dot > 0 ? name.substring(0, dot) : name;
}

/// Uppercase display label for a [groupKeyOf] result (`software.fault` ->
/// `SOFTWARE · FAULT`, `hardware.current_sense` -> `HARDWARE · CURRENT SENSE`).
String groupLabelOf(String key) =>
    key.split('.').join(' · ').replaceAll('_', ' ').toUpperCase();

/// Maps a bridge failure to the user-facing message for the last error.
String faultText(AppState s) {
  switch (s.lastErrorCode) {
    case 4:
      return '设备联锁拒绝：先 STOP / FAULT_CLEAR';
    case 6:
      return '设备判定越界';
    default:
      return s.lastError;
  }
}

/// 参数**域**的语义色。
///
/// 三个域各占一个色相，让长表在滚动时一眼能看出「现在读到哪一域了」。
/// 这是**强化**而非承载 —— 域名字始终显示在标题里，颜色只是同一信息的
/// 第二种编码，符合设计系统「颜色不得单独承载语义」的约束。
///
/// - `motor` → 绿（机械/电磁主体）
/// - `hardware` → 青（板级电路，与主强调同族）
/// - `software` → 琥珀（软件策略/门限）
/// - 其他 → 淡青（不猜）
Color domainColor(GuiPalette palette, String groupKey) {
  final head = groupKey.split('.').first;
  return switch (head) {
    'motor' => palette.valueGreen,
    'hardware' => palette.accent,
    'software' => palette.valueAmber,
    _ => palette.accentBlue,
  };
}

/// 按名从设备元数据里取参数；找不到返回 null。
///
/// 设备才是参数集的权威 —— 取不到就是「设备未提供」，调用方须如实呈现，
/// 不得用猜测的默认值兜底。
ParamMetaDto? metaOf(AppState state, String name) {
  for (final m in state.metas) {
    if (m.name == name) return m;
  }
  return null;
}

/// A small square LIVE/REBOOT chip marking when a parameter takes effect.
class ApplyBadge extends StatelessWidget {
  const ApplyBadge({
    super.key,
    required this.label,
    required this.color,
    this.fontSize,
  });

  final String label;
  final Color color;

  /// 字号覆盖；为 null 时用 [GuiTypeScale.label] 原生字号。
  final double? fontSize;

  @override
  Widget build(BuildContext context) {
    final scale = GuiTypeScale.of(context);
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 1),
      decoration: BoxDecoration(
        border: Border.all(color: color.withValues(alpha: 0.55)),
      ),
      child: Text(
        label,
        style: scale.label.copyWith(
          color: color,
          fontSize: fontSize ?? scale.label.fontSize,
          decoration: TextDecoration.none,
        ),
      ),
    );
  }
}

/// 安全相关徽标 —— 固件把过流/过压/欠压/PWM 频率/死区等列为安全参数
/// （终端 set 时会警告）。这里让它们在界面上同样醒目。
class SafetyBadge extends StatelessWidget {
  const SafetyBadge({super.key, this.fontSize});

  /// 字号覆盖；透传给 [ApplyBadge]。
  final double? fontSize;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    return ApplyBadge(
      label: 'SAFETY',
      color: palette.accent2,
      fontSize: fontSize,
    );
  }
}

/// Per-parameter edit dialog with type-aware parsing and range validation.
class ParamEditDialog extends StatefulWidget {
  const ParamEditDialog({super.key, required this.state, required this.meta});

  final AppState state;
  final ParamMetaDto meta;

  @override
  State<ParamEditDialog> createState() => _ParamEditDialogState();
}

class _ParamEditDialogState extends State<ParamEditDialog> {
  late final TextEditingController _valueCtrl;
  String _error = '';
  bool _busy = false;

  @override
  void initState() {
    super.initState();
    final current = widget.state.values[widget.meta.name];
    final initial = current == null
        ? ''
        : formatValue(widget.meta.ty, current.raw);
    _valueCtrl = TextEditingController(text: initial);
  }

  @override
  void dispose() {
    _valueCtrl.dispose();
    super.dispose();
  }

  TextInputType get _keyboard => switch (widget.meta.ty) {
    0 => const TextInputType.numberWithOptions(decimal: true, signed: true),
    1 || 2 => TextInputType.number,
    _ => TextInputType.text,
  };

  Future<void> _submit() async {
    final meta = widget.meta;
    final text = _valueCtrl.text.trim();

    double? numeric;
    int? raw;

    switch (meta.ty) {
      case 0:
        final parsed = double.tryParse(text);
        if (parsed == null) {
          setState(() => _error = '请输入有效数值');
          return;
        }
        numeric = parsed;
        raw = f32ToRaw(parsed);
      case 1:
        final parsed = int.tryParse(text);
        if (parsed == null || parsed < 0) {
          setState(() => _error = '请输入有效的非负整数');
          return;
        }
        numeric = parsed.toDouble();
        raw = parsed;
      case 2:
        final parsed = int.tryParse(text);
        if (parsed == null) {
          setState(() => _error = '请输入有效整数');
          return;
        }
        numeric = parsed.toDouble();
        raw = parsed;
      case 3:
        final lower = text.toLowerCase();
        if (lower == '1' || lower == 'true') {
          numeric = 1;
          raw = 1;
        } else if (lower == '0' || lower == 'false') {
          numeric = 0;
          raw = 0;
        } else {
          setState(() => _error = '布尔仅接受 0/1');
          return;
        }
      default:
        setState(() => _error = '未知类型 ty=${meta.ty}');
        return;
    }

    if (meta.min <= meta.max && (numeric < meta.min || numeric > meta.max)) {
      setState(
        () => _error =
            '超出范围 ${numText(meta.min)}..${numText(meta.max)} ${meta.unit}'
                .trimRight(),
      );
      return;
    }

    setState(() {
      _busy = true;
      _error = '';
    });
    final ok = await widget.state.writeParam(meta.name, meta.ty, raw);
    if (!mounted) return;
    if (ok) {
      Navigator.pop(context);
      return;
    }
    setState(() {
      _busy = false;
      _error = faultText(widget.state);
    });
  }

  @override
  Widget build(BuildContext context) {
    final meta = widget.meta;
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return AlertDialog(
      title: Text(meta.name),
      content: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Text(
            '${typeName(meta.ty)} · ${rangeText(meta)} ${meta.unit}'
                .trimRight(),
            style: scale.label.copyWith(
              color: palette.fgDim,
              decoration: TextDecoration.none,
            ),
          ),
          const SizedBox(height: 8),
          GuiTextField(
            controller: _valueCtrl,
            keyboardType: _keyboard,
            enabled: !_busy,
            autofocus: true,
            labelText: '新值',
          ),
          if (_error.isNotEmpty)
            Padding(
              padding: const EdgeInsets.only(top: 12),
              child: Text(
                _error,
                style: scale.label.copyWith(
                  color: palette.stFault,
                  decoration: TextDecoration.none,
                ),
              ),
            ),
        ],
      ),
      actions: <Widget>[
        GuiButton(
          label: '取消',
          onPressed: _busy ? null : () => Navigator.pop(context),
        ),
        GuiButton(
          label: '写入',
          variant: GuiButtonVariant.primary,
          onPressed: _busy ? null : _submit,
        ),
      ],
    );
  }
}

/// 弹出参数编辑对话框。写入成功返回 true。
Future<bool> openParamEditor(
  BuildContext context,
  AppState state,
  ParamMetaDto meta,
) async {
  final result = await showDialog<bool>(
    context: context,
    builder: (_) => ParamEditDialog(state: state, meta: meta),
  );
  return result ?? false;
}

/// 紧凑可编辑参数字段 —— 供控制板块（保护门限 / 电流环 PI / 调试配置）使用。
///
/// 与参数页的表格行不同，这里是「标签 + 值 + 量程」的纵向小块，
/// 可嵌进窄栏。点击弹出 [ParamEditDialog]。
///
/// 参数**必须由设备元数据提供**：取不到即渲染为「设备未提供」并禁用，
/// 绝不用猜测值兜底 —— 否则界面会显示一个设备根本没有的参数。
class ParamField extends StatelessWidget {
  const ParamField({
    super.key,
    required this.state,
    required this.name,
    required this.label,
    this.safety = false,
    this.compact = false,
  });

  /// 参数全名（域限定），如 `software.fault.oc_trip_a`。
  final String name;

  /// 中文显示名。
  final String label;

  /// 是否为固件标注的安全相关参数（改变时设备会警告）。
  final bool safety;

  /// 紧凑模式：更紧的垂直留白，用于多列并排（字号统一，不因紧凑而缩小）。
  final bool compact;

  final AppState state;

  Future<void> _edit(BuildContext context) async {
    final meta = metaOf(state, name);
    if (meta == null) return;
    await openParamEditor(context, state, meta);
  }

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);

    final meta = metaOf(state, name);
    final value = meta == null ? null : state.values[name];
    final dirty = state.dirty.contains(name);

    if (meta == null) {
      // 设备未提供此参数 —— 如实呈现，不给假值。
      return _shell(
        palette: palette,
        scale: scale,
        label: label,
        valueText: '—',
        valueColor: palette.fgMute,
        hint: '设备未提供',
        hintColor: palette.fgMute,
        enabled: false,
      );
    }

    final shown = value == null ? '—' : formatValue(meta.ty, value.raw);
    final unit = value == null ? '' : meta.unit;

    // 脏（已下发未落盘）走告警黄并加点；其余走琥珀读数色。
    final valueColor = value == null
        ? palette.fgMute
        : (dirty ? palette.stWarn : palette.valueAmber);

    final hint = dirty
        ? '● 未落盘'
        : (rangeText(meta).isEmpty
              ? (meta.apply == 1 ? 'REBOOT 生效' : 'LIVE 生效')
              : '${rangeText(meta)} ${meta.unit}'.trimRight());

    return InkWell(
      onTap: () => _edit(context),
      child: _shell(
        palette: palette,
        scale: scale,
        label: label,
        valueText: '$shown${unit.isEmpty ? '' : ' $unit'}',
        valueColor: valueColor,
        hint: hint,
        hintColor: dirty ? palette.stWarn : palette.fgMute,
        enabled: true,
        safety: safety,
        compact: compact,
      ),
    );
  }

  Widget _shell({
    required GuiPalette palette,
    required GuiTypeScale scale,
    required String label,
    required String valueText,
    required Color valueColor,
    required String hint,
    required Color hintColor,
    required bool enabled,
    bool safety = false,
    bool compact = false,
  }) {
    return Opacity(
      opacity: enabled ? 1 : 0.55,
      child: Container(
        padding: EdgeInsets.symmetric(
          horizontal: 9,
          vertical: compact ? 6 : 7,
        ),
        decoration: BoxDecoration(
          color: palette.bgVoid,
          border: Border.all(color: palette.lineDim, width: palette.lineWidth),
        ),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            Row(
              children: <Widget>[
                Expanded(
                  child: Text(
                    label,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: scale.label.copyWith(color: palette.fgDim),
                  ),
                ),
                if (safety) const SafetyBadge(),
              ],
            ),
            const SizedBox(height: 5),
            FittedBox(
              fit: BoxFit.scaleDown,
              alignment: Alignment.centerLeft,
              child: Text(
                valueText,
                maxLines: 1,
                // 单层同色相辉光（0.12）：小字号琥珀值不再拾取青色跨色相光晕，
                // 字符基线下方因此不再叠出第二条线（「双黄线」根治）。
                style: GuiGlowStyles.softTint(
                  valueColor,
                  scale.data.copyWith(
                    fontSize: kParamRowFontSize,
                    color: valueColor,
                  ),
                ),
              ),
            ),
            const SizedBox(height: 3),
            Text(
              hint,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: scale.label.copyWith(color: hintColor),
            ),
          ],
        ),
      ),
    );
  }
}
