import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_glow.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/section_title.dart';
import '../widgets/stale_gate.dart';

/// Formats a stale age: whole seconds without decimals, otherwise one.
String _seconds(double value) => value == value.roundToDouble()
    ? value.toStringAsFixed(0)
    : value.toStringAsFixed(1);

/// One status tile: uppercase key + Chinese caption, a dimension-driven
/// tabular value with unit, and a footer note.
///
/// The value carries the `live / stale / unknown` tri-state from the design
/// system (§6 rule 2): stale shows `— <unit>` inside a dashed frame plus a
/// yellow "N 秒未更新" note and no glow, unknown shows a bare `—` with no
/// footer warning at all (never-read is not the same as a failed poll). A real
/// `0` is never confused with the fallback. [warn] colours the value yellow
/// with a same-hue glow and shows `▲` + [warnText], the icon completing the
/// colour + icon + text triple encoding (design-system §2.4/§6).
class StatusTile extends StatelessWidget {
  const StatusTile({
    super.key,
    required this.label,
    required this.zh,
    required this.value,
    required this.unit,
    required this.precision,
    this.note = '',
    this.freshness = Freshness.live,
    this.staleSeconds,
    this.warn = false,
    this.warnText,
    this.valueColor,
  });

  /// Wire key, rendered uppercase (e.g. `VBUS`, `I_U`).
  final String label;

  /// Chinese caption shown dim beside the key.
  final String zh;

  /// The reading, or null when unavailable.
  final num? value;

  /// Unit suffix, rendered uppercase in the label tier.
  final String unit;

  /// Digits after the decimal point, chosen by dimension.
  final int precision;

  /// Footer note when neither stale nor warning (e.g. `RMS`).
  final String note;

  /// Trust level of the reading.
  final Freshness freshness;

  /// Age in seconds, used for the stale note.
  final double? staleSeconds;

  /// Whether the reading is approaching its threshold.
  final bool warn;

  /// Warning note; defaults to `接近门限`.
  final String? warnText;

  /// Optional series colour for the value (e.g. phase tint).
  final Color? valueColor;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);

    final TextStyle valueStyle;
    if (warn) {
      valueStyle = GuiGlowStyles.status(palette.stWarn, scale.data);
    } else if (valueColor != null) {
      valueStyle = GuiGlowStyles.status(valueColor!, scale.data);
    } else {
      valueStyle = GuiGlowStyles.strong(scale.data);
    }

    final valueRow = Row(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.baseline,
      textBaseline: TextBaseline.alphabetic,
      children: <Widget>[
        Text(
          value == null ? '—' : value!.toStringAsFixed(precision),
          style: valueStyle,
        ),
        const SizedBox(width: 4),
        Text(unit.toUpperCase(), style: GuiGlowStyles.weak(scale.label)),
      ],
    );

    return Container(
      color: palette.bgBase,
      constraints: const BoxConstraints(minHeight: 84),
      padding: const EdgeInsets.fromLTRB(14, 12, 14, 12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        children: <Widget>[
          Row(
            crossAxisAlignment: CrossAxisAlignment.baseline,
            textBaseline: TextBaseline.alphabetic,
            children: <Widget>[
              Text(label.toUpperCase(), style: scale.label),
              const SizedBox(width: 6),
              Flexible(
                child: Text(
                  zh,
                  overflow: TextOverflow.ellipsis,
                  style: scale.body.copyWith(
                    fontSize: 10,
                    color: palette.fgDim,
                  ),
                ),
              ),
            ],
          ),
          StaleGate(
            freshness: freshness,
            // Stale keeps the unit (PREVIEW: `— A`) so the dimension is not
            // lost; unknown stays a bare `—` with no pretence of a reading.
            fallback: freshness == Freshness.stale && unit.isNotEmpty
                ? '— ${unit.toUpperCase()}'
                : '—',
            live: valueRow,
          ),
          _footer(context, palette, scale),
        ],
      ),
    );
  }

  Widget _footer(
    BuildContext context,
    GuiPalette palette,
    GuiTypeScale scale,
  ) {
    if (freshness == Freshness.stale) {
      final seconds = staleSeconds;
      final text = seconds == null
          ? '⚠ 未更新'
          : '⚠ ${_seconds(seconds)} 秒未更新';
      return Text(
        text,
        style: GuiGlowStyles.status(palette.stWarn, scale.label),
      );
    }
    // Never-read (`unknown`) is not a failure: keep the footer silent so a
    // start-up or disconnected panel does not raise eight false "stale"
    // alarms.
    if (freshness == Freshness.unknown) {
      return const SizedBox.shrink();
    }
    if (warn) {
      return Text(
        '▲ ${warnText ?? '接近门限'}',
        style: GuiGlowStyles.status(palette.stWarn, scale.label),
      );
    }
    return Text(note, style: scale.label);
  }
}

