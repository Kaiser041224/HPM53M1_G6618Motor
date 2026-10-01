import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/widgets/vfd_bar_painter.dart';

Future<void> _pump(WidgetTester tester, Widget child) {
  return tester.pumpWidget(
    MaterialApp(theme: buildGuiTheme(), home: Scaffold(body: child)),
  );
}

VfdBarPainter _painterOf(WidgetTester tester) {
  final paint = tester.widget<CustomPaint>(
    find.descendant(
      of: find.byType(VfdBar),
      matching: find.byType(CustomPaint),
    ),
  );
  return paint.painter! as VfdBarPainter;
}

void main() {
  group('VfdBarPainter 构造常量（§2.6.5）', () {
    test('细管周期 4px：暗缝 .5 / 羽化 1 / 亮芯 1.5', () {
      expect(VfdBarPainter.tubePeriod, 4.0);
      expect(VfdBarPainter.seamWidth, 0.5);
      expect(VfdBarPainter.featherWidth, 1.0);
      expect(VfdBarPainter.coreWidth, 1.5);
      expect(
        VfdBarPainter.seamWidth +
            VfdBarPainter.featherWidth +
            VfdBarPainter.coreWidth +
            VfdBarPainter.featherWidth,
        VfdBarPainter.tubePeriod,
      );
    });

    test('亮芯 α.85 / 羽化 α.16 / 幽灵 α.028', () {
      expect(VfdBarPainter.coreAlpha, 0.85);
      expect(VfdBarPainter.featherAlpha, 0.16);
      expect(VfdBarPainter.ghostCoreAlpha, closeTo(0.028, 1e-6));
    });

    test('spill 30px、雾面 4/10/22px', () {
      expect(VfdBarPainter.spillWidth, 30.0);
      expect(VfdBarPainter.glowRadii, <double>[4, 10, 22]);
      expect(VfdBarPainter.glowAlphas, <double>[0.5, 0.3, 0.18]);
    });

    test('门限刻度 50% / 85%（α.3）', () {
      expect(VfdBarPainter.bandMarks, <double>[0.5, 0.85]);
      expect(VfdBarPainter.bandAlpha, closeTo(0.3, 1e-6));
    });

    test('峰值线 2px', () {
      expect(VfdBarPainter.peakWidth, 2.0);
    });
  });

  group('VfdBarPainter.fillWidth 按周期 floor 防半管', () {
    test('满量程等于整宽；半量程向下取整到整管', () {
      expect(VfdBarPainter.fillWidth(100, 1.0), 100.0);
      expect(VfdBarPainter.fillWidth(100, 0.5), 48.0);
      expect(VfdBarPainter.fillWidth(100, 0.0), 0.0);
      expect(VfdBarPainter.fillWidth(100, 0.01), 0.0);
    });
  });

  group('VfdBarPainter.shouldRepaint', () {
    const tint = Color(0xFF3DDB6E);

    test('相同 ratio/peak/tint 不重绘', () {
      const a = VfdBarPainter(ratio: 0.5, peak: 0.7, tint: tint);
      const b = VfdBarPainter(ratio: 0.5, peak: 0.7, tint: tint);
      expect(b.shouldRepaint(a), isFalse);
    });

    test('ratio / peak / tint 任一变化即重绘', () {
      const base = VfdBarPainter(ratio: 0.5, peak: 0.7, tint: tint);
      expect(
        const VfdBarPainter(ratio: 0.6, peak: 0.7, tint: tint).shouldRepaint(base),
        isTrue,
      );
      expect(
        const VfdBarPainter(ratio: 0.5, peak: 0.9, tint: tint).shouldRepaint(base),
        isTrue,
      );
      expect(
        const VfdBarPainter(
          ratio: 0.5,
          peak: 0.7,
          tint: Color(0xFF2ED8E8),
        ).shouldRepaint(base),
        isTrue,
      );
    });
  });

  group('VfdBar 封装', () {
    const tint = Color(0xFF2ED8E8);

    testWidgets('把 ratio/peak/tint 透传给 painter', (tester) async {
      await _pump(
        tester,
        const SizedBox(
          width: 200,
          child: VfdBar(ratio: 0.42, peak: 0.66, tint: tint),
        ),
      );

      final painter = _painterOf(tester);
      expect(painter.ratio, closeTo(0.42, 1e-6));
      expect(painter.peak, closeTo(0.66, 1e-6));
      expect(painter.tint, tint);
      expect(painter.warnBand, isTrue);
    });

    testWidgets('ratio > 1 透传（仅填充宽由 painter 钳到满量程）；peak null 不画', (
      tester,
    ) async {
      await _pump(
        tester,
        const SizedBox(width: 200, child: VfdBar(ratio: 1.8, tint: tint)),
      );

      final painter = _painterOf(tester);
      // Over-limit ratio is preserved so the caller can show the true `%`.
      expect(painter.ratio, 1.8);
      expect(painter.peak, isNull);
      // The lit width itself still saturates at full scale.
      expect(VfdBarPainter.fillWidth(200, 1.8), 200.0);
    });

    testWidgets('负 ratio 落地为 0', (tester) async {
      await _pump(
        tester,
        const SizedBox(width: 200, child: VfdBar(ratio: -0.4, tint: tint)),
      );
      expect(_painterOf(tester).ratio, 0.0);
    });

    testWidgets('warnBand 可关闭门限刻度', (tester) async {
      await _pump(
        tester,
        const SizedBox(
          width: 200,
          child: VfdBar(ratio: 0.3, tint: tint, warnBand: false),
        ),
      );
      expect(_painterOf(tester).warnBand, isFalse);
    });
  });

  group('tintForUnit 量纲专色（§2.6.3）', () {
    const palette = GuiPalette();

    test('V / ° → 青；A / RPM → 翠绿；W / % → 洋红', () {
      expect(tintForUnit(palette, 'V'), palette.valueCyan);
      expect(tintForUnit(palette, '°'), palette.valueCyan);
      expect(tintForUnit(palette, 'A'), palette.valueGreen);
      expect(tintForUnit(palette, 'RPM'), palette.valueGreen);
      expect(tintForUnit(palette, 'W'), palette.valueMagenta);
      expect(tintForUnit(palette, '%'), palette.valueMagenta);
    });

    test('°C / ℃ → 橙；Hz → 紫；未知/空 → 中性白青', () {
      expect(tintForUnit(palette, '°C'), palette.valueOrange);
      expect(tintForUnit(palette, '℃'), palette.valueOrange);
      expect(tintForUnit(palette, 'Hz'), palette.valuePurple);
      expect(tintForUnit(palette, ''), palette.fgStrong);
      expect(tintForUnit(palette, 'X'), palette.fgStrong);
    });
  });
}
