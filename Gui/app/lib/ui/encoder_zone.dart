import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/encoder_schematic.dart';
import '../widgets/glow_value.dart';
import '../widgets/hairline_frame.dart';
import '../widgets/section_title.dart';
import '../widgets/stale_gate.dart';
import '../widgets/status_badge.dart';

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
          child: Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              Expanded(
                flex: 5,
                child: EncoderSchematic(
                  key: const Key('encoder_schematic'),
                  ratio: state.outputRatio,
                ),
              ),
              const SizedBox(width: 28),
              Expanded(
                flex: 4,
                child: _dataColumn(context, rotorFreshness, outputFreshness),
              ),
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
        const SizedBox(height: 14),
        _encoderRow(
          context,
          key: const Key('encoder_row_output'),
          label: 'ENC O',
          zh: '出轴角',
          value: state.status?.encOutputDeg,
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
        _plainRow(context, key: const Key('encoder_zero'), label: '零点', zh: 'ZERO'),
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
        SizedBox(width: 64, child: Text(label, style: scale.label)),
        Expanded(
          child: Text(
            zh,
            overflow: TextOverflow.ellipsis,
            style: scale.body.copyWith(fontSize: 10, color: palette.fgDim),
          ),
        ),
        StaleGate(
          freshness: freshness,
          fallback: '—',
          live: GlowValue(
            value: value,
            unit: '°',
            precision: 1,
            tone: Tone.neutral,
          ),
        ),
        const SizedBox(width: 12),
        StatusBadge(state: item, label: text),
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
        SizedBox(width: 64, child: Text(label, style: scale.label)),
        Expanded(
          child: Text(
            zh,
            overflow: TextOverflow.ellipsis,
            style: scale.body.copyWith(fontSize: 10, color: palette.fgDim),
          ),
        ),
        GlowValue(value: value, precision: 3, tone: Tone.neutral),
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
        SizedBox(width: 64, child: Text(label, style: scale.label)),
        Expanded(
          child: Text(
            zh,
            overflow: TextOverflow.ellipsis,
            style: scale.body.copyWith(fontSize: 10, color: palette.fgDim),
          ),
        ),
        Text(
          value ?? '—',
          style: scale.label.copyWith(
            color: value == null ? palette.fgMute : palette.fg,
          ),
        ),
      ],
    );
  }

  Widget _divider(BuildContext context) {
    final palette = GuiPalette.of(context);
    return Container(height: palette.lineWidth, color: palette.lineDim);
  }

  /// Triple-encoded status badge for an encoder [freshness].
  (ItemState, String) _badgeFor(Freshness freshness) => switch (freshness) {
    Freshness.live => (ItemState.normal, '正常'),
    Freshness.stale => (ItemState.stale, '失效'),
    Freshness.unknown => (ItemState.pending, '未读'),
  };
}
