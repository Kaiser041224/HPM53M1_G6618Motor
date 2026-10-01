import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/panel_title.dart';
import 'package:g6618_gui/ui/pid_panel.dart';
import 'package:g6618_gui/widgets/gui_button.dart';

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

int f32(double v) =>
    (ByteData(4)..setFloat32(0, v, Endian.little)).getInt32(0, Endian.little);

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

Future<void> _pump(WidgetTester tester, PidPanel panel) {
  tester.view.physicalSize = const Size(1600, 1600);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  return tester.pumpWidget(
    MaterialApp(
      theme: buildGuiTheme(),
      home: Scaffold(body: SingleChildScrollView(child: panel)),
    ),
  );
}

void main() {
  const currentKp = 'software.control.current_loop.kp';
  const speedKp = 'software.control.speed_loop.kp';
  const angleKp = 'software.control.angle_loop.kp';

  group('PID 三环合并面板', () {
    testWidgets('单面板结构：一个外框 + 一条主标题，无旧三块独立外框', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, PidPanel(state: state));

      expect(find.byType(PanelBox), findsOneWidget);
      expect(find.byKey(const Key('pid_panel_frame')), findsOneWidget);
      expect(find.byKey(const Key('gui_title_bar')), findsOneWidget);
      expect(find.text('PID 参数'), findsOneWidget);
      expect(find.byKey(const Key('current_loop_frame')), findsNothing);
      expect(find.byKey(const Key('speed_loop_frame')), findsNothing);
      expect(find.byKey(const Key('angle_loop_frame')), findsNothing);
    });

    testWidgets('三节副标题 + lineDim 1px 分割线，顺序为电流 → 速度 → 角度', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, PidPanel(state: state));

      for (final t in <String>['电流环', '速度环', '角度环']) {
        expect(find.text(t), findsOneWidget, reason: '$t 副标题');
      }

      final current = find.byKey(const Key('pid_current_section'));
      final speed = find.byKey(const Key('pid_speed_section'));
      final angle = find.byKey(const Key('pid_angle_section'));
      expect(current, findsOneWidget);
      expect(speed, findsOneWidget);
      expect(angle, findsOneWidget);

      final currentTop = tester.getTopLeft(current).dy;
      final speedTop = tester.getTopLeft(speed).dy;
      final angleTop = tester.getTopLeft(angle).dy;
      expect(currentTop, lessThan(speedTop));
      expect(speedTop, lessThan(angleTop));

      for (final key in <Key>[
        const Key('pid_current_divider'),
        const Key('pid_speed_divider'),
        const Key('pid_angle_divider'),
      ]) {
        final divider = tester.widget<Container>(find.byKey(key));
        expect(divider.color, const GuiPalette().lineDim, reason: '$key 颜色');
        expect(
          divider.constraints?.maxHeight,
          const GuiPalette().lineWidth,
          reason: '$key 高度',
        );
      }

      // 电流环的诚实标注仍在（FOC 预留）。
      expect(find.byKey(const Key('current_loop_note')), findsOneWidget);
    });

    testWidgets('三节各 2 个参数位：kp/ki 标签各三份', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, PidPanel(state: state));

      expect(find.text('比例增益 kp'), findsNWidgets(3));
      expect(find.text('积分增益 ki'), findsNWidgets(3));
    });

    testWidgets('缺参时三节共 6 个参数如实显示「设备未提供」', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, PidPanel(state: state));

      expect(find.text('设备未提供'), findsNWidgets(6));
    });

    testWidgets('设备提供元数据时按全限定名渲染当前值', (tester) async {
      final state = AppState.test(
        status: makeStatus(),
        metas: <ParamMetaDto>[_f32Meta(currentKp)],
      );
      addTearDown(state.dispose);
      state.values[currentKp] = ParamValueDto(ty: 0, raw: f32(1.5));

      await _pump(tester, PidPanel(state: state));

      expect(find.text('1.50'), findsOneWidget);
      expect(find.text('设备未提供'), findsNWidgets(5));
    });

    testWidgets('暂存字段显示「旧值 → 新值」对比（共用 ParamField 语义）', (tester) async {
      final state = AppState.test(
        status: makeStatus(),
        metas: <ParamMetaDto>[_f32Meta(speedKp, min: 0, max: 10)],
      );
      addTearDown(state.dispose);
      state.values[speedKp] = ParamValueDto(ty: 0, raw: f32(1.5));
      state.stageParam(speedKp, 0, f32(2.0));

      await _pump(tester, PidPanel(state: state));

      final staged = tester.widget<Text>(
        find.byKey(const Key('field_staged_$speedKp')),
      );
      expect(staged.data, '2.00');
      expect(staged.style?.color, const GuiPalette().stPending);

      final strike = tester.widget<Container>(
        find.byKey(const Key('field_strike_$speedKp')),
      );
      expect(strike.color, const GuiPalette().fgDim);
      expect(find.text('→'), findsOneWidget);
    });

    testWidgets('共用按钮行：读取 / 写入 / 恢复出厂；无暂存时写入禁用', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, PidPanel(state: state));

      expect(find.widgetWithText(GuiButton, '读取'), findsOneWidget);
      expect(find.widgetWithText(GuiButton, '恢复出厂'), findsOneWidget);
      final write = tester.widget<GuiButton>(
        find.widgetWithText(GuiButton, '写入'),
      );
      expect(write.variant, GuiButtonVariant.primary);
      expect(write.onPressed, isNull);
    });

    testWidgets('待写入徽标在按钮行尾（恢复出厂之后）', (tester) async {
      final state = AppState.test(
        status: makeStatus(),
        metas: <ParamMetaDto>[_f32Meta(currentKp, min: 0, max: 10)],
      );
      addTearDown(state.dispose);
      state.stageParam(currentKp, 0, f32(2.0));

      await _pump(tester, PidPanel(state: state));

      final badge = find.byKey(const Key('pid_pending_badge'));
      expect(badge, findsOneWidget);
      expect(
        tester.getTopLeft(badge).dx,
        greaterThan(
          tester.getTopLeft(find.widgetWithText(GuiButton, '恢复出厂')).dx,
        ),
        reason: '待写入徽标应在按钮行尾',
      );
    });

    testWidgets('「写入」共用键只下发三环名集合，外部暂存保留', (tester) async {
      const external = 'software.fault.vbus_ov_v';
      final state = AppState.test(
        status: makeStatus(),
        metas: <ParamMetaDto>[
          _f32Meta(currentKp, min: 0, max: 10),
          _f32Meta(speedKp, min: 0, max: 10),
          _f32Meta(angleKp, min: 0, max: 10),
          ParamMetaDto(
            index: 3,
            name: external,
            ty: 0,
            apply: 0,
            unit: 'V',
            min: 0,
            max: 80,
          ),
        ],
      );
      addTearDown(state.dispose);
      final writes = <String>[];
      var commits = 0;
      state.onParamWrite = (name, ty, raw) async => writes.add(name);
      state.onParamCommit = () async => commits++;
      state.stageParam(currentKp, 0, f32(1.0));
      state.stageParam(speedKp, 0, f32(2.0));
      state.stageParam(angleKp, 0, f32(3.0));
      state.stageParam(external, 0, 123);

      await _pump(tester, PidPanel(state: state));

      await tester.tap(find.text('写入'));
      await tester.pumpAndSettle();

      expect(writes, <String>[currentKp, speedKp, angleKp]);
      expect(commits, 1);
      expect(state.staged.containsKey(currentKp), isFalse);
      expect(state.staged.containsKey(speedKp), isFalse);
      expect(state.staged.containsKey(angleKp), isFalse);
      expect(state.staged[external], 123);
    });

    testWidgets('「恢复出厂」确认后按三环名单下发 param_reset_factory', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      final resets = <List<String>>[];
      state.onParamReset = (names) async => resets.add(names);
      state.onParamRead = (name) async => const ParamValueDto(ty: 0, raw: 0);

      await _pump(tester, PidPanel(state: state));
      await tester.tap(find.text('恢复出厂'));
      await tester.pumpAndSettle();
      expect(find.widgetWithText(GuiButton, '确认恢复'), findsOneWidget);

      await tester.tap(find.text('确认恢复'));
      await tester.pumpAndSettle();

      expect(resets.single, PidPanel.resetNames);
    });

    testWidgets('有暂存时点「读取」弹「暂存改动将被丢弃，继续？」确认', (tester) async {
      final state = AppState.test(
        status: makeStatus(),
        metas: <ParamMetaDto>[_f32Meta(currentKp, min: 0, max: 10)],
      );
      addTearDown(state.dispose);
      state.stageParam(currentKp, 0, f32(2.0));

      await _pump(tester, PidPanel(state: state));
      await tester.tap(find.text('读取'));
      await tester.pumpAndSettle();

      expect(find.textContaining('暂存改动将被丢弃'), findsOneWidget);
      expect(find.widgetWithText(GuiButton, '继续'), findsOneWidget);
    });
  });
}
