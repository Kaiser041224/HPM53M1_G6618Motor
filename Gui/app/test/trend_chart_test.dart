import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/widgets/hairline_frame.dart';
import 'package:g6618_gui/widgets/stale_gate.dart';
import 'package:g6618_gui/widgets/trend_chart.dart';

/// Three polled samples, oldest first, so the painter series are predictable.
const List<TrendSample> _samples = <TrendSample>[
  (iU: 1.0, iV: -1.0, iW: 0.5),
  (iU: 2.0, iV: -2.0, iW: 1.0),
  (iU: 3.0, iV: -3.0, iW: 1.5),
];

Future<void> _pump(WidgetTester tester, Widget child) {
  tester.view.physicalSize = const Size(1000, 1000);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  return tester.pumpWidget(
    MaterialApp(
      theme: buildGuiTheme(),
      home: Scaffold(
        body: Center(child: SizedBox(width: 640, child: child)),
      ),
    ),
  );
}

Finder _chartPaint() => find.byWidgetPredicate(
  (w) => w is CustomPaint && w.painter is TrendChartPainter,
);

void main() {
  group('TrendChartPainter', () {
    const palette = GuiPalette();

    test('y 轴半幅 = 最大 |样本|，钳位最小 ±1A', () {
      expect(TrendChartPainter.yLimit(const <List<double>>[[]]), 1.0);
      expect(
        TrendChartPainter.yLimit(const <List<double>>[
          [0.5, -0.3, 0.2],
        ]),
        1.0,
      );
      expect(TrendChartPainter.yLimit(const <List<double>>[[3.0, -2.0]]), 3.0);
      expect(
        TrendChartPainter.yLimit(const <List<double>>[
          [1.0],
          [double.nan, -7.5],
        ]),
        7.5,
      );
    });

    test('抽稀：2000 点每像素列取 min/max，顶点 ≤ 2×宽度', () {
      final values = List<double>.generate(
        2000,
        (i) => math.sin(i / 20) * 5,
      );
      const size = Size(600, 200);
      final points = TrendChartPainter.seriesPoints(values, size, 5);

      expect(points.length, lessThanOrEqualTo(2 * size.width.ceil()));
      // Peaks survive the downsample: both envelope extremes are still reached.
      final ys = points.map((p) => p.dy).toList();
      expect(ys.reduce(math.min), lessThan(size.height / 2));
      expect(ys.reduce(math.max), greaterThan(size.height / 2));
    });

    test('抽稀：样本少于列数时逐点绘制；< 2 点为空', () {
      expect(
        TrendChartPainter.seriesPoints(const <double>[], const Size(600, 200), 1),
        isEmpty,
      );
      expect(
        TrendChartPainter.seriesPoints(const <double>[1], const Size(600, 200), 1),
        isEmpty,
      );
      final points = TrendChartPainter.seriesPoints(
        const <double>[1, 2, 3],
        const Size(600, 200),
        1,
      );
      expect(points, hasLength(3));
    });

    test('窗口容量 2000 点 = 2 s @1kHz', () {
      expect(TrendChart.capacity, 2000);
      expect(TrendChart.caption, contains('1 kHz'));
      expect(TrendChart.caption, contains('2 s 窗'));
      expect(TrendChart.caption, contains('STREAM_SUB'));
    });

    test('三通道 1.5px 细线：描边、扁平无辉光', () {
      final painter = TrendChartPainter(
        series: const <List<double>>[
          [1, 2],
          [3, 4],
          [5, 6],
        ],
        colors: <Color>[palette.series1, palette.series2, palette.series3],
        gridColor: palette.lineDim,
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

    test('数据变化触发重绘，相同则否', () {
      TrendChartPainter make(List<List<double>> s) => TrendChartPainter(
        series: s,
        colors: const <Color>[Colors.red, Colors.green, Colors.blue],
        gridColor: palette.lineDim,
      );
      final a = make(const <List<double>>[
        [1, 2],
      ]);
      expect(
        a.shouldRepaint(
          make(const <List<double>>[
            [1, 3],
          ]),
        ),
        isTrue,
      );
      expect(
        a.shouldRepaint(
          make(const <List<double>>[
            [1, 2],
          ]),
        ),
        isFalse,
      );
    });
  });

  group('TrendChart 空态', () {
    testWidgets('不足 2 点：网格仍在 + 居中 —（unknown 语义）', (tester) async {
      await _pump(
        tester,
        const TrendChart(history: <TrendSample>[(iU: 0, iV: 0, iW: 0)]),
      );

      expect(_chartPaint(), findsOneWidget);
      expect(find.text('—'), findsWidgets);
    });
  });

  group('TrendChart 渲染', () {
    testWidgets('三通道折线映射 + 图注 + 图例 U/V/W', (tester) async {
      await _pump(tester, const TrendChart(history: _samples));

      final painter =
          tester.widget<CustomPaint>(_chartPaint()).painter
              as TrendChartPainter;
      expect(painter.series, hasLength(3));
      expect(painter.series[0], <double>[1.0, 2.0, 3.0]);
      expect(painter.series[1], <double>[-1.0, -2.0, -3.0]);
      expect(painter.series[2], <double>[0.5, 1.0, 1.5]);

      expect(find.text(TrendChart.caption), findsOneWidget);
      expect(find.textContaining('1 kHz 采样'), findsOneWidget);
      expect(find.textContaining('STREAM_SUB'), findsOneWidget);
      // Time axis: 2 s window, 1 s midpoint, now.
      expect(find.text('2s'), findsOneWidget);
      expect(find.text('1s'), findsOneWidget);
      expect(find.text('0'), findsOneWidget);
      expect(find.text('U'), findsOneWidget);
      expect(find.text('V'), findsOneWidget);
      expect(find.text('W'), findsOneWidget);
      expect(find.text('3.000'), findsOneWidget);
    });

    testWidgets('图表外框为 HairlineFrame main', (tester) async {
      await _pump(tester, const TrendChart(history: _samples));

      final frame = tester.widget<HairlineFrame>(find.byType(HairlineFrame));
      expect(frame.variant, HairlineVariant.main);
    });

    testWidgets('stale：图注叠加 StatusBadge STALE 徽标', (tester) async {
      await _pump(
        tester,
        const TrendChart(history: _samples, freshness: Freshness.stale),
      );

      expect(find.text('STALE'), findsOneWidget);
      expect(find.text('∅'), findsOneWidget);
    });

    testWidgets('live：无 STALE 徽标', (tester) async {
      await _pump(
        tester,
        const TrendChart(history: _samples, freshness: Freshness.live),
      );

      expect(find.text('STALE'), findsNothing);
    });
  });
}
