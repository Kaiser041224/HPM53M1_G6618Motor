import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/param_zone.dart';
import 'package:g6618_gui/widgets/gui_button.dart';
import 'package:g6618_gui/widgets/hairline_frame.dart';
import 'package:g6618_gui/widgets/param_controls.dart';

ParamMetaDto makeMeta(
  int index,
  String name, {
  int ty = 0,
  int apply = 0,
  String unit = '',
  double min = 0,
  double max = 0,
}) => ParamMetaDto(
  index: index,
  name: name,
  ty: ty,
  apply: apply,
  unit: unit,
  min: min,
  max: max,
);

int f32(double v) =>
    (ByteData(4)..setFloat32(0, v, Endian.little)).getInt32(0, Endian.little);

StatusDto makeStatus({bool motorRunning = false}) => StatusDto(
  faultFlags: 0,
  motorRunning: motorRunning,
  motorState: motorRunning ? 2 : 0,
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

Future<void> _pump(WidgetTester tester, AppState state) {
  tester.view.physicalSize = const Size(1400, 2200);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  return tester.pumpWidget(
    MaterialApp(
      theme: buildGuiTheme(),
      home: Scaffold(
        body: SingleChildScrollView(child: ParamZone(state: state)),
      ),
    ),
  );
}

void main() {
  testWidgets('参数面板由元数据自动生成：域分组 + 搜索过滤', (tester) async {
    final state = AppState.test(
      metas: [
        makeMeta(0, 'motor.pole_pairs', ty: 1, unit: '', min: 1, max: 64),
        makeMeta(
          1,
          'software.fault.vbus_ov_v',
          ty: 0,
          unit: 'V',
          min: 0,
          max: 80,
        ),
      ],
    );
    addTearDown(state.dispose);

    await _pump(tester, state);

    expect(find.text('motor.pole_pairs'), findsOneWidget);
    // 双栏分组标题：命名空间至叶名前一段
    expect(find.text('MOTOR'), findsOneWidget);
    expect(find.text('SOFTWARE · FAULT'), findsOneWidget);

    // 搜索过滤
    await tester.enterText(find.byType(TextField).first, 'vbus');
    await tester.pump();
    expect(find.text('motor.pole_pairs'), findsNothing);
    expect(find.text('software.fault.vbus_ov_v'), findsOneWidget);
  });

  testWidgets('双栏把四组参数分到两列（宽视口）', (tester) async {
    final state = AppState.test(
      metas: [
        makeMeta(0, 'motor.pole_pairs', ty: 1, min: 1, max: 64),
        makeMeta(1, 'hardware.current_sense.a_per_volt', ty: 0, min: 0, max: 1),
        makeMeta(2, 'software.fault.vbus_ov_v', ty: 0, min: 0, max: 80),
        makeMeta(3, 'software.control.freq_hz', ty: 0, min: 0, max: 100),
      ],
    );
    addTearDown(state.dispose);

    await _pump(tester, state);

    expect(find.text('MOTOR'), findsOneWidget);
    expect(find.text('HARDWARE · CURRENT SENSE'), findsOneWidget);
    expect(find.text('SOFTWARE · FAULT'), findsOneWidget);
    expect(find.text('SOFTWARE · CONTROL'), findsOneWidget);
  });

  testWidgets('范围越界输入被拒绝并提示', (tester) async {
    final state = AppState.test(
      metas: [
        makeMeta(
          0,
          'software.fault.vbus_ov_v',
          ty: 0,
          unit: 'V',
          min: 0,
          max: 80,
        ),
      ],
    );
    addTearDown(state.dispose);

    await _pump(tester, state);
    await tester.tap(find.text('software.fault.vbus_ov_v'));
    await tester.pumpAndSettle();
    await tester.enterText(find.byType(TextField).last, '120');
    await tester.tap(find.text('写入'));
    await tester.pumpAndSettle();
    expect(find.textContaining('范围'), findsOneWidget);
  });

  testWidgets('编辑写入后往返：值更新且标记 dirty（圆点 + 黄值）', (tester) async {
    final state = AppState.test(
      metas: [makeMeta(0, 'motor.i_rated_a', ty: 0, min: 0, max: 10)],
    );
    addTearDown(state.dispose);
    state.values['motor.i_rated_a'] = ParamValueDto(ty: 0, raw: f32(7.0));
    state.dirty.add('motor.i_rated_a');

    await _pump(tester, state);

    expect(
      find.byKey(const Key('param_dirty_motor.i_rated_a')),
      findsOneWidget,
    );
    final value = tester.widget<Text>(find.text('7.00'));
    expect(value.style?.color, const GuiPalette().stWarn);
    expect(find.textContaining('项待落盘'), findsOneWidget);
  });

  testWidgets('COMMIT 二次确认文案保留「停机」「故障锁存」', (tester) async {
    final state = AppState.test(metas: [makeMeta(0, 'motor.pole_pairs')]);
    addTearDown(state.dispose);

    await _pump(tester, state);
    await tester.tap(find.text('COMMIT 落盘'));
    await tester.pumpAndSettle();

    expect(find.textContaining('停机'), findsWidgets);
    expect(find.textContaining('故障锁存'), findsWidgets);
  });

  testWidgets('联锁提示行：运行 + 锁存时琥珀拒绝（▲ 前缀），含当前状态与锁存计数', (tester) async {
    final state = AppState.test(
      metas: [makeMeta(0, 'motor.pole_pairs')],
      status: makeStatus(motorRunning: true),
    );
    addTearDown(state.dispose);
    state.latchedFlags = 1;

    await _pump(tester, state);

    final line = tester.widget<Text>(find.textContaining('落盘需停机'));
    // v3 tone mapping: a refused flush is flat amber, not the warn status hue.
    expect(line.style?.color, const GuiPalette().valueAmber);
    expect(find.textContaining('▲ 落盘需停机'), findsOneWidget);
    expect(find.textContaining('当前 Run'), findsOneWidget);
    expect(find.textContaining('锁存 1 项'), findsOneWidget);
    expect(find.textContaining('COMMIT 将被拒绝'), findsOneWidget);
  });

  testWidgets('联锁提示行：停机无锁存时绿字可落盘', (tester) async {
    final state = AppState.test(
      metas: [makeMeta(0, 'motor.pole_pairs')],
      status: makeStatus(),
    );
    addTearDown(state.dispose);

    await _pump(tester, state);

    final line = tester.widget<Text>(find.textContaining('可以落盘'));
    expect(line.style?.color, const GuiPalette().valueGreen);
  });

  testWidgets('参数值列量纲着色：无单位→白青中性；脏值走 stWarn', (tester) async {
    final state = AppState.test(
      metas: [
        makeMeta(0, 'motor.rs_ohm', ty: 0, min: 0, max: 10),
        makeMeta(1, 'motor.ls_h', ty: 0, min: 0, max: 1),
      ],
    );
    addTearDown(state.dispose);
    state.values['motor.rs_ohm'] = ParamValueDto(ty: 0, raw: f32(0.158));
    state.values['motor.ls_h'] = ParamValueDto(ty: 0, raw: f32(0.000119));
    state.dirty.add('motor.ls_h');

    await _pump(tester, state);

    final clean = tester.widget<Text>(find.text('0.16'));
    expect(clean.style?.color, const GuiPalette().fgStrong);
    final dirtyValue = tester.widget<Text>(find.text('0.00'));
    expect(dirtyValue.style?.color, const GuiPalette().stWarn);
  });

  testWidgets('参数值列按量纲着色（V→cyan）', (tester) async {
    final state = AppState.test(
      metas: [
        makeMeta(
          0,
          'software.fault.vbus_ov_v',
          ty: 0,
          unit: 'V',
          min: 0,
          max: 80,
        ),
      ],
    );
    addTearDown(state.dispose);
    state.values['software.fault.vbus_ov_v'] = ParamValueDto(
      ty: 0,
      raw: f32(48.2),
    );

    await _pump(tester, state);

    final value = tester.widget<Text>(find.text('48.20'));
    expect(value.style?.color, const GuiPalette().valueCyan);
  });

  testWidgets('参数名统一青色 + 弱青辉光', (tester) async {
    final state = AppState.test(
      metas: [makeMeta(0, 'motor.pole_pairs', ty: 1, min: 1, max: 64)],
    );
    addTearDown(state.dispose);

    await _pump(tester, state);

    final name = tester.widget<Text>(find.text('motor.pole_pairs'));
    expect(name.style?.color, const GuiPalette().accent);
    expect(name.style?.shadows, isNotEmpty);
  });

  testWidgets('行分割归一为低亮青 line（#173E44）', (tester) async {
    final state = AppState.test(
      metas: [makeMeta(0, 'motor.pole_pairs', ty: 1, min: 1, max: 64)],
    );
    addTearDown(state.dispose);

    await _pump(tester, state);

    final row = tester.widget<Container>(
      find
          .ancestor(
            of: find.text('motor.pole_pairs'),
            matching: find.byType(Container),
          )
          .first,
    );
    final border = (row.decoration! as BoxDecoration).border! as Border;
    expect(border.bottom.color, const GuiPalette().line);
    expect(border.bottom.color, const Color(0xFF173E44));
  });

  testWidgets('工具栏与对话框按钮为 GuiButton（读全部/COMMIT primary）', (tester) async {
    final state = AppState.test(metas: [makeMeta(0, 'motor.pole_pairs')]);
    addTearDown(state.dispose);

    await _pump(tester, state);

    expect(find.widgetWithText(GuiButton, '读全部'), findsOneWidget);
    final commit = tester.widget<GuiButton>(
      find.widgetWithText(GuiButton, 'COMMIT 落盘'),
    );
    expect(commit.variant, GuiButtonVariant.primary);

    await tester.tap(find.text('COMMIT 落盘'));
    await tester.pumpAndSettle();
    expect(find.widgetWithText(GuiButton, '确认写入'), findsOneWidget);
    expect(find.widgetWithText(GuiButton, '取消'), findsOneWidget);
  });

  testWidgets('参数分页各列字号统一（名称/类型/范围/徽标/值）', (tester) async {
    expect(
      kParamRowFontSize,
      inInclusiveRange(15, 16),
      reason: '参数表统一到一个较大可读字号档',
    );
    final state = AppState.test(
      metas: [makeMeta(0, 'motor.pole_pairs', ty: 1, min: 1, max: 64)],
    );
    addTearDown(state.dispose);
    state.values['motor.pole_pairs'] = ParamValueDto(ty: 1, raw: 8);

    await _pump(tester, state);

    for (final label in <String>[
      'motor.pole_pairs', // 名称
      'u32', // 类型
      '1.0..64.0', // 范围
      'LIVE', // 生效徽标
      '8', // 值
    ]) {
      final text = tester.widget<Text>(find.text(label));
      expect(
        text.style?.fontSize,
        kParamRowFontSize,
        reason: '「$label」应与参数表统一字号，不得大小混排',
      );
    }
  });

  testWidgets('ParamField 编辑控件值字号与参数页统一', (tester) async {
    final state = AppState.test(
      metas: [makeMeta(0, 'motor.pole_pairs', ty: 1, min: 1, max: 64)],
    );
    addTearDown(state.dispose);
    state.values['motor.pole_pairs'] = ParamValueDto(ty: 1, raw: 8);

    await tester.pumpWidget(
      MaterialApp(
        theme: buildGuiTheme(),
        home: Scaffold(
          body: ParamField(state: state, name: 'motor.pole_pairs', label: '极对数'),
        ),
      ),
    );

    final value = tester.widget<Text>(find.text('8'));
    expect(value.style?.fontSize, kParamRowFontSize);
  });

  testWidgets('参数分组各自包一层亮青外框（v3 面板密度）', (tester) async {
    final state = AppState.test(
      metas: [
        makeMeta(0, 'motor.pole_pairs', ty: 1, min: 1, max: 64),
        makeMeta(1, 'hardware.current_sense.a_per_volt', ty: 0, min: 0, max: 1),
        makeMeta(2, 'software.fault.vbus_ov_v', ty: 0, min: 0, max: 80),
      ],
    );
    addTearDown(state.dispose);

    await _pump(tester, state);

    expect(find.byKey(const Key('param_group_motor')), findsOneWidget);
    expect(
      find.byKey(const Key('param_group_hardware.current_sense')),
      findsOneWidget,
    );
    final frame = tester.widget<HairlineFrame>(
      find.byKey(const Key('param_group_motor')),
    );
    expect(frame.variant, HairlineVariant.main);
  });
}
