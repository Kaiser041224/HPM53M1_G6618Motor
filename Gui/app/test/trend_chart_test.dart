import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/bridge/frb/api.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/widgets/chart_history.dart';
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
      expect(find.text('U'), findsOneWidget);
      expect(find.text('V'), findsOneWidget);
      expect(find.text('W'), findsOneWidget);
      expect(find.text('3.000'), findsOneWidget);
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

    testWidgets('滚轮缩放 span 钳位 0.5..60', (tester) async {
      await _pump(tester, TrendChart(history: _history(3000)));
      final state = _state(tester);
      final center = tester.getCenter(find.byType(TrendChart));
      final pointer = TestPointer(1, PointerDeviceKind.mouse);
      pointer.hover(center);

      // Scroll up (negative dy) zooms in to the 0.5 s floor.
      await tester.sendEventToBinding(pointer.scroll(const Offset(0, -500)));
      await tester.pump();
      expect(state.viewSpanSec, closeTo(0.5, 1e-9));

      // Scroll down (positive dy) zooms out to the 60 s ceiling.
      await tester.sendEventToBinding(pointer.scroll(const Offset(0, 5000)));
      await tester.pump();
      expect(state.viewSpanSec, closeTo(60.0, 1e-9));
    });
  });

  group('TrendChart 控制件', () {
    testWidgets('SPAN 滑块存在：程序化设值生效并钳位 0.5..60', (tester) async {
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
      expect(state.viewSpanSec, 0.5);
    });

    testWidgets('− / + 按钮 span×2 / ÷2 并钳位', (tester) async {
      await _pump(tester, TrendChart(history: _history(3000)));
      final state = _state(tester);
      expect(state.viewSpanSec, 2.0);

      await tester.tap(find.byKey(TrendChart.zoomInKey));
      await tester.pump();
      expect(state.viewSpanSec, 1.0);

      await tester.tap(find.byKey(TrendChart.zoomInKey));
      await tester.pump();
      expect(state.viewSpanSec, 0.5);

      await tester.tap(find.byKey(TrendChart.zoomInKey));
      await tester.pump();
      expect(state.viewSpanSec, 0.5);

      for (var i = 0; i < 10; i++) {
        await tester.tap(find.byKey(TrendChart.zoomOutKey));
        await tester.pump();
      }
      expect(state.viewSpanSec, 60.0);
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
