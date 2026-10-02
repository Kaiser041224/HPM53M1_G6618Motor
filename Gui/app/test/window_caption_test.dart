import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/widgets/window_caption.dart';

Future<void> _pump(WidgetTester tester, Widget child) {
  return tester.pumpWidget(
    MaterialApp(theme: buildGuiTheme(), home: Scaffold(body: child)),
  );
}

WindowCaptionGlyphPainter _painterFor(WidgetTester tester, Key key) {
  return tester
      .widgetList<CustomPaint>(
        find.descendant(of: find.byKey(key), matching: find.byType(CustomPaint)),
      )
      .map((c) => c.painter)
      .whereType<WindowCaptionGlyphPainter>()
      .first;
}

/// Perceptual luminance (sRGB coefficients) of a resolved [Color].
double _luminance(Color color) =>
    0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b;

BoxDecoration _surfaceDecoration(WidgetTester tester, String kind) {
  final container = tester.widget<Container>(
    find.byKey(Key('window_caption_surface_$kind')),
  );
  return container.decoration! as BoxDecoration;
}

void main() {
  group('WindowCaptionBar', () {
    testWidgets('renders minimize / maximize / close with tooltips', (
      tester,
    ) async {
      await _pump(tester, const WindowCaptionBar());

      expect(find.byKey(const Key('window_caption_minimize')), findsOneWidget);
      expect(find.byKey(const Key('window_caption_maximize')), findsOneWidget);
      expect(find.byKey(const Key('window_caption_close')), findsOneWidget);

      expect(find.byTooltip('最小化'), findsOneWidget);
      expect(find.byTooltip('最大化/还原'), findsOneWidget);
      expect(find.byTooltip('关闭'), findsOneWidget);
    });

    testWidgets('invokes the injected callbacks', (tester) async {
      var minimized = 0;
      var toggled = 0;
      var closed = 0;

      await _pump(
        tester,
        WindowCaptionBar(
          onMinimize: () => minimized++,
          onToggleMaximize: () => toggled++,
          onClose: () => closed++,
        ),
      );

      await tester.tap(find.byKey(const Key('window_caption_minimize')));
      await tester.tap(find.byKey(const Key('window_caption_maximize')));
      await tester.tap(find.byKey(const Key('window_caption_close')));

      expect(minimized, 1);
      expect(toggled, 1);
      expect(closed, 1);
    });

    testWidgets('maximize glyph is one frame, maximized is two frames', (
      tester,
    ) async {
      await _pump(tester, const WindowCaptionBar(maximized: false));
      final single = _painterFor(
        tester,
        const Key('window_caption_maximize'),
      );
      expect(single.maximized, isFalse);
      expect(
        WindowCaptionGlyphPainter.buildPaths(
          WindowCaptionButtonKind.maximize,
          const Size(44, 30),
        ).length,
        1,
        reason: '最大化态为单框',
      );

      await _pump(tester, const WindowCaptionBar(maximized: true));
      final restore = _painterFor(
        tester,
        const Key('window_caption_maximize'),
      );
      expect(restore.maximized, isTrue);
      expect(
        WindowCaptionGlyphPainter.buildPaths(
          WindowCaptionButtonKind.maximize,
          const Size(44, 30),
          maximized: true,
        ).length,
        2,
        reason: '还原态为双方框',
      );
    });
  });

  group('WindowCaptionButton 实心分色色块', () {
    const palette = GuiPalette();

    test('每键一色：min 浅青 / max 紫 / close 洋红，三色互异', () {
      final minimize = WindowCaptionButton.fillFor(
        palette,
        WindowCaptionButtonKind.minimize,
      );
      final maximize = WindowCaptionButton.fillFor(
        palette,
        WindowCaptionButtonKind.maximize,
      );
      final close = WindowCaptionButton.fillFor(
        palette,
        WindowCaptionButtonKind.close,
      );

      expect(maximize, palette.valuePurple);
      expect(close, palette.valueMagenta);
      // 浅青：比 valueCyan 更亮，且仍是青色系（g/b 高于 r）。
      expect(_luminance(minimize), greaterThan(_luminance(palette.valueCyan)));
      expect(minimize.g, greaterThan(minimize.r));
      expect(minimize.b, greaterThan(minimize.r));

      expect(<Color>{minimize, maximize, close}.length, 3, reason: '三键分色');
    });

    test('rest：实心填充、深色描线、无辉光、无边框概念', () {
      for (final kind in WindowCaptionButtonKind.values) {
        final style = WindowCaptionButton.styleFor(
          palette,
          kind,
          hovered: false,
        );
        expect(style.background, WindowCaptionButton.fillFor(palette, kind));
        expect(style.color, palette.bgVoid, reason: '深色描线压在色块上');
        expect(style.glowShadows, isEmpty);
      }
    });

    test('hover 提亮一档 + 同色极弱辉光；press 压暗', () {
      const kind = WindowCaptionButtonKind.maximize;
      final base = WindowCaptionButton.fillFor(palette, kind);

      final hover = WindowCaptionButton.styleFor(
        palette,
        kind,
        hovered: true,
      );
      expect(
        hover.background,
        Color.lerp(base, Colors.white, WindowCaptionButton.hoverLift),
      );
      expect(_luminance(hover.background), greaterThan(_luminance(base)));
      expect(hover.glowShadows, isNotEmpty);
      for (final shadow in hover.glowShadows) {
        expect(shadow.color.r, closeTo(base.r, 0.02));
        expect(shadow.color.g, closeTo(base.g, 0.02));
        expect(shadow.color.b, closeTo(base.b, 0.02));
      }

      final pressed = WindowCaptionButton.styleFor(
        palette,
        kind,
        hovered: true,
        pressed: true,
      );
      expect(_luminance(pressed.background), lessThan(_luminance(base)));
    });
  });

  testWidgets('三键色块为直角实心、无边框，尺寸等宽 44×30', (tester) async {
    await _pump(tester, const WindowCaptionBar());
    const palette = GuiPalette();

    for (final kind in WindowCaptionButtonKind.values) {
      final deco = _surfaceDecoration(tester, kind.name);
      expect(deco.color, WindowCaptionButton.fillFor(palette, kind));
      expect(deco.border, isNull, reason: '色块无边框');
      expect(deco.borderRadius, isNull, reason: '直角');
    }

    final widths = <double>[
      for (final kind in WindowCaptionButtonKind.values)
        tester.getSize(find.byKey(Key('window_caption_surface_${kind.name}'))).width,
    ];
    expect(widths.toSet().length, 1, reason: '三键等宽');
    expect(widths.first, WindowCaptionButton.width);
    expect(
      tester
          .getSize(find.byKey(const Key('window_caption_surface_minimize')))
          .height,
      WindowCaptionButton.height,
    );
  });

  testWidgets('按下态把色块压暗', (tester) async {
    await _pump(tester, const WindowCaptionBar());
    const palette = GuiPalette();
    final base = WindowCaptionButton.fillFor(
      palette,
      WindowCaptionButtonKind.minimize,
    );

    final gesture = await tester.startGesture(
      tester.getCenter(find.byKey(const Key('window_caption_minimize'))),
    );
    await tester.pump();
    final pressed = _surfaceDecoration(tester, 'minimize').color!;
    expect(_luminance(pressed), lessThan(_luminance(base)));
    await gesture.up();
    await tester.pumpAndSettle();
  });

  testWidgets('close 在真实指针下获得同色辉光', (tester) async {
    await _pump(tester, const WindowCaptionBar());
    const palette = GuiPalette();
    final base = WindowCaptionButton.fillFor(
      palette,
      WindowCaptionButtonKind.close,
    );

    final gesture = await tester.createGesture(kind: PointerDeviceKind.mouse);
    await gesture.addPointer(location: Offset.zero);
    addTearDown(gesture.removePointer);
    await gesture.moveTo(
      tester.getCenter(find.byKey(const Key('window_caption_close'))),
    );
    await tester.pumpAndSettle();

    final painter = _painterFor(tester, const Key('window_caption_close'));
    expect(painter.glowShadows, isNotEmpty);
    for (final shadow in painter.glowShadows) {
      expect(shadow.color.r, closeTo(base.r, 0.02));
      expect(shadow.color.g, closeTo(base.g, 0.02));
      expect(shadow.color.b, closeTo(base.b, 0.02));
    }
  });
}
