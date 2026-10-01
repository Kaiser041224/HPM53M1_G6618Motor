import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_glow.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/glow_value.dart';
import '../widgets/gui_button.dart';
import '../widgets/gui_text_field.dart';
import '../widgets/hairline_frame.dart';
import '../widgets/param_controls.dart';
import '../widgets/section_title.dart';
import '../widgets/stable_digits.dart';
import '../widgets/vfd_bar_painter.dart';
import 'toast_overlay.dart';

/// Metadata-driven parameter panel (design-system §4.1 参数区).
///
/// The entire table — groups, rows, types, ranges, apply badges and values —
/// is generated from [AppState.metas]; no parameter name is hard-coded here.
/// Values are tabular and right-aligned; a locally staged edit shows
/// `旧值 → 新值` (baseline struck through, staged value in the pending hue)
/// instead of touching the device. The write button carries the live interlock
/// directly beneath it: amber when the drive is running or a fault is latched,
/// green when a write would be accepted.
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
        ],
      ),
    );
  }

  Widget _header(BuildContext context, AppState state) {
    final scale = GuiTypeScale.of(context);
    return SectionTitle(
      title: '参数',
      en: 'Parameters · 元数据驱动',
      hue: TitleHue.violet,
      trailing: Text(
        '${state.metas.length} 项 · ${state.staged.length} 项待写入',
        style: scale.label,
      ),
    );
  }

  Widget _toolbar(BuildContext context, AppState state) {
    return Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        Expanded(
          child: GuiTextField(
            controller: _searchCtrl,
            onChanged: (_) => setState(() {}),
            hintText: '按名称过滤',
            prefixIcon: Icons.search,
          ),
        ),
        const SizedBox(width: 12),
        GuiButton(
          label: '读取',
          icon: Icons.refresh,
          onPressed: state.loadingParams ? null : () => _onRead(context, state),
        ),
        const SizedBox(width: 8),
        // 写入按钮与其联锁提示同属一组：提示紧贴按钮正下方，随按钮一起移动。
        Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.end,
          children: <Widget>[
            GuiButton(
              label: '写入',
              variant: GuiButtonVariant.primary,
              icon: Icons.save,
              onPressed: state.staged.isEmpty
                  ? null
                  : () => _onWrite(context, state),
            ),
            const SizedBox(height: 6),
            _interlock(context, state),
          ],
        ),
        const SizedBox(width: 8),
        GuiButton(
          label: '恢复出厂',
          icon: Icons.settings_backup_restore,
          onPressed: state.loadingParams
              ? null
              : () => _onResetFactory(context, state),
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
        // 每行固定列合计约 366px（类型 34 + 量程 84 + 生效徽标 ~66 +
        // 数值 140 + 段间距）；参数表统一 15px 后，最长参数名
        // `hardware.vbus_sense.divider_high_ohm`（37 字符）约需 ~300px
        // → 每列需 ~666px，两列加 28px 栏距 ≈ 1360px 内容宽。
        // 阈值 1360 保证每列参数名不被省略号截断（数值列加宽以容
        // `旧值 → 新值` 对比，故阈值随之上调）。
        if (constraints.maxWidth < 1360 || entries.length < 2) {
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
          // 标题 / 组线归一：低亮紫 groupLine（§2.6.2）。
          Container(height: palette.lineWidth, color: palette.groupLine),
          for (final meta in metas) _row(context, state, meta),
        ],
      ),
    );
  }

  Widget _row(BuildContext context, AppState state, ParamMetaDto meta) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);

    final stagedRaw = state.staged[meta.name];
    final isStaged = stagedRaw != null;
    final value = state.values[meta.name];
    final shown = value == null ? '—' : formatValue(value.ty, value.raw);

    // v5 tone mapping: a parameter value takes its physical-dimension accent
    // (§2.6.3; unitless → neutral white-cyan). A locally staged edit renders as
    // `旧值 → 新值`: the last-read baseline struck through in fgDim, a mute
    // arrow, then the staged value in the pending hue. Float values render
    // through `GlowValue(tint:)`; integer / bool values keep the plain tabular
    // text (GlowValue is a measured-value widget, not a decoder).
    final tint = tintForUnit(palette, meta.unit);
    final valueBase = scale.data.copyWith(fontSize: kParamRowFontSize);
    final Widget throttledValue;
    if (isStaged) {
      throttledValue = Row(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.center,
        children: <Widget>[
          _strikeThrough(
            shown,
            valueBase.copyWith(color: palette.fgDim),
            palette,
            lineKey: Key('param_strike_${meta.name}'),
          ),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 5),
            child: Text(
              '→',
              maxLines: 1,
              style: valueBase.copyWith(color: palette.fgMute),
            ),
          ),
          Text(
            formatValue(meta.ty, stagedRaw),
            key: Key('param_staged_${meta.name}'),
            maxLines: 1,
            style: GuiGlowStyles.status(palette.stPending, valueBase),
          ),
        ],
      );
    } else {
      final Widget plain;
      if (value == null) {
        plain = Text(
          '—',
          maxLines: 1,
          style: valueBase.copyWith(color: palette.fgMute),
        );
      } else if (meta.ty == 0) {
        plain = GlowValue(
          value: f32FromRaw(value.raw),
          precision: 2,
          tint: tint,
          fontSize: kParamRowFontSize,
        );
      } else {
        plain = Text(
          shown,
          maxLines: 1,
          style: valueBase.copyWith(color: tint),
        );
      }
      // Monitor readout: the value column is sampled at 5 Hz. `shown` is the
      // exact string rendered (including `—` / `true` / `false`), so the refresh
      // key and the display never disagree. A staged row is local, not live, so
      // it bypasses the throttle.
      throttledValue = StableDigits(text: shown, builder: (context) => plain);
    }

    final range = '${numText(meta.min)}..${numText(meta.max)}';
    final reboot = meta.apply == 1;

    return InkWell(
      onTap: () => openParamEditor(context, state, meta),
      child: Container(
        decoration: BoxDecoration(
          // 行分割归一：低亮青 line（#173E44）。
          border: Border(
            bottom: BorderSide(color: palette.line, width: palette.lineWidth),
          ),
        ),
        padding: const EdgeInsets.symmetric(vertical: 5),
        child: Row(
          children: <Widget>[
            Expanded(
              child: Text(
                meta.name,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                // 参数名统一青色 + 弱青辉光（§2.6.3）；暂存状态只体现在值列。
                style: GuiGlowStyles.weak(
                  scale.data.copyWith(
                    fontSize: kParamRowFontSize,
                    color: palette.accent,
                  ),
                ),
              ),
            ),
            const SizedBox(width: 8),
            SizedBox(
              width: 34,
              child: Text(
                typeName(meta.ty),
                style: scale.label.copyWith(fontSize: kParamRowFontSize),
              ),
            ),
            const SizedBox(width: 8),
            SizedBox(
              width: 84,
              child: Text(
                range,
                textAlign: TextAlign.right,
                style: scale.label.copyWith(fontSize: kParamRowFontSize),
              ),
            ),
            const SizedBox(width: 8),
            ApplyBadge(
              label: reboot ? 'REBOOT' : 'LIVE',
              color: reboot ? palette.accent2 : palette.accent,
              fontSize: kParamRowFontSize,
            ),
            const SizedBox(width: 10),
            SizedBox(
              width: 140,
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

  /// Strikes [text] through with a 1px [GuiPalette.fgDim] line. The project
  /// bans `TextDecoration` emphasis (it can render as a second glyph-baseline
  /// line), so the strike is drawn as a [Container] line centred over the text.
  Widget _strikeThrough(
    String text,
    TextStyle style,
    GuiPalette palette, {
    Key? lineKey,
  }) {
    return IntrinsicWidth(
      child: Stack(
        alignment: Alignment.center,
        children: <Widget>[
          Text(text, maxLines: 1, style: style),
          Container(
            key: lineKey,
            height: palette.lineWidth,
            width: double.infinity,
            color: palette.fgDim,
          ),
        ],
      ),
    );
  }

  Widget _interlock(BuildContext context, AppState state) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final allowed = state.commitAllowed;
    // v3 tone mapping: a refused write is flat amber (▲ prefix), an accepted
    // one flat green; the icon keeps the meaning off colour alone.
    final color = GlowValue.colorFor(
      palette,
      allowed ? Tone.green : Tone.amber,
    );
    final text = allowed
        ? '✓ 可以写入'
        : '▲ 写入需停机且无故障锁存——当前 ${state.motorRunning ? 'Run' : 'Stop'}，'
              '锁存 ${state.latchedCount} 项，COMMIT 将被拒绝';
    return ConstrainedBox(
      constraints: const BoxConstraints(maxWidth: 360),
      child: Text(
        text,
        textAlign: TextAlign.right,
        style: scale.body.copyWith(fontSize: 12, color: color),
      ),
    );
  }

  /// Re-reads the device table; when edits are staged, confirms first that they
  /// will be discarded.
  Future<void> _onRead(BuildContext context, AppState state) async {
    if (state.staged.isNotEmpty) {
      final confirmed = await showDialog<bool>(
        context: context,
        builder: (dialogContext) => AlertDialog(
          title: const Text('读取设备参数'),
          content: const Text('暂存改动将被丢弃，继续？'),
          actions: <Widget>[
            GuiButton(
              label: '取消',
              onPressed: () => Navigator.pop(dialogContext, false),
            ),
            GuiButton(
              label: '继续',
              variant: GuiButtonVariant.primary,
              onPressed: () => Navigator.pop(dialogContext, true),
            ),
          ],
        ),
      );
      if (confirmed != true || !context.mounted) return;
    }
    final overlay = Overlay.maybeOf(context);
    final hadStaged = state.staged.isNotEmpty;
    await state.readAll();
    if (overlay == null) return;
    showGuiToastIn(
      overlay,
      hadStaged ? '已读取设备参数，暂存改动已丢弃' : '已读取设备参数',
      tone: ToastTone.info,
    );
  }

  /// Sends every staged edit (`PARAM_WRITE`) then persists them in one
  /// `PARAM_COMMIT`, reporting the count written (or the device fault).
  Future<void> _onWrite(BuildContext context, AppState state) async {
    final count = state.staged.length;
    final overlay = Overlay.maybeOf(context);
    final ok = await state.writeStaged();
    if (overlay == null) return;
    showGuiToastIn(
      overlay,
      ok ? '已写入 $count 项' : '写入失败：${faultText(state)}',
      tone: ok ? ToastTone.success : ToastTone.error,
    );
  }

  /// Restores every parameter to its factory default (and persists it on the
  /// device), after a confirmation.
  Future<void> _onResetFactory(BuildContext context, AppState state) async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (dialogContext) => AlertDialog(
        title: const Text('恢复出厂参数'),
        content: const Text('将所有参数恢复出厂值并写入设备。'),
        actions: <Widget>[
          GuiButton(
            label: '取消',
            onPressed: () => Navigator.pop(dialogContext, false),
          ),
          GuiButton(
            label: '确认恢复',
            variant: GuiButtonVariant.primary,
            onPressed: () => Navigator.pop(dialogContext, true),
          ),
        ],
      ),
    );
    if (confirmed != true || !context.mounted) return;
    final overlay = Overlay.maybeOf(context);
    final ok = await state.resetFactory();
    if (overlay == null) return;
    showGuiToastIn(
      overlay,
      ok ? '已恢复出厂参数' : '恢复失败：${faultText(state)}',
      tone: ok ? ToastTone.success : ToastTone.error,
    );
  }
}
