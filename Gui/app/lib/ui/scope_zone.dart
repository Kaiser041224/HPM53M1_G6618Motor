import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_type.dart';
import '../widgets/section_title.dart';
import '../widgets/trend_chart.dart';

/// The 波形 (waveform) view of the 示波 category: the live 1 kHz trend chart
/// with its built-in control bar (log SPAN slider, one-tap presets, `− + LIVE`
/// zoom group, cursor readout).
///
/// The heat/load threshold meters and the phase-current readouts that used to
/// share this zone's right column moved to `limit_zone.dart` ([LimitZone]); the
/// motor commands and fault composition moved to `control_zone.dart`.
///
/// The first-class `STREAM_SUB` stream is still reserved — this is the 1 kHz
/// watch preview — so the caption honestly keeps naming it.
class ScopeZone extends StatelessWidget {
  const ScopeZone({super.key, required this.state});

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
            title: '波形',
            en: 'Waveform',
            trailing: Text(
              '一级流预留 · STREAM_SUB 返回 UNSUPPORTED',
              style: scale.label,
            ),
          ),
          const SizedBox(height: 14),
          TrendChart(
            key: const Key('scope_curve_frame'),
            history: state.chartHistory,
            freshness: state.freshnessOf(AppState.statusTileKey('I_U')),
          ),
        ],
      ),
    );
  }
}
