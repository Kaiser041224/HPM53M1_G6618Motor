import 'dart:async';

import 'package:flutter/material.dart';

/// Refresh rate [Hz] for human-readable numeric monitor text.
///
/// The widget tree is notified at `kUiRefreshHz` (120 Hz, at or above panel
/// vsync) so the trend waveform follows the 1 kHz feed every frame; monitor
/// readouts only need to be legible, so
/// [StableDigits] samples them at this reduced rate instead. 5 Hz is calm
/// enough to stop a counter or a current reading from visibly flickering, yet
/// fast enough to read as "live".
const int kTextRefreshHz = 5;

/// Period between accepted value updates: `1000 / kTextRefreshHz` = 200 ms.
const Duration kTextRefreshPeriod = Duration(
  milliseconds: 1000 ~/ kTextRefreshHz,
);

/// Throttles the rebuild of an arbitrary subtree to [period].
///
/// The parent rebuilds at the full UI cadence (50 Hz) and hands the latest
/// state in [refreshKey] together with a [builder] that closes over the current
/// values. The builder's subtree is cached and only rebuilt once per [period]
/// when [refreshKey] differs from the last accepted key; the very first subtree
/// is built immediately. Returning the same cached instance on non-tick frames
/// lets Flutter short-circuit the child update, which is what actually keeps
/// the subtree off the full-rate path.
///
/// [period] defaults to [kTextRefreshPeriod] (5 Hz); [StableDigits] is the
/// numeric specialisation built on top of this.
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
///   accepted update, so a 50 Hz parent feed does not repaint it 50 times a
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
