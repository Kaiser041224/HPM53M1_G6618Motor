import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/stable_digits.dart';
import '../widgets/stale_gate.dart' show Freshness;
import '../widgets/status_badge.dart';

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

/// Persistent bottom strip: link verdict, transport counters and telemetry.
///
/// Left: a [StatusBadge] (color + icon + text, design-system §6 rule 1) naming
/// the link verdict — `READY`, `DEGRADED` or `未连接` — with a plain-language
/// explanation of any degradation. Right: the link counters and telemetry.
/// Values the protocol does not provide (SEQ) stay `—`; every value that is
/// unknown or aged out also degrades to `—`, never a fabricated `0`
/// (design-system §6 rule 2).
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
      child: Row(
        children: <Widget>[
          _status(context),
          const Spacer(),
          _telemetry(context),
        ],
      ),
    );
  }

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
            border: Border.all(color: palette.lineDim, width: palette.lineWidth),
          ),
          child: StatusBadge(state: item, label: label),
        ),
        const SizedBox(width: 10),
        Text(
          detail,
          key: const Key('bottom_bar_detail'),
          style: scale.body.copyWith(fontSize: 11, color: palette.fgDim),
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

  Widget _telemetry(BuildContext context) {
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
    final resyncLive =
        state.freshnessOf(AppState.keyResync) == Freshness.live;
    final retryLive = state.freshnessOf(AppState.keyRetry) == Freshness.live;
    final framesLive = state.freshnessOf(AppState.keyFrames) == Freshness.live;

    final segments = <(String, String, String)>[
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
      (
        'bottom_bar_frames',
        '帧',
        n && framesLive ? '${state.attempts}' : '—',
      ),
    ];

    final children = <Widget>[];
    for (var i = 0; i < segments.length; i++) {
      if (i > 0) children.add(_separator(context));
      children.add(
        _segment(context, segments[i].$1, segments[i].$2, segments[i].$3),
      );
    }
    return Row(mainAxisSize: MainAxisSize.min, children: children);
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
        Text(label, style: scale.label),
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
