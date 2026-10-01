import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import 'chart_history.dart';
import 'hairline_frame.dart';

/// The small `ΣI(t)` sparkline that sits to the right of the `ΣI` readout.
///
/// Where [TrendChart] keeps a 1 kHz three-channel trace, this companion plots a
/// single derived series — the three-phase sum `i_u + i_v + i_w` — over a
/// **fixed rolling window of the most recent [windowSec]** (decoupled from the
/// main scope viewport: zooming, panning or changing the `SPAN` slider never
/// moves it). Like the main trace, the window is decimated to one min/max pair
/// per pixel column so peaks survive, and the y axis is symmetric around zero:
/// `±max(|min|, |max|)` over the window, floored at [minYLimit] so an idle
/// window does not collapse onto the baseline.
///
/// The look follows the TNO economic line-chart vocabulary: a dark panel, a
/// pale bidirectional 1px grid ([GuiPalette.lineDim]), a dashed zero baseline
/// ([GuiPalette.fgDim]), a flat 1.5px magenta line ([GuiPalette.valueMagenta],
/// no glow), two small `+/−` bound labels at the left edge and a small
/// `−50ms … 0` time ruler along the bottom edge. With no sample in the window
/// it renders a centred `—`.
class SigmaTrend extends StatelessWidget {
  const SigmaTrend({super.key, required this.history});

  /// The same ring the main trend chart slices.
  final ChartHistory history;

  /// Fixed rolling window width [s] shown by the sparkline: 50 ms = 50 samples
  /// at 1 kHz. Independent of the main [TrendChart] viewport.
  static const double windowSec = 0.05;

  /// Fixed plot width [logical px].
  static const double width = 200;

  /// Fixed plot height [logical px].
  static const double height = 88;

  /// Minimum symmetric y half-range [A], so an all-flat window keeps a readable
  /// scale instead of collapsing.
  static const double minYLimit = 0.5;

  /// Flat line weight [px] — 1.5, never scaled for emphasis; no glow.
  static const double strokeWidth = 1.5;

  /// `±` bound label font size [logical px].
  static const double boundLabelSize = 10;

  /// Time-ruler label font size [logical px].
  static const double axisLabelSize = 9;

  /// Key of the fixed-size frame box (mirrors the plot dimensions).
  static const Key frameKey = Key('sigma_trend_frame');

  /// Key of the plotted sparkline.
  static const Key plotKey = Key('sigma_trend_plot');

  /// Tolerance [s] for the inclusive window edge, mirroring the main painter.
  static const double _windowEdgeEpsilon = 1e-9;

  /// The fixed rolling window `[lastTime − windowSec, lastTime]` [s] over the
  /// ring, taken from the newest sample's timestamp. Empty history yields an
  /// empty window ending at 0.
  static ({double startSec, double spanSec}) windowOf(ChartHistory history) {
    final endSec = history.lastTime;
    return (startSec: endSec - windowSec, spanSec: windowSec);
  }

  /// Formats a time-ruler offset [seconds] inside the fixed window: `0` at the
  /// right edge and `−50ms` / `−25ms` to its left.
  static String formatAxisLabel(double seconds) {
    final millis = (seconds * 1000).round();
    if (millis == 0) return '0';
    final sign = millis < 0 ? '−' : '+';
    return '$sign${millis.abs()}ms';
  }

  /// Three-phase sum `i_u + i_v + i_w` of the oldest-first sample at [index].
  static double sigmaAt(ChartHistory history, int index) =>
      history.channelAt(0, index) +
      history.channelAt(1, index) +
      history.channelAt(2, index);

  /// Symmetric y half-range [A] over `[startSec, startSec + spanSec]` (both
  /// edges inclusive): `max(|min|, |max|)` of the σ series, floored at
  /// [minYLimit]. A window with no finite sample also returns [minYLimit].
  static double yLimit(
    ChartHistory history,
    double startSec,
    double spanSec,
  ) {
    final endSec = startSec + spanSec;
    final first = history.lowerBoundTime(startSec);
    final last = history.lowerBoundTime(endSec);
    var end = last;
    if (last < history.len &&
        history.timeAt(last) <= endSec + _windowEdgeEpsilon) {
      end = last + 1;
    }
    var maxAbs = 0.0;
    for (var i = first; i < end; i++) {
      final magnitude = sigmaAt(history, i).abs();
      if (magnitude.isFinite && magnitude > maxAbs) maxAbs = magnitude;
    }
    return maxAbs < minYLimit ? minYLimit : maxAbs;
  }

  /// Number of retained σ samples inside `[startSec, startSec + spanSec]`
  /// (inclusive). At least two are needed to draw a line.
  static int sampleCount(
    ChartHistory history,
    double startSec,
    double spanSec,
  ) => history.countInWindow(startSec, startSec + spanSec);

