import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import 'glow_value.dart';
import 'hairline_frame.dart';
import 'stale_gate.dart';
import 'status_badge.dart';

/// One status sample reduced to the three phase currents [A].
///
/// The history is a 1 kHz batched preview (≈20 ms batches, 2 s window), not
/// the reserved `STREAM_SUB` — see [TrendChart].
typedef TrendSample = ({double iU, double iV, double iW});

/// Live 1 kHz trend of the three phase currents (i_u / i_v / i_w).
///
/// Samples arrive in ~20 ms batches from the status watch (1 kHz, 2000-point /
/// 2 s window), not the reserved `STREAM_SUB` stream, so the caption keeps that
/// one-class-stream placeholder note. The chart draws a bidirectional dark-cyan
/// grid, a flat thin polyline per channel (series1/2/3 colours, no glow) and an
/// auto-scaled symmetric y axis clamped to at least ±1 A. With far more samples
/// than pixels, each channel is decimated to one min/max pair per pixel column
/// so peaks survive. Fewer than two samples is an unknown reading: the grid
/// stays but the centre honestly shows `—`.
class TrendChart extends StatelessWidget {
  const TrendChart({
    super.key,
    required this.history,
    this.freshness = Freshness.unknown,
  });

  /// Oldest-first status samples; the last entry is the current value.
  final List<TrendSample> history;

  /// Trust level of the newest sample (design-system §6 rule 2). Only
  /// [Freshness.stale] adds the `stale` badge to the caption.
  final Freshness freshness;

  /// Maximum plotted samples: 2000 = 2 s at 1 kHz, matching
  /// `AppState.statusHistoryLimit`.
  static const int capacity = 2000;

  /// Minimum number of samples needed to draw a line.
  static const int minSamples = 2;

  /// Honest caption: names the 1 kHz batched source, the 2 s window and the
  /// still-reserved first-class stream.
  static const String caption = '1 kHz 采样 · 2 s 窗 · 一级流（STREAM_SUB）预留';

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
          const SizedBox(height: 4),
          // Time axis: oldest at the left (2 s ago), newest at the right (now).
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: <Widget>[
              Text('2s', style: scale.label.copyWith(color: palette.fgMute)),
              Text('1s', style: scale.label.copyWith(color: palette.fgMute)),
              Text('0', style: scale.label.copyWith(color: palette.fgMute)),
            ],
          ),
          const SizedBox(height: 8),
          Row(
            children: <Widget>[
              Expanded(child: Text(caption, style: scale.label)),
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

/// Paints the 1 kHz trend: a bidirectional grid plus one flat polyline per
/// channel, with a symmetric auto-scaled y axis (time on x). Each channel is
/// decimated to one min/max pair per pixel column so peaks survive.
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

  /// Decimates one channel to at most one min/max pair per pixel column.
  ///
  /// [values] is oldest-first and [size] is the paint area, so the sample index
  /// maps to time and `ceil(size.width)` columns each cover an equal slice.
  /// Keeping the two extremes per slice preserves peaks that a naive
  /// every-nth-sample downsample would drop. Returns the plotted vertices in
  /// draw order (≤ 2 per column); an unknown channel (< 2 samples) is empty.
  static List<Offset> seriesPoints(
    List<double> values,
    Size size,
    double limit,
  ) {
    final count = values.length;
    if (count < 2) return const <Offset>[];
    final columns = size.width.ceil();
    if (columns <= 0) return const <Offset>[];
    final midY = size.height / 2;
    final scaleY = midY / limit;
    double yOf(double value) =>
        value.isFinite ? midY - value * scaleY : midY;

    // Fewer samples than columns: plot each one directly, no decimation.
    if (count <= columns) {
      final dx = size.width / (count - 1);
      return <Offset>[
        for (var i = 0; i < count; i++) Offset(dx * i, yOf(values[i])),
      ];
    }

    final points = <Offset>[];
    for (var column = 0; column < columns; column++) {
      final start = column * count ~/ columns;
      var end = (column + 1) * count ~/ columns;
      if (end <= start) end = start + 1;
      var minIndex = start;
      var maxIndex = start;
      for (var i = start + 1; i < end; i++) {
        final value = values[i];
        if (value < values[minIndex]) minIndex = i;
        if (value > values[maxIndex]) maxIndex = i;
      }
      final x = (column + 0.5) * size.width / columns;
      // Draw the extremes in time order so the envelope reads as one trace.
      final first = minIndex <= maxIndex ? minIndex : maxIndex;
      final second = minIndex <= maxIndex ? maxIndex : minIndex;
      points.add(Offset(x, yOf(values[first])));
      if (second != first) points.add(Offset(x, yOf(values[second])));
    }
    return points;
  }

  void _paintSeries(
    Canvas canvas,
    Size size,
    List<double> values,
    Paint paint,
    double limit,
  ) {
    final points = seriesPoints(values, size, limit);
    if (points.length < 2) return;
    final path = Path()..moveTo(points.first.dx, points.first.dy);
    for (var i = 1; i < points.length; i++) {
      path.lineTo(points[i].dx, points[i].dy);
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
