import 'dart:math' as math;

import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';

import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import 'chart_history.dart';
import 'glow_value.dart';
import 'gui_button.dart';
import 'hairline_frame.dart';
import 'stale_gate.dart';
import 'status_badge.dart';

/// Formats a viewport axis offset [seconds] for display, e.g. `2s` / `0.5s`.
String formatAxisSeconds(double seconds) {
  if (seconds <= 0) return '0';
  final rounded = (seconds * 10).round() / 10;
  final text = rounded == rounded.roundToDouble()
      ? rounded.toStringAsFixed(0)
      : rounded.toStringAsFixed(1);
  return '${text}s';
}

/// Live 1 kHz trend of the three phase currents (i_u / i_v / i_w).
///
/// Samples arrive in ~20 ms batches from the status watch and are retained in a
/// [ChartHistory] ring (60 s at 1 kHz). The viewport shows the newest
/// [defaultSpanSec] seconds live-tail; the mouse wheel zooms (anchor at the
/// pointer) within [minSpanSec]..[maxSpanSec], horizontal drag pans into an
/// inspect window, and a double-tap or the `LIVE` button snaps back to the
/// tail. The chart draws a bidirectional dark-cyan grid, a flat thin polyline
/// per channel (series1/2/3 colours, no glow) and an auto-scaled symmetric y
/// axis clamped to at least ±1 A. With far more samples than pixels, each
/// channel is decimated to one min/max pair per pixel column so peaks survive.
///
/// The same frame carries the control chrome: a logarithmic `SPAN` slider
/// (`0.5..60 s`), a `−` / `+` / `LIVE` [GuiButton] group with `2s/10s/60s`
/// presets, and a mouse-following crosshair whose readout is the nearest
/// [ChartHistory] sample under the pointer.
class TrendChart extends StatefulWidget {
  const TrendChart({
    super.key,
    required this.history,
    this.freshness = Freshness.unknown,
  });

  /// Timestamped 60 s ring of the three phase currents.
  final ChartHistory history;

  /// Trust level of the newest sample (design-system §6 rule 2). Only
  /// [Freshness.stale] adds the `stale` badge to the caption.
  final Freshness freshness;

  /// Honest caption: names the 1 kHz source, the 60 s history, the viewport
  /// gestures and the still-reserved first-class stream.
  static const String caption =
      '1 kHz 采样 · 60 s 历史 · 拖动平移 / 滚轮缩放 · 一级流预留';

  /// Default live-tail window [s].
  static const double defaultSpanSec = 2.0;

  /// Tightest zoom window [s].
  static const double minSpanSec = 0.5;

  /// Widest zoom window [s]: the whole retained history.
  static const double maxSpanSec = 60.0;

  /// Plot area height [logical px].
  static const double plotHeight = 200;

  /// Height [logical px] of the bottom control bar sharing the chart frame.
  static const double controlBarHeight = 36;

  /// One-tap preset spans [s]; each snaps the viewport back to the tail.
  static const List<double> spanPresets = <double>[2, 10, 60];

  /// Key of the plot area (hover / cursor hit region).
  static const Key plotKey = Key('trend_plot');

  /// Key of the logarithmic span slider.
  static const Key spanSliderKey = Key('trend_span_slider');

  /// Key of the `−` (span×2) button.
  static const Key zoomOutKey = Key('trend_zoom_out');

  /// Key of the `+` (span÷2) button.
  static const Key zoomInKey = Key('trend_zoom_in');

  /// Key of the `LIVE` button that returns the viewport to live-tail.
  static const Key liveKey = Key('trend_live_chip');

  /// Key of the hover crosshair readout panel.
  static const Key readoutKey = Key('trend_cursor_readout');

  @override
  State<TrendChart> createState() => TrendChartState();
}

/// Viewport model for [TrendChart]: span, inspect anchor, cursor and gestures.
class TrendChartState extends State<TrendChart> {
  double _viewSpanSec = TrendChart.defaultSpanSec;
  double _anchorSec = 0;
  double _plotWidth = 0;

  /// Crosshair position as a normalized fraction of the plot area, or null
  /// while the pointer is outside.
  Offset? _cursor;

