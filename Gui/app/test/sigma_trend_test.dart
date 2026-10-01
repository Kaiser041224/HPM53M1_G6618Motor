import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/widgets/chart_history.dart';
import 'package:g6618_gui/widgets/hairline_frame.dart';
import 'package:g6618_gui/widgets/sigma_trend.dart';

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
      (i) => _sample(iU: math.sin(i / 20) * 5),
    ),
  );
  return history;
}

/// Three pinned samples (sigma 0.5 / 1.0 / 1.5), oldest first.
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
  tester.view.physicalSize = const Size(800, 600);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  return tester.pumpWidget(
    MaterialApp(
      theme: buildGuiTheme(),
      home: Scaffold(body: Center(child: child)),
    ),
  );
}

Finder _sigmaPaint() => find.byWidgetPredicate(
  (w) => w is CustomPaint && w.painter is SigmaTrendPainter,
);

void main() {
  group('SigmaTrend 逻辑', () {
    test('sigmaAt：逐样本三相和 iu + iv + iw', () {
      final history = ChartHistory()
        ..pushBatch(<StatusDto>[
          _sample(iU: 1, iV: 2, iW: 3),
          _sample(iU: -4, iV: 2, iW: 0.5),
        ]);
      expect(SigmaTrend.sigmaAt(history, 0), 6.0);
      expect(SigmaTrend.sigmaAt(history, 1), -1.5);
    });

    test('yLimit：窗口内 σ 的 ±max(|min|,|max|)，退化窗给 ±0.5', () {
      expect(SigmaTrend.yLimit(ChartHistory(), 0, 2), SigmaTrend.minYLimit);

      final zero = ChartHistory()..pushBatch(<StatusDto>[_sample()]);
      expect(SigmaTrend.yLimit(zero, 0, 2), SigmaTrend.minYLimit);

      final mixed = ChartHistory()
        ..pushBatch(<StatusDto>[
          _sample(iU: 1, iV: 2, iW: 3), // σ 6
          _sample(iU: 0, iV: 0, iW: 0), // σ 0
          _sample(iU: 2, iV: -5, iW: 1), // σ -2
        ]);
      expect(SigmaTrend.yLimit(mixed, 0, 0.01), 6.0);

      // A window past the retained history has no sample → degenerate ±0.5.
      expect(SigmaTrend.yLimit(mixed, 100, 1), SigmaTrend.minYLimit);
    });

    test('seriesPoints：像素列 min/max 抽稀，顶点 ≤ 2×宽度；窗内不足 2 点为空', () {
      final history = _history(2000);
      const size = Size(100, SigmaTrend.height);
      final points = SigmaTrend.seriesPoints(history, size, 0, 2, 5);

      expect(points.length, lessThanOrEqualTo(2 * size.width.ceil()));
      final ys = points.map((p) => p.dy).toList();
      expect(ys.reduce(math.min), lessThan(size.height / 2));
      expect(ys.reduce(math.max), greaterThan(size.height / 2));

      // Empty ring / window beyond the data → nothing to draw.
      expect(
        SigmaTrend.seriesPoints(ChartHistory(), size, 0, 2, 5),
        isEmpty,
      );
      expect(SigmaTrend.seriesPoints(history, size, 100, 2, 5), isEmpty);

      // Fewer samples than columns → one vertex per sample.
      final sparse = SigmaTrend.seriesPoints(_three(), size, 0, 0.01, 5);
      expect(sparse, hasLength(3));
    });

    test('formatBound：一位小数带显式符号', () {
      expect(SigmaTrend.formatBound(1.5), '+1.5');
      expect(SigmaTrend.formatBound(-1.5), '-1.5');
      expect(SigmaTrend.formatBound(0.0), '+0.0');
    });
  });

  group('SigmaTrend 组件', () {
    testWidgets('渲染：200×88 sub 外框 + magenta 1.5px 折线（无辉光）', (tester) async {
      await _pump(
        tester,
        SigmaTrend(history: _three(), viewStartSec: 0, viewSpanSec: 0.01),
      );

      expect(tester.getSize(find.byKey(SigmaTrend.frameKey)), const Size(200, 88));
      final frame = tester.widget<HairlineFrame>(
        find.descendant(
          of: find.byKey(SigmaTrend.frameKey),
          matching: find.byType(HairlineFrame),
        ),
      );
      expect(frame.variant, HairlineVariant.sub);

      expect(_sigmaPaint(), findsOneWidget);
      final painter =
          tester.widget<CustomPaint>(_sigmaPaint()).painter!
              as SigmaTrendPainter;
      final line = painter.buildSeriesPaint();
      expect(line.color.toARGB32(), const GuiPalette().valueMagenta.toARGB32());
      expect(line.strokeWidth, SigmaTrend.strokeWidth);
      expect(line.style, PaintingStyle.stroke);
      expect(line.maskFilter, isNull);
      expect(line.shader, isNull);
    });

    testWidgets('空数据：居中 —', (tester) async {
      await _pump(
        tester,
        SigmaTrend(history: ChartHistory(), viewStartSec: 0, viewSpanSec: 2),
      );

      expect(find.byKey(SigmaTrend.frameKey), findsOneWidget);
      expect(_sigmaPaint(), findsNothing);
      expect(find.text('—'), findsOneWidget);
    });

    testWidgets('y 极值双向标注：+/− bound（fgDim 小字）', (tester) async {
      await _pump(
        tester,
        SigmaTrend(history: _three(), viewStartSec: 0, viewSpanSec: 0.01),
      );

      final plus = tester.widget<Text>(find.text('+1.5'));
      final minus = tester.widget<Text>(find.text('-1.5'));
      for (final label in <Text>[plus, minus]) {
        expect(label.style?.color, const GuiPalette().fgDim);
        expect(label.style?.fontSize, greaterThanOrEqualTo(9));
        expect(label.style?.fontSize, lessThanOrEqualTo(11));
      }
      // +bound sits above −bound.
      expect(
        tester.getCenter(find.text('+1.5')).dy,
        lessThan(tester.getCenter(find.text('-1.5')).dy),
      );
    });
  });
}
