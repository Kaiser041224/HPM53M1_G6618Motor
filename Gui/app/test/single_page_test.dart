import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/bottom_bar.dart';
import 'package:g6618_gui/ui/control_zone.dart';
import 'package:g6618_gui/ui/encoder_zone.dart';
import 'package:g6618_gui/ui/limit_zone.dart';
import 'package:g6618_gui/ui/param_zone.dart';
import 'package:g6618_gui/ui/single_page.dart';
import 'package:g6618_gui/ui/status_zone.dart';
import 'package:g6618_gui/ui/top_bar.dart';
import 'package:g6618_gui/widgets/trend_chart.dart';

/// The nav shell's information architecture: sidebar L1 + tabs L2, with the
/// top connection bar and bottom verdict bar unchanged.
///
/// Keep-alive semantics: the shell uses an `Offstage` + `TickerMode` stack (an
/// `IndexedStack` equivalent) so a page that is switched away stays built and
/// retains its state. Inactive pages are offstage, so the default
/// (`skipOffstage: true`) finders do not see them; `skipOffstage: false` proves
/// they were kept alive.
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
  testWidgets('侧边栏含四项 L1：监视 / 示波 / 控制 / 参数（图标+中文+拉丁）', (tester) async {
    final state = AppState.test(status: _status(), metas: const []);
    addTearDown(state.dispose);

    await _pump(tester, state);

    const items = <(String, String, String)>[
      ('nav_monitor', '监视', 'MONITOR'),
      ('nav_scope', '示波', 'SCOPE'),
      ('nav_control', '控制', 'CONTROL'),
      ('nav_params', '参数', 'PARAMS'),
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

  testWidgets('默认落在监视·状态；点击示波 → 趋势图可见、状态瓦片退出活动树', (tester) async {
    final state = AppState.test(status: _status(), metas: const []);
    addTearDown(state.dispose);

    await _pump(tester, state);

    // Monitor / status is the landing page.
    expect(find.text('状态监视'), findsOneWidget);
    expect(find.byType(TrendChart), findsNothing);

    await tester.tap(find.byKey(const Key('nav_scope')));
    await tester.pumpAndSettle();

    expect(find.byKey(const Key('scope_curve_frame')), findsOneWidget);
    expect(find.byType(TrendChart), findsOneWidget);
    expect(find.text('状态监视'), findsNothing);
    // The status page is offstage but kept alive in the element tree.
    expect(
      find.byType(StatusZone, skipOffstage: false),
      findsOneWidget,
      reason: '切换保活：状态页仍在元素树中',
    );
  });

  testWidgets('监视页 L2 选项卡：状态 ↔ 双编码器', (tester) async {
    final state = AppState.test(status: _status(), metas: const []);
    addTearDown(state.dispose);

    await _pump(tester, state);

    expect(find.byType(StatusZone), findsOneWidget);
    expect(find.byType(EncoderZone), findsNothing);
    expect(find.text('状态监视'), findsOneWidget);

    await tester.tap(find.byKey(const Key('tab_monitor_encoders')));
    // EncoderZone carries a continuous gear animation, so settle-by-frame is
    // not possible; a single frame is enough to swap the offstage page.
    await tester.pump();

    expect(find.byType(EncoderZone), findsOneWidget);
    expect(find.byType(StatusZone), findsNothing);
    expect(find.text('状态监视'), findsNothing);

    await tester.tap(find.byKey(const Key('tab_monitor_status')));
    await tester.pump();

    expect(find.byType(StatusZone), findsOneWidget);
    expect(find.byType(EncoderZone), findsNothing);
  });

  testWidgets('示波页 L2 选项卡：波形 ↔ 门限（六条响度条）', (tester) async {
    final state = AppState.test(status: _status(), metas: const []);
    addTearDown(state.dispose);

    await _pump(tester, state);
    await tester.tap(find.byKey(const Key('nav_scope')));
    await tester.pumpAndSettle();

    expect(find.byType(TrendChart), findsOneWidget);

    await tester.tap(find.byKey(const Key('tab_scope_limits')));
    await tester.pumpAndSettle();

    expect(find.byType(LimitZone), findsOneWidget);
    expect(find.byType(HeatLoadBar), findsNWidgets(6));
    expect(find.text('热与负载 · 相对门限'), findsOneWidget);
    expect(find.byType(TrendChart), findsNothing);
  });

  testWidgets('控制页：启动 / 停止 / 清除故障 与 故障构成面板', (tester) async {
    final state = AppState.test(status: _status(), metas: const []);
    addTearDown(state.dispose);

    await _pump(tester, state);
    await tester.tap(find.byKey(const Key('nav_control')));
    await tester.pumpAndSettle();

    expect(find.byType(ControlZone), findsOneWidget);
    expect(find.text('启动'), findsOneWidget);
    expect(find.text('停止'), findsOneWidget);
    expect(find.text('清除故障'), findsOneWidget);
    expect(find.text('故障构成与指令'), findsOneWidget);
  });

  testWidgets('参数页：无 tab，直接呈现参数表', (tester) async {
    final state = AppState.test(status: _status(), metas: const []);
    addTearDown(state.dispose);

    await _pump(tester, state);
    await tester.tap(find.byKey(const Key('nav_params')));
    await tester.pumpAndSettle();

    expect(find.byType(ParamZone), findsOneWidget);
    expect(find.text('COMMIT 落盘'), findsOneWidget);
    // Control and params carry no L2 tab row.
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
