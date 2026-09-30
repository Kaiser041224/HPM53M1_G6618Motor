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

/// Throttles the refresh of a monitor value to [kTextRefreshHz].
///
/// The parent rebuilds at the full UI cadence and hands the latest value in
/// [text] on every build. This widget keeps showing the last accepted value
/// until the next [kTextRefreshPeriod] tick, then copies [text] across in a
/// single repaint. The very first value is shown immediately.
///
/// Two shapes:
/// * plain — pass [text] plus an optional [style] and it renders a [Text];
/// * builder — pass [text] as the refresh key plus a [builder] that paints a
///   richer readout (for example a `GlowValue` number + unit pair). The
///   builder's subtree is cached and only rebuilt on an accepted update, so a
///   25 Hz parent feed does not repaint it 25 times a second.
class StableDigits extends StatefulWidget {
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
  State<StableDigits> createState() => _StableDigitsState();
}

class _StableDigitsState extends State<StableDigits> {
  /// The value currently on screen; [widget.text] is only copied here on a tick.
  late String _shown;

  /// 5 Hz sampler. Cancelled in [dispose] so widget tests leave no pending
  /// timer and the tree stops rebuilding once unmounted.
  late final Timer _timer;

  /// Cached subtree, rebuilt only when [_pending] is set. Returning the same
  /// instance from [build] lets Flutter short-circuit the child update, which
  /// is what actually keeps the painter off the 25 Hz path.
  Widget? _cached;

  /// Set on an accepted tick to force one subtree rebuild on the next build.
  bool _pending = true;

  @override
  void initState() {
    super.initState();
    _shown = widget.text;
    _timer = Timer.periodic(kTextRefreshPeriod, _onTick);
  }

  @override
  void dispose() {
    _timer.cancel();
    super.dispose();
  }

  void _onTick(Timer timer) {
    if (widget.text == _shown) return;
    setState(() {
      _shown = widget.text;
      _pending = true;
    });
  }

  @override
  Widget build(BuildContext context) {
    if (_cached == null || _pending) {
      _cached = widget.builder?.call(context) ?? _plain();
      _pending = false;
    }
    return _cached!;
  }

  Widget _plain() => Text(
    _shown,
    style: widget.style,
    textAlign: widget.textAlign,
    maxLines: widget.maxLines,
    overflow: widget.overflow,
  );
}
