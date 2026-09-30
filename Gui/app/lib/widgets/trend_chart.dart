import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import 'glow_value.dart';
import 'hairline_frame.dart';
import 'stale_gate.dart';
import 'status_badge.dart';

/// One polled status sample reduced to the three phase currents [A].
///
/// The history is a 2 Hz polled preview, not the reserved 25 kHz
/// `STREAM_SUB` — see [TrendChart].
typedef TrendSample = ({double iU, double iV, double iW});

/// Live polled trend of the three phase currents (i_u / i_v / i_w).
///
/// This is an honest **polled** preview: samples arrive with the 500 ms status
/// poll (≈2 Hz, 60 points ≈ 30 s), not the reserved 25 kHz `STREAM_SUB`
/// stream, so the caption keeps that one-class-stream placeholder note. The
/// chart draws a bidirectional dark-cyan grid, a flat thin polyline per
/// channel (series1/2/3 colours, no glow) and an auto-scaled symmetric y axis
/// clamped to at least ±1 A. Fewer than two samples is an unknown reading:
/// the grid stays but the centre honestly shows `—`.
class TrendChart extends StatelessWidget {
  const TrendChart({
    super.key,
    required this.history,
    this.freshness = Freshness.unknown,
  });

  /// Oldest-first polled samples; the last entry is the current value.
  final List<TrendSample> history;

  /// Trust level of the newest sample (design-system §6 rule 2). Only
  /// [Freshness.stale] adds the `stale` badge to the caption.
  final Freshness freshness;

  /// Maximum plotted samples (60 ≈ 30 s at the 500 ms poll), matching
  /// `AppState.statusHistoryLimit`.
  static const int capacity = 60;

  /// Minimum number of samples needed to draw a line.
  static const int minSamples = 2;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final hasData = history.length >= minSamples;
    final last = history.isEmpty ? null : history.last;

    return HairlineFrame(
      variant: HairlineVariant.main,
      padding: const EdgeInsets.all(10),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          SizedBox(
            height: 200,
            child: Stack(
              children: <Widget>[
                Positioned.fill(
                  child: CustomPaint(
                    painter: TrendChartPainter(
                      series: <List<double>>[
                        <double>[for (final s in history) s.iU],
                        <double>[for (final s in history) s.iV],
                        <double>[for (final s in history) s.iW],
                      ],
                      colors: <Color>[
                        palette.series1,
                        palette.series2,
                        palette.series3,
                      ],
                      gridColor: palette.lineDim,
                    ),
                  ),
                ),
                if (!hasData)
                  Center(
                    child: Text(
                      '—',
                      style: scale.data.copyWith(
                        color: palette.fgMute,
                        shadows: const <Shadow>[],
                      ),
                    ),
                  ),
              ],
            ),
          ),
          const SizedBox(height: 8),
          Row(
            children: <Widget>[
              Expanded(
                child: Text(
                  '轮询 2 Hz 预览 · 一级流（STREAM_SUB）预留',
                  style: scale.label,
                ),
              ),
              if (freshness == Freshness.stale)
                const StatusBadge(state: ItemState.stale, label: 'stale'),
            ],
          ),
          const SizedBox(height: 8),
          Wrap(
            spacing: 24,
            runSpacing: 6,
            children: <Widget>[
              _legend(
                context,
                palette,
                scale,
                'U',
                palette.series1,
                last?.iU,
              ),
              _legend(
                context,
                palette,
                scale,
                'V',
                palette.series2,
                last?.iV,
              ),
              _legend(
                context,
                palette,
                scale,
                'W',
                palette.series3,
                last?.iW,
              ),
            ],
          ),
        ],
      ),
    );
  }

  Widget _legend(
    BuildContext context,
    GuiPalette palette,
    GuiTypeScale scale,
    String name,
    Color color,
    double? value,
  ) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: <Widget>[
        Container(width: 10, height: 10, color: color),
        const SizedBox(width: 8),
        Text(name, style: scale.label),
        const SizedBox(width: 10),
        GlowValue(value: value, unit: 'A', precision: 3),
      ],
    );
  }
}

