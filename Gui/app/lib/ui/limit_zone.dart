import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/glow_value.dart';
import '../widgets/section_title.dart';
import '../widgets/stable_digits.dart';
import '../widgets/vfd_bar_painter.dart';
import 'label_style.dart';
import 'panel_title.dart';
import 'status_zone.dart' show HeatLoadBar;

/// The 门限 (limits) view of the 示波 category: the six relative-threshold
/// loudness meters (NTC / U / V / W / VBUS / 模组) beside the measured
/// electrical frequency.
///
/// The three phase currents no longer live here: their I_U / I_V / I_W boxes
/// moved into the trend chart's readout area (`TrendChart`), where they sit
/// below the SPAN control bar next to the waveform they summarise. Only the
/// F_ELEC readout stays in place.
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
            trailing: Text('相对门限响度条 · 电频率', style: scale.label),
          ),
          const SizedBox(height: 14),
          Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              Expanded(child: _HeatLoadPanel(state: state)),
              const SizedBox(width: 34),
              Expanded(child: _FrequencyPanel(state: state)),
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
  const _HeatLoadPanel({required this.state});

  final AppState state;

  @override
  Widget build(BuildContext context) {
    final status = state.status;
    return PanelBox(
      frameKey: const Key('limit_heat_panel'),
      title: '热与负载 · 相对门限',
      en: '%',
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          for (final bar in _bars(status)) ...<Widget>[
            bar,
            const SizedBox(height: 10),
          ],
        ],
      ),
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

/// The measured electrical frequency (F_ELEC).
///
/// The three phase currents that used to share this panel now live in the
/// trend chart's readout area; only the F_ELEC row stays in place here
/// (`SETPOINT` footnote, 2 decimals Hz). A missing value renders `—`, never a
/// fabricated `0` (design-system §6 rule 2).
class _FrequencyPanel extends StatelessWidget {
  const _FrequencyPanel({required this.state});

  final AppState state;

  @override
  Widget build(BuildContext context) {
    final status = state.status;
    return PanelBox(
      frameKey: const Key('limit_freq_panel'),
      title: '电频率',
      en: 'ELECTRICAL FREQ',
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          _valueRow(
            context,
            name: 'F_ELEC',
            value: status?.freqHz,
            unit: 'Hz',
            precision: 2,
            note: 'SETPOINT',
          ),
        ],
      ),
    );
  }

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
          // Monitor readout: sample at 5 Hz so it does not flicker at 120 Hz.
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
