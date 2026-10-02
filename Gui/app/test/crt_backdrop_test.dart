import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/widgets/crt_backdrop.dart';

/// CRT 背景层：静止扫描线 + ~12Hz 动态噪点，且噪点重绘被 RepaintBoundary 隔离。
void main() {
  group('CrtBackdrop CRT 纹理', () {
    testWidgets('存在：扫描线 + 噪点两层，RepaintBoundary 隔离', (tester) async {
      await tester.pumpWidget(
        MaterialApp(
          theme: buildGuiTheme(),
          home: const Scaffold(body: CrtBackdrop()),
        ),
      );

      expect(find.byKey(const Key('crt_backdrop')), findsOneWidget);
      final canvas = find.byKey(const Key('crt_canvas'));
      expect(canvas, findsOneWidget);
      final paint = tester.widget<CustomPaint>(canvas);
      expect(paint.painter, isA<CrtScanlinePainter>());
      expect(paint.foregroundPainter, isA<CrtNoisePainter>());
      expect(
        find.descendant(
          of: find.byKey(const Key('crt_backdrop')),
          matching: find.byType(RepaintBoundary),
        ),
        findsOneWidget,
      );

      await tester.pumpWidget(const SizedBox());
    });

    testWidgets('噪点按 ~12Hz 重绘：generation 随时间递增', (tester) async {
      await tester.pumpWidget(
        MaterialApp(
          theme: buildGuiTheme(),
          home: const Scaffold(body: CrtBackdrop()),
        ),
      );
      CrtNoisePainter noise() =>
          tester
                  .widget<CustomPaint>(find.byKey(const Key('crt_canvas')))
                  .foregroundPainter!
              as CrtNoisePainter;

      final before = noise().generation;
      await tester.pump(const Duration(milliseconds: 100));
      expect(noise().generation, greaterThan(before));

      await tester.pumpWidget(const SizedBox());
    });

    test('噪点重绘判定：generation 变化 → shouldRepaint', () {
      final a = CrtNoisePainter(
        seed: ValueNotifier<int>(0),
        bright: const Color(0xFFB8F4F8),
        dark: const Color(0xFF04080C),
      );
      final b = CrtNoisePainter(
        seed: ValueNotifier<int>(1),
        bright: const Color(0xFFB8F4F8),
        dark: const Color(0xFF04080C),
      );
      expect(b.shouldRepaint(a), isTrue);
      expect(a.shouldRepaint(a), isFalse);
    });
  });
}
