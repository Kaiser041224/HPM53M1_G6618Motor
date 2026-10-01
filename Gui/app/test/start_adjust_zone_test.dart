import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/bridge/frb/api.dart' show FrbError;
import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/start_adjust_zone.dart';
import 'package:g6618_gui/widgets/gui_button.dart';

StatusDto makeStatus({bool running = false}) => StatusDto(
  faultFlags: 0,
  motorRunning: running,
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

Future<void> _pump(WidgetTester tester, StartAdjustZone zone) {
  tester.view.physicalSize = const Size(1600, 1600);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  return tester.pumpWidget(
    MaterialApp(
      theme: buildGuiTheme(),
      home: Scaffold(body: SingleChildScrollView(child: zone)),
    ),
  );
}

void main() {
  group('启动·调整 快捷控制模式', () {
    testWidgets('面板与三枚模式按钮在位', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, StartAdjustZone(state: state));

      expect(find.byKey(const Key('quick_control_frame')), findsOneWidget);
      expect(find.text('启动 / 调整'), findsOneWidget);
      for (final key in <Key>[
        const Key('ctrl_mode_1'),
        const Key('ctrl_mode_2'),
        const Key('ctrl_mode_3'),
      ]) {
        expect(find.byKey(key), findsOneWidget);
      }
    });

    testWidgets('三枚按钮均调用 setMode 注入，成功后本地记录 ctrlMode', (tester) async {
      final ops = <String>[];
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async => ops.add(op);

      await _pump(tester, StartAdjustZone(state: state));

      for (final (mode, key) in <(int, Key)>[
        (1, const Key('ctrl_mode_1')),
        (2, const Key('ctrl_mode_2')),
        (3, const Key('ctrl_mode_3')),
      ]) {
        final finder = find.byKey(key);
        await tester.ensureVisible(finder);
        await tester.pumpAndSettle();
        await tester.tap(finder);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 50));
        expect(state.ctrlMode, mode);
      }

      expect(ops, <String>['setMode', 'setMode', 'setMode']);
    });

    testWidgets('模式高亮：选中态徽标只出现在当前模式', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async {};

      await _pump(tester, StartAdjustZone(state: state));
      expect(find.byKey(const Key('ctrl_mode_badge_2')), findsNothing);

      final speed = find.byKey(const Key('ctrl_mode_2'));
      await tester.ensureVisible(speed);
      await tester.pumpAndSettle();
      await tester.tap(speed);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));

      expect(find.byKey(const Key('ctrl_mode_badge_2')), findsOneWidget);
      expect(find.byKey(const Key('ctrl_mode_badge_1')), findsNothing);
      expect(find.byKey(const Key('ctrl_mode_badge_3')), findsNothing);
    });

    testWidgets('成功 SnackBar「控制模式：速度」', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async {};

      await _pump(tester, StartAdjustZone(state: state));

      final speed = find.byKey(const Key('ctrl_mode_2'));
      await tester.ensureVisible(speed);
      await tester.pumpAndSettle();
      await tester.tap(speed);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));

      expect(find.text('控制模式：速度'), findsOneWidget);
    });

    testWidgets('运行中切换被拒 LOCKED(code 4) → 既有文案表 SnackBar', (tester) async {
      final state = AppState.test(
        status: makeStatus(running: true),
        metas: const [],
      );
      addTearDown(state.dispose);
      state.onCtrl = (op) async =>
          throw const FrbError(code: 4, message: 'interlock');

      await _pump(tester, StartAdjustZone(state: state));

      final angle = find.byKey(const Key('ctrl_mode_3'));
      await tester.ensureVisible(angle);
      await tester.pumpAndSettle();
      await tester.tap(angle);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));

      expect(find.text('设备联锁拒绝：先 STOP / FAULT_CLEAR'), findsOneWidget);
      expect(state.ctrlMode, 0);
    });
  });

  group('启动·调整 诚实标注', () {
    testWidgets('模式按钮标注协议 v1.1', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, StartAdjustZone(state: state));

      expect(find.byKey(const Key('ctrl_mode_v11_note')), findsOneWidget);
      expect(find.text('协议 v1.1'), findsOneWidget);
    });

    testWidgets('模式无回读（协议 v1.1）小字在位', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, StartAdjustZone(state: state));

      expect(find.byKey(const Key('ctrl_mode_readback_note')), findsOneWidget);
      expect(find.textContaining('无回读'), findsOneWidget);
    });
  });

  group('启动·调整 电机启停按钮行（自控制面板迁入）', () {
    testWidgets('启动 primary / 停止 normal 两枚按钮在位', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, StartAdjustZone(state: state));

      expect(find.byKey(const Key('ctrl_start_button')), findsOneWidget);
      expect(find.byKey(const Key('ctrl_stop_button')), findsOneWidget);
      expect(
        tester
            .widget<GuiButton>(find.byKey(const Key('ctrl_start_button')))
            .variant,
        GuiButtonVariant.primary,
      );
      expect(find.widgetWithText(GuiButton, '停止'), findsOneWidget);
    });

    testWidgets('启动调用 ctrlStart，停止调用 ctrlStop（行为不变）', (tester) async {
      final ops = <String>[];
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async => ops.add(op);

      await _pump(tester, StartAdjustZone(state: state));

      await tester.tap(find.byKey(const Key('ctrl_start_button')));
      await tester.pump();
      await tester.tap(find.byKey(const Key('ctrl_stop_button')));
      await tester.pump();

      expect(ops, <String>['start', 'stop']);
    });

    testWidgets('启动失败 code 4 → 联锁 SnackBar 文案', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async =>
          throw const FrbError(code: 4, message: 'interlock');

      await _pump(tester, StartAdjustZone(state: state));

      await tester.tap(find.byKey(const Key('ctrl_start_button')));
      await tester.pumpAndSettle();

      final snack = find.byType(SnackBar);
      expect(
        find.descendant(of: snack, matching: find.textContaining('联锁')),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: snack,
          matching: find.textContaining('FAULT_CLEAR'),
        ),
        findsOneWidget,
      );
    });
  });
}