  /// Current viewport width [s], clamped to [TrendChart.minSpanSec]..[maxSpanSec].
  @visibleForTesting
  double get viewSpanSec => _viewSpanSec;

  /// Time offset [s] of the viewport's right edge behind the newest sample.
  /// Zero while live-tailing.
  @visibleForTesting
  double get anchorSec => _anchorSec;

  /// True while the viewport follows the newest sample.
  @visibleForTesting
  bool get isLive => _anchorSec == 0;

  /// Normalized crosshair position, or null while hidden.
  @visibleForTesting
  Offset? get cursorFraction => _cursor;

  double get _lastTime => widget.history.lastTime;

  double get _maxAnchor => math.max(0.0, TrendChart.maxSpanSec - _viewSpanSec);

  /// Snaps back to the live tail.
  void goLive() => setState(() => _anchorSec = 0);

  /// Sets the viewport span [s], clamped to the zoom limits. Anchored at the
  /// viewport centre while inspecting; a live viewport stays live.
  @visibleForTesting
  void setSpanSec(double seconds) => _applySpan(seconds);

  /// Zoom out one click (`−`: span ×2, clamped to [TrendChart.maxSpanSec]).
  void zoomOut() => _applySpan(_viewSpanSec * 2);

  /// Zoom in one click (`+`: span ÷2, clamped to [TrendChart.minSpanSec]).
  void zoomIn() => _applySpan(_viewSpanSec / 2);

  /// Applies a new span, clamped to the zoom limits. [forceLive] also snaps
  /// back to the tail (used by the presets).
  void _applySpan(double raw, {bool forceLive = false}) {
    final newSpan = raw
        .clamp(TrendChart.minSpanSec, TrendChart.maxSpanSec)
        .toDouble();
    setState(() {
      if (forceLive || _anchorSec == 0) {
        _viewSpanSec = newSpan;
        _anchorSec = 0;
        return;
      }
      final centre = _lastTime - _anchorSec - _viewSpanSec / 2;
      final newEnd = centre + newSpan / 2;
      _viewSpanSec = newSpan;
      final maxAnchor = math.max(0.0, TrendChart.maxSpanSec - newSpan);
      _anchorSec = (_lastTime - newEnd).clamp(0.0, maxAnchor);
    });
  }

  /// Pans the viewport: dragging right reveals older samples.
  void _onHorizontalDragUpdate(DragUpdateDetails details) {
    if (_plotWidth <= 0) return;
    final deltaSec = details.delta.dx / _plotWidth * _viewSpanSec;
    setState(() {
      _anchorSec = (_anchorSec + deltaSec).clamp(0.0, _maxAnchor);
    });
  }

  /// Zooms the viewport around the pointer, clamped to the span limits.
  void _onPointerSignal(PointerSignalEvent event) {
    if (event is! PointerScrollEvent || _plotWidth <= 0) return;
    final dy = event.scrollDelta.dy;
    if (dy == 0) return;
    final factor = math.pow(2, dy / 200).toDouble();
    final newSpan = (_viewSpanSec * factor).clamp(
      TrendChart.minSpanSec,
      TrendChart.maxSpanSec,
    );
    final frac = (event.localPosition.dx / _plotWidth).clamp(0.0, 1.0);
    final viewEnd = _lastTime - _anchorSec;
    final viewStart = viewEnd - _viewSpanSec;
    final timeAtPointer = viewStart + frac * _viewSpanSec;
    final newEnd = timeAtPointer + (1 - frac) * newSpan;
    setState(() {
      _viewSpanSec = newSpan;
      final maxAnchor = math.max(0.0, TrendChart.maxSpanSec - newSpan);
      _anchorSec = (_lastTime - newEnd).clamp(0.0, maxAnchor);
    });
  }

  /// Updates the crosshair from a pointer position local to the plot area.
  void _updateCursor(Offset local) {
    if (_plotWidth <= 0) return;
    final next = Offset(
      (local.dx / _plotWidth).clamp(0.0, 1.0),
      (local.dy / TrendChart.plotHeight).clamp(0.0, 1.0),
    );
    if (_cursor == next) return;
    setState(() => _cursor = next);
  }

