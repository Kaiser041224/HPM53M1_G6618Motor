import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/stable_digits.dart';
import '../widgets/stale_gate.dart' show Freshness;
import '../widgets/status_badge.dart';
import 'label_style.dart';

/// Chinese display name of a status tile, keyed by its [AppState] tile suffix
/// (mirrors the captions in `status_zone.dart`).
const Map<String, String> _tileNames = <String, String>{
  'VBUS': '母线电压',
  'I_IN': '输入电流',
  'P_IN': '输入功率',
  'NTC': 'NTC PCB',
  'NTC_STATOR': 'NTC 定子',
  'I_U': 'U 相电流',
  'I_V': 'V 相电流',
  'I_W': 'W 相电流',
  'ENC_R': '转子角',
  'ENC_O': '出轴角',
  'F_ELEC': '电频率',
};

/// Number of set bits in [mask].
int _bitCount(int mask) {
  var bits = mask;
  var count = 0;
  while (bits != 0) {
    count += bits & 1;
    bits >>= 1;
  }
  return count;
}

/// Formats a session duration as `HH:MM:SS` (hours may exceed two digits).
String formatUptime(Duration d) {
  String two(int v) => v.toString().padLeft(2, '0');
  return '${two(d.inHours)}:${two(d.inMinutes % 60)}:${two(d.inSeconds % 60)}';
}