/// Paints the polled trend: a bidirectional grid plus one flat polyline per
/// channel, with a symmetric auto-scaled y axis (sample index on x).
class TrendChartPainter extends CustomPainter {
  TrendChartPainter({
    required this.series,
    required this.colors,
    required this.gridColor,
  });

  /// Oldest-first values per channel ([iU], [iV], [iW] order); every channel
  /// shares the same sample count. A channel with fewer than two values is
  /// skipped.
  final List<List<double>> series;

  /// Line colour per channel (series1/2/3); flat — no glow.
  final List<Color> colors;

  /// Grid line colour (the palette's dark cyan seam).
  final Color gridColor;

  /// Thin line weight (design-system): 1.5px, never scaled for emphasis.
  static const double strokeWidth = 1.5;

  /// Grid pitch in logical pixels.
  static const double gridStep = 28;

  /// Minimum symmetric y half-range, so an all-zero trace does not collapse
  /// onto the mid line.
  static const double minYLimit = 1;

  /// Symmetric y half-range: the largest finite |sample| across all channels,
  /// clamped to at least [minYLimit].
  static double yLimit(List<List<double>> series) {
    var maxAbs = 0.0;
    for (final channel in series) {
      for (final value in channel) {
        final magnitude = value.abs();
        if (magnitude.isFinite && magnitude > maxAbs) maxAbs = magnitude;
      }
    }
    return maxAbs < minYLimit ? minYLimit : maxAbs;
  }

  /// The per-channel stroke paints; a test seam mirroring [SolidPiePainter].
  List<Paint> buildSeriesPaints() => <Paint>[
    for (final color in colors)
      Paint()
        ..color = color
        ..style = PaintingStyle.stroke
        ..strokeWidth = strokeWidth
        ..strokeCap = StrokeCap.round
        ..strokeJoin = StrokeJoin.round,
  ];

  @override
  void paint(Canvas canvas, Size size) {
    _paintGrid(canvas, size);
    final limit = yLimit(series);
    final paints = buildSeriesPaints();
    for (var channel = 0; channel < series.length; channel++) {
      _paintSeries(canvas, size, series[channel], paints[channel], limit);
    }
  }

  void _paintGrid(Canvas canvas, Size size) {
    final paint = Paint()
      ..color = gridColor
      ..strokeWidth = 1;
    for (var x = 0.0; x <= size.width; x += gridStep) {
      canvas.drawLine(Offset(x, 0), Offset(x, size.height), paint);
    }
    for (var y = 0.0; y <= size.height; y += gridStep) {
      canvas.drawLine(Offset(0, y), Offset(size.width, y), paint);
    }
  }

  void _paintSeries(
    Canvas canvas,
    Size size,
    List<double> values,
    Paint paint,
    double limit,
  ) {
    if (values.length < 2) return;
    final dx = size.width / (values.length - 1);
    final midY = size.height / 2;
    final scaleY = midY / limit;
    final path = Path();
    for (var i = 0; i < values.length; i++) {
      final value = values[i];
      final y = value.isFinite ? midY - value * scaleY : midY;
      final point = Offset(dx * i, y);
      if (i == 0) {
        path.moveTo(point.dx, point.dy);
      } else {
        path.lineTo(point.dx, point.dy);
      }
    }
    canvas.drawPath(path, paint);
  }

  @override
  bool shouldRepaint(covariant TrendChartPainter oldDelegate) {
    if (oldDelegate.gridColor != gridColor) return true;
    if (!listEquals(oldDelegate.colors, colors)) return true;
    if (oldDelegate.series.length != series.length) return true;
    for (var channel = 0; channel < series.length; channel++) {
      if (!listEquals(oldDelegate.series[channel], series[channel])) {
        return true;
      }
    }
    return false;
  }
}
