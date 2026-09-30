import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../state/app_state.dart';
import '../theme/gui_glow.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/section_title.dart';

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
int f32ToRaw(double value) =>
    (ByteData(4)..setFloat32(0, value, Endian.little)).getInt32(0, Endian.little);

/// Formats [raw] for display according to its wire type.
String formatValue(int ty, int raw) => switch (ty) {
  0 => f32FromRaw(raw).toStringAsFixed(2),
  3 => raw == 1 ? 'true' : 'false',
  _ => raw.toString(),
};

/// Short numeric rendering used inside range hints.
String _num(double v) => v == v.roundToDouble()
    ? v.toStringAsFixed(v.abs() >= 1000 ? 0 : 1)
    : v.toStringAsFixed(2);

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

/// Metadata-driven parameter panel (design-system §4.1 参数区).
///
/// The entire table — groups, rows, types, ranges, apply badges and values —
/// is generated from [AppState.metas]; no parameter name is hard-coded here.
/// Values are tabular and right-aligned; a local edit not yet flushed to flash
/// gets a warning dot and a same-hue yellow value (design-system §6). The
/// footer shows the live COMMIT interlock: red/yellow when the drive is
/// running or a fault is latched, green when a flush would be accepted.
class ParamZone extends StatefulWidget {
  const ParamZone({super.key, required this.state});

  /// Observable parameter table, values and dirty set.
  final AppState state;

  @override
  State<ParamZone> createState() => _ParamZoneState();
}

class _ParamZoneState extends State<ParamZone> {
  final TextEditingController _searchCtrl = TextEditingController();

