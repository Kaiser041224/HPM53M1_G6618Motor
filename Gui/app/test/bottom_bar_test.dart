import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/bottom_bar.dart';
import 'package:g6618_gui/widgets/status_badge.dart';

StatusDto makeStatus({int faultFlags = 0}) => StatusDto(
  faultFlags: faultFlags,
  motorRunning: false,
  motorState: 0,
  freqHz: 0,
  modPct: 0,
  iU: 0,
  iV: 0,
  iW: 0,
  vbus: 0,
  ntcTemp: 0,
  encRotorDeg: 0,
  encOutputDeg: 0,
);

/// Pumps [BottomBar] on the Gui theme at a wide surface so all counters fit.
Future<void> _pump(WidgetTester tester, AppState state) async {
  tester.view.physicalSize = const Size(1800, 600);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  await tester.pumpWidget(
    MaterialApp(
      theme: buildGuiTheme(),
      home: Scaffold(body: BottomBar(state: state)),
    ),
  );
}

/// Reads the value Text rendered inside the counter keyed [key].
Finder _value(String key, String text) =>
    find.descendant(of: find.byKey(Key(key)), matching: find.text(text));

void main() {
  testWidgets('未连接：徽标为「未连接」，计数与 SEQ 全为 —，不冒充 0', (tester) async {
    final state = AppState.test();
    addTearDown(state.dispose);

    await _pump(tester, state);

    expect(find.text('未连接'), findsOneWidget);
    for (final key in const <String>[
      'bottom_bar_rtt',
      'bottom_bar_crc',
      'bottom_bar_resync',
      'bottom_bar_retry',
      'bottom_bar_dirty',
      'bottom_bar_seq',
      'bottom_bar_frames',
    ]) {
      expect(_value(key, '—'), findsOneWidget, reason: '$key 应显示 —');
    }
    expect(find.text('0'), findsNothing);
  });

  testWidgets('已连接且健康：READY，计数显示真实 0 而非 —', (tester) async {
    final state = AppState.test(
      connected: true,
      status: makeStatus(),
      deviceName: 'G6618 Motor Controller',
      attempts: 14208,
      rttMs: 12.4,
    );
    addTearDown(state.dispose);

    await _pump(tester, state);

    expect(find.text('READY'), findsOneWidget);
    expect(_value('bottom_bar_rtt', '12.4 MS'), findsOneWidget);
    expect(_value('bottom_bar_crc', '0'), findsOneWidget);
    // v3: counters are flat neutral, no glow.
    final crc = tester.widget<Text>(_value('bottom_bar_crc', '0'));
    expect(crc.style?.color, const GuiPalette().fgStrong);
    expect(crc.style?.shadows, anyOf(isNull, isEmpty));
    expect(_value('bottom_bar_resync', '0'), findsOneWidget);
    expect(_value('bottom_bar_retry', '0'), findsOneWidget);
    expect(_value('bottom_bar_dirty', '0'), findsOneWidget);
    // SEQ is not provided by the protocol: honestly `—` even when connected.
    expect(_value('bottom_bar_seq', '—'), findsOneWidget);
    expect(_value('bottom_bar_frames', '14208'), findsOneWidget);
  });

  testWidgets('已连接但 rtt 超时：RTT 降级为 —，其余计数仍在', (tester) async {
    final state = AppState.test(connected: true, status: makeStatus(), rttMs: 9.9);
    addTearDown(state.dispose);
    state.seedFreshness(
      AppState.keyRtt,
      DateTime.now().subtract(const Duration(seconds: 2)),
    );

    await _pump(tester, state);

    expect(_value('bottom_bar_rtt', '—'), findsOneWidget);
    expect(_value('bottom_bar_crc', '0'), findsOneWidget);
  });

  testWidgets('已连接 + 失效瓦片：DEGRADED 并点名失效通道', (tester) async {
    final state = AppState.test(connected: true, status: makeStatus());
    addTearDown(state.dispose);
    state.seedFreshness(
      AppState.statusTileKey('I_V'),
      DateTime.now().subtract(const Duration(seconds: 2)),
    );

    await _pump(tester, state);

    expect(find.text('DEGRADED'), findsOneWidget);
    expect(find.textContaining('V 相电流失效'), findsOneWidget);
  });

  testWidgets('已连接 + 锁存故障：DEGRADED 并说明锁存', (tester) async {
    final state = AppState.test(connected: true, status: makeStatus());
    addTearDown(state.dispose);
    state.latchedFlags = 1 << 1;

    await _pump(tester, state);

    expect(find.text('DEGRADED'), findsOneWidget);
    expect(find.textContaining('锁存'), findsOneWidget);
  });

  testWidgets('状态徽标为三重编码（色 + 图标 + 文字）', (tester) async {
    final state = AppState.test(connected: true, status: makeStatus());
    addTearDown(state.dispose);

    await _pump(tester, state);

    expect(find.byType(StatusBadge), findsOneWidget);
  });

  testWidgets('已连接但轮询失败：CRC/RESYNC/RETRY/帧 一并降级为 —，不冒充实时', (
    tester,
  ) async {
    final state = AppState.test(
      connected: true,
      status: makeStatus(),
      attempts: 14208,
    );
    addTearDown(state.dispose);
    final aged = DateTime.now().subtract(const Duration(seconds: 2));
    for (final key in const <String>[
      AppState.keyCrc,
      AppState.keyResync,
      AppState.keyRetry,
      AppState.keyFrames,
    ]) {
      state.seedFreshness(key, aged);
    }

    await _pump(tester, state);

    for (final key in const <String>[
      'bottom_bar_crc',
      'bottom_bar_resync',
      'bottom_bar_retry',
      'bottom_bar_frames',
    ]) {
      expect(_value(key, '—'), findsOneWidget, reason: '$key 应降级为 —');
    }
  });

  testWidgets('已连接但尚未读到 status：不报 READY，显示等待数据', (tester) async {
    final state = AppState.test(connected: true);
    addTearDown(state.dispose);

    await _pump(tester, state);

    expect(find.text('READY'), findsNothing);
    expect(find.text('等待数据'), findsOneWidget);
  });
}