  /// Hides the crosshair when the pointer leaves the plot area.
  void _clearCursor() {
    if (_cursor == null) return;
    setState(() => _cursor = null);
  }

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final history = widget.history;
    final len = history.len;

    final viewEnd = _lastTime - _anchorSec;
    final viewStart = viewEnd - _viewSpanSec;
    final visible = history.countInWindow(viewStart, viewEnd);
    final hasData = visible >= 2;
    final lastU = len == 0 ? null : history.channelAt(0, len - 1);
    final lastV = len == 0 ? null : history.channelAt(1, len - 1);
    final lastW = len == 0 ? null : history.channelAt(2, len - 1);

    final cursor = _cursor;
    final readoutIndex = (cursor != null && hasData)
        ? TrendChartPainter.nearestIndex(
            history,
            viewStart + cursor.dx * _viewSpanSec,
          )
        : -1;

    return HairlineFrame(
      variant: HairlineVariant.main,
      padding: const EdgeInsets.all(10),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          SizedBox(
            key: TrendChart.plotKey,
            height: TrendChart.plotHeight,
            child: LayoutBuilder(
              builder: (context, constraints) {
                _plotWidth = constraints.maxWidth;
                return MouseRegion(
                  onHover: (event) => _updateCursor(event.localPosition),
                  onExit: (_) => _clearCursor(),
                  child: GestureDetector(
                    behavior: HitTestBehavior.opaque,
                    onHorizontalDragUpdate: _onHorizontalDragUpdate,
                    onDoubleTap: goLive,
                    child: Listener(
                      onPointerSignal: _onPointerSignal,
                      onPointerDown: (event) =>
                          _updateCursor(event.localPosition),
                      onPointerMove: (event) =>
                          _updateCursor(event.localPosition),
                      child: Stack(
                        children: <Widget>[
                          Positioned.fill(
                            child: CustomPaint(
                              painter: TrendChartPainter(
                                history: history,
                                colors: <Color>[
                                  palette.series1,
                                  palette.series2,
                                  palette.series3,
                                ],
                                gridColor: palette.lineDim,
                                viewStartSec: viewStart,
                                viewSpanSec: _viewSpanSec,
                                cursor: cursor,
                                cursorColor: palette.fgDim,
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
                          if (cursor != null)
                            Positioned(
                              top: 6,
                              right: 6,
                              child: _readout(
                                palette,
                                scale,
                                history,
                                readoutIndex,
                              ),
                            ),
                        ],
                      ),
                    ),
                  ),
                );
              },
            ),
          ),
          const SizedBox(height: 4),
          // Time axis follows the viewport: left = oldest visible, right = now
          // (live) or the inspect offset.
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: <Widget>[
              Text(
                formatAxisSeconds(_anchorSec + _viewSpanSec),
                style: scale.label.copyWith(color: palette.fgMute),
              ),
              Text(
                formatAxisSeconds(_anchorSec + _viewSpanSec / 2),
                style: scale.label.copyWith(color: palette.fgMute),
              ),
              Text(
                isLive ? '0' : formatAxisSeconds(_anchorSec),
                style: scale.label.copyWith(color: palette.fgMute),
              ),
            ],
          ),
          const SizedBox(height: 8),
          Row(
            children: <Widget>[
              Expanded(child: Text(TrendChart.caption, style: scale.label)),
              if (widget.freshness == Freshness.stale) ...<Widget>[
                const StatusBadge(state: ItemState.stale, label: 'stale'),
                const SizedBox(width: 8),
              ],
            ],
          ),
          const SizedBox(height: 8),
          Wrap(
            spacing: 24,
            runSpacing: 6,
            children: <Widget>[
              _legend(context, palette, scale, 'U', palette.series1, lastU),
              _legend(context, palette, scale, 'V', palette.series2, lastV),
              _legend(context, palette, scale, 'W', palette.series3, lastW),
            ],
          ),
          const SizedBox(height: 10),
          _controlBar(palette, scale),
        ],
      ),
    );
  }

  /// The bottom control bar: `SPAN` slider, readout, presets and `− + LIVE`.
  Widget _controlBar(GuiPalette palette, GuiTypeScale scale) {
    return SizedBox(
      height: TrendChart.controlBarHeight,
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.center,
        children: <Widget>[
          Text('SPAN', style: scale.label),
          const SizedBox(width: 8),
          Expanded(
            child: _SpanSlider(
              key: TrendChart.spanSliderKey,
              value: _viewSpanSec,
              min: TrendChart.minSpanSec,
              max: TrendChart.maxSpanSec,
              onChanged: _applySpan,
            ),
          ),
          const SizedBox(width: 8),
          Text(
            formatAxisSeconds(_viewSpanSec),
            style: scale.data.copyWith(
              fontSize: 12,
              color: palette.valueAmber,
              shadows: const <Shadow>[],
            ),
          ),
          const SizedBox(width: 14),
          for (final preset in TrendChart.spanPresets) ...<Widget>[
            GuiButton(
              label: formatAxisSeconds(preset),
              onPressed: () => _applySpan(preset, forceLive: true),
            ),
            const SizedBox(width: 6),
          ],
          GuiButton(key: TrendChart.zoomOutKey, label: '−', onPressed: zoomOut),
          const SizedBox(width: 6),
          GuiButton(key: TrendChart.zoomInKey, label: '+', onPressed: zoomIn),
          const SizedBox(width: 6),
          GuiButton(
            key: TrendChart.liveKey,
            label: 'LIVE',
            variant: isLive ? GuiButtonVariant.normal : GuiButtonVariant.primary,
            onPressed: isLive ? null : goLive,
          ),
        ],
      ),
    );
  }

  /// The crosshair readout: `t` relative to the newest sample plus the nearest
  /// `U/V/W` sample. Shows `—` when the window holds no sample.
  Widget _readout(
    GuiPalette palette,
    GuiTypeScale scale,
    ChartHistory history,
    int index,
  ) {
    final live = index >= 0;
    final offset = live ? history.lastTime - history.timeAt(index) : 0.0;
    return HairlineFrame(
      key: TrendChart.readoutKey,
      variant: HairlineVariant.sub,
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 6),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Text(
            live ? 't = −${offset.toStringAsFixed(2)} s' : 't = —',
            style: scale.label.copyWith(color: palette.valueAmber),
          ),
          const SizedBox(height: 4),
          _readoutChannel(palette, scale, 'U', palette.series1, history, index, 0),
          _readoutChannel(palette, scale, 'V', palette.series2, history, index, 1),
          _readoutChannel(palette, scale, 'W', palette.series3, history, index, 2),
        ],
      ),
    );
  }

  Widget _readoutChannel(
    GuiPalette palette,
    GuiTypeScale scale,
    String name,
    Color color,
    ChartHistory history,
    int index,
    int channel,
  ) {
    final value = index < 0
        ? '—'
        : history.channelAt(channel, index).toStringAsFixed(3);
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: <Widget>[
        Text(name, style: scale.label.copyWith(color: color)),
        const SizedBox(width: 6),
        Text(
          value,
          style: scale.data.copyWith(
            fontSize: 11,
            color: palette.fgStrong,
            shadows: const <Shadow>[],
          ),
        ),
      ],
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

/// Logarithmic span slider in the TNO muted vocabulary: a 3px track (lineDim
/// base / accent active segment) and a square 14×20 thumb (bgPanel fill with a
/// 1.5px accent border, no glow). Its position maps logarithmically onto
/// `[min, max]`, so every octave of the `0.5..60 s` range gets equal travel.
class _SpanSlider extends StatelessWidget {
  const _SpanSlider({
    super.key,
    required this.value,
    required this.min,
    required this.max,
    required this.onChanged,
  });

  final double value;
  final double min;
  final double max;
  final ValueChanged<double> onChanged;

  double _spanFor(double fraction) {
    final lo = math.log(min);
    final hi = math.log(max);
    return math.exp(lo + fraction.clamp(0.0, 1.0) * (hi - lo));
  }

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final lo = math.log(min);
    final hi = math.log(max);
    final fraction = ((math.log(value) - lo) / (hi - lo)).clamp(0.0, 1.0);
    return LayoutBuilder(
      builder: (context, constraints) {
        final width = constraints.maxWidth;
        void setFrom(double dx) {
          if (width <= 0) return;
          onChanged(_spanFor(dx / width));
        }

        return MouseRegion(
          cursor: SystemMouseCursors.click,
          child: GestureDetector(
            behavior: HitTestBehavior.opaque,
            onTapDown: (details) => setFrom(details.localPosition.dx),
            onHorizontalDragStart: (details) =>
                setFrom(details.localPosition.dx),
            onHorizontalDragUpdate: (details) =>
                setFrom(details.localPosition.dx),
            child: SizedBox(
              width: width,
              height: 20,
              child: CustomPaint(
                painter: _SpanSliderPainter(
                  fraction: fraction,
                  palette: palette,
                ),
              ),
            ),
          ),
        );
      },
    );
  }
}

