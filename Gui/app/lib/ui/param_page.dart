import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../bridge/frb/api.dart';
import '../state/app_state.dart';

/// Human-readable name of a parameter wire type.
String typeName(int ty) => switch (ty) {
  0 => 'f32',
  1 => 'u32',
  2 => 'i32',
  3 => 'bool',
  _ => 'ty$ty',
};

/// Reinterprets a 32-bit [raw] bit pattern as an IEEE-754 single.
double f32FromRaw(int raw) =>
    (ByteData(4)..setInt32(0, raw, Endian.little)).getFloat32(0);

/// Encodes an IEEE-754 single into its 32-bit [raw] bit pattern.
int f32ToRaw(double value) =>
    (ByteData(4)..setFloat32(0, value, Endian.little)).getInt32(0);

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

/// Metadata-driven parameter panel. The UI is generated entirely from
/// [AppState.metas]; no parameter name is hard-coded here.
class ParamPage extends StatefulWidget {
  const ParamPage({super.key, required this.state});

  final AppState state;

  @override
  State<ParamPage> createState() => _ParamPageState();
}

class _ParamPageState extends State<ParamPage> {
  final TextEditingController _searchCtrl = TextEditingController();

  @override
  void dispose() {
    _searchCtrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final s = widget.state;
    return Padding(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Expanded(
                child: TextField(
                  controller: _searchCtrl,
                  onChanged: (_) => setState(() {}),
                  decoration: const InputDecoration(
                    hintText: '按名称过滤',
                    prefixIcon: Icon(Icons.search),
                    isDense: true,
                    border: OutlineInputBorder(),
                  ),
                ),
              ),
              const SizedBox(width: 12),
              OutlinedButton.icon(
                onPressed: s.loadingParams ? null : () => s.loadMetas(),
                icon: const Icon(Icons.refresh),
                label: const Text('读全部'),
              ),
              const SizedBox(width: 8),
              FilledButton.icon(
                onPressed: () => _confirmCommit(context),
                icon: const Icon(Icons.save),
                label: const Text('COMMIT 落盘'),
              ),
            ],
          ),
          const SizedBox(height: 16),
          Expanded(child: ListenableBuilder(
            listenable: s,
            builder: (context, _) => _buildBody(context, s),
          )),
        ],
      ),
    );
  }

  Widget _buildBody(BuildContext context, AppState s) {
    if (s.metas.isEmpty) {
      if (s.loadingParams) {
        return const Center(child: CircularProgressIndicator());
      }
      return const Center(child: Text('连接设备后自动加载参数表'));
    }

    final query = _searchCtrl.text.trim().toLowerCase();
    final groups = <String, List<ParamMetaDto>>{};
    for (final m in s.metas) {
      if (query.isNotEmpty && !m.name.toLowerCase().contains(query)) continue;
      final dot = m.name.indexOf('.');
      final domain = dot > 0 ? m.name.substring(0, dot) : m.name;
      groups.putIfAbsent(domain, () => []).add(m);
    }

    if (groups.isEmpty) {
      return const Center(child: Text('没有匹配的参数'));
    }

    return ListView(
      children: [
        for (final entry in groups.entries)
          ExpansionTile(
            key: PageStorageKey<String>(entry.key),
            initiallyExpanded: true,
            title: Text(entry.key, style: Theme.of(context).textTheme.titleMedium),
            children: [
              for (final m in entry.value) _paramTile(context, s, m),
            ],
          ),
      ],
    );
  }

  Widget _paramTile(BuildContext context, AppState s, ParamMetaDto m) {
    final dirty = s.dirty.contains(m.name);
    final value = s.values[m.name];
    final shown = value == null ? '--' : formatValue(value.ty, value.raw);
    final applyLabel = m.apply == 1 ? 'reboot' : 'live';

    return ListTile(
      dense: true,
      title: Text(
        m.name,
        style: dirty ? const TextStyle(fontWeight: FontWeight.bold) : null,
      ),
      subtitle: Text(
        '${typeName(m.ty)} · ${_num(m.min)}..${_num(m.max)} ${m.unit}'.trimRight(),
      ),
      trailing: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          if (dirty)
            const Padding(
              padding: EdgeInsets.only(right: 4),
              child: Icon(Icons.edit, size: 16),
            ),
          Chip(
            label: Text(applyLabel),
            visualDensity: VisualDensity.compact,
          ),
          const SizedBox(width: 8),
          Text(shown, style: Theme.of(context).textTheme.bodyMedium),
        ],
      ),
      onTap: () => _openEditor(context, s, m),
    );
  }

  Future<void> _openEditor(
    BuildContext context,
    AppState s,
    ParamMetaDto meta,
  ) async {
    await showDialog<void>(
      context: context,
      builder: (_) => _ParamEditDialog(state: s, meta: meta),
    );
  }

  Future<void> _confirmCommit(BuildContext context) async {
    final s = widget.state;
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (dialogContext) => AlertDialog(
        title: const Text('COMMIT 落盘'),
        content: const Text('将参数写入设备 flash。须在停机且无故障锁存时执行。'),
        actions: [
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
    final ok = await s.commit();
    messenger.showSnackBar(
      SnackBar(content: Text(ok ? '参数已落盘' : '落盘失败：${_faultText(s)}')),
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
        children: [
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
              child: Text(_error, style: TextStyle(color: Theme.of(context).colorScheme.error)),
            ),
        ],
      ),
      actions: [
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