  @override
  void dispose() {
    _searchCtrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final state = widget.state;
    return ListenableBuilder(
      listenable: state,
      builder: (context, _) => Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          _header(context, state),
          const SizedBox(height: 14),
          _toolbar(context, state),
          const SizedBox(height: 16),
          _body(context, state),
          const SizedBox(height: 16),
          _interlock(context, state),
        ],
      ),
    );
  }

  Widget _header(BuildContext context, AppState state) {
    final scale = GuiTypeScale.of(context);
    return SectionTitle(
      title: '参数',
      en: 'Parameters · 元数据驱动',
      trailing: Text(
        '${state.metas.length} 项 · ${state.dirty.length} 项待落盘',
        style: scale.label,
      ),
    );
  }

  Widget _toolbar(BuildContext context, AppState state) {
    return Row(
      children: <Widget>[
        Expanded(
          child: TextField(
            controller: _searchCtrl,
            onChanged: (_) => setState(() {}),
            decoration: const InputDecoration(
              hintText: '按名称过滤',
              prefixIcon: Icon(Icons.search, size: 18),
              isDense: true,
              border: OutlineInputBorder(),
            ),
          ),
        ),
        const SizedBox(width: 12),
        OutlinedButton.icon(
          onPressed: state.loadingParams ? null : () => state.loadMetas(),
          icon: const Icon(Icons.refresh, size: 16),
          label: const Text('读全部'),
        ),
        const SizedBox(width: 8),
        FilledButton.icon(
          onPressed: () => _confirmCommit(context, state),
          icon: const Icon(Icons.save, size: 16),
          label: const Text('COMMIT 落盘'),
        ),
      ],
    );
  }

  Widget _body(BuildContext context, AppState state) {
    if (state.metas.isEmpty) {
      if (state.loadingParams) {
        return const Padding(
          padding: EdgeInsets.symmetric(vertical: 32),
          child: Center(child: CircularProgressIndicator()),
        );
      }
      return _empty(context, '连接设备后自动加载参数表');
    }

    final query = _searchCtrl.text.trim().toLowerCase();
    final groups = <String, List<ParamMetaDto>>{};
    for (final m in state.metas) {
      if (query.isNotEmpty && !m.name.toLowerCase().contains(query)) continue;
      groups.putIfAbsent(groupKeyOf(m.name), () => []).add(m);
    }
    if (groups.isEmpty) return _empty(context, '没有匹配的参数');

    final entries = groups.entries.toList();
    return LayoutBuilder(
      builder: (context, constraints) {
        if (constraints.maxWidth < 720 || entries.length < 2) {
          return _column(context, state, entries);
        }
        final half = (entries.length + 1) ~/ 2;
        return Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Expanded(child: _column(context, state, entries.sublist(0, half))),
            const SizedBox(width: 28),
            Expanded(child: _column(context, state, entries.sublist(half))),
          ],
        );
      },
    );
  }

  Widget _empty(BuildContext context, String message) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 32),
      child: Center(
        child: Text(message, style: scale.body.copyWith(color: palette.fgMute)),
      ),
    );
  }

  Widget _column(
    BuildContext context,
    AppState state,
    List<MapEntry<String, List<ParamMetaDto>>> groups,
  ) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        for (final group in groups) ...[
          _group(context, state, group.key, group.value),
          const SizedBox(height: 20),
        ],
      ],
    );
  }

  Widget _group(
    BuildContext context,
    AppState state,
    String key,
    List<ParamMetaDto> metas,
  ) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        Row(
          crossAxisAlignment: CrossAxisAlignment.baseline,
          textBaseline: TextBaseline.alphabetic,
          children: <Widget>[
            Expanded(
              child: Text(
                groupLabelOf(key),
                style: scale.section.copyWith(fontSize: 11),
                overflow: TextOverflow.ellipsis,
              ),
            ),
            Text('${metas.length} 项', style: scale.label),
          ],
        ),
        const SizedBox(height: 6),
        Container(height: palette.lineWidth, color: palette.line),
        for (final meta in metas) _row(context, state, meta),
      ],
    );
  }

  Widget _row(BuildContext context, AppState state, ParamMetaDto meta) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);

    final dirty = state.dirty.contains(meta.name);
    final value = state.values[meta.name];
    final shown = value == null ? '—' : formatValue(value.ty, value.raw);

    final valueBase = scale.data.copyWith(
      fontSize: 12.5,
      color: palette.fgStrong,
    );
    final valueStyle = dirty
        ? GuiGlowStyles.status(palette.stWarn, valueBase)
        : GuiGlowStyles.strong(valueBase);

    final range = '${_num(meta.min)}..${_num(meta.max)}';
    final reboot = meta.apply == 1;

    return InkWell(
      onTap: () => _openEditor(context, state, meta),
      child: Container(
        decoration: BoxDecoration(
          border: Border(
            bottom: BorderSide(color: palette.lineDim, width: palette.lineWidth),
          ),
        ),
        padding: const EdgeInsets.symmetric(vertical: 5),
        child: Row(
          children: <Widget>[
            SizedBox(
              width: 10,
              child: dirty
                  ? Text(
                      '•',
                      key: Key('param_dirty_${meta.name}'),
                      style: GuiGlowStyles.status(palette.stWarn, valueBase),
                    )
                  : null,
            ),
            Expanded(
              child: Text(
                meta.name,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: scale.data.copyWith(
                  fontSize: 11.5,
                  color: dirty ? palette.stWarn : palette.fg,
                ),
              ),
            ),
            const SizedBox(width: 8),
            SizedBox(
              width: 34,
              child: Text(typeName(meta.ty), style: scale.label),
            ),
            const SizedBox(width: 8),
            SizedBox(
              width: 84,
              child: Text(
                range,
                textAlign: TextAlign.right,
                style: scale.label,
              ),
            ),
            const SizedBox(width: 8),
            _ApplyBadge(
              label: reboot ? 'REBOOT' : 'LIVE',
              color: reboot ? palette.accent2 : palette.accent,
            ),
            const SizedBox(width: 10),
            SizedBox(
              width: 80,
              child: Text(
                shown,
                textAlign: TextAlign.right,
                maxLines: 1,
                style: valueStyle,
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _interlock(BuildContext context, AppState state) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final allowed = state.commitAllowed;
    final color = allowed ? palette.stNormal : palette.stWarn;
    final text = allowed
        ? '✓ 可以落盘'
        : '• 落盘需停机且无故障锁存——当前 ${state.motorRunning ? 'Run' : 'Stop'}，'
              '锁存 ${state.latchedCount} 项，COMMIT 将被拒绝';
    return Text(
      text,
      style: GuiGlowStyles.status(color, scale.body.copyWith(fontSize: 12)),
    );
  }

  Future<void> _openEditor(
    BuildContext context,
    AppState state,
    ParamMetaDto meta,
  ) async {
    await showDialog<void>(
      context: context,
      builder: (_) => _ParamEditDialog(state: state, meta: meta),
    );
  }

  Future<void> _confirmCommit(BuildContext context, AppState state) async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (dialogContext) => AlertDialog(
        title: const Text('COMMIT 落盘'),
        content: const Text('将参数写入设备 flash。须在停机且无故障锁存时执行。'),
        actions: <Widget>[
          TextButton(
            onPressed: () => Navigator.pop(dialogContext, false),
            child: const Text('取消'),
          ),
          FilledButton(
            onPressed: () => Navigator.pop(dialogContext, true),
            child: const Text('确认写入'),
          ),
        ],
      ),
    );
    if (confirmed != true || !context.mounted) return;
    final messenger = ScaffoldMessenger.of(context);
    final ok = await state.commit();
    messenger.showSnackBar(
      SnackBar(content: Text(ok ? '参数已落盘' : '落盘失败：${_faultText(state)}')),
    );
  }
}

/// A small square LIVE/REBOOT chip marking when a parameter takes effect.
class _ApplyBadge extends StatelessWidget {
  const _ApplyBadge({required this.label, required this.color});

  final String label;
  final Color color;

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
        style: scale.label.copyWith(color: color),
      ),
    );
  }
}