/// Paints the [_SpanSlider] track and thumb. Flat lines only — no glow.
class _SpanSliderPainter extends CustomPainter {
  _SpanSliderPainter({required this.fraction, required this.palette});

  final double fraction;
  final GuiPalette palette;

  /// Track weight [px] (design-system thin line).
  static const double trackWidth = 3;

  /// Thumb size [px]: tall square block, never rounded.
  static const Size thumbSize = Size(14, 20);

  @override
  void paint(Canvas canvas, Size size) {
    final midY = size.height / 2;
    final track = Paint()
      ..color = palette.lineDim
      ..strokeWidth = trackWidth
      ..strokeCap = StrokeCap.square;
    canvas.drawLine(Offset(0, midY), Offset(size.width, midY), track);

    final thumbX = thumbSize.width / 2 + fraction * (size.width - thumbSize.width);
    final active = Paint()
      ..color = palette.accent
      ..strokeWidth = trackWidth
      ..strokeCap = StrokeCap.square;
    canvas.drawLine(Offset(0, midY), Offset(thumbX, midY), active);

    final rect = Rect.fromCenter(
      center: Offset(thumbX, midY),
      width: thumbSize.width,
      height: thumbSize.height,
    );
    canvas.drawRect(rect, Paint()..color = palette.bgPanel);
    canvas.drawRect(
      rect,
      Paint()
        ..color = palette.accent
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1.5,
    );
  }

