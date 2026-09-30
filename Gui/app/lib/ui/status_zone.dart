import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_glow.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/glow_value.dart';
import '../widgets/hairline_frame.dart';
import '../widgets/section_title.dart';
import '../widgets/stable_digits.dart';
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
/// `0` is never confused with the fallback. [warn] colours the value with the
/// status warning hue and shows `▲` + [warnText], the icon completing the
/// colour + icon + text triple encoding (design-system §2.4/§6). Otherwise the
/// measured value is flat neutral white (v3 tone mapping).
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

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);

    // v3 tone mapping: a measured tile value is flat neutral white and renders
    // through `GlowValue(tone:)` so numeric formatting + unit live in one place.
    // The warn path keeps the same-hue status glow — a status hue is
    // deliberately not one of the value tones, so it is not cross-colorised.
    final Widget valueRow;
    if (warn) {
      valueRow = Row(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.baseline,
        textBaseline: TextBaseline.alphabetic,
        children: <Widget>[
          Text(
            value == null ? '—' : value!.toStringAsFixed(precision),
            style: GuiGlowStyles.status(palette.stWarn, scale.data),
          ),
          const SizedBox(width: 4),
          Text(unit.toUpperCase(), style: GuiGlowStyles.weak(scale.label)),
        ],
      );
    } else {
      valueRow = GlowValue(
        value: value,
        unit: unit,
        precision: precision,
        tone: Tone.neutral,
      );
    }

    // Monitor readout: the tile value is sampled at 5 Hz so it does not
    // flicker under the 25 Hz telemetry feed. Tri-state / precision / tone
    // semantics are unchanged — only the cadence at which the string swaps.
    final Widget throttledValue = StableDigits(
      text: value == null ? '—' : value!.toStringAsFixed(precision),
      builder: (context) => valueRow,
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
            live: throttledValue,
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

/// How long the [HeatLoadBar] peak marker holds before it starts to fall.
const Duration kHeatPeakHold = Duration(milliseconds: 800);

/// Peak fall-back rate, in full-scale fraction per second (loudness-meter
/// peak decay).
const double kHeatPeakDecayPerSecond = 0.35;

/// Tick period for the peak-hold decay — aligned with the 5 Hz UI cadence.
const Duration kHeatPeakTick = kTextRefreshPeriod;

/// The peak-hold marker drawn by [HeatLoadBar].
///
/// Exposed as a public widget so tests can read the held [position] without
/// reaching into the private bar state.
class HeatPeakLine extends StatelessWidget {
  const HeatPeakLine({
    super.key,
    required this.position,
    required this.color,
  });

  /// Peak position as a fraction of full scale, clamped to `[0, 1]`.
  final double position;

  /// Brightened band color of the marker.
  final Color color;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: color,
        boxShadow: <BoxShadow>[
          BoxShadow(color: color.withValues(alpha: 0.12), blurRadius: 3),
        ],
      ),
    );
  }
}

/// Relative load against a threshold, drawn as an audio loudness meter.
///
/// The bar is filled by `|value| / threshold` and both the fill and the right
/// cell readout take a three-band semantic tone from the ratio — green below
/// 50 %, amber for 50–85 %, red above 85 % (v3 tone mapping). The body is a
/// raised slot with 1 px dim border, 50 % / 85 % band ticks, a low-saturation
/// translucent fill with a bright 2 px leading edge, and a peak-hold marker
/// that holds for [kHeatPeakHold] then decays at [kHeatPeakDecayPerSecond]. The
/// whole row is sampled at [kTextRefreshHz] through [StableRefresh]. When no
/// threshold (or no value) is available the bar stays an empty slot and shows
/// `—` — never a fabricated `0 %` (design-system §6 rule 2).
class HeatLoadBar extends StatefulWidget {
  const HeatLoadBar({
    super.key,
    required this.name,
    required this.value,
    required this.threshold,
  });

  /// Row label.
  final String name;

