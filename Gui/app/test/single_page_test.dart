import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/bottom_bar.dart';
import 'package:g6618_gui/ui/control_zone.dart';
import 'package:g6618_gui/ui/encoder_zone.dart';
import 'package:g6618_gui/ui/limit_zone.dart';
import 'package:g6618_gui/ui/motor_drive_zone.dart';
import 'package:g6618_gui/ui/param_zone.dart';
import 'package:g6618_gui/ui/pid_zone.dart';
import 'package:g6618_gui/ui/single_page.dart';
import 'package:g6618_gui/ui/start_adjust_zone.dart';
import 'package:g6618_gui/ui/status_zone.dart';
import 'package:g6618_gui/ui/top_bar.dart';
import 'package:g6618_gui/widgets/trend_chart.dart';

/// The nav shell's information architecture: sidebar L1 only, each page lays
/// its content out flat (stacked) in one scroll column — no L2 tabs.
///
/// Keep-alive semantics: the shell still uses an `Offstage` + `TickerMode` stack
/// (an `IndexedStack` equivalent) for the four L1 pages, so a page switched away
/// stays built and retains its state. Inactive pages are offstage, so the
/// default (`skipOffstage: true`) finders do not see them; `skipOffstage: false`
/// proves they were kept alive.
Future<void> _pump(WidgetTester tester, AppState state) async {
  tester.view.physicalSize = const Size(1600, 1400);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  await tester.pumpWidget(
    MaterialApp(theme: buildGuiTheme(), home: SinglePage(state: state)),
  );
}