  /// Decimates the σ series over the viewport to at most one min/max pair per
  /// pixel column (mirrors [TrendChartPainter.seriesPoints]).
  ///
  /// [size] is the plot area; `ceil(size.width)` columns each cover an equal
  /// slice of `[startSec, startSec + spanSec)`. A window holding fewer than two
  /// samples yields no vertices. [limit] is the symmetric y half-range.
  static List<Offset> seriesPoints(
    ChartHistory history,
    Size size,
    double startSec,
    double spanSec,
    double limit,
  ) {
    final columns = size.width.ceil();
    if (columns <= 0 || spanSec <= 0 || limit <= 0) {
      return const <Offset>[];
    }
    final first = history.lowerBoundTime(startSec);
    final last = history.lowerBoundTime(startSec + spanSec);
    final count = last - first;
    if (count < 2) return const <Offset>[];

    final midY = size.height / 2;
    final scaleY = midY / limit;
    double yOf(double value) => value.isFinite ? midY - value * scaleY : midY;
    double xOfTime(double t) => (t - startSec) / spanSec * size.width;

    // Fewer samples than columns: plot each one directly, no decimation.
    if (count <= columns) {
      return <Offset>[
        for (var i = first; i < last; i++)
          Offset(xOfTime(history.timeAt(i)), yOf(sigmaAt(history, i))),
      ];
    }

    final points = <Offset>[];
    for (var column = 0; column < columns; column++) {
      final sliceStart = startSec + column * spanSec / columns;
      final sliceEnd = startSec + (column + 1) * spanSec / columns;
      final start = history.lowerBoundTime(sliceStart);
      final end = history.lowerBoundTime(sliceEnd);
      if (end <= start) continue;
      var minIndex = start;
      var maxIndex = start;
      for (var i = start + 1; i < end; i++) {
        final value = sigmaAt(history, i);
        if (value < sigmaAt(history, minIndex)) minIndex = i;
        if (value > sigmaAt(history, maxIndex)) maxIndex = i;
      }
      final x = (column + 0.5) * size.width / columns;
      final lower = minIndex <= maxIndex ? minIndex : maxIndex;
      final upper = minIndex <= maxIndex ? maxIndex : minIndex;
      points.add(Offset(x, yOf(sigmaAt(history, lower))));
      if (upper != lower) {
        points.add(Offset(x, yOf(sigmaAt(history, upper))));
      }
    }
    return points;
  }

  /// Formats a y bound with one decimal and an explicit sign (`+1.5` / `-1.5`).
  static String formatBound(double value) {
    final text = value.toStringAsFixed(1);
    return value >= 0 ? '+$text' : text;
  }

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final window = windowOf(history);
    final startSec = window.startSec;
    final spanSec = window.spanSec;
    final limit = yLimit(history, startSec, spanSec);
    final hasData = sampleCount(history, startSec, spanSec) >= 2;

    return SizedBox(
      key: frameKey,
      width: width,
      height: height,
      child: HairlineFrame(
        variant: HairlineVariant.sub,
        child: ColoredBox(
          color: palette.bgPanel,
          child: Stack(
            fit: StackFit.expand,
            children: <Widget>[
              if (hasData)
                CustomPaint(
                  key: plotKey,
                  painter: SigmaTrendPainter(
                    history: history,
                    gridColor: palette.lineDim,
                    lineColor: palette.valueMagenta,
                    fgColor: palette.fgDim,
                    viewStartSec: startSec,
                    viewSpanSec: spanSec,
                    limit: limit,
                  ),
                )
              else
                Center(
                  child: Text(
                    '—',
                    style: scale.data.copyWith(
                      fontSize: 24,
                      color: palette.fgMute,
                      shadows: const <Shadow>[],
                    ),
                  ),
                ),
              if (hasData) ...<Widget>[
                Positioned(
                  left: 3,
                  top: 1,
                  child: _boundLabel(palette, scale, limit),
                ),
                // Raised above the bottom time ruler so the two labels do not
                // collide at the left edge.
                Positioned(
                  left: 3,
                  bottom: _axisLabelClearance,
                  child: _boundLabel(palette, scale, -limit),
                ),
                _timeRuler(palette, scale),
              ],
            ],
          ),
        ),
      ),
    );
  }

  /// The small `−50ms … 0` time ruler along the bottom edge of the fixed
  /// window, in the dim annotation tier.
  Widget _timeRuler(GuiPalette palette, GuiTypeScale scale) {
    return Positioned(
      left: 3,
      right: 3,
      bottom: 1,
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        children: <Widget>[
          _axisLabel(palette, scale, formatAxisLabel(-windowSec)),
          _axisLabel(palette, scale, formatAxisLabel(-windowSec / 2)),
          _axisLabel(palette, scale, formatAxisLabel(0)),
        ],
      ),
    );
  }

  /// One small time-ruler tick label in the dim annotation tier.
  Widget _axisLabel(GuiPalette palette, GuiTypeScale scale, String text) => Text(
    text,
    style: scale.label.copyWith(fontSize: axisLabelSize, color: palette.fgDim),
  );

  /// Bottom offset [px] of the lower `−` bound label, clearing the time ruler.
  static const double _axisLabelClearance = 15;

  /// One small `+/−` y bound label in the dim annotation tier.
  Widget _boundLabel(GuiPalette palette, GuiTypeScale scale, double value) =>
      Text(
        SigmaTrend.formatBound(value),
        style: scale.label.copyWith(
          fontSize: boundLabelSize,
          color: palette.fgDim,
        ),
      );
}