/// Relative load against a threshold, using the LED bar motif.
///
/// The bar is filled by `|value| / threshold` and the right cell shows the
/// percentage. When no threshold (or no value) is available the bar stays
/// empty and shows `—` — never a fabricated `0 %` (design-system §6 rule 2).
class HeatLoadBar extends StatelessWidget {
  const HeatLoadBar({
    super.key,
    required this.name,
    required this.value,
    required this.threshold,
    this.color,
  });

  /// Row label.
  final String name;

  /// The measured quantity, or null.
  final num? value;

  /// The trip threshold, or null when the parameter is unloaded.
  final double? threshold;

  /// Fill colour; defaults to the cyan accent.
  final Color? color;

  /// `|value| / threshold` clamped to `[0, 1]`, or null when undefined.
  double? get ratio {
    final t = threshold;
    final v = value;
    if (t == null || v == null || t <= 0) return null;
    return (v.abs() / t).clamp(0.0, 1.0).toDouble();
  }

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final value01 = ratio;
    final fill = color ?? palette.accent;
    return Row(
      children: <Widget>[
        SizedBox(width: 52, child: Text(name, style: scale.label)),
        const SizedBox(width: 11),
        Expanded(
          child: Container(
            height: 13,
            decoration: BoxDecoration(
              color: palette.bgVoid,
              border: Border.all(color: palette.lineDim),
            ),
            child: value01 == null
                ? null
                : FractionallySizedBox(
                    alignment: Alignment.centerLeft,
                    widthFactor: value01,
                    child: Container(color: fill),
                  ),
          ),
        ),
        const SizedBox(width: 11),
        SizedBox(
          width: 66,
          child: Text(
            value01 == null ? '—' : '${(value01 * 100).round()} %',
            textAlign: TextAlign.right,
            style: value01 == null
                ? scale.label
                : GuiGlowStyles.soft(
                    scale.label.copyWith(color: palette.fgStrong),
                  ),
          ),
        ),
      ],
    );
  }
}

/// A single tile's dimension-driven spec, kept as plain data so the header
/// counters and the widgets are derived from one source.
class _TileSpec {
  const _TileSpec({
    required this.keyName,
    required this.label,
    required this.zh,
    required this.value,
    required this.unit,
    required this.precision,
    required this.note,
    this.threshold,
    this.absThreshold = false,
    this.valueColor,
  });

  final String keyName;
  final String label;
  final String zh;
  final num? value;
  final String unit;
  final int precision;
  final String note;
  final double? threshold;
  final bool absThreshold;
  final Color? valueColor;

  /// Fraction of the threshold reached, or null when undefined.
  double? get load {
    final t = threshold;
    final v = value;
    if (t == null || v == null || t <= 0) return null;
    return (absThreshold ? v.abs() : v) / t;
  }
}

/// The 状态监视 zone: eight tri-state tiles plus the heat/load bars.
///
/// Everything is derived from [AppState]; when no status snapshot has been
/// read the tiles fall back to `—` rather than showing stale numbers
/// (design-system §6 rule 2). Thresholds come from the parameter table via
/// [AppState.thresholdNtc] / [AppState.thresholdVbus] / [AppState
/// .thresholdCurrent]; an unloaded threshold leaves its bar grey.
class StatusZone extends StatelessWidget {
  const StatusZone({super.key, required this.state});