/// Trivial status so the zones have a live snapshot to render.
StatusDto _status() => StatusDto(
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

void main() {
  testWidgets('侧边栏含四项 L1：监视·控制 / 示波 / 参数 / 电机·驱动', (tester) async {
    final state = AppState.test(status: _status(), metas: const []);
    addTearDown(state.dispose);

    await _pump(tester, state);

    // 控制已并入监视页（调参时要同时看读数），腾出的槽位给「电机 · 驱动」。
    const items = <(String, String, String)>[
      ('nav_monitor', '监视 · 控制', 'MONITOR'),
      ('nav_scope', '示波', 'SCOPE'),
      ('nav_params', '参数', 'PARAMS'),
      ('nav_motor', '电机 · 驱动', 'MOTOR'),
    ];
    for (final (key, zh, latin) in items) {
      final slot = find.byKey(Key(key));
      expect(slot, findsOneWidget, reason: key);
      expect(
        find.descendant(of: slot, matching: find.text(zh)),
        findsOneWidget,
        reason: '$key 中文名',
      );
      expect(
        find.descendant(of: slot, matching: find.text(latin)),
        findsOneWidget,
        reason: '$key 拉丁副标',
      );
      expect(
        find.descendant(of: slot, matching: find.byType(Icon)),
        findsOneWidget,
        reason: '$key 图标',
      );
    }
  });

  testWidgets('默认落在监视；监视页直铺 StatusZone → EncoderZone（无 L2 选项卡）', (tester) async {
    final state = AppState.test(status: _status(), metas: const []);
    addTearDown(state.dispose);

    await _pump(tester, state);

    expect(find.byType(StatusZone), findsOneWidget);
    expect(find.byType(EncoderZone), findsOneWidget);
    expect(find.text('状态监视'), findsOneWidget);
    // Flat stack: status zone sits above the encoder zone.
    expect(
      tester.getTopLeft(find.byType(StatusZone)).dy,
      lessThan(tester.getTopLeft(find.byType(EncoderZone)).dy),
      reason: 'StatusZone 应堆叠在 EncoderZone 之上',
    );
    // The L2 tab row is gone.
    expect(find.byKey(const Key('tab_monitor_status')), findsNothing);
    expect(find.byKey(const Key('tab_monitor_encoders')), findsNothing);
  });

  testWidgets('监视 → 示波：直铺 TrendChart → LimitZone，监视页退回后保活', (tester) async {
    final state = AppState.test(status: _status(), metas: const []);
    addTearDown(state.dispose);

    await _pump(tester, state);

    await tester.tap(find.byKey(const Key('nav_scope')));
    await tester.pumpAndSettle();

    expect(find.byType(TrendChart), findsOneWidget);
    expect(find.byType(LimitZone), findsOneWidget);
    // Flat stack: trend chart panel sits above the limits zone.
    expect(
      tester.getTopLeft(find.byType(TrendChart)).dy,
      lessThan(tester.getTopLeft(find.byType(LimitZone)).dy),
      reason: 'TrendChart 应堆叠在 LimitZone 之上',
    );
    expect(find.text('状态监视'), findsNothing);
    // The monitor page is offstage but kept alive in the element tree.
    expect(
      find.byType(StatusZone, skipOffstage: false),
      findsOneWidget,
      reason: '切换保活：状态页仍在元素树中',
    );
    expect(
      find.byType(EncoderZone, skipOffstage: false),
      findsOneWidget,
      reason: '切换保活：编码器页仍在元素树中',
    );
  });

  testWidgets('示波页直铺：六条响度条与电频率面板同页', (tester) async {
    final state = AppState.test(status: _status(), metas: const []);
    addTearDown(state.dispose);

    await _pump(tester, state);
    await tester.tap(find.byKey(const Key('nav_scope')));
    await tester.pumpAndSettle();

    expect(find.byType(LimitZone), findsOneWidget);
    expect(find.byType(HeatLoadBar), findsNWidgets(6));
    expect(find.text('热与负载 · 相对门限'), findsOneWidget);
    // The phase-current boxes moved into the TrendChart readout area; the
    // limits zone keeps only the F_ELEC panel.
    expect(find.text('电频率'), findsOneWidget);
    expect(find.text('相电流'), findsNothing);
  });

  testWidgets('监视·控制合并页：控制栏与故障构成同在，且无需切页', (tester) async {
    final state = AppState.test(status: _status(), metas: const []);
    addTearDown(state.dispose);

    await _pump(tester, state);

    // 合并后默认页即同时含监视与控制 —— 不再需要切到「控制」页。
    expect(find.byType(ControlZone), findsOneWidget);
    expect(find.byType(StatusZone), findsOneWidget);
    expect(find.byType(EncoderZone), findsOneWidget);
    // 启停合并为单键切换（停机态显示「▶ 启动」）。
    expect(find.byKey(const Key('ctrl_start_stop_toggle')), findsOneWidget);
    expect(find.text('清除故障'), findsOneWidget);
    expect(find.text('故障构成与指令'), findsOneWidget);
  });

  testWidgets('合并页右栏六段自上而下：故障 → 启动/调整 → 电流环 → 速度环 → 角度环 → 保护门限', (
    tester,
  ) async {
    final state = AppState.test(status: _status(), metas: const []);
    addTearDown(state.dispose);

    await _pump(tester, state);

    final fault = find.byKey(const Key('fault_frame'));
    final start = find.byKey(const Key('quick_control_frame'));
    final current = find.byKey(const Key('current_loop_frame'));
    final speed = find.byKey(const Key('speed_loop_frame'));
    final angle = find.byKey(const Key('angle_loop_frame'));
    final protection = find.byKey(const Key('protection_limits_frame'));

    expect(fault, findsOneWidget);
    expect(start, findsOneWidget);
    expect(current, findsOneWidget);
    expect(speed, findsOneWidget);
    expect(angle, findsOneWidget);
    expect(protection, findsOneWidget);

    final faultTop = tester.getTopLeft(fault).dy;
    final startTop = tester.getTopLeft(start).dy;
    final currentTop = tester.getTopLeft(current).dy;
    final speedTop = tester.getTopLeft(speed).dy;
    final angleTop = tester.getTopLeft(angle).dy;
    final protectionTop = tester.getTopLeft(protection).dy;

    expect(faultTop, lessThan(startTop), reason: '故障在最上');
    expect(startTop, lessThan(currentTop), reason: '启动/调整在电流环之上');
    expect(currentTop, lessThan(speedTop), reason: '电流环在速度环之上');
    expect(speedTop, lessThan(angleTop), reason: '速度环在角度环之上');
    expect(angleTop, lessThan(protectionTop), reason: '保护门限在最底部（角度环之下）');

    // 右栏在左栏（监视）右侧，即确属「右栏」而非堆叠。
    expect(
      tester.getTopLeft(fault).dx,
      greaterThan(tester.getTopLeft(find.byType(StatusZone)).dx),
      reason: '右栏应位于监视栏右侧',
    );
    expect(
      tester.getTopLeft(protection).dx,
      greaterThan(tester.getTopLeft(find.byType(StatusZone)).dx),
      reason: '保护门限在右栏',
    );
  });

  testWidgets('FREQ / MOD 与设备回读面板已删除；保护门限回归合并页右栏底部', (tester) async {
    final state = AppState.test(status: _status(), metas: const []);
    addTearDown(state.dispose);

    await _pump(tester, state);

    expect(find.text('FREQ'), findsNothing);
    expect(find.text('MOD'), findsNothing);
    expect(find.byKey(const Key('setpoint_readback_frame')), findsNothing);
    expect(find.text('设备回读'), findsNothing);
    // 保护门限自「电机 · 驱动」页回迁到合并页右栏最底部。
    expect(find.byKey(const Key('protection_limits_frame')), findsOneWidget);
    expect(find.text('保护门限'), findsOneWidget);
  });

  testWidgets('左栏保持 StatusZone → EncoderZone；启动/调整与 PID 已移至右栏', (tester) async {
    final state = AppState.test(status: _status(), metas: const []);
    addTearDown(state.dispose);

    await _pump(tester, state);

    expect(find.byType(StartAdjustZone), findsOneWidget);
    expect(find.byType(PidZone), findsOneWidget);
    expect(find.text('启动 / 调整'), findsOneWidget);
    expect(find.text('速度环'), findsOneWidget);
    expect(find.text('角度环'), findsOneWidget);

    // 顺序：StatusZone → EncoderZone（启动/调整与 PID 不再在左栏）。
    final monitorTop = tester.getTopLeft(find.byType(StatusZone)).dy;
    final encTop = tester.getTopLeft(find.byType(EncoderZone)).dy;
    expect(monitorTop, lessThan(encTop));
    // 它们改在右栏（更靠右）。
    expect(
      tester.getTopLeft(find.byType(StartAdjustZone)).dx,
      greaterThan(tester.getTopLeft(find.byType(EncoderZone)).dx),
    );
    expect(
      tester.getTopLeft(find.byType(PidZone)).dx,
      greaterThan(tester.getTopLeft(find.byType(EncoderZone)).dx),
    );
  });

  testWidgets('电机·驱动页不再含迁出的三块（启动/调整 · 速度环 · 角度环）', (tester) async {
    final state = AppState.test(status: _status(), metas: const []);
    addTearDown(state.dispose);

    await _pump(tester, state);
    await tester.tap(find.byKey(const Key('nav_motor')));
    await tester.pumpAndSettle();

    // 监视页 offstage 保活，但当前可见的电机·驱动页不应再含这三块。
    expect(find.byKey(const Key('quick_control_frame')), findsNothing);
    expect(find.byKey(const Key('speed_loop_frame')), findsNothing);
    expect(find.byKey(const Key('angle_loop_frame')), findsNothing);
    expect(find.byKey(const Key('ctrl_mode_1')), findsNothing);
    expect(find.byType(StartAdjustZone), findsNothing);
    expect(find.byType(PidZone), findsNothing);
    // 保护门限已回迁合并页右栏底部，不再在此页。
    expect(find.byKey(const Key('protection_limits_frame')), findsNothing);
    // 仍保留：标定程序 + 参数对照。
    expect(find.byKey(const Key('calib_programs_frame')), findsOneWidget);
    expect(find.byKey(const Key('config_compare_frame')), findsOneWidget);
  });

  testWidgets('电机·驱动页：标定程序全部禁用并标注协议未暴露；参数对照表在位', (tester) async {
    final state = AppState.test(status: _status(), metas: const []);
    addTearDown(state.dispose);

    await _pump(tester, state);
    await tester.tap(find.byKey(const Key('nav_motor')));
    await tester.pumpAndSettle();

    expect(find.byType(MotorDriveZone), findsOneWidget);
    // 固件例程已实现但协议未暴露 —— 必须如实说明，不能做成可点的假按钮。
    expect(find.byKey(const Key('calib_unavailable_note')), findsOneWidget);
    expect(find.text('电流零点标定'), findsOneWidget);
    expect(find.text('cal current'), findsOneWidget);
    expect(find.text('逆变器相测试'), findsOneWidget);
    // 参数对照表与「出厂预设」列均在位。
    expect(find.byKey(const Key('config_compare_frame')), findsOneWidget);
    expect(find.byKey(const Key('factory_col_header')), findsOneWidget);
  });

  testWidgets('合并页窄屏兜底：内容宽不足时改为上下堆叠，不溢出', (tester) async {
    // 800 减去侧边栏 150 后内容宽约 650 < 880 阈值 → 应走堆叠路径。
    // 这同时是一道溢出防线：并排布局在窄窗口下曾把编码器面板挤爆。
    tester.view.physicalSize = const Size(800, 1400);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);

    final state = AppState.test(status: _status(), metas: const []);
    addTearDown(state.dispose);
    await tester.pumpWidget(
      MaterialApp(theme: buildGuiTheme(), home: SinglePage(state: state)),
    );
    await tester.pump(const Duration(milliseconds: 400));

    // 堆叠后监视与控制仍全部在位。
    expect(find.byType(StatusZone), findsOneWidget);
    expect(find.byType(EncoderZone), findsOneWidget);
    expect(find.byKey(const Key('fault_frame')), findsOneWidget);
    expect(find.byKey(const Key('quick_control_frame')), findsOneWidget);
  });

  testWidgets('参数页：直铺参数表，无 L2 选项卡', (tester) async {
    final state = AppState.test(status: _status(), metas: const []);
    addTearDown(state.dispose);

    await _pump(tester, state);
    await tester.tap(find.byKey(const Key('nav_params')));
    await tester.pumpAndSettle();

    expect(find.byType(ParamZone), findsOneWidget);
    expect(find.text('写入'), findsOneWidget);
    // No L2 tab row survives anywhere.
    expect(find.byKey(const Key('tab_monitor_status')), findsNothing);
    expect(find.byKey(const Key('tab_scope_waveform')), findsNothing);
  });

  testWidgets('顶栏 / 底栏常驻：连接语义不变', (tester) async {
    final state = AppState.test(status: _status(), metas: const []);
    addTearDown(state.dispose);

    await _pump(tester, state);

    expect(find.byType(TopBar), findsOneWidget);
    expect(find.byType(BottomBar), findsOneWidget);
    expect(find.text('DEVICE ID PENDING'), findsOneWidget);
    expect(find.text('未连接'), findsOneWidget);
  });
}
