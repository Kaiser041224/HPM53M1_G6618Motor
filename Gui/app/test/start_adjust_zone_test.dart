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

/// 让一条 toast 走完整个生命周期，避免测试结束时残留动画回调。
Future<void> _drainToast(WidgetTester tester) async {
  await tester.pump(const Duration(milliseconds: 3000));
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
      await _drainToast(tester);
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
      await _drainToast(tester);
    });

    testWidgets('成功 toast「控制模式：速度」', (tester) async {
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

      expect(
        find.descendant(
          of: find.byKey(const Key('gui_toast_frame')),
          matching: find.text('控制模式：速度'),
        ),
        findsOneWidget,
      );
      await _drainToast(tester);
    });

    testWidgets('切换失败 LOCKED(code 4) → 既有文案表 toast（停止态兜底）', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
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

      expect(
        find.descendant(
          of: find.byKey(const Key('gui_toast_frame')),
          matching: find.text('设备联锁拒绝：先 STOP / FAULT_CLEAR'),
        ),
        findsOneWidget,
      );
      expect(state.ctrlMode, 0);
      await _drainToast(tester);
    });
  });

  group('启动·调整 诚实标注', () {
    testWidgets('模式按钮标注协议 v1.1 / v1.2', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, StartAdjustZone(state: state));

      expect(find.byKey(const Key('ctrl_mode_v11_note')), findsOneWidget);
      expect(find.text('协议 v1.1 / v1.2'), findsOneWidget);
    });

    testWidgets('模式无回读（协议 v1.1）小字在位', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, StartAdjustZone(state: state));

      expect(find.byKey(const Key('ctrl_mode_readback_note')), findsOneWidget);
      expect(find.textContaining('无回读'), findsOneWidget);
    });
  });

  group('启动·调整 启停单键切换', () {
    testWidgets('停机态显示「▶ 启动」灰青，点击调用 ctrlStart', (tester) async {
      final ops = <String>[];
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async => ops.add(op);

      await _pump(tester, StartAdjustZone(state: state));

      final toggle = find.byKey(const Key('ctrl_start_stop_toggle'));
      expect(toggle, findsOneWidget);
      final label = tester.widget<Text>(
        find.descendant(of: toggle, matching: find.byType(Text)),
      );
      expect(label.data, '▶ 启动');
      expect(label.style?.color, const GuiPalette().fgDim);

      await tester.tap(toggle);
      await tester.pump();
      expect(ops, <String>['start']);
    });

    testWidgets('运行态显示「■ 停止」翠绿，点击调用 ctrlStop', (tester) async {
      final ops = <String>[];
      final state = AppState.test(
        status: makeStatus(running: true),
        metas: const [],
      );
      addTearDown(state.dispose);
      state.onCtrl = (op) async => ops.add(op);

      await _pump(tester, StartAdjustZone(state: state));

      final toggle = find.byKey(const Key('ctrl_start_stop_toggle'));
      final label = tester.widget<Text>(
        find.descendant(of: toggle, matching: find.byType(Text)),
      );
      expect(label.data, '■ 停止');
      expect(label.style?.color, const GuiPalette().stNormal);

      await tester.tap(toggle);
      await tester.pump();
      expect(ops, <String>['stop']);
    });

    testWidgets('启停失败 code 4 → 既有联锁 toast 文案', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async =>
          throw const FrbError(code: 4, message: 'interlock');

      await _pump(tester, StartAdjustZone(state: state));

      await tester.tap(find.byKey(const Key('ctrl_start_stop_toggle')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));

      final toast = find.byKey(const Key('gui_toast_frame'));
      expect(
        find.descendant(of: toast, matching: find.textContaining('联锁')),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: toast,
          matching: find.textContaining('FAULT_CLEAR'),
        ),
        findsOneWidget,
      );
      await _drainToast(tester);
    });
  });

  group('启动·调整 运行态锁定（停止态才可用）', () {
    testWidgets('运行中：三模式按钮禁用 + 输入禁用 + 下发禁用 + 「请先停止后切换」', (
      tester,
    ) async {
      final state = AppState.test(
        status: makeStatus(running: true),
        metas: const [],
      );
      addTearDown(state.dispose);
      state.onCtrl = (op) async {};
      await state.setCtrlMode(2);

      await _pump(tester, StartAdjustZone(state: state));

      for (final key in <Key>[
        const Key('ctrl_mode_1'),
        const Key('ctrl_mode_2'),
        const Key('ctrl_mode_3'),
      ]) {
        expect(
          tester.widget<GuiButton>(find.byKey(key)).onPressed,
          isNull,
          reason: '$key 运行中必须禁用',
        );
      }
      expect(
        tester.widget<TextField>(
          find.byKey(const Key('ctrl_setpoint_input')),
        ).enabled,
        isFalse,
      );
      expect(
        tester.widget<GuiButton>(find.byKey(const Key('ctrl_setpoint_send'))).onPressed,
        isNull,
      );
      // 三重编码：文字提示（禁用样式 + tooltip 由组件提供）。
      expect(
        find.byKey(const Key('ctrl_locked_note')),
        findsOneWidget,
      );
      expect(find.text('请先停止后切换'), findsWidgets);
    });

    testWidgets('停机后恢复可用：按钮 / 输入 / 下发均启用，无锁定提示', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async {};
      await state.setCtrlMode(2);

      await _pump(tester, StartAdjustZone(state: state));

      for (final key in <Key>[
        const Key('ctrl_mode_1'),
        const Key('ctrl_mode_2'),
        const Key('ctrl_mode_3'),
      ]) {
        expect(tester.widget<GuiButton>(find.byKey(key)).onPressed, isNotNull);
      }
      expect(
        tester.widget<TextField>(
          find.byKey(const Key('ctrl_setpoint_input')),
        ).enabled,
        isTrue,
      );
      expect(
        tester.widget<GuiButton>(find.byKey(const Key('ctrl_setpoint_send'))).onPressed,
        isNotNull,
      );
      expect(find.byKey(const Key('ctrl_locked_note')), findsNothing);
    });
  });

  group('启动·调整 控制模式设定输入', () {
    Future<void> selectMode(WidgetTester tester, int mode) async {
      final button = find.byKey(Key('ctrl_mode_$mode'));
      await tester.ensureVisible(button);
      await tester.pumpAndSettle();
      await tester.tap(button);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));
    }

    testWidgets('单位随模式切换：力矩 N·m / 速度 rpm / 角度 °', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async {};

      await _pump(tester, StartAdjustZone(state: state));

      Finder unit() => find.byKey(const Key('ctrl_setpoint_unit'));
      expect(tester.widget<Text>(unit()).data, '—');
      // 未选模式：输入框与下发均禁用。
      expect(
        tester.widget<TextField>(find.byKey(const Key('ctrl_setpoint_input'))).enabled,
        isFalse,
      );

      await selectMode(tester, 1);
      expect(tester.widget<Text>(unit()).data, 'N·m');
      await selectMode(tester, 2);
      expect(tester.widget<Text>(unit()).data, 'rpm');
      await selectMode(tester, 3);
      expect(tester.widget<Text>(unit()).data, '°');
      await _drainToast(tester);
    });

    testWidgets('下发调用注入 API 并弹「已下发」toast', (tester) async {
      final ops = <String>[];
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async => ops.add(op);
      // 预置模式（不经 UI 选模式，避免其 toast 占队遮挡「已下发」）。
      await state.setCtrlMode(2);
      ops.clear();

      await _pump(tester, StartAdjustZone(state: state));

      await tester.enterText(
        find.byKey(const Key('ctrl_setpoint_input')),
        '600',
      );
      await tester.tap(find.byKey(const Key('ctrl_setpoint_send')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));

      expect(ops.last, 'setSpeed');
      expect(
        find.descendant(
          of: find.byKey(const Key('gui_toast_frame')),
          matching: find.textContaining('已下发'),
        ),
        findsOneWidget,
      );
      await _drainToast(tester);
    });

    testWidgets('速度越界 |v|>20000 → 本地校验拦截，不调用 API', (tester) async {
      final ops = <String>[];
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async => ops.add(op);

      await _pump(tester, StartAdjustZone(state: state));
      await selectMode(tester, 2);
      ops.clear();

      await tester.enterText(
        find.byKey(const Key('ctrl_setpoint_input')),
        '30000',
      );
      await tester.tap(find.byKey(const Key('ctrl_setpoint_send')));
      await tester.pump();

      expect(find.byKey(const Key('ctrl_setpoint_error')), findsOneWidget);
      expect(find.textContaining('越界'), findsOneWidget);
      expect(ops, isEmpty);
      await _drainToast(tester);
    });

    testWidgets('非有限值 → 本地校验拦截，不调用 API', (tester) async {
      final ops = <String>[];
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async => ops.add(op);

      await _pump(tester, StartAdjustZone(state: state));
      await selectMode(tester, 1);
      ops.clear();

      await tester.enterText(
        find.byKey(const Key('ctrl_setpoint_input')),
        '1e999',
      );
      await tester.tap(find.byKey(const Key('ctrl_setpoint_send')));
      await tester.pump();

      expect(find.byKey(const Key('ctrl_setpoint_error')), findsOneWidget);
      expect(ops, isEmpty);
      await _drainToast(tester);
    });

    testWidgets('下发失败 RANGE_ERR(code 6) → 既有越界文案表 toast', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async {
        if (op == 'setMode') return;
        throw const FrbError(code: 6, message: 'range');
      };
      // 预置模式（不经 UI 选模式，避免其 toast 占队遮挡失败文案）。
      await state.setCtrlMode(1);

      await _pump(tester, StartAdjustZone(state: state));

      await tester.enterText(
        find.byKey(const Key('ctrl_setpoint_input')),
        '1.0',
      );
      await tester.tap(find.byKey(const Key('ctrl_setpoint_send')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));

      expect(
        find.descendant(
          of: find.byKey(const Key('gui_toast_frame')),
          matching: find.text('设备判定越界'),
        ),
        findsOneWidget,
      );
      await _drainToast(tester);
    });
  });

  group('启动·调整 徽标尺寸', () {
    testWidgets('选中态徽标完整显示「◆ 当前」（不裁字）', (tester) async {
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

      final badge = find.byKey(const Key('ctrl_mode_badge_2'));
      expect(badge, findsOneWidget);
      final text = find.descendant(of: badge, matching: find.text('◆ 当前'));
      expect(text, findsOneWidget);
      // 容器自适应内容宽度：徽标不小于其文字，且无溢出异常（文字完整）。
      expect(
        tester.getSize(badge).width,
        greaterThanOrEqualTo(tester.getSize(text).width),
      );
      expect(
        tester.getSize(badge).height,
        greaterThanOrEqualTo(tester.getSize(text).height),
      );
      expect(tester.takeException(), isNull);
      await _drainToast(tester);
    });
  });
}
