import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/motor_drive_zone.dart';
import 'package:g6618_gui/widgets/gui_button.dart';

ParamMetaDto makeMeta(
  String name, {
  int ty = 1,
  int apply = 0,
  String unit = '',
  double min = 0,
  double max = 0,
}) => ParamMetaDto(
  index: 0,
  name: name,
  ty: ty,
  apply: apply,
  unit: unit,
  min: min,
  max: max,
);

StatusDto makeStatus() => StatusDto(
  faultFlags: 0,
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

void main() {
  group('电机·驱动 标定程序与参数对照', () {
    testWidgets('标定例程全部禁用并标注协议未暴露', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, MotorDriveZone(state: state));

      expect(find.byKey(const Key('calib_programs_frame')), findsOneWidget);
      expect(find.byKey(const Key('calib_unavailable_note')), findsOneWidget);
      expect(find.text('电流零点标定'), findsOneWidget);
      expect(find.text('cal current'), findsOneWidget);
      expect(find.text('逆变器相测试'), findsOneWidget);
    });

    testWidgets('参数对照表与「出厂预设」列在位', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, MotorDriveZone(state: state));

      expect(find.byKey(const Key('config_compare_frame')), findsOneWidget);
      expect(find.byKey(const Key('factory_col_header')), findsOneWidget);
      expect(find.text('参数对照'), findsOneWidget);
    });

    testWidgets('已迁出的三块不再出现：启动/调整 · 速度环 · 角度环', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, MotorDriveZone(state: state));

      expect(find.byKey(const Key('quick_control_frame')), findsNothing);
      expect(find.byKey(const Key('speed_loop_frame')), findsNothing);
      expect(find.byKey(const Key('angle_loop_frame')), findsNothing);
      expect(find.byKey(const Key('ctrl_mode_1')), findsNothing);
    });

    testWidgets('保护门限不在此页（已回迁「监视 · 控制」右栏底部）', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, MotorDriveZone(state: state));

      expect(find.byKey(const Key('protection_limits_frame')), findsNothing);
      expect(find.text('保护门限'), findsNothing);
      // 仍保留参数对照表。
      expect(find.byKey(const Key('config_compare_frame')), findsOneWidget);
    });
  });

  group('电机·驱动 参数对照暂存-写入', () {
    const pwm = 'hardware.inverter.pwm_freq_hz';

    testWidgets('点行弹 ParamEditDialog，确定进入暂存（不触设备）', (tester) async {
      final state = AppState.test(
        status: makeStatus(),
        metas: <ParamMetaDto>[
          makeMeta(pwm, ty: 1, min: 1000, max: 20000),
        ],
      );
      addTearDown(state.dispose);
      state.values[pwm] = const ParamValueDto(ty: 1, raw: 16000);

      await _pump(tester, MotorDriveZone(state: state));
      await tester.tap(find.text('开关频率'));
      await tester.pumpAndSettle();
      expect(find.byType(TextField), findsOneWidget);

      await tester.enterText(find.byType(TextField), '12000');
      await tester.tap(find.text('确定'));
      await tester.pumpAndSettle();

      expect(state.staged[pwm], 12000);
      expect(state.requestFailures, 0);
    });

    testWidgets('暂存行显示「旧值 → 新值」：旧值划线，新值 stPending', (tester) async {
      final state = AppState.test(
        status: makeStatus(),
        metas: <ParamMetaDto>[
          makeMeta(pwm, ty: 1, min: 1000, max: 20000),
        ],
      );
      addTearDown(state.dispose);
      state.values[pwm] = const ParamValueDto(ty: 1, raw: 16000);
      state.stageParam(pwm, 1, 12000);

      await _pump(tester, MotorDriveZone(state: state));

      final stagedValue = tester.widget<Text>(
        find.byKey(const Key('config_staged_$pwm')),
      );
      expect(stagedValue.data, '12000');
      expect(stagedValue.style?.color, const GuiPalette().stPending);

      final strike = tester.widget<Container>(
        find.byKey(const Key('config_strike_$pwm')),
      );
      expect(strike.color, const GuiPalette().fgDim);
      expect(strike.constraints?.maxHeight, 1);

      expect(find.text('→'), findsOneWidget);
      expect(find.widgetWithText(GuiButton, '写入'), findsOneWidget);
    });

    testWidgets('三按钮组：读取 / 写入 / 恢复出厂；无暂存时写入禁用', (tester) async {
      final state = AppState.test(
        status: makeStatus(),
        metas: <ParamMetaDto>[makeMeta(pwm, ty: 1, min: 1000, max: 20000)],
      );
      addTearDown(state.dispose);

      await _pump(tester, MotorDriveZone(state: state));

      expect(find.widgetWithText(GuiButton, '读取'), findsOneWidget);
      expect(find.widgetWithText(GuiButton, '恢复出厂'), findsOneWidget);
      final write = tester.widget<GuiButton>(
        find.widgetWithText(GuiButton, '写入'),
      );
      expect(write.onPressed, isNull);
    });

    testWidgets('有暂存时点「读取」弹「暂存改动将被丢弃，继续？」确认', (tester) async {
      final state = AppState.test(
        status: makeStatus(),
        metas: <ParamMetaDto>[makeMeta(pwm, ty: 1, min: 1000, max: 20000)],
      );
      addTearDown(state.dispose);
      state.stageParam(pwm, 1, 12000);

      await _pump(tester, MotorDriveZone(state: state));
      await tester.tap(find.text('读取'));
      await tester.pumpAndSettle();

      expect(find.textContaining('暂存改动将被丢弃'), findsOneWidget);
      expect(find.widgetWithText(GuiButton, '继续'), findsOneWidget);
    });

    testWidgets('「写入」只下发本表暂存（子集），外部暂存保留', (tester) async {
      const external = 'software.fault.vbus_ov_v';
      final state = AppState.test(
        status: makeStatus(),
        metas: <ParamMetaDto>[
          makeMeta(pwm, ty: 1, min: 1000, max: 20000),
          makeMeta(external, ty: 0, unit: 'V', min: 0, max: 80),
        ],
      );
      addTearDown(state.dispose);
      final writes = <String>[];
      var commits = 0;
      state.onParamWrite = (name, ty, raw) async => writes.add(name);
      state.onParamCommit = () async => commits++;
      state.stageParam(pwm, 1, 12000);
      state.stageParam(external, 0, 123);

      await _pump(tester, MotorDriveZone(state: state));
      await tester.tap(find.text('写入'));
      await tester.pumpAndSettle();

      expect(writes, <String>[pwm]);
      expect(commits, 1);
      expect(state.staged.containsKey(pwm), isFalse);
      expect(state.staged[external], 123);
    });

    testWidgets('「恢复出厂」确认后按本表名单下发 param_reset_factory', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      final resets = <List<String>>[];
      state.onParamReset = (names) async => resets.add(names);
      state.onParamRead = (name) async => const ParamValueDto(ty: 1, raw: 0);

      await _pump(tester, MotorDriveZone(state: state));
      await tester.tap(find.text('恢复出厂'));
      await tester.pumpAndSettle();
      expect(find.widgetWithText(GuiButton, '确认恢复'), findsOneWidget);

      await tester.tap(find.text('确认恢复'));
      await tester.pumpAndSettle();

      expect(resets.single, MotorDriveZone.configResetNames);
      // 派生参数不下发（设备参数表无独立条目）；非派生项全部保留。
      expect(
        resets.single,
        isNot(contains('hardware.current_sense.a_per_volt')),
      );
      expect(resets.single.length, MotorDriveZone.configParamNames.length - 4);
    });
  });
}
