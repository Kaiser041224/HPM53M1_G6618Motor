import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/widgets/chart_history.dart';
import 'package:g6618_gui/widgets/gui_button.dart';
import 'package:g6618_gui/widgets/hairline_frame.dart';
import 'package:g6618_gui/widgets/stale_gate.dart';
import 'package:g6618_gui/widgets/trend_chart.dart';

StatusDto _sample({double iU = 0, double iV = 0, double iW = 0}) => StatusDto(
  faultFlags: 0,
  motorRunning: false,
  motorState: 0,
  freqHz: 0,
  modPct: 0,
  iU: iU,
  iV: iV,
  iW: iW,
  vbus: 0,
  ntcTemp: 0,
  encRotorDeg: 0,
  encOutputDeg: 0,
);

ChartHistory _history(int count) {
  final history = ChartHistory();
  history.pushBatch(
    List<StatusDto>.generate(
      count,
      (i) => _sample(
        iU: math.sin(i / 20) * 5,
        iV: -math.sin(i / 20) * 4,
        iW: math.cos(i / 20) * 3,
      ),
    ),
  );
  return history;
}

/// Three pinned samples, oldest first, so the painter series are predictable.
ChartHistory _three() {
  final history = ChartHistory();
  history.pushBatch(<StatusDto>[
    _sample(iU: 1.0, iV: -1.0, iW: 0.5),
    _sample(iU: 2.0, iV: -2.0, iW: 1.0),
    _sample(iU: 3.0, iV: -3.0, iW: 1.5),
  ]);
  return history;
}

Future<void> _pump(WidgetTester tester, Widget child) {
  tester.view.physicalSize = const Size(1000, 1000);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  return tester.pumpWidget(
    MaterialApp(
      theme: buildGuiTheme(),
      home: Scaffold(
        body: SingleChildScrollView(
          child: Center(child: SizedBox(width: 640, child: child)),
        ),
      ),
    ),
  );
}

Finder _chartPaint() => find.byWidgetPredicate(
  (w) => w is CustomPaint && w.painter is TrendChartPainter,
);

TrendChartState _state(WidgetTester tester) =>
    tester.state<TrendChartState>(find.byType(TrendChart));

