import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/glow_value.dart';
import '../widgets/hairline_frame.dart';
import '../widgets/section_title.dart';
import '../widgets/stable_digits.dart';
import '../widgets/vfd_bar_painter.dart';
import 'label_style.dart';
import 'panel_title.dart';
import 'status_zone.dart' show HeatLoadBar;

/// The 门限 (limits) view of the 示波 category: the six relative-threshold
/// loudness meters (NTC / U / V / W / VBUS / 模组) beside the three phase
/// currents and the measured electrical frequency.
///
/// Split out of the retired `scope_zone.dart` right column so the scope
/// category can tab between the waveform and its limits.
class LimitZone extends StatelessWidget {
  const LimitZone({super.key, required this.state});

  final AppState state;

  @override
  Widget build(BuildContext context) {
    final scale = GuiTypeScale.of(context);
    return ListenableBuilder(
      listenable: state,
      builder: (context, _) => Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          SectionTitle(
            title: '门限',
            en: 'Limits',
            trailing: Text('相对门限响度条 · 相电流 RMS', style: scale.label),
          ),
          const SizedBox(height: 14),
          Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              Expanded(
                child: HairlineFrame(
                  padding: const EdgeInsets.all(10),
                  child: _HeatLoadPanel(
                    key: const Key('limit_heat_panel'),
                    state: state,
                  ),
                ),
              ),
              const SizedBox(width: 34),
              Expanded(
                child: HairlineFrame(
                  key: const Key('limit_phase_panel'),
                  padding: const EdgeInsets.all(10),
                  child: _PhaseCurrentPanel(state: state),
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }
}

/// Relative load against a threshold, drawn as an audio loudness meter.
///
/// Six rows (NTC / U / V / W / VBUS / 模组) reuse [HeatLoadBar]: the fill and
/// the right cell take the three-band semantic tone from the ratio, and a row
/// with no threshold (or no value) stays an empty grey bar with an honest `—`
/// — never a fabricated `0 %` (design-system §6 rule 2).
class _HeatLoadPanel extends StatelessWidget {
  const _HeatLoadPanel({super.key, required this.state});

  final AppState state;

  @override
  Widget build(BuildContext context) {
    final status = state.status;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        const PanelTitle(title: '热与负载 · 相对门限', en: '%'),
        const SizedBox(height: 12),
        for (final bar in _bars(status)) ...<Widget>[
          bar,
          const SizedBox(height: 10),
        ],
      ],
    );
  }

  List<Widget> _bars(StatusDto? status) {
    return <Widget>[
      // 显式携带量纲：管色与读数由单位派生（不再依赖行名推断）。
      HeatLoadBar(
        name: 'NTC',
        value: status?.ntcTemp,
        threshold: state.thresholdNtc,
        unit: '℃',
      ),
      HeatLoadBar(
        name: 'U 相',
        value: status?.iU,
        threshold: state.thresholdCurrent,
        unit: 'A',
      ),
      HeatLoadBar(
        name: 'V 相',
        value: status?.iV,
        threshold: state.thresholdCurrent,
        unit: 'A',
      ),
      HeatLoadBar(
        name: 'W 相',
        value: status?.iW,
        threshold: state.thresholdCurrent,
        unit: 'A',
      ),
      HeatLoadBar(
        name: 'VBUS',
        value: status?.vbus,
        threshold: state.thresholdVbus,
        unit: 'V',
      ),
      const HeatLoadBar(name: '模组', value: null, threshold: null, unit: ''),
    ];
  }
}

/// The three phase currents (moved here from the 状态监视 tiles) plus the
/// electrical frequency.
///
/// Each phase row shows its channel name in the matching series colour, the RMS
/// reading in A (3 decimals, neutral tone) and an `RMS` footnote; the measured
/// F_ELEC follows as a fourth row (`SETPOINT` footnote, 2 decimals Hz). Missing
/// values render `—`, never a fabricated `0` (design-system §6 rule 2).
class _PhaseCurrentPanel extends StatelessWidget {
  const _PhaseCurrentPanel({required this.state});

  final AppState state;

  @override
  Widget build(BuildContext context) {
    final status = state.status;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        const PanelTitle(title: '相电流', en: 'PHASE CURRENTS'),
        const SizedBox(height: 12),
        _phaseRow(context, name: 'I_U', value: status?.iU),
        _hairline(context),
        _phaseRow(context, name: 'I_V', value: status?.iV),
        _hairline(context),
        _phaseRow(context, name: 'I_W', value: status?.iW),
        _hairline(context),
        _valueRow(
          context,
          name: 'F_ELEC',
          value: status?.freqHz,
          unit: 'Hz',
          precision: 2,
          note: 'SETPOINT',
        ),
      ],
    );
  }

  Widget _hairline(BuildContext context) {
    final palette = GuiPalette.of(context);
    // 行分割归一：低亮青 line（#173E44），不再用更暗的 lineDim。
    return Container(height: palette.lineWidth, color: palette.line);
  }

  Widget _phaseRow(
    BuildContext context, {
    required String name,
    required double? value,
  }) => _valueRow(
    context,
    name: name,
    value: value,
    unit: 'A',
    precision: 3,
    note: 'RMS',
  );

  /// One aligned readout row: cyan channel name + dimension-tinted value + note.
  Widget _valueRow(
    BuildContext context, {
    required String name,
    required double? value,
    required String unit,
    required int precision,
    required String note,
  }) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 6),
      child: Row(
        children: <Widget>[
          Text(name, style: guiLabelStyle(palette, scale)),
          const Spacer(),
          // Monitor readout: sample at 5 Hz so it does not flicker at 25 Hz.
          StableDigits(
            text: value == null ? '—' : value.toStringAsFixed(precision),
            builder: (context) => GlowValue(
              value: value,
              unit: unit,
              precision: precision,
              tone: Tone.neutral,
              tint: tintForUnit(palette, unit),
            ),
          ),
          const SizedBox(width: 8),
          Text(note, style: scale.label),
        ],
      ),
    );
  }
}