/// Maps a bridge failure to the user-facing message for the last error.
String _faultText(AppState s) {
  switch (s.lastErrorCode) {
    case 4:
      return '设备联锁拒绝：先 STOP / FAULT_CLEAR';
    case 6:
      return '设备判定越界';
    default:
      return s.lastError;
  }
}

/// Per-parameter edit dialog with type-aware parsing and range validation.
class _ParamEditDialog extends StatefulWidget {
  const _ParamEditDialog({required this.state, required this.meta});

  final AppState state;
  final ParamMetaDto meta;

  @override
  State<_ParamEditDialog> createState() => _ParamEditDialogState();
}

class _ParamEditDialogState extends State<_ParamEditDialog> {
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
    0 => const TextInputType.numberWithOptions(
      decimal: true,
      signed: true,
    ),
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

    if (meta.min <= meta.max &&
        (numeric < meta.min || numeric > meta.max)) {
      setState(
        () => _error =
            '超出范围 ${_num(meta.min)}..${_num(meta.max)} ${meta.unit}'.trimRight(),
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
      _error = _faultText(widget.state);
    });
  }

  @override
  Widget build(BuildContext context) {
    final meta = widget.meta;
    return AlertDialog(
      title: Text(meta.name),
      content: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Text(
            '${typeName(meta.ty)} · ${_num(meta.min)}..${_num(meta.max)} ${meta.unit}'
                .trimRight(),
            style: Theme.of(context).textTheme.bodySmall,
          ),
          const SizedBox(height: 8),
          TextField(
            controller: _valueCtrl,
            keyboardType: _keyboard,
            enabled: !_busy,
            autofocus: true,
            decoration: const InputDecoration(
              labelText: '新值',
              border: OutlineInputBorder(),
            ),
          ),
          if (_error.isNotEmpty)
            Padding(
              padding: const EdgeInsets.only(top: 12),
              child: Text(
                _error,
                style: TextStyle(color: Theme.of(context).colorScheme.error),
              ),
            ),
        ],
      ),
      actions: <Widget>[
        TextButton(
          onPressed: _busy ? null : () => Navigator.pop(context),
          child: const Text('取消'),
        ),
        FilledButton(
          onPressed: _busy ? null : _submit,
          child: const Text('写入'),
        ),
      ],
    );
  }
}