void main() {
  group('TrendChartPainter', () {
    const palette = GuiPalette();

    test('y 轴半幅 = 窗口内最大 |样本|，钳位最小 ±1A', () {
      final empty = ChartHistory();
      expect(
        TrendChartPainter.yLimit(empty, 0, 2),
        TrendChartPainter.minYLimit,
      );

      final small = ChartHistory()..pushBatch(<StatusDto>[_sample(iU: 0.5)]);
      expect(TrendChartPainter.yLimit(small, 0, 2), 1.0);

      final big = ChartHistory()
        ..pushBatch(<StatusDto>[_sample(iU: 3.0, iV: -2.0)]);
      expect(TrendChartPainter.yLimit(big, 0, 2), 3.0);
    });

    test('抽稀：每像素列取 min/max，顶点 ≤ 2×宽度', () {
      final history = _history(2000);
      const size = Size(600, 200);
      final points = TrendChartPainter.seriesPoints(
        history,
        0,
        size,
        0,
        2,
        5,
      );

      expect(points.length, lessThanOrEqualTo(2 * size.width.ceil()));
      final ys = points.map((p) => p.dy).toList();
      expect(ys.reduce(math.min), lessThan(size.height / 2));
      expect(ys.reduce(math.max), greaterThan(size.height / 2));
    });

    test('窗口内不足 2 点为空；窗口取样只覆盖视口', () {
      final history = _history(100);
      const size = Size(600, 200);
      // Window far beyond the data → nothing to draw.
      expect(
        TrendChartPainter.seriesPoints(history, 0, size, 100, 2, 5),
        isEmpty,
      );
      // A 10 ms window covers ~10 samples, so the trace is sparse.
      final windowed = TrendChartPainter.seriesPoints(
        history,
        0,
        size,
        0.05,
        0.01,
        5,
      );
      expect(windowed.length, lessThanOrEqualTo(2 * size.width.ceil()));
      expect(windowed, isNotEmpty);
    });

    test('三通道 1.5px 细线：描边、扁平无辉光', () {
      final painter = TrendChartPainter(
        history: _three(),
        colors: <Color>[palette.series1, palette.series2, palette.series3],
        gridColor: palette.lineDim,
        viewStartSec: 0,
        viewSpanSec: 2,
      );
      final paints = painter.buildSeriesPaints();
      expect(paints, hasLength(3));
      for (final p in paints) {
        expect(p.strokeWidth, TrendChartPainter.strokeWidth);
        expect(p.style, PaintingStyle.stroke);
        expect(p.maskFilter, isNull);
        expect(p.shader, isNull);
      }
      expect(paints[0].color.toARGB32(), palette.series1.toARGB32());
      expect(paints[1].color.toARGB32(), palette.series2.toARGB32());
      expect(paints[2].color.toARGB32(), palette.series3.toARGB32());
    });

    test('窗口变化触发重绘，相同则否', () {
      TrendChartPainter make(double start, double span) => TrendChartPainter(
        history: _three(),
        colors: const <Color>[Colors.red, Colors.green, Colors.blue],
        gridColor: palette.lineDim,
        viewStartSec: start,
        viewSpanSec: span,
      );
      final a = make(0, 2);
      expect(a.shouldRepaint(make(1, 2)), isTrue);
      expect(a.shouldRepaint(make(0, 4)), isTrue);
      expect(a.shouldRepaint(make(0, 2)), isFalse);
    });

    test('图注声明 1kHz / 60 s 历史 / 拖动缩放 / 一级流预留', () {
      expect(TrendChart.caption, contains('1 kHz'));
      expect(TrendChart.caption, contains('60 s 历史'));
      expect(TrendChart.caption, contains('拖动'));
      expect(TrendChart.caption, contains('缩放'));
      expect(TrendChart.caption, contains('一级流预留'));
    });

    test('高倍缩放：可见样本 ≤120 出采样点标记，>120 回纯线', () {
      const colors = <Color>[Colors.red, Colors.green, Colors.blue];
      final fine = TrendChartPainter(
        history: _history(100),
        colors: colors,
        gridColor: palette.lineDim,
        viewStartSec: 0.05,
        viewSpanSec: 0.01,
      );
      expect(fine.showsMarkers, isTrue);
      final markers = fine.buildMarkerPaints();
      expect(markers, hasLength(3));
      for (final p in markers) {
        expect(p.style, PaintingStyle.fill);
        expect(p.maskFilter, isNull);
      }
      expect(TrendChartPainter.markerSize, 3);

      final coarse = TrendChartPainter(
        history: _history(3000),
        colors: colors,
        gridColor: palette.lineDim,
        viewStartSec: 0,
        viewSpanSec: 2,
      );
      expect(coarse.showsMarkers, isFalse);
    });

    test('时间单位自适应：<1s 用 ms，≥1s 用 s', () {
      expect(formatAxisSeconds(0), '0');
      expect(formatAxisSeconds(0.01), '10ms');
      expect(formatAxisSeconds(0.05), '50ms');
      expect(formatAxisSeconds(0.2), '200ms');
      expect(formatAxisSeconds(0.5), '500ms');
      expect(formatAxisSeconds(2), '2s');
      expect(formatAxisSeconds(10), '10s');
      expect(formatAxisSeconds(60), '60s');
    });

    test('windowExtrema：窗口内逐通道有符号 min/max；无数据为 null', () {
      final history = ChartHistory()..pushBatch(<StatusDto>[
        _sample(iU: 1, iV: -2, iW: 0.5),
        _sample(iU: -3, iV: 4, iW: -1.5),
        _sample(iU: 2, iV: -1, iW: 0.5),
      ]);

      // Default 1 kHz → samples at 0 / 1 / 2 ms; the 10 ms window holds all.
      final extrema = TrendChartPainter.windowExtrema(history, 0, 0.01);
      expect(extrema, hasLength(3));
      expect(extrema[0]!.max, 2.0);
      expect(extrema[0]!.min, -3.0);
      expect(extrema[1]!.max, 4.0);
      expect(extrema[1]!.min, -2.0);
      expect(extrema[2]!.max, 0.5);
      expect(extrema[2]!.min, -1.5);

      // A window past the retained history has no sample on any channel.
      expect(TrendChartPainter.windowExtrema(history, 100, 1), <Object?>[
        null,
        null,
        null,
      ]);
      expect(
        TrendChartPainter.windowExtrema(ChartHistory(), 0, 2),
        <Object?>[null, null, null],
      );
    });

    test('formatSigned / formatExtrema：有符号两位，空为 null', () {
      expect(TrendChart.formatSigned(10.02), '+10.02');
      expect(TrendChart.formatSigned(-9.87), '-9.87');
      expect(TrendChart.formatExtrema(10.02, -9.87), '+10.02 / -9.87');
      expect(TrendChart.formatExtrema(null, null), isNull);
      expect(TrendChart.formatExtrema(1.0, null), isNull);
    });

    test('轴标单位由 span 决定：inspect 远偏移仍用 ms', () {
      expect(formatAxisOffset(0, 0.1), '0');
      expect(formatAxisOffset(5.1, 0.1), '5100ms');
      expect(formatAxisOffset(0.05, 0.1), '50ms');
      expect(formatAxisOffset(1, 2), '1s');
    });

    test('绘图区高度 ≥320 px', () {
      expect(TrendChart.plotHeight, greaterThanOrEqualTo(320));
    });

    test('网格自适应：竖线 ≤8 条并随窗口变密', () {
      final narrow = TrendChartPainter.gridLines(200);
      expect(narrow.length, lessThanOrEqualTo(8));
      expect(narrow.first, 0);
      expect(narrow.last, 200);

      final wide = TrendChartPainter.gridLines(800);
      expect(wide.length, narrow.length);
      // 细窗口下同一组竖线间距更小（更密）。
      expect(narrow[1] - narrow[0], lessThan(wide[1] - wide[0]));
    });
  });

  group('TrendChart 空态', () {
    testWidgets('无数据：网格仍在 + 居中 —（unknown 语义）', (tester) async {
      await _pump(tester, TrendChart(history: ChartHistory()));

      expect(_chartPaint(), findsOneWidget);
      expect(find.text('—'), findsWidgets);
    });
  });

  group('TrendChart 渲染', () {
    testWidgets('三通道映射 + 图注 + 图例 U/V/W', (tester) async {
      await _pump(tester, TrendChart(history: _three()));

      final painter =
          tester.widget<CustomPaint>(_chartPaint()).painter
              as TrendChartPainter;
      expect(painter.history.len, 3);
      expect(painter.history.channelAt(0, 0), 1.0);
      expect(painter.history.channelAt(0, 2), 3.0);

      expect(find.text(TrendChart.caption), findsOneWidget);
      expect(find.textContaining('1 kHz 采样'), findsOneWidget);
      expect(find.textContaining('一级流预留'), findsOneWidget);
      // Time axis: live-tail 2 s window, 1 s midpoint, now. `2s` also appears
      // on the SPAN readout and the 2s preset, so match at least one.
      expect(find.text('2s'), findsWidgets);
      expect(find.text('1s'), findsOneWidget);
      expect(find.text('0'), findsOneWidget);
      // Channel letters now come only from the extrema row; the old U/V/W live
      // legend was removed.
      expect(find.text('U'), findsOneWidget);
      expect(find.text('V'), findsOneWidget);
      expect(find.text('W'), findsOneWidget);
      // Signed window extrema in the readout row.
      expect(find.text('+3.00 / +1.00'), findsOneWidget);
      expect(find.text('-1.00 / -3.00'), findsOneWidget);
      expect(find.text('+1.50 / +0.50'), findsOneWidget);
    });

    testWidgets('图表外框为 HairlineFrame main', (tester) async {
      await _pump(tester, TrendChart(history: _three()));

      final frame = tester.widget<HairlineFrame>(find.byType(HairlineFrame));
      expect(frame.variant, HairlineVariant.main);
    });

    testWidgets('stale：图注叠加 StatusBadge STALE 徽标', (tester) async {
      await _pump(
        tester,
        TrendChart(history: _three(), freshness: Freshness.stale),
      );

      expect(find.text('STALE'), findsOneWidget);
    });

    testWidgets('live：无 STALE 徽标，LIVE Chip 在场', (tester) async {
      await _pump(
        tester,
        TrendChart(history: _three(), freshness: Freshness.live),
      );

      expect(find.text('STALE'), findsNothing);
      expect(find.text('LIVE'), findsOneWidget);
    });
  });

  group('TrendChart 窗口极值与三相和行', () {
    testWidgets('极值行显示窗口内 U/V/W 有符号 max/min（2 位 / green）', (tester) async {
      await _pump(tester, TrendChart(history: _three()));

      final row = find.byKey(TrendChart.extremaRowKey);
      expect(row, findsOneWidget);
      // U max 3 / min 1, V max −1 / min −3, W max 1.5 / min 0.5.
      expect(
        find.descendant(of: row, matching: find.text('+3.00 / +1.00')),
        findsOneWidget,
      );
      expect(
        find.descendant(of: row, matching: find.text('-1.00 / -3.00')),
        findsOneWidget,
      );
      expect(
        find.descendant(of: row, matching: find.text('+1.50 / +0.50')),
        findsOneWidget,
      );
      // Values take the A-dimension green; channel letters take series colours.
      final value = tester.widget<Text>(
        find.descendant(of: row, matching: find.text('+3.00 / +1.00')),
      );
      expect(value.style?.color, const GuiPalette().valueGreen);
      expect(
        tester
            .widget<Text>(find.descendant(of: row, matching: find.text('U')))
            .style
            ?.color,
        const GuiPalette().series1,
      );
    });

    testWidgets('极值行窗口无数据：单个 —', (tester) async {
      await _pump(tester, TrendChart(history: ChartHistory()));

      final row = find.byKey(TrendChart.extremaRowKey);
      expect(row, findsOneWidget);
      expect(
        find.descendant(of: row, matching: find.text('—')),
        findsOneWidget,
      );
    });

    testWidgets('ΣI 行：最新样本三相和（有符号 / magenta / 三相和）', (tester) async {
      await _pump(tester, TrendChart(history: _three()));

      final row = find.byKey(TrendChart.sigmaRowKey);
      expect(row, findsOneWidget);
      // Newest sample: iU 3 + iV −3 + iW 1.5 = +1.5.
      final sum = find.descendant(of: row, matching: find.text('+1.50'));
      expect(sum, findsOneWidget);
      expect(
        tester.widget<Text>(sum).style?.color,
        const GuiPalette().valueMagenta,
      );
      expect(
        find.descendant(of: row, matching: find.text('三相和')),
        findsOneWidget,
      );
    });

    testWidgets('ΣI 符号正确性：三相和为负时带负号', (tester) async {
      final history = ChartHistory()
        ..pushBatch(<StatusDto>[
          _sample(iU: 1.0, iV: 2.0, iW: 3.0),
          _sample(iU: -4.0, iV: 2.0, iW: 0.5),
        ]);
      await _pump(tester, TrendChart(history: history));

      final row = find.byKey(TrendChart.sigmaRowKey);
      // Newest sample: −4 + 2 + 0.5 = −1.5.
      final sum = find.descendant(of: row, matching: find.text('-1.50'));
      expect(sum, findsOneWidget);
      expect(
        tester.widget<Text>(sum).style?.color,
        const GuiPalette().valueMagenta,
      );
    });

    testWidgets('ΣI 无数据：—', (tester) async {
      await _pump(tester, TrendChart(history: ChartHistory()));

      final row = find.byKey(TrendChart.sigmaRowKey);
      expect(row, findsOneWidget);
      expect(
        find.descendant(of: row, matching: find.text('—')),
        findsOneWidget,
      );
    });
  });

  group('TrendChart 波形高刷（节流移除）', () {
    test('UI 刷新上限提升到 50 Hz（20 ms 通知窗口）', () {
      expect(kUiRefreshHz, 50);
    });

    testWidgets('新样本即时推进视口（无 100ms 数据节流）', (tester) async {
      final history = ChartHistory()
        ..pushBatch(List<StatusDto>.generate(50, (i) => _sample(iU: i / 10)));
      await _pump(tester, TrendChart(history: history));

      TrendChartPainter painterOf() =>
          tester.widget<CustomPaint>(_chartPaint()).painter!
              as TrendChartPainter;

      final before = painterOf().viewStartSec;
      history.pushBatch(
        List<StatusDto>.generate(40, (i) => _sample(iU: (i + 50) / 10)),
      );

      // The parent rebuild (AppState notification) is the only gate now; there
      // is no internal 100 ms chart timer to wait out.
      await _pump(tester, TrendChart(history: history));

      expect(
        painterOf().viewStartSec,
        greaterThan(before),
        reason: 'the live viewport follows every batch with no chart throttle',
      );
    });

    testWidgets('示数（极值 / ΣI）保持 5 Hz 节流：<200ms 不更新，200ms 才更新', (
      tester,
    ) async {
      final history = ChartHistory()
        ..pushBatch(<StatusDto>[_sample(iU: 1), _sample(iU: 2), _sample(iU: 3)]);
      await _pump(tester, TrendChart(history: history));
      expect(find.text('+3.00 / +1.00'), findsOneWidget);

      history.pushBatch(<StatusDto>[_sample(iU: 50)]);
      await _pump(tester, TrendChart(history: history));

      expect(
        find.text('+50.00 / +1.00'),
        findsNothing,
        reason: 'readouts keep the 200 ms digit throttle',
      );

      await tester.pump(const Duration(milliseconds: 200));
      expect(find.text('+50.00 / +1.00'), findsOneWidget);
    });
  });

  group('TrendChart 视口', () {
    testWidgets('默认 live-tail：span 2s / anchor 0', (tester) async {
      await _pump(tester, TrendChart(history: _history(3000)));
      final state = _state(tester);
      expect(state.viewSpanSec, 2.0);
      expect(state.anchorSec, 0.0);
      expect(state.isLive, isTrue);
    });

    testWidgets('水平拖入 inspect，LIVE 回尾', (tester) async {
      await _pump(tester, TrendChart(history: _history(3000)));

      await tester.drag(find.byType(TrendChart), const Offset(120, 0));
      await tester.pump();

      final state = _state(tester);
      expect(state.anchorSec, greaterThan(0));
      expect(state.isLive, isFalse);

      await tester.tap(find.text('LIVE'));
      await tester.pump();
      expect(state.anchorSec, 0.0);
      expect(state.isLive, isTrue);

      // Flush the double-tap recognizer's first-tap countdown timer.
      await tester.pump(const Duration(milliseconds: 350));
    });

    testWidgets('滚轮缩放 span 钳位 0.01..60', (tester) async {
      await _pump(tester, TrendChart(history: _history(3000)));
      final state = _state(tester);
      final center = tester.getCenter(find.byType(TrendChart));
      final pointer = TestPointer(1, PointerDeviceKind.mouse);
      pointer.hover(center);

      // Scroll up (negative dy) zooms in to the 10 ms floor.
      await tester.sendEventToBinding(pointer.scroll(const Offset(0, -5000)));
      await tester.pump();
      expect(state.viewSpanSec, closeTo(0.01, 1e-9));

      // Scroll down (positive dy) zooms out to the 60 s ceiling.
      await tester.sendEventToBinding(pointer.scroll(const Offset(0, 5000)));
      await tester.pump();
      expect(state.viewSpanSec, closeTo(60.0, 1e-9));
    });

    testWidgets('绘图区实际高度 ≥320', (tester) async {
      await _pump(tester, TrendChart(history: _history(10)));
      expect(
        tester.getSize(find.byKey(TrendChart.plotKey)).height,
        greaterThanOrEqualTo(320),
      );
    });

    testWidgets('时间单位：span=0.1 轴标/读数含 ms，span=2 含 s', (tester) async {
      await _pump(tester, TrendChart(history: _history(3000)));
      final state = _state(tester);

      state.setSpanSec(0.1);
      await tester.pump();
      expect(find.textContaining('ms'), findsWidgets);

      final gesture = await tester.createGesture(kind: PointerDeviceKind.mouse);
      await gesture.addPointer(location: Offset.zero);
      addTearDown(gesture.removePointer);
      final rect = tester.getRect(find.byKey(TrendChart.plotKey));
      await gesture.moveTo(rect.center);
      await tester.pump();
      expect(
        find.descendant(
          of: find.byKey(TrendChart.readoutKey),
          matching: find.textContaining('ms'),
        ),
        findsOneWidget,
      );
      await gesture.moveTo(const Offset(0, 0));
      await tester.pump();

      state.setSpanSec(2);
      await tester.pump();
      expect(find.text('1s'), findsOneWidget);

      await gesture.moveTo(rect.center);
      await tester.pump();
      expect(
        find.descendant(
          of: find.byKey(TrendChart.readoutKey),
          matching: find.text('t = −1.00 s'),
        ),
        findsOneWidget,
      );
    });
  });

  group('TrendChart 控制件', () {
    testWidgets('SPAN 滑块存在：程序化设值生效并钳位 0.01..60', (tester) async {
      await _pump(tester, TrendChart(history: _history(3000)));
      final state = _state(tester);

      expect(find.byKey(TrendChart.spanSliderKey), findsOneWidget);

      state.setSpanSec(10);
      await tester.pump();
      expect(state.viewSpanSec, 10.0);

      state.setSpanSec(1000);
      await tester.pump();
      expect(state.viewSpanSec, 60.0);

      state.setSpanSec(0.01);
      await tester.pump();
      expect(state.viewSpanSec, 0.01);

      state.setSpanSec(0.001);
      await tester.pump();
      expect(state.viewSpanSec, 0.01);
    });

    testWidgets('− / + 按钮 span×2 / ÷2 并钳位', (tester) async {
      await _pump(tester, TrendChart(history: _history(3000)));
      final state = _state(tester);
      expect(state.viewSpanSec, 2.0);

      await tester.tap(find.byKey(TrendChart.zoomInKey));
      await tester.pump();
      expect(state.viewSpanSec, 1.0);

      // Keep halving until the 10 ms floor: 2 → … → 0.015625 → 0.01.
      for (var i = 0; i < 10; i++) {
        await tester.tap(find.byKey(TrendChart.zoomInKey));
        await tester.pump();
      }
      expect(state.viewSpanSec, closeTo(0.01, 1e-9));

      for (var i = 0; i < 20; i++) {
        await tester.tap(find.byKey(TrendChart.zoomOutKey));
        await tester.pump();
      }
      expect(state.viewSpanSec, 60.0);
    });

    testWidgets('六档预设各自设 span 并回尾', (tester) async {
      await _pump(tester, TrendChart(history: _history(3000)));
      final state = _state(tester);

      const presets = <double>[0.01, 0.05, 0.2, 2, 10, 60];
      expect(TrendChart.spanPresets, presets);

      for (final preset in presets) {
        await tester.drag(find.byType(TrendChart), const Offset(120, 0));
        await tester.pump();
        expect(state.isLive, isFalse);

        await tester.tap(
          find.widgetWithText(GuiButton, formatAxisSeconds(preset)),
        );
        await tester.pump();
        expect(state.viewSpanSec, closeTo(preset, 1e-9));
        expect(state.isLive, isTrue);
      }

      await tester.pump(const Duration(milliseconds: 350));
    });

    testWidgets('预设 60s 设 span 并回尾', (tester) async {
      await _pump(tester, TrendChart(history: _history(3000)));
      final state = _state(tester);

      await tester.drag(find.byType(TrendChart), const Offset(120, 0));
      await tester.pump();
      expect(state.isLive, isFalse);

      await tester.tap(find.text('60s'));
      await tester.pump();
      expect(state.viewSpanSec, 60.0);
      expect(state.isLive, isTrue);

      await tester.pump(const Duration(milliseconds: 350));
    });

    testWidgets('LIVE 按钮：inspect → live', (tester) async {
      await _pump(tester, TrendChart(history: _history(3000)));
      final state = _state(tester);

      await tester.drag(find.byType(TrendChart), const Offset(120, 0));
      await tester.pump();
      expect(state.anchorSec, greaterThan(0));

      await tester.tap(find.byKey(TrendChart.liveKey));
      await tester.pump();
      expect(state.anchorSec, 0.0);
      expect(state.isLive, isTrue);

      await tester.pump(const Duration(milliseconds: 350));
    });
  });

  group('TrendChart 十字光标', () {
    testWidgets('悬停显示读数（t + U/V/W 最近样本），离开隐藏', (tester) async {
      final history = ChartHistory(sampleRateHz: 1)
        ..pushBatch(<StatusDto>[
          _sample(iU: 5, iV: -5, iW: 2.5),
          _sample(iU: 10, iV: -10, iW: 5),
          _sample(iU: 15, iV: -15, iW: 7.5),
          _sample(iU: 20, iV: -20, iW: 10),
          _sample(iU: 25, iV: -25, iW: 12.5),
        ]);
      await _pump(tester, TrendChart(history: history));

      expect(find.byKey(TrendChart.readoutKey), findsNothing);

      final gesture = await tester.createGesture(kind: PointerDeviceKind.mouse);
      await gesture.addPointer(location: Offset.zero);
      addTearDown(gesture.removePointer);
      final rect = tester.getRect(find.byKey(TrendChart.plotKey));
      // Centre of the live-tail 2 s window = t 3.0 s → nearest sample index 3.
      await gesture.moveTo(rect.center);
      await tester.pump();

      final readout = find.byKey(TrendChart.readoutKey);
      expect(readout, findsOneWidget);
      expect(
        find.descendant(of: readout, matching: find.text('t = −1.00 s')),
        findsOneWidget,
      );
      expect(
        find.descendant(of: readout, matching: find.text('20.000')),
        findsOneWidget,
      );
      expect(
        find.descendant(of: readout, matching: find.text('-20.000')),
        findsOneWidget,
      );
      expect(
        find.descendant(of: readout, matching: find.text('10.000')),
        findsOneWidget,
      );

      await gesture.moveTo(const Offset(0, 0));
      await tester.pump();
      expect(find.byKey(TrendChart.readoutKey), findsNothing);
    });
  });
}
