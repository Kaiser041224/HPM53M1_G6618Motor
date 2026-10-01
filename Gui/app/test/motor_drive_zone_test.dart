import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/bridge/frb/api.dart' show FrbError, ParamMetaDto;
import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/motor_drive_zone.dart';

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

int f32(double v) =>
    (ByteData(4)..setFloat32(0, v, Endian.little)).getInt32(0, Endian.little);

Future<void> _pump(WidgetTester tester, MotorDriveZone zone) {
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

ParamMetaDto _f32Meta(String name, {double min = 0, double max = 100}) =>
    ParamMetaDto(
      index: 0,
      name: name,
      ty: 0,
      apply: 0,
      unit: '',
      min: min,
      max: max,
    );

void main() {
  group('电机·驱动 右栏 PID 调整入口', () {
    testWidgets('速度环 / 角度环两组标题与 4 个参数位渲染', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, MotorDriveZone(state: state));

      expect(find.byKey(const Key('speed_loop_frame')), findsOneWidget);
      expect(find.byKey(const Key('angle_loop_frame')), findsOneWidget);
      expect(find.text('速度环'), findsOneWidget);
      expect(find.text('角度环'), findsOneWidget);
    });

    testWidgets('缺参时两组共 4 个参数如实显示「设备未提供」', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, MotorDriveZone(state: state));

      expect(find.text('设备未提供'), findsNWidgets(4));
    });

    testWidgets('设备提供元数据时按全限定名渲染当前值', (tester) async {
      final state = AppState.test(
        status: makeStatus(),
        metas: <ParamMetaDto>[_f32Meta('software.control.speed_loop.kp')],
      );
      addTearDown(state.dispose);
      state.values['software.control.speed_loop.kp'] = ParamValueDto(
        ty: 0,
        raw: f32(1.5),
      );

      await _pump(tester, MotorDriveZone(state: state));

      // speed_loop.kp 有值 → 1.50；其余缺值仍「设备未提供」（4 - 1 = 3）。
      expect(find.text('1.50'), findsOneWidget);
      expect(find.text('设备未提供'), findsNWidgets(3));
    });
  });

  group('电机·驱动 快捷控制模式', () {
    testWidgets('三枚按钮均调用 setMode 注入，成功后本地记录 ctrlMode', (tester) async {
      final ops = <String>[];
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async => ops.add(op);

      await _pump(tester, MotorDriveZone(state: state));

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

      await _pump(tester, MotorDriveZone(state: state));
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

      await _pump(tester, MotorDriveZone(state: state));

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

      await _pump(tester, MotorDriveZone(state: state));

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

  group('电机·驱动 模式区诚实标注', () {
    testWidgets('模式按钮标注协议 v1.1，标定按钮仍禁用未暴露', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, MotorDriveZone(state: state));

      expect(find.byKey(const Key('ctrl_mode_v11_note')), findsOneWidget);
      // 标定程序的协议未暴露说明仍在位，两者区分标注。
      expect(find.byKey(const Key('calib_unavailable_note')), findsOneWidget);
    });

    testWidgets('模式无回读（协议 v1.1）小字在位', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, MotorDriveZone(state: state));

      expect(find.byKey(const Key('ctrl_mode_readback_note')), findsOneWidget);
      expect(find.textContaining('无回读'), findsOneWidget);
    });
  });
}
