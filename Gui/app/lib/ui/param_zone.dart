import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_glow.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/glow_value.dart';
import '../widgets/gui_button.dart';
import '../widgets/hairline_frame.dart';
import '../widgets/param_controls.dart';
import '../widgets/section_title.dart';
import '../widgets/stable_digits.dart';

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
        GuiButton(
          label: '读全部',
          icon: Icons.refresh,
          onPressed: state.loadingParams ? null : () => state.loadMetas(),
        ),
        const SizedBox(width: 8),
        GuiButton(
          label: 'COMMIT 落盘',
          variant: GuiButtonVariant.primary,
          icon: Icons.save,
          onPressed: () => _confirmCommit(context, state),
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
        // 两列阈值按「最长参数名放得下」反推，不拍脑袋：
        // 每行固定列合计约 285px（脏点 10 + 类型 34 + 量程 84 +
        // 生效徽标 ~43 + 数值 80 + 段间距）；最长参数名
        // `hardware.current_sense.a_per_volt`（32 字符）在 13px VT323 下
        // 约 166px，留余量按 200px 计 → 每列需 ~485px，
        // 两列加 28px 栏距 ≈ 1000px 内容宽。
        // 阈值 720 时每列仅 ~390px、参数名只剩 ~105px，长名一律被截断。
        if (constraints.maxWidth < 1000 || entries.length < 2) {
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
    // v3: every parameter domain (MOTOR / HARDWARE / SOFTWARE …) is one bright
    // hairline panel, matching the reference's dense framed blocks.
    return HairlineFrame(
      key: Key('param_group_$key'),
      padding: const EdgeInsets.all(10),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          Row(
            crossAxisAlignment: CrossAxisAlignment.baseline,
            textBaseline: TextBaseline.alphabetic,
            children: <Widget>[
              Expanded(
                child: Text(
                  groupLabelOf(key),
                  // 域色：motor 绿 / hardware 青 / software 琥珀 ——
                  // 长表滚动时一眼看出读到哪一域（域名字仍在，颜色只是强化）。
                  style: scale.section.copyWith(
                    fontSize: 11,
                    color: domainColor(palette, key),
                  ),
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
      ),
    );
  }

  Widget _row(BuildContext context, AppState state, ParamMetaDto meta) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);

    final dirty = state.dirty.contains(meta.name);
    final value = state.values[meta.name];
    final shown = value == null ? '—' : formatValue(value.ty, value.raw);

    // v3 tone mapping: a parameter value is flat amber; a local edit not yet
    // flushed takes the warning hue plus the dirty dot below. Float values
    // render through `GlowValue(tone:)`; integer / bool values keep the plain
    // tabular text (GlowValue is a measured-value widget, not a decoder).
    final valueBase = scale.data.copyWith(fontSize: 12.5);
    final Widget valueWidget;
    if (value == null) {
      valueWidget = Text(
        '—',
        maxLines: 1,
        style: valueBase.copyWith(color: palette.fgMute),
      );
    } else if (dirty) {
      valueWidget = Text(
        shown,
        maxLines: 1,
        style: valueBase.copyWith(color: palette.stWarn),
      );
    } else if (meta.ty == 0) {
      valueWidget = GlowValue(
        value: f32FromRaw(value.raw),
        precision: 2,
        tone: Tone.amber,
      );
    } else {
      valueWidget = Text(
        shown,
        maxLines: 1,
        style: valueBase.copyWith(color: palette.valueAmber),
      );
    }

    // Monitor readout: the value column is sampled at 5 Hz. `shown` is the
    // exact string rendered (including `—` / `true` / `false`), so the refresh
    // key and the display never disagree.
    final Widget throttledValue = StableDigits(
      text: shown,
      builder: (context) => valueWidget,
    );

    final range = '${numText(meta.min)}..${numText(meta.max)}';
    final reboot = meta.apply == 1;

    return InkWell(
      onTap: () => openParamEditor(context, state, meta),
      child: Container(
        decoration: BoxDecoration(
          border: Border(
            bottom: BorderSide(
              color: palette.lineDim,
              width: palette.lineWidth,
            ),
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
                  // 参数名放大到 13 —— 它是这一页的主要阅读对象，
                  // 11.5 在长表里偏小。VT323 字宽窄（约 0.48em），
                  // 加宽后长名（如 hardware.current_sense.a_per_volt）仍放得下。
                  fontSize: 13,
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
            ApplyBadge(
              label: reboot ? 'REBOOT' : 'LIVE',
              color: reboot ? palette.accent2 : palette.accent,
            ),
            const SizedBox(width: 10),
            SizedBox(
              width: 80,
              child: FittedBox(
                fit: BoxFit.scaleDown,
                alignment: Alignment.centerRight,
                child: throttledValue,
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
    // v3 tone mapping: a refused flush is flat amber (▲ prefix), an accepted
    // one flat green; the icon keeps the meaning off colour alone.
    final color = GlowValue.colorFor(
      palette,
      allowed ? Tone.green : Tone.amber,
    );
    final text = allowed
        ? '✓ 可以落盘'
        : '▲ 落盘需停机且无故障锁存——当前 ${state.motorRunning ? 'Run' : 'Stop'}，'
              '锁存 ${state.latchedCount} 项，COMMIT 将被拒绝';
    return Text(text, style: scale.body.copyWith(fontSize: 12, color: color));
  }

  Future<void> _confirmCommit(BuildContext context, AppState state) async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (dialogContext) => AlertDialog(
        title: const Text('COMMIT 落盘'),
        content: const Text('将参数写入设备 flash。须在停机且无故障锁存时执行。'),
        actions: <Widget>[
          GuiButton(
            label: '取消',
            onPressed: () => Navigator.pop(dialogContext, false),
          ),
          GuiButton(
            label: '确认写入',
            variant: GuiButtonVariant.primary,
            onPressed: () => Navigator.pop(dialogContext, true),
          ),
        ],
      ),
    );
    if (confirmed != true || !context.mounted) return;
    final messenger = ScaffoldMessenger.of(context);
    final ok = await state.commit();
    messenger.showSnackBar(
      SnackBar(content: Text(ok ? '参数已落盘' : '落盘失败：${faultText(state)}')),
    );
  }
}
