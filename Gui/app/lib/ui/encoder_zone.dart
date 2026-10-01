import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/encoder_schematic.dart';
import '../widgets/glow_value.dart';
import '../widgets/hairline_frame.dart';
import '../widgets/section_title.dart';
import '../widgets/stable_digits.dart';
import '../widgets/stale_gate.dart';
import '../widgets/status_badge.dart';
import '../widgets/vfd_bar_painter.dart';
import 'label_style.dart';

/// The 双编码器 zone: a thin-line schematic beside the two encoder readings.
///
/// The rotor (`ENC R`) and output-shaft (`ENC O`) angles are shown with a
/// [StatusBadge] and gated by their own [Freshness] — a stale or never-read
/// angle degrades to `—` rather than passing an old reading off as current
/// (design-system §6 rule 2). The gear ratios and the resolution width come
/// from the parameter table; a mechanical constant the protocol does not expose
/// (zero point) stays an honest `—`.
class EncoderZone extends StatelessWidget {
  const EncoderZone({super.key, required this.state});

  /// Observable telemetry + parameter table.
  final AppState state;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: state,
      builder: (context, _) => _build(context),
    );
  }

  /// 齿轮示意图的最大宽度 [logical px] —— 居中限宽，避免大窗口下过度拉伸。
  static const double _schematicMaxWidth = 460;

  Widget _build(BuildContext context) {
    final scale = GuiTypeScale.of(context);
    final status = state.status;
    final Freshness rotorFreshness = status == null
        ? Freshness.unknown
        : state.freshnessOf(AppState.statusTileKey('ENC_R'));
    final Freshness outputFreshness = status == null
        ? Freshness.unknown
        : state.freshnessOf(AppState.statusTileKey('ENC_O'));

    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        SectionTitle(
          title: '双编码器',
          en: 'DUAL ENCODERS',
          trailing: Text('KTH7823 ×2 · 转子 / 出轴', style: scale.label),
        ),
        const SizedBox(height: 14),
        HairlineFrame(
          key: const Key('encoder_frame'),
          padding: const EdgeInsets.all(14),
          // **上下排布**（动画在上、读数在下），不是左右并排。
          //
          // 并排时读数区只分到约 40% 宽（合并页左栏里约 280px），
          // 「标签 + 中文注记 + 数值 + 状态徽标」这一行必然被截断。
          // 改为上下后读数区拿到整幅宽度，文字不再压缩。
          // 动画本身也不需要横向铺满，故居中并限宽。
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: <Widget>[
              Center(
                child: ConstrainedBox(
                  constraints: const BoxConstraints(
                    maxWidth: _schematicMaxWidth,
                  ),
                  child: EncoderSchematic(
                    key: const Key('encoder_schematic'),
                    rotorDeg: rotorFreshness == Freshness.live
                        ? state.status?.encRotorDeg
                        : null,
                    outputDeg: outputFreshness == Freshness.live
                        ? state.status?.encOutputDeg
                        : null,
                    ratioLabel: state.outputRatio == null
                        ? '—'
                        : state.outputRatio!.toStringAsFixed(2),
                  ),
                ),
              ),
              const SizedBox(height: 18),
              _dataColumn(context, rotorFreshness, outputFreshness),
            ],
          ),
        ),
      ],
    );
  }

  Widget _dataColumn(
    BuildContext context,
    Freshness rotorFreshness,
    Freshness outputFreshness,
  ) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        _encoderRow(
          context,
          key: const Key('encoder_row_rotor'),
          label: 'ENC R',
          zh: '转子角',
          value: state.status?.encRotorDeg,
          freshness: rotorFreshness,
        ),
        const SizedBox(height: 8),
        _rpmRow(
          context,
          key: const Key('encoder_rpm_rotor'),
          zh: 'RPM',
          note: '角差分 EST',
          value: state.rpmRotor,
          freshness: rotorFreshness,
        ),
        const SizedBox(height: 14),
        _encoderRow(
          context,
          key: const Key('encoder_row_output'),
          label: 'ENC O',
          zh: '出轴角',
          value: state.status?.encOutputDeg,
          freshness: outputFreshness,
        ),
        const SizedBox(height: 8),
        _rpmRow(
          context,
          key: const Key('encoder_rpm_output'),
          zh: 'RPM',
          note: '磁编 · 角差分 EST',
          value: state.rpmOutput,
          freshness: outputFreshness,
        ),
        const SizedBox(height: 14),
        _divider(context),
        const SizedBox(height: 14),
        _ratioRow(
          context,
          key: const Key('encoder_ratio_rotor'),
          label: '齿比 R',
          zh: '转子 / 转子',
          value: state.rotorRatio,
        ),
        const SizedBox(height: 10),
        _ratioRow(
          context,
          key: const Key('encoder_ratio_output'),
          label: '齿比 O',
          zh: '出轴 / 转子',
          value: state.outputRatio,
        ),
        const SizedBox(height: 14),
        _divider(context),
        const SizedBox(height: 14),
        _plainRow(
          context,
          key: const Key('encoder_zero'),
          label: '零点',
          zh: 'ZERO',
        ),
        const SizedBox(height: 10),
        _plainRow(
          context,
          key: const Key('encoder_resolution'),
          label: '分辨率',
          zh: 'RES',
          value: state.encoderResolutionBits == null
              ? null
              : '${state.encoderResolutionBits} bit',
        ),
      ],
    );
  }

  Widget _encoderRow(
    BuildContext context, {
    required Key key,
    required String label,
    required String zh,
    required double? value,
    required Freshness freshness,
  }) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final (ItemState item, String text) = _badgeFor(freshness);
    return Row(
      key: key,
      crossAxisAlignment: CrossAxisAlignment.center,
      children: <Widget>[
        SizedBox(
          width: 64,
          child: Text(label, style: guiLabelStyle(palette, scale)),
        ),
        Expanded(
          child: Text(
            zh,
            overflow: TextOverflow.ellipsis,
            style: scale.body.copyWith(
              fontSize: GuiTypeScale.pixelSize,
              color: palette.fgDim,
            ),
          ),
        ),
        StaleGate(
          freshness: freshness,
          fallback: '—',
          // Angle readout is a monitor value: sample it at 5 Hz.
          live: StableDigits(
            text: value == null ? '—' : value.toStringAsFixed(1),
            builder: (context) => GlowValue(
              value: value,
              unit: '°',
              precision: 1,
              tint: tintForUnit(palette, '°'),
            ),
          ),
        ),
        const SizedBox(width: 12),
        StatusBadge(state: item, label: text),
      ],
    );
  }

  /// One angular-speed estimate row: a `转速` label, the `RPM` caption, the
  /// value in rpm (1 decimal, neutral) and the **EST** footnote naming how it
  /// was derived. Gated by the same encoder [freshness] as the angle above it,
  /// so a stalled feed degrades the estimate to `—` (design-system §6 rule 2).
  Widget _rpmRow(
    BuildContext context, {
    required Key key,
    required String zh,
    required String note,
    required double? value,
    required Freshness freshness,
  }) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return Row(
      key: key,
      crossAxisAlignment: CrossAxisAlignment.center,
      children: <Widget>[
        SizedBox(
          width: 64,
          child: Text('转速', style: guiLabelStyle(palette, scale)),
        ),
        Expanded(
          child: Text(
            zh,
            overflow: TextOverflow.ellipsis,
            style: scale.body.copyWith(
              fontSize: GuiTypeScale.pixelSize,
              color: palette.fgDim,
            ),
          ),
        ),
        StaleGate(
          freshness: freshness,
          fallback: '—',
          // Speed estimate is a monitor value: sample it at 5 Hz.
          live: StableDigits(
            text: value == null ? '—' : value.toStringAsFixed(1),
            builder: (context) => GlowValue(
              value: value,
              precision: 1,
              tint: tintForUnit(palette, 'RPM'),
            ),
          ),
        ),
        const SizedBox(width: 12),
        // 中文 note 在 12px 下比 10px 宽约 1/5 —— 必须可收缩，
        // 否则窄栏（分栏左列）会被它撑破。
        Flexible(
          child: Text(
            note,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            textAlign: TextAlign.right,
            style: scale.label,
          ),
        ),
      ],
    );
  }

  Widget _ratioRow(
    BuildContext context, {
    required Key key,
    required String label,
    required String zh,
    required double? value,
  }) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return Row(
      key: key,
      crossAxisAlignment: CrossAxisAlignment.center,
      children: <Widget>[
        SizedBox(
          width: 64,
          child: Text(label, style: guiLabelStyle(palette, scale)),
        ),
        Expanded(
          child: Text(
            zh,
            overflow: TextOverflow.ellipsis,
            style: scale.body.copyWith(
              fontSize: GuiTypeScale.pixelSize,
              color: palette.fgDim,
            ),
          ),
        ),
        StableDigits(
          text: value == null ? '—' : value.toStringAsFixed(3),
          builder: (context) => GlowValue(
            value: value,
            precision: 3,
            tint: tintForUnit(palette, ''),
          ),
        ),
      ],
    );
  }

  Widget _plainRow(
    BuildContext context, {
    required Key key,
    required String label,
    required String zh,
    String? value,
  }) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return Row(
      key: key,
      crossAxisAlignment: CrossAxisAlignment.center,
      children: <Widget>[
        SizedBox(
          width: 64,
          child: Text(label, style: guiLabelStyle(palette, scale)),
        ),
        Expanded(
          child: Text(
            zh,
            overflow: TextOverflow.ellipsis,
            style: scale.body.copyWith(
              fontSize: GuiTypeScale.pixelSize,
              color: palette.fgDim,
            ),
          ),
        ),
        // Resolution / zero readouts are monitor values: sample at 5 Hz.
        // Unitless → the neutral white-cyan dimension tint (§2.6.3).
        StableDigits(
          text: value ?? '—',
          style: scale.label.copyWith(
            color: value == null ? palette.fgMute : tintForUnit(palette, 'bit'),
          ),
        ),
      ],
    );
  }

  Widget _divider(BuildContext context) {
    final palette = GuiPalette.of(context);
    // 行分割归一：低亮青 line（#173E44）。
    return Container(height: palette.lineWidth, color: palette.line);
  }

  /// Triple-encoded status badge for an encoder [freshness].
  (ItemState, String) _badgeFor(Freshness freshness) => switch (freshness) {
    Freshness.live => (ItemState.normal, '正常'),
    Freshness.stale => (ItemState.stale, '失效'),
    Freshness.unknown => (ItemState.pending, '未读'),
  };
}