  @override
  bool shouldRepaint(covariant _SpanSliderPainter oldDelegate) =>
      oldDelegate.fraction != fraction || oldDelegate.palette != palette;
}

/// Paints the 1 kHz trend: a bidirectional grid plus one flat polyline per
/// channel, with a symmetric auto-scaled y axis (time on x). Each channel is
/// decimated to one min/max pair per pixel column so peaks survive. An optional
/// mouse-following crosshair is drawn on top.
class TrendChartPainter extends CustomPainter {
  TrendChartPainter({
    required this.history,
    required this.colors,
    required this.gridColor,
    required this.viewStartSec,
    required this.viewSpanSec,
    this.cursor,
    this.cursorColor,
  });

  /// Timestamped ring the polyline is sliced from.
  final ChartHistory history;

  /// Line colour per channel (series1/2/3); flat — no glow.
  final List<Color> colors;

  /// Grid line colour (the palette's dark cyan seam).
  final Color gridColor;

  /// Viewport left edge [s] (oldest visible sample time).
  final double viewStartSec;

  /// Viewport width [s].
  final double viewSpanSec;

  /// Crosshair position as a normalized fraction of the plot area, or null.
  final Offset? cursor;

  /// Crosshair colour ([GuiPalette.fgDim]); null hides the crosshair.
  final Color? cursorColor;

  /// Thin line weight (design-system): 1.5px, never scaled for emphasis.
  static const double strokeWidth = 1.5;

  /// Grid pitch in logical pixels.
  static const double gridStep = 28;

  /// Minimum symmetric y half-range, so an all-zero trace does not collapse
  /// onto the mid line.
  static const double minYLimit = 1;

  /// Symmetric y half-range over the visible window: the largest finite
  /// |sample| across all channels, clamped to at least [minYLimit].
  static double yLimit(
    ChartHistory history,
    double startSec,
    double spanSec,
  ) {
    final first = history.lowerBoundTime(startSec);
    final last = history.lowerBoundTime(startSec + spanSec);
    var maxAbs = 0.0;
    for (var i = first; i < last; i++) {
      for (var channel = 0; channel < ChartHistory.channelCount; channel++) {
        final magnitude = history.channelAt(channel, i).abs();
        if (magnitude.isFinite && magnitude > maxAbs) maxAbs = magnitude;
      }
    }
    return maxAbs < minYLimit ? minYLimit : maxAbs;
  }

