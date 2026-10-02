import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/top_bar.dart';
import 'package:g6618_gui/widgets/gui_button.dart';

/// Pumps [TopBar] on the Gui theme at a wide surface so the resident connection
/// block, the device identity and the telemetry column all fit. Window-chrome
/// callbacks are injectable so the caption buttons and drag region are testable
/// without a native window.
Future<void> _pump(
  WidgetTester tester,
  AppState state, {
  Size size = const Size(1500, 900),
  VoidCallback? onMinimize,
  VoidCallback? onToggleMaximize,
  VoidCallback? onClose,
  VoidCallback? onDragStart,
}) async {
  tester.view.physicalSize = size;
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  await tester.pumpWidget(
    MaterialApp(
      theme: buildGuiTheme(),
      home: Scaffold(
        body: TopBar(
          state: state,
          onMinimize: onMinimize,
          onToggleMaximize: onToggleMaximize,
          onClose: onClose,
          onDragStart: onDragStart,
        ),
      ),
    ),
  );
}

void main() {
  testWidgets('未连接：设备标识为 DEVICE ID PENDING，连接控件常驻顶栏', (
    WidgetTester tester,
  ) async {
    final state = AppState.test();
    addTearDown(state.dispose);

    await _pump(tester, state);

    expect(find.text('DEVICE ID PENDING'), findsOneWidget);
    // The connection controls live on the bar itself, not behind a tab.
    expect(find.byKey(const Key('top_bar_connection_block')), findsOneWidget);
    expect(find.widgetWithText(GuiButton, '连接'), findsOneWidget);
    expect(find.widgetWithText(GuiButton, '断开'), findsOneWidget);
  });

  testWidgets('顶栏三区常驻：连接块 / 设备信息 / 遥测', (WidgetTester tester) async {
    final state = AppState.test();
    addTearDown(state.dispose);

    await _pump(tester, state);

    expect(find.byKey(const Key('top_bar_connection_block')), findsOneWidget);
    expect(find.byKey(const Key('top_bar_device_info')), findsOneWidget);
    expect(find.byKey(const Key('top_bar_telemetry')), findsOneWidget);
  });

  testWidgets('顶栏无展开交互：不再有连接选项卡与二级细目', (WidgetTester tester) async {
    final state = AppState.test();
    addTearDown(state.dispose);

    await _pump(tester, state);

    // No expandable connect tab survives.
    expect(find.byKey(const Key('top_bar_connect_tab')), findsNothing);
    // The handshake / link-quality details moved to the bottom bar; the top bar
    // never grows a second row of sections.
    for (final label in const <String>[
      'TRANSPORT',
      'ADDRESS',
      'HANDSHAKE',
      'LINK QUALITY',
    ]) {
      expect(find.text(label), findsNothing, reason: label);
    }
  });

  testWidgets('超宽：三区同排（连接块在左 / 设备信息居中 / 遥测在右）', (
    WidgetTester tester,
  ) async {
    final state = AppState.test();
    addTearDown(state.dispose);

    await _pump(tester, state, size: const Size(2000, 900));

    final connection = tester.getTopLeft(
      find.byKey(const Key('top_bar_connection_block')),
    );
    final info = tester.getTopLeft(
      find.byKey(const Key('top_bar_device_info')),
    );
    final telemetry = tester.getTopLeft(
      find.byKey(const Key('top_bar_telemetry')),
    );
    expect(connection.dx, lessThan(info.dx));
    expect(info.dx, lessThan(telemetry.dx));
    // All three share the same row on an ultra-wide window.
    expect((connection.dy - info.dy).abs(), lessThan(4));
    expect((info.dy - telemetry.dy).abs(), lessThan(4));
  });

  testWidgets('链路分段：TCP 可用，USB / UART 禁用并标 M2', (WidgetTester tester) async {
    final state = AppState.test();
    addTearDown(state.dispose);

    await _pump(tester, state);

    final tcp = tester.widget<GuiButton>(
      find.byKey(const Key('top_bar_link_tcp')),
    );
    expect(tcp.onPressed, isNotNull);
    final usb = tester.widget<GuiButton>(
      find.byKey(const Key('top_bar_link_usb')),
    );
    final uart = tester.widget<GuiButton>(
      find.byKey(const Key('top_bar_link_uart')),
    );
    expect(usb.onPressed, isNull);
    expect(uart.onPressed, isNull);
    expect(usb.label, contains('M2'));
    expect(uart.label, contains('M2'));
  });

  testWidgets('连接按钮 primary，未连接时断开禁用', (WidgetTester tester) async {
    final state = AppState.test();
    addTearDown(state.dispose);

    await _pump(tester, state);

    final connect = tester.widget<GuiButton>(
      find.widgetWithText(GuiButton, '连接'),
    );
    expect(connect.variant, GuiButtonVariant.primary);
    final disconnect = tester.widget<GuiButton>(
      find.widgetWithText(GuiButton, '断开'),
    );
    expect(disconnect.variant, GuiButtonVariant.normal);
    expect(disconnect.onPressed, isNull);
  });

  testWidgets('未连接：型号 / PROTO / CAP 诚实显示 —', (WidgetTester tester) async {
    final state = AppState.test();
    addTearDown(state.dispose);

    await _pump(tester, state);

    for (final key in const <Key>[
      Key('top_bar_model_value'),
      Key('top_bar_proto_value'),
      Key('top_bar_cap_value'),
    ]) {
      expect(
        tester.widget<Text>(find.byKey(key)).data,
        '—',
        reason: '$key',
      );
    }
  });

  testWidgets('已连接：显示 PROTO_INFO 型号、MCU、协议号与能力', (
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
    final model = tester.widget<Text>(
      find.byKey(const Key('top_bar_model_value')),
    );
    expect(model.data, 'G6618 Motor Controller');
    expect(model.style?.color, const GuiPalette().fgStrong);
    expect(tester.widget<Text>(find.byKey(const Key('top_bar_mcu_value'))).data, 'HPM53M1');
    expect(tester.widget<Text>(find.byKey(const Key('top_bar_proto_value'))).data, 'v1.0');
    expect(tester.widget<Text>(find.byKey(const Key('top_bar_cap_value'))).data, '0x03');
  });

  testWidgets('FW 协议未提供：诚实显示 —，禁止编造版本', (WidgetTester tester) async {
    final state = AppState.test(
      connected: true,
      deviceName: 'G6618 Motor Controller',
      protoMajor: 1,
      capabilities: 0x03,
    );
    addTearDown(state.dispose);

    await _pump(tester, state);

    final fw = tester.widget<Text>(find.byKey(const Key('top_bar_fw_value')));
    expect(fw.data, '—');
    expect(find.textContaining('FW v'), findsNothing);
  });

  testWidgets('未连接：UPTIME / SEQ / 帧 均诚实显示 —', (WidgetTester tester) async {
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

  testWidgets('SEQ 协议未提供：即使已连接也显示 —，禁止估算', (WidgetTester tester) async {
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

  group('window chrome', () {
    testWidgets('renders the minimize / maximize / close cluster at the right', (
      WidgetTester tester,
    ) async {
      final state = AppState.test();
      addTearDown(state.dispose);

      await _pump(tester, state);

      expect(find.byKey(const Key('window_caption_minimize')), findsOneWidget);
      expect(find.byKey(const Key('window_caption_maximize')), findsOneWidget);
      expect(find.byKey(const Key('window_caption_close')), findsOneWidget);
    });

    testWidgets('caption buttons fire the injected callbacks', (
      WidgetTester tester,
    ) async {
      final state = AppState.test();
      addTearDown(state.dispose);
      var minimized = 0;
      var toggled = 0;
      var closed = 0;

      await _pump(
        tester,
        state,
        onMinimize: () => minimized++,
        onToggleMaximize: () => toggled++,
        onClose: () => closed++,
      );

      await tester.tap(find.byKey(const Key('window_caption_minimize')));
      await tester.tap(find.byKey(const Key('window_caption_maximize')));
      await tester.tap(find.byKey(const Key('window_caption_close')));

      expect(minimized, 1);
      expect(toggled, 1);
      expect(closed, 1);
    });

    testWidgets('dragging the blank bar area starts a window drag', (
      WidgetTester tester,
    ) async {
      final state = AppState.test();
      addTearDown(state.dispose);
      var drags = 0;

      await _pump(tester, state, onDragStart: () => drags++);

      await tester.drag(
        find.byKey(const Key('top_bar_drag_region')),
        const Offset(-40, 0),
      );
      await tester.pumpAndSettle();
      expect(drags, 1);
    });

    testWidgets('double-clicking the blank bar area toggles maximize', (
      WidgetTester tester,
    ) async {
      final state = AppState.test();
      addTearDown(state.dispose);
      var toggles = 0;

      await _pump(tester, state, onToggleMaximize: () => toggles++);

      final center = tester.getCenter(
        find.byKey(const Key('top_bar_drag_region')),
      );
      await tester.tapAt(center);
      await tester.pump(const Duration(milliseconds: 50));
      await tester.tapAt(center);
      await tester.pumpAndSettle();

      expect(toggles, 1);
    });
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