  /// The measured quantity, or null.
  final num? value;

  /// The trip threshold, or null when the parameter is unloaded.
  final double? threshold;

  /// `|value| / threshold` clamped to `[0, 1]`, or null when undefined.
  double? get ratio {
    final t = threshold;
    final v = value;
    if (t == null || v == null || t <= 0) return null;
    return (v.abs() / t).clamp(0.0, 1.0).toDouble();
  }

  /// Three-band semantic tone for a load [ratio] (v3 tone mapping):
  /// `< 0.50` green, `0.50..0.85` amber, `> 0.85` red.
  static Tone toneForRatio(double? ratio) {
    if (ratio == null) return Tone.neutral;
    if (ratio < 0.5) return Tone.green;
    if (ratio <= 0.85) return Tone.amber;
    return Tone.red;
  }

  @override
  State<HeatLoadBar> createState() => _HeatLoadBarState();
}

class _HeatLoadBarState extends State<HeatLoadBar> {
  /// Held peak, full scale fraction in `[0, 1]`.
  double _peak = 0.0;

  /// Time accumulated since the last new high; drives the hold window.
  Duration _sinceHigh = Duration.zero;

  /// 5 Hz decay sampler. Cancelled in [dispose]; it only calls `setState` when
  /// the peak actually moves, so an idle bar schedules no frames.
  late final Timer _peakTimer;

  @override
  void initState() {
    super.initState();
    _peak = widget.ratio ?? 0.0;
    _peakTimer = Timer.periodic(kHeatPeakTick, _onPeakTick);
  }

  @override
  void didUpdateWidget(covariant HeatLoadBar oldWidget) {
    super.didUpdateWidget(oldWidget);
    final ratio = widget.ratio;
    if (ratio == null) {
      _peak = 0.0;
      _sinceHigh = Duration.zero;
    } else if (ratio > _peak) {
      // A new high jumps the marker immediately and restarts the hold window.
      _peak = ratio;
      _sinceHigh = Duration.zero;
    }
  }

  @override
  void dispose() {
    _peakTimer.cancel();
    super.dispose();
  }

