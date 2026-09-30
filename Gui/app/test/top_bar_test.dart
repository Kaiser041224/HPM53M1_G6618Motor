import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/top_bar.dart';

ParamMetaDto makeMeta(int index, String name) => ParamMetaDto(
  index: index,
  name: name,
  ty: 0,
  apply: 0,
  unit: '',
  min: 0,
  max: 0,
);

/// Pumps [TopBar] on the Gui theme at a wide surface so the device art and the
/// telemetry column both fit without overflow.
Future<void> _pump(WidgetTester tester, AppState state) async {
  tester.view.physicalSize = const Size(1500, 900);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  await tester.pumpWidget(
    MaterialApp(theme: buildGuiTheme(), home: Scaffold(body: TopBar(state: state))),
  );
}

void main() {
  testWidgets('未连接：设备标识为 DEVICE ID PENDING，选项卡未建立', (
    WidgetTester tester,
  ) async {
    final state = AppState.test();
    addTearDown(state.dispose);

    await _pump(tester, state);

    expect(find.text('DEVICE ID PENDING'), findsOneWidget);
    expect(find.textContaining('未建立'), findsOneWidget);
    expect(find.textContaining('TCP'), findsNothing);
  });

  testWidgets('未连接：UPTIME / SEQ / 帧 均诚实显示 —', (
    WidgetTester tester,
  ) async {
    final state = AppState.test();
    addTearDown(state.dispose);

    await _pump(tester, state);

    for (final entry in const <(Key, String)>[
      (Key('top_bar_poll_value'), 'POLL'),
      (Key('top_bar_uptime_value'), 'UPTIME'),
      (Key('top_bar_seq_value'), 'SEQ'),
      (Key('top_bar_frames_value'), '帧'),
    ]) {
      expect(
        find.descendant(of: find.byKey(entry.$1), matching: find.text('—')),
        findsOneWidget,
        reason: '${entry.$2} 应显示 —',
      );
    }
  });

  testWidgets('已连接：显示 PROTO_INFO 型号与协议号，不再显示占位', (
    WidgetTester tester,
  ) async {
    final state = AppState.test(
      connected: true,
      deviceName: 'G6618 Motor Controller',
      protoMajor: 1,
      protoMinor: 0,
      capabilities: 0x03,
      sessionUptime: const Duration(seconds: 30),
      attempts: 14288,
    );
    addTearDown(state.dispose);

    await _pump(tester, state);

    expect(find.text('DEVICE ID PENDING'), findsNothing);
    expect(find.text('G6618 Motor Controller'), findsOneWidget);
    expect(find.textContaining('PROTO v1.0'), findsOneWidget);
    expect(find.textContaining('TCP'), findsOneWidget);
    expect(find.text('FW — · SERIAL 未提供'), findsOneWidget);
    expect(find.textContaining('FW v0.1.0'), findsNothing);
  });

  testWidgets('已连接：UPTIME 为会话时长 HH:MM:SS，帧为 attempts', (
    WidgetTester tester,
  ) async {
    final state = AppState.test(
      connected: true,
      deviceName: 'G6618 Motor Controller',
      protoMajor: 1,
      capabilities: 0x03,
      sessionUptime: const Duration(hours: 0, minutes: 14, seconds: 22),
      attempts: 14288,
    );
    addTearDown(state.dispose);

    await _pump(tester, state);

    expect(
      find.descendant(
        of: find.byKey(const Key('top_bar_uptime_value')),
        matching: find.text('00:14:22'),
      ),
      findsOneWidget,
    );
    expect(
      find.descendant(
        of: find.byKey(const Key('top_bar_frames_value')),
        matching: find.text('14288'),
      ),
      findsOneWidget,
    );
  });

  testWidgets('SEQ 协议未提供：即使已连接也显示 —，禁止估算', (
    WidgetTester tester,
  ) async {
    final state = AppState.test(
      connected: true,
      deviceName: 'G6618 Motor Controller',
      protoMajor: 1,
      capabilities: 0x03,
      sessionUptime: const Duration(seconds: 5),
      attempts: 12,
    );
    addTearDown(state.dispose);

    await _pump(tester, state);

    expect(
      find.descendant(
        of: find.byKey(const Key('top_bar_seq_value')),
        matching: find.text('—'),
      ),
      findsOneWidget,
    );
  });

  testWidgets('点击连接选项卡展开面板：含 TRANSPORT / ADDRESS / HANDSHAKE / LINK QUALITY', (
    WidgetTester tester,
  ) async {
    final state = AppState.test();
    addTearDown(state.dispose);

    await _pump(tester, state);
    expect(find.text('TRANSPORT'), findsNothing);

    await tester.tap(find.byKey(const Key('top_bar_connect_tab')));
    await tester.pumpAndSettle();

    expect(find.text('TRANSPORT'), findsOneWidget);
    expect(find.text('ADDRESS'), findsOneWidget);
    expect(find.text('HANDSHAKE'), findsOneWidget);
    expect(find.text('LINK QUALITY'), findsOneWidget);
  });

  testWidgets('PARAM META 为单计数（N 项），不显示 N / N', (
    WidgetTester tester,
  ) async {
    final state = AppState.test(
      metas: [makeMeta(0, 'a'), makeMeta(1, 'b'), makeMeta(2, 'c')],
    );
    addTearDown(state.dispose);

    await _pump(tester, state);
    await tester.tap(find.byKey(const Key('top_bar_connect_tab')));
    await tester.pumpAndSettle();

    expect(find.text('3 项'), findsOneWidget);
    expect(find.text('3 / 3'), findsNothing);
  });

  testWidgets('断开：CRC / 重连协议无数据，诚实显示 — 而非 0', (
    WidgetTester tester,
  ) async {
    final state = AppState.test();
    addTearDown(state.dispose);

    await _pump(tester, state);
    await tester.tap(find.byKey(const Key('top_bar_connect_tab')));
    await tester.pumpAndSettle();

    expect(find.text('0'), findsNothing);
  });

  testWidgets('已连接：CRC / 重连显示设备计数', (WidgetTester tester) async {
    final state = AppState.test(
      connected: true,
      deviceName: 'G6618 Motor Controller',
      protoMajor: 1,
      protoMinor: 2,
      capabilities: 0x03,
      sessionUptime: const Duration(seconds: 1),
      attempts: 7,
    )..crcErrors = 3
     ..reconnects = 2;
    addTearDown(state.dispose);

    await _pump(tester, state);
    await tester.tap(find.byKey(const Key('top_bar_connect_tab')));
    await tester.pumpAndSettle();

    expect(find.text('3'), findsOneWidget);
    expect(find.text('2'), findsOneWidget);
  });

  test('formatUptime renders HH:MM:SS', () {
    expect(formatUptime(Duration.zero), '00:00:00');
    expect(
      formatUptime(const Duration(hours: 2, minutes: 5, seconds: 9)),
      '02:05:09',
    );
    expect(
      formatUptime(const Duration(hours: 100, minutes: 0, seconds: 1)),
      '100:00:01',
    );
  });
}