  /// Oldest-first index of the sample nearest (in arrival time) to [seconds],
  /// or -1 when the ring is empty. Shares the [ChartHistory.lowerBoundTime]
  /// conversion the decimator uses, so cursor and trace agree on the sample.
  static int nearestIndex(ChartHistory history, double seconds) {
    final len = history.len;
    if (len == 0) return -1;
    final at = history.lowerBoundTime(seconds);
    if (at <= 0) return 0;
    if (at >= len) return len - 1;
    final before = history.timeAt(at - 1);
    final after = history.timeAt(at);
    return (seconds - before) <= (after - seconds) ? at - 1 : at;
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
    final limit = yLimit(history, viewStartSec, viewSpanSec);
    final paints = buildSeriesPaints();
    for (var channel = 0; channel < colors.length; channel++) {
      _paintSeries(canvas, channel, size, paints[channel], limit);
    }
    _paintCrosshair(canvas, size);
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

  /// Draws the mouse-following crosshair: a dashed 1px vertical (fgDim) and a
  /// weaker 1px horizontal guide.
  void _paintCrosshair(Canvas canvas, Size size) {
    final at = cursor;
    final color = cursorColor;
    if (at == null || color == null) return;
    final x = at.dx * size.width;
    final y = at.dy * size.height;
    final vertical = Paint()
      ..color = color
      ..strokeWidth = 1;
    const dash = 4.0;
    for (var start = 0.0; start < size.height; start += dash * 2) {
      final end = math.min(start + dash, size.height);
      canvas.drawLine(Offset(x, start), Offset(x, end), vertical);
    }
    final horizontal = Paint()
      ..color = color.withValues(alpha: 0.5)
      ..strokeWidth = 1;
    canvas.drawLine(Offset(0, y), Offset(size.width, y), horizontal);
  }

  /// Decimates one channel over the viewport to at most one min/max pair per
  /// pixel column.
  ///
  /// [history] is sliced by the time window `[startSec, startSec + spanSec)`;
  /// `ceil(size.width)` columns each cover an equal slice. Keeping the two
  /// extremes per slice preserves peaks a naive every-nth-sample downsample
  /// would drop. Returns the plotted vertices in draw order (≤ 2 per column);
  /// a window holding fewer than two samples is empty.
  static List<Offset> seriesPoints(
    ChartHistory history,
    int channel,
    Size size,
    double startSec,
    double spanSec,
    double limit,
  ) {
    final columns = size.width.ceil();
    if (columns <= 0 || spanSec <= 0) return const <Offset>[];
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
          Offset(xOfTime(history.timeAt(i)), yOf(history.channelAt(channel, i))),
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
        final value = history.channelAt(channel, i);
        if (value < history.channelAt(channel, minIndex)) minIndex = i;
        if (value > history.channelAt(channel, maxIndex)) maxIndex = i;
      }
      final x = (column + 0.5) * size.width / columns;
      // Draw the extremes in time order so the envelope reads as one trace.
      final lower = minIndex <= maxIndex ? minIndex : maxIndex;
      final upper = minIndex <= maxIndex ? maxIndex : minIndex;
      points.add(Offset(x, yOf(history.channelAt(channel, lower))));
      if (upper != lower) {
        points.add(Offset(x, yOf(history.channelAt(channel, upper))));
      }
    }
    return points;
  }

  void _paintSeries(
    Canvas canvas,
    int channel,
    Size size,
    Paint paint,
    double limit,
  ) {
    final points = seriesPoints(
      history,
      channel,
      size,
      viewStartSec,
      viewSpanSec,
      limit,
    );
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
    if (oldDelegate.viewStartSec != viewStartSec) return true;
    if (oldDelegate.viewSpanSec != viewSpanSec) return true;
    if (oldDelegate.cursor != cursor) return true;
    if (oldDelegate.cursorColor != cursorColor) return true;
    if (oldDelegate.history.version != history.version) return true;
    return false;
  }
}