  void _onPeakTick(Timer timer) {
    final ratio = widget.ratio;
    if (ratio == null) return;
    _sinceHigh += kHeatPeakTick;
    if (_sinceHigh <= kHeatPeakHold) return;
    final step =
        kHeatPeakDecayPerSecond * (kHeatPeakTick.inMilliseconds / 1000.0);
    // Decay toward the live value, never below it, never below zero.
    final next = (_peak - step).clamp(ratio, 1.0);
    if (next != _peak) {
      setState(() => _peak = next);
    }
  }

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final ratio = widget.ratio;
    // Peak is part of the refresh key so its (throttled) decay repaints too.
    return StableRefresh(
      refreshKey: (ratio, _peak),
      builder: (context) =>
          ratio == null ? _emptyRow(palette, scale) : _meterRow(palette, scale, ratio, _peak),
    );
  }

  /// Honest empty state: raised slot + `—`, no fill and no peak marker.
  Widget _emptyRow(GuiPalette palette, GuiTypeScale scale) {
    return Row(
      children: <Widget>[
        SizedBox(width: 52, child: Text(widget.name, style: scale.label)),
        const SizedBox(width: 11),
        Expanded(
          child: SizedBox(
            height: 13,
            child: DecoratedBox(
              decoration: BoxDecoration(
                color: palette.bgRaise,
                border: Border.all(color: palette.lineDim),
              ),
            ),
          ),
        ),
        const SizedBox(width: 11),
        SizedBox(
          width: 66,
          child: Text('—', textAlign: TextAlign.right, style: scale.label),
        ),
      ],
    );
  }

  Widget _meterRow(
    GuiPalette palette,
    GuiTypeScale scale,
    double ratio,
    double peak,
  ) {
    final tone = HeatLoadBar.toneForRatio(ratio);
    final band = GlowValue.colorFor(palette, tone);
    // Low-saturation translucent body + brightened leading edge / peak marker.
    final fillColor = band.withValues(alpha: 0.55);
    final capColor = Color.lerp(band, palette.white, 0.35)!;
    final peakColor = Color.lerp(band, palette.white, 0.2)!;
    return Row(
      children: <Widget>[
        SizedBox(width: 52, child: Text(widget.name, style: scale.label)),
        const SizedBox(width: 11),
        Expanded(
          child: SizedBox(
            height: 13,
            child: LayoutBuilder(
              builder: (context, constraints) {
                final width = constraints.maxWidth;
                final fillWidth = (width * ratio).clamp(0.0, width);
                final peakLeft = width <= 2
                    ? 0.0
                    : (width * peak - 1).clamp(0.0, width - 2);
                return Stack(
                  clipBehavior: Clip.none,
                  children: <Widget>[
                    Positioned.fill(
                      child: ColoredBox(color: palette.bgRaise),
                    ),
                    if (fillWidth > 0) ...<Widget>[
                      Positioned(
                        left: 0,
                        top: 0,
                        bottom: 0,
                        width: fillWidth,
                        child: ColoredBox(
                          key: Key('heat_fill_${widget.name}'),
                          color: fillColor,
                        ),
                      ),
                      Positioned(
                        left: (fillWidth - 2).clamp(0.0, width),
                        top: 0,
                        bottom: 0,
                        width: 2,
                        child: DecoratedBox(
                          decoration: BoxDecoration(
                            color: capColor,
                            boxShadow: <BoxShadow>[
                              BoxShadow(
                                color: band.withValues(alpha: 0.15),
                                blurRadius: 4,
                              ),
                            ],
                          ),
                        ),
                      ),
                    ],
                    Positioned(
                      left: width * 0.5,
                      top: 0,
                      bottom: 0,
                      width: 1,
                      child: ColoredBox(
                        key: Key('heat_tick_50_${widget.name}'),
                        color: palette.lineDim,
                      ),
                    ),
                    Positioned(
                      left: width * 0.85,
                      top: 0,
                      bottom: 0,
                      width: 1,
                      child: ColoredBox(
                        key: Key('heat_tick_85_${widget.name}'),
                        color: palette.lineDim,
                      ),
                    ),
                    Positioned(
                      left: peakLeft,
                      top: 0,
                      bottom: 0,
                      width: 2,
                      child: HeatPeakLine(position: peak, color: peakColor),
                    ),
                    Positioned.fill(
                      child: DecoratedBox(
                        decoration: BoxDecoration(
                          border: Border.all(color: palette.lineDim),
                        ),
                      ),
                    ),
                  ],
                );
              },
            ),
          ),
        ),
        const SizedBox(width: 11),
        SizedBox(
          width: 66,
          child: FittedBox(
            fit: BoxFit.scaleDown,
            alignment: Alignment.centerRight,
            // The whole row is throttled by the outer StableRefresh; the %
            // readout keeps the three-band tone.
            child: GlowValue(
              value: (ratio * 100).round(),
              unit: '%',
              precision: 0,
              tone: tone,
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
  });

  final String keyName;
  final String label;
  final String zh;
  final num? value;
  final String unit;
  final int precision;
  final String note;
  final double? threshold;

  /// Fraction of the threshold reached, or null when undefined.
  double? get load {
    final t = threshold;
    final v = value;
    if (t == null || v == null || t <= 0) return null;
    return v / t;
  }
}

/// The 状态监视 zone: five tri-state tiles (VBUS / I_IN / P_IN / NTC /
/// NTC_STATOR).
///
/// Everything is derived from [AppState]; when no status snapshot has been
/// read the tiles fall back to `—` rather than showing stale numbers
/// (design-system §6 rule 2). Two tiles are host-side **estimates** (I_IN,
/// P_IN, NTC_STATOR) and carry an explicit `EST · …` footer so an estimate can
/// never pass itself off as a measurement; the value tone stays neutral for
/// both since the footer already distinguishes them. The two encoder tiles
/// moved out to [EncoderZone]; the phase currents and F_ELEC move to the
/// [ScopeZone] right column; the relative-threshold heat/load bars also live in
/// [ScopeZone].
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
        keyName: 'I_IN',
        label: 'I_IN',
        zh: '输入电流',
        value: state.iInEst,
        unit: 'A',
        precision: 3,
        note: 'EST · 相电流反推',
      ),
      _TileSpec(
        keyName: 'P_IN',
        label: 'P_IN',
        zh: '输入功率',
        value: state.pInEst,
        unit: 'W',
        precision: 1,
        note: 'EST · 相电流反推',
      ),
      _TileSpec(
        keyName: 'NTC',
        label: 'NTC',
        zh: 'PCB',
        value: status?.ntcTemp,
        unit: '℃',
        precision: 1,
        note: 'NOMINAL',
        threshold: state.thresholdNtc,
      ),
      _TileSpec(
        keyName: 'NTC_STATOR',
        label: 'NTC',
        zh: '定子',
        value: state.ntcStatorEst,
        unit: '℃',
        precision: 1,
        note: 'EST · 热估算',
      ),
    ];

    final tiles = <Widget>[];
    var failed = 0;
    var warned = 0;
    for (final spec in specs) {
      final tileKey = 'status_tile_${spec.keyName}';
      // Tri-state from the single source in AppState: live until the poll ages
      // out, never-read stays unknown (design-system §6 rule 2).
      final freshness = status == null
          ? Freshness.unknown
          : state.freshnessOf(tileKey);
      final load = spec.load;
      final warn = load != null && load >= 0.9;
      // Only a failed poll counts as 失效; a never-read value is not a fault,
      // so start-up / disconnect does not report `8 项失效` (PREVIEW counts
      // its single stale tile and single warning).
      if (freshness == Freshness.stale) failed++;
      if (warn) warned++;
      tiles.add(
        // v3 nested-panel motif: every tile carries its own bright frame inside
        // the group frame (see `_grid`).
        HairlineFrame(
          key: Key('status_frame_${spec.keyName}'),
          padding: EdgeInsets.zero,
          child: StatusTile(
            key: Key(tileKey),
            label: spec.label,
            zh: spec.zh,
            value: spec.value,
            unit: spec.unit,
            precision: spec.precision,
            note: spec.note,
            freshness: freshness,
            staleSeconds: state.staleFor(tileKey)?.inSeconds.toDouble(),
            warn: warn,
            warnText: warn
                ? '接近门限 ${spec.threshold!.toStringAsFixed(0)}'
                : null,
          ),
        ),
      );
    }

    final header = StringBuffer('${specs.length} 项');
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
        _grid(tiles),
      ],
    );
  }

  Widget _grid(List<Widget> cells) {
    const perRow = 3;
    Widget row(List<Widget> rowCells) => IntrinsicHeight(
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          for (var i = 0; i < rowCells.length; i++) ...<Widget>[
            if (i > 0) const SizedBox(width: 8),
            Expanded(child: rowCells[i]),
          ],
        ],
      ),
    );
    // Chunk the tiles into rows of [perRow]; the 5 tiles form a 3 + 2 grid.
    final rows = <Widget>[];
    for (var i = 0; i < cells.length; i += perRow) {
      if (rows.isNotEmpty) rows.add(const SizedBox(height: 8));
      final end = math.min(i + perRow, cells.length);
      rows.add(row(cells.sublist(i, end)));
    }
    // v3: the tile group is one bright frame, each tile nests its own bright
    // frame inside it — the signature nested cyan panels of the reference.
    return HairlineFrame(
      key: const Key('status_grid_frame'),
      padding: const EdgeInsets.all(8),
      child: Column(children: rows),
    );
  }
}
