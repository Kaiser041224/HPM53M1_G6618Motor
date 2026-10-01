import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/motor_drive_zone.dart';

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
  });
}
