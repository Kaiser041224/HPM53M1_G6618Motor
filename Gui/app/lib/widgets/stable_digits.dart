import 'dart:async';

import 'package:flutter/material.dart';

/// Refresh rate [Hz] for human-readable numeric monitor text.
///
/// The widget tree is notified at `kUiRefreshHz` (25 Hz) so charts and cursor
/// feedback stay smooth; monitor readouts only need to be legible, so
/// [StableDigits] samples them at this reduced rate instead. 5 Hz is calm
/// enough to stop a counter or a current reading from visibly flickering, yet
/// fast enough to read as "live".
const int kTextRefreshHz = 5;

/// Period between accepted value updates: `1000 / kTextRefreshHz` = 200 ms.
const Duration kTextRefreshPeriod = Duration(
  milliseconds: 1000 ~/ kTextRefreshHz,
);

/// Refresh rate [Hz] for the trend chart's data-driven repaint.
///
/// The chart plot is far denser than a text readout, so it is throttled to a
/// gentler 10 Hz — smooth enough to read as live, calm enough that the eye is
/// not asked to track a 25 Hz repaint. Pointer interaction bypasses this gate
/// (see `TrendChart`): only the data-driven cadence is limited.
const int kChartRefreshHz = 10;

/// Period between accepted chart data updates: `1000 / kChartRefreshHz` = 100 ms.
const Duration kChartRefreshPeriod = Duration(
  milliseconds: 1000 ~/ kChartRefreshHz,
);

/// Throttles the rebuild of an arbitrary subtree to [period].
///
/// The parent rebuilds at the full UI cadence (25 Hz) and hands the latest
/// state in [refreshKey] together with a [builder] that closes over the current
/// values. The builder's subtree is cached and only rebuilt once per [period]
/// when [refreshKey] differs from the last accepted key; the very first subtree
/// is built immediately. Returning the same cached instance on non-tick frames
/// lets Flutter short-circuit the child update, which is what actually keeps
/// the subtree off the 25 Hz path.
///
/// [period] defaults to [kTextRefreshPeriod] (5 Hz); pass
/// [kChartRefreshPeriod] for the trend chart's 10 Hz data cadence.
/// [StableDigits] is the numeric specialisation built on top of this.
class StableRefresh extends StatefulWidget {
  const StableRefresh({
    super.key,
    required this.refreshKey,
    required this.builder,
    this.period = kTextRefreshPeriod,
  });

  /// Latest refresh key. The subtree is only rebuilt when this changes and the
  /// next [period] tick has elapsed.
  final Object? refreshKey;

  /// Builds the throttled subtree from the values captured in its closure.
  final WidgetBuilder builder;

  /// Refresh period; defaults to the 5 Hz [kTextRefreshPeriod].
  final Duration period;

  @override
  State<StableRefresh> createState() => _StableRefreshState();
}

class _StableRefreshState extends State<StableRefresh> {
  /// The key currently on screen; [widget.refreshKey] only replaces it on a
  /// tick.
  late Object? _shownKey;

  /// Period sampler. Cancelled in [dispose] so widget tests leave no pending
  /// timer and the tree stops rebuilding once unmounted.
  late Timer _timer;

  /// Cached subtree, rebuilt only when [_pending] is set.
  Widget? _cached;

  /// Set on an accepted tick to force one subtree rebuild on the next build.
  bool _pending = true;

  @override
  void initState() {
    super.initState();
    _shownKey = widget.refreshKey;
    _timer = Timer.periodic(widget.period, _onTick);
  }

  @override
  void didUpdateWidget(covariant StableRefresh oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.period != widget.period) {
      _timer.cancel();
      _timer = Timer.periodic(widget.period, _onTick);
    }
  }

  @override
  void dispose() {
    _timer.cancel();
    super.dispose();
  }

  void _onTick(Timer timer) {
    if (widget.refreshKey == _shownKey) return;
    setState(() {
      _shownKey = widget.refreshKey;
      _pending = true;
    });
  }

  @override
  Widget build(BuildContext context) {
    if (_cached == null || _pending) {
      _cached = widget.builder(context);
      _pending = false;
    }
    return _cached!;
  }
}

/// Throttles the refresh of a monitor value to [kTextRefreshHz].
///
/// A [StableRefresh] keyed on [text]. Two shapes:
/// * plain — pass [text] plus an optional [style] and it renders a [Text];
/// * builder — pass a [builder] that paints a richer readout (for example a
///   `GlowValue` number + unit pair). Its subtree is only rebuilt on an
///   accepted update, so a 25 Hz parent feed does not repaint it 25 times a
///   second.
class StableDigits extends StatelessWidget {
  const StableDigits({
    super.key,
    required this.text,
    this.style,
    this.textAlign,
    this.maxLines,
    this.overflow,
    this.builder,
  });

  /// Latest value. Used as the refresh key, and as the rendered string when
  /// [builder] is null.
  final String text;

  /// Base style for the plain [Text] form.
  final TextStyle? style;

  /// Horizontal alignment for the plain [Text] form.
  final TextAlign? textAlign;

  /// Optional line cap for the plain [Text] form.
  final int? maxLines;

  /// Overflow policy for the plain [Text] form.
  final TextOverflow? overflow;

  /// Builds the throttled content. When provided, [style], [textAlign],
  /// [maxLines] and [overflow] are ignored.
  final Widget Function(BuildContext context)? builder;

  @override
  Widget build(BuildContext context) {
    final builder = this.builder;
    if (builder != null) {
      return StableRefresh(refreshKey: text, builder: builder);
    }
    return StableRefresh(
      refreshKey: text,
      builder: (context) => Text(
        text,
        style: style,
        textAlign: textAlign,
        maxLines: maxLines,
        overflow: overflow,
      ),
    );
  }
}