/// Persistent bottom strip: link verdict, transport counters and telemetry.
///
/// Left: a [StatusBadge] (color + icon + text, design-system §6 rule 1) naming
/// the link verdict — `READY`, `DEGRADED` or `未连接` — with a plain-language
/// explanation of any degradation. Right: the link counters and the telemetry
/// migrated down from the top bar (POLL / UPTIME / SEQ / 帧) merged with the
/// existing RTT / CRC / RESYNC / RETRY / DIRTY strip. When the strip is narrow
/// the badge outranks the counters, which in turn outrank the frame / SEQ tail,
/// so the tail drops first instead of overflowing. Values the protocol does not
/// provide (SEQ) stay `—`; every value that is unknown or aged out also
/// degrades to `—`, never a fabricated `0` (design-system §6 rule 2).
class BottomBar extends StatelessWidget {
  const BottomBar({super.key, required this.state});

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
    return Container(
      decoration: BoxDecoration(
        color: palette.bgPanel,
        border: Border(
          top: BorderSide(color: palette.lineHi, width: palette.lineWidth),
        ),
      ),
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
      child: LayoutBuilder(
        builder: (context, constraints) {
          // The verdict badge outranks every counter, and the counters outrank
          // the tail SEQ / 帧 telemetry migrated down from the top bar. Reserve
          // a floor for the badge, then hand the remainder to the counter strip,
          // which drops its least-important trailing segments when tight.
          final budget = constraints.maxWidth - _badgeReserve;
          return Row(
            children: <Widget>[
              Expanded(child: _status(context)),
              const SizedBox(width: 16),
              _telemetry(context, budget),
            ],
          );
        },
      ),
    );
  }

  /// Horizontal room reserved for the verdict badge + its detail before any
  /// counter may claim space (design-system §4.4 priority).
  static const double _badgeReserve = 260;

  // ── left: verdict ────────────────────────────────────────────────────────

  Widget _status(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final (ItemState item, String label, String detail) = _verdict();

    return Row(
      mainAxisSize: MainAxisSize.min,
      children: <Widget>[
        Container(
          key: const Key('bottom_bar_badge'),
          padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
          decoration: BoxDecoration(
            border: Border.all(
              color: palette.lineDim,
              width: palette.lineWidth,
            ),
          ),
          child: StatusBadge(state: item, label: label),
        ),
        const SizedBox(width: 10),
        Flexible(
          child: Text(
            detail,
            key: const Key('bottom_bar_detail'),
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            // 中文落在像素网格上（10 为 Fusion Pixel 小号原生档）。
            style: scale.body.copyWith(
              fontSize: GuiTypeScale.pixelSize,
              color: palette.fgDim,
            ),
          ),
        ),
      ],
    );
  }

  /// Derives the badge state, its uppercase label and the explanation text.
  (ItemState, String, String) _verdict() {
    if (!state.connected) {
      return (ItemState.stale, '未连接', '设备未连接');
    }

    // READY is a claim about live telemetry: require at least one status tile
    // to have ever been read. Connected-but-no-status-yet is a neutral "waiting
    // for data", never a fabricated READY over a bare 0 (design-system §6
    // rule 2).
    final everRead = AppState.statusTileKeys.any(
      (key) => state.freshnessOf(key) != Freshness.unknown,
    );
    if (!everRead) {
      return (ItemState.pending, '等待数据', '尚未收到状态数据');
    }

    final stale = state.staleStatusTiles;
    final latched = state.latchedCount;
    final live = state.status?.faultFlags ?? 0;
    if (stale.isEmpty && latched == 0 && live == 0) {
      return (ItemState.normal, 'READY', '链路正常 · 无故障');
    }

    final reasons = <String>[
      for (final key in stale)
        '${_tileNames[key.substring('status_tile_'.length)] ?? key}失效',
    ];
    if (live != 0) reasons.add('${_bitCount(live)} 项故障');
    if (latched != 0) reasons.add('${_bitCount(latched)} 项锁存');

    final item = latched != 0
        ? ItemState.latched
        : (live != 0 ? ItemState.fault : ItemState.warn);
    return (item, 'DEGRADED', reasons.join(' · '));
  }

  // ── right: counters ──────────────────────────────────────────────────────

  Widget _telemetry(BuildContext context, double maxWidth) {
    final n = state.connected;
    final rttLive = state.freshnessOf(AppState.keyRtt) == Freshness.live;
    final rtt = (n && rttLive && state.rttMs != null)
        ? '${state.rttMs!.toStringAsFixed(1)} MS'
        : '—';
    // Every polled counter is gated by its own freshness, exactly like RTT:
    // a failed poll ages the value out to `—` rather than passing an old
    // reading off as live (design-system §6 rule 2). DIRTY is client-local
    // state (not a polled reading), so it is gated only on the connection.
    final crcLive = state.freshnessOf(AppState.keyCrc) == Freshness.live;
    final resyncLive = state.freshnessOf(AppState.keyResync) == Freshness.live;
    final retryLive = state.freshnessOf(AppState.keyRetry) == Freshness.live;
    final framesLive = state.freshnessOf(AppState.keyFrames) == Freshness.live;

    // POLL / UPTIME / SEQ / 帧 migrated here from the top bar; the order keeps
    // the counters first and the frame/SEQ tail last so the tail drops first
    // when the strip runs out of room.
    final poll = n ? '500 ms' : '—';
    final uptime = (n && state.sessionUptime != null)
        ? formatUptime(state.sessionUptime!)
        : '—';

    final segments = <(String, String, String)>[
      ('bottom_bar_poll', 'POLL', poll),
      ('bottom_bar_uptime', 'UPTIME', uptime),
      ('bottom_bar_rtt', 'RTT', rtt),
      ('bottom_bar_crc', 'CRC', n && crcLive ? '${state.crcErrors}' : '—'),
      (
        'bottom_bar_resync',
        'RESYNC',
        n && resyncLive ? '${state.reconnects}' : '—',
      ),
      (
        'bottom_bar_retry',
        'RETRY',
        n && retryLive ? '${state.requestFailures}' : '—',
      ),
      ('bottom_bar_dirty', 'DIRTY', n ? '${state.dirty.length}' : '—'),
      ('bottom_bar_seq', 'SEQ', '—'),
      ('bottom_bar_frames', '帧', n && framesLive ? '${state.attempts}' : '—'),
    ];

    final visible = _fitSegments(context, segments, maxWidth);
    final children = <Widget>[];
    for (var i = 0; i < visible.length; i++) {
      if (i > 0) children.add(_separator(context));
      children.add(
        _segment(context, visible[i].$1, visible[i].$2, visible[i].$3),
      );
    }
    return Row(mainAxisSize: MainAxisSize.min, children: children);
  }

  /// Keeps the leading segments that fit in [maxWidth], dropping the trailing
  /// (least important) ones so the strip degrades instead of overflowing.
  List<(String, String, String)> _fitSegments(
    BuildContext context,
    List<(String, String, String)> all,
    double maxWidth,
  ) {
    final scale = GuiTypeScale.of(context);
    final palette = GuiPalette.of(context);
    final labelStyle = guiLabelStyle(palette, scale);
    final valueStyle = scale.data.copyWith(
      fontSize: 12,
      color: palette.fgStrong,
      fontFeatures: const [FontFeature.tabularFigures()],
    );
    const gap = 6.0;
    const separator = 24.0; // 8px padding either side + the 8px pipe glyph.
    var used = 0.0;
    final out = <(String, String, String)>[];
    for (final segment in all) {
      final width =
          _measure(segment.$2, labelStyle) +
          gap +
          _measure(segment.$3, valueStyle) +
          (out.isEmpty ? 0.0 : separator);
      if (out.isNotEmpty && used + width > maxWidth) break;
      used += width;
      out.add(segment);
    }
    return out;
  }

  /// Lays [text] out with [style] and returns its width, for the counter fit.
  double _measure(String text, TextStyle style) {
    final painter = TextPainter(
      text: TextSpan(text: text, style: style),
      textDirection: TextDirection.ltr,
    )..layout();
    return painter.width;
  }

  Widget _separator(BuildContext context) => Padding(
    padding: const EdgeInsets.symmetric(horizontal: 8),
    child: Text('|', style: GuiTypeScale.of(context).label),
  );

  Widget _segment(
    BuildContext context,
    String key,
    String label,
    String value,
  ) {
    final scale = GuiTypeScale.of(context);
    final palette = GuiPalette.of(context);
    final known = value != '—';
    return Row(
      key: Key(key),
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.baseline,
      textBaseline: TextBaseline.alphabetic,
      children: <Widget>[
        Text(label, style: guiLabelStyle(palette, scale)),
        const SizedBox(width: 6),
        // Monitor counter: sample at 5 Hz. The style is rebuilt with the
        // accepted string so a `—` and a live number never share a look.
        StableDigits(
          text: value,
          builder: (context) => Text(
            value,
            // v3: counters are flat neutral; only the verdict badge carries
            // status colour.
            style: known
                ? scale.data.copyWith(
                    fontSize: 12,
                    color: palette.fgStrong,
                    fontFeatures: const [FontFeature.tabularFigures()],
                  )
                : scale.label.copyWith(color: palette.fgMute),
          ),
        ),
      ],
    );
  }
}