  final AppState state;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: state,
      builder: (context, _) => _build(context),
    );
  }

  Widget _build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final status = state.status;

    final specs = <_TileSpec>[
      _TileSpec(
        keyName: 'VBUS',
        label: 'VBUS',
        zh: '母线电压',
        value: status?.vbus,
        unit: 'V',
        precision: 1,
        note: 'NOMINAL 48',
        threshold: state.thresholdVbus,
      ),
      _TileSpec(
        keyName: 'I_U',
        label: 'I_U',
        zh: 'U 相电流',
        value: status?.iU,
        unit: 'A',
        precision: 3,
        note: 'RMS',
        threshold: state.thresholdCurrent,
        absThreshold: true,
        valueColor: palette.series1,
      ),
      _TileSpec(
        keyName: 'I_V',
        label: 'I_V',
        zh: 'V 相电流',
        value: status?.iV,
        unit: 'A',
        precision: 3,
        note: 'RMS',
        threshold: state.thresholdCurrent,
        absThreshold: true,
        valueColor: palette.series2,
      ),
      _TileSpec(
        keyName: 'I_W',
        label: 'I_W',
        zh: 'W 相电流',
        value: status?.iW,
        unit: 'A',
        precision: 3,
        note: 'RMS',
        threshold: state.thresholdCurrent,
        absThreshold: true,
        valueColor: palette.series3,
      ),
      _TileSpec(
        keyName: 'NTC',
        label: 'NTC',
        zh: '温度',
        value: status?.ntcTemp,
        unit: '℃',
        precision: 1,
        note: 'NOMINAL',
        threshold: state.thresholdNtc,
      ),
      _TileSpec(
        keyName: 'ENC_R',
        label: 'ENC R',
        zh: '转子角',
        value: status?.encRotorDeg,
        unit: '°',
        precision: 1,
        note: 'ROTOR · 正常',
      ),
      _TileSpec(
        keyName: 'ENC_O',
        label: 'ENC O',
        zh: '出轴角',
        value: status?.encOutputDeg,
        unit: '°',
        precision: 1,
        note: 'OUTPUT · 正常',
      ),
      _TileSpec(
        keyName: 'F_ELEC',
        label: 'F_ELEC',
        zh: '电频率',
        value: status?.freqHz,
        unit: 'Hz',
        precision: 2,
        note: 'SETPOINT',
      ),
    ];

    final tiles = <Widget>[];
    var failed = 0;
    var warned = 0;
    for (final spec in specs) {
      final freshness = status == null || spec.value == null
          ? Freshness.unknown
          : Freshness.live;
      final load = spec.load;
      final warn = load != null && load >= 0.9;
      // Only a failed poll counts as 失效; a never-read value is not a fault,
      // so start-up / disconnect does not report `8 项失效` (PREVIEW counts
      // its single stale tile and single warning).
      if (freshness == Freshness.stale) failed++;
      if (warn) warned++;
      tiles.add(
        StatusTile(
          key: Key('status_tile_${spec.keyName}'),
          label: spec.label,
          zh: spec.zh,
          value: spec.value,
          unit: spec.unit,
          precision: spec.precision,
          note: spec.note,
          freshness: freshness,
          warn: warn,
          warnText: warn
              ? '接近门限 ${spec.threshold!.toStringAsFixed(0)}'
              : null,
          valueColor: spec.valueColor,
        ),
      );
    }

    final header = StringBuffer('8 项');
    if (failed > 0) header.write(' · $failed 项失效');
    if (warned > 0) header.write(' · $warned 项告警');

    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        SectionTitle(
          title: '状态监视',
          en: 'Status',
          trailing: Text(header.toString(), style: scale.label),
        ),
        const SizedBox(height: 14),
        _grid(context, tiles),
        const SizedBox(height: 24),
        Text(
          '热与负载 · 相对门限',
          style: scale.section.copyWith(fontSize: 11),
        ),
        const SizedBox(height: 12),
        for (final bar in _heatLoads(context, status)) ...[
          bar,
          const SizedBox(height: 10),
        ],
      ],
    );
  }

  Widget _grid(BuildContext context, List<Widget> cells) {
    final palette = GuiPalette.of(context);
    Widget gap() => Container(width: 1, color: palette.lineDim);
    Widget row(List<Widget> rowCells) => IntrinsicHeight(
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          for (var i = 0; i < rowCells.length; i++) ...<Widget>[
            if (i > 0) gap(),
            Expanded(child: rowCells[i]),
          ],
        ],
      ),
    );
    return Container(
      color: palette.lineDim,
      child: Column(
        children: <Widget>[
          row(cells.sublist(0, 4)),
          Container(height: 1, color: palette.lineDim),
          row(cells.sublist(4, 8)),
        ],
      ),
    );
  }

  List<Widget> _heatLoads(BuildContext context, StatusDto? status) {
    final palette = GuiPalette.of(context);
    return <Widget>[
      HeatLoadBar(
        name: 'NTC',
        value: status?.ntcTemp,
        threshold: state.thresholdNtc,
        color: palette.stWarn,
      ),
      HeatLoadBar(
        name: 'U 相',
        value: status?.iU,
        threshold: state.thresholdCurrent,
        color: palette.series1,
      ),
      HeatLoadBar(
        name: 'V 相',
        value: status?.iV,
        threshold: state.thresholdCurrent,
        color: palette.series2,
      ),
      HeatLoadBar(
        name: 'W 相',
        value: status?.iW,
        threshold: state.thresholdCurrent,
        color: palette.series3,
      ),
      HeatLoadBar(
        name: 'VBUS',
        value: status?.vbus,
        threshold: state.thresholdVbus,
        color: palette.stNormal,
      ),
      const HeatLoadBar(name: '模组', value: null, threshold: null),
    ];
  }
}