/// Paints the [SigmaTrend] sparkline: a pale bidirectional grid, a dashed zero
/// baseline, and one flat magenta min/max-decimated polyline (no glow).
class SigmaTrendPainter extends CustomPainter {
  SigmaTrendPainter({
    required this.history,
    required this.gridColor,
    required this.lineColor,
    required this.fgColor,
    required this.viewStartSec,
    required this.viewSpanSec,
    required this.limit,
  });

  /// Timestamped ring the σ series is derived from.
  final ChartHistory history;

  /// Pale grid colour ([GuiPalette.lineDim]).
  final Color gridColor;

  /// Flat line colour ([GuiPalette.valueMagenta]).
  final Color lineColor;

  /// Baseline / annotation colour ([GuiPalette.fgDim]).
  final Color fgColor;

  /// Viewport left edge [s].
  final double viewStartSec;

  /// Viewport width [s].
  final double viewSpanSec;

  /// Symmetric y half-range [A].
  final double limit;

  /// Grid lines per axis (both edges inclusive).
  static const int gridLineCount = 5;

  /// Inset [px] keeping the trace clear of the frame border.
  static const double plotInset = 2;

  /// Dash length [px] of the zero baseline.
  static const double baselineDash = 3;

  /// The flat magenta stroke; a test seam mirroring [TrendChartPainter].
  Paint buildSeriesPaint() => Paint()
    ..color = lineColor
    ..style = PaintingStyle.stroke
    ..strokeWidth = SigmaTrend.strokeWidth
    ..strokeCap = StrokeCap.round
    ..strokeJoin = StrokeJoin.round;

  @override
  void paint(Canvas canvas, Size size) {
    _paintGrid(canvas, size);
    _paintBaseline(canvas, size);
    _paintLine(canvas, size);
  }

  /// 1px pale bidirectional grid.
  void _paintGrid(Canvas canvas, Size size) {
    final paint = Paint()
      ..color = gridColor
      ..strokeWidth = 1;
    for (var i = 0; i < gridLineCount; i++) {
      final fraction = i / (gridLineCount - 1);
      final x = size.width * fraction;
      canvas.drawLine(Offset(x, 0), Offset(x, size.height), paint);
      final y = size.height * fraction;
      canvas.drawLine(Offset(0, y), Offset(size.width, y), paint);
    }
  }

  /// Dashed zero baseline; the symmetric axis keeps zero at mid-height.
  void _paintBaseline(Canvas canvas, Size size) {
    final paint = Paint()
      ..color = fgColor
      ..strokeWidth = 1;
    final midY = size.height / 2;
    for (var x = 0.0; x < size.width; x += baselineDash * 2) {
      final end = math.min(x + baselineDash, size.width);
      canvas.drawLine(Offset(x, midY), Offset(end, midY), paint);
    }
  }

  /// The decimated σ polyline, inset from the frame border.
  void _paintLine(Canvas canvas, Size size) {
    final plotWidth = math.max(0.0, size.width - plotInset * 2);
    final plotHeight = math.max(0.0, size.height - plotInset * 2);
    final points = SigmaTrend.seriesPoints(
      history,
      Size(plotWidth, plotHeight),
      viewStartSec,
      viewSpanSec,
      limit,
    );
    if (points.length < 2) return;
    final path = Path()
      ..moveTo(plotInset + points.first.dx, plotInset + points.first.dy);
    for (var i = 1; i < points.length; i++) {
      path.lineTo(plotInset + points[i].dx, plotInset + points[i].dy);
    }
    canvas.drawPath(path, buildSeriesPaint());
  }

  @override
  bool shouldRepaint(covariant SigmaTrendPainter oldDelegate) {
    if (oldDelegate.history != history) return true;
    if (oldDelegate.gridColor != gridColor) return true;
    if (oldDelegate.lineColor != lineColor) return true;
    if (oldDelegate.fgColor != fgColor) return true;
    if (oldDelegate.viewStartSec != viewStartSec) return true;
    if (oldDelegate.viewSpanSec != viewSpanSec) return true;
    if (oldDelegate.limit != limit) return true;
    return false;
  }
}
