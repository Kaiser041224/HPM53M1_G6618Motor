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

  group('WindowCaptionButton.styleFor', () {
    const palette = GuiPalette();

    test('idle is transparent with the fg symbol and no glow', () {
      final style = WindowCaptionButton.styleFor(
        palette,
        WindowCaptionButtonKind.minimize,
        hovered: false,
      );
      expect(style.background, Colors.transparent);
      expect(style.color, palette.fg);
      expect(style.glowShadows, isEmpty);
    });

    test('minimize hover uses bgHover with a cyan glow, never red', () {
      final style = WindowCaptionButton.styleFor(
        palette,
        WindowCaptionButtonKind.minimize,
        hovered: true,
      );
      expect(style.background, palette.bgHover);
      expect(style.color, palette.fg);
      expect(style.glowShadows, isNotEmpty);
      for (final shadow in style.glowShadows) {
        expect(shadow.color.r, closeTo(palette.accent.r, 0.001));
        expect(shadow.color.g, closeTo(palette.accent.g, 0.001));
        expect(shadow.color.b, closeTo(palette.accent.b, 0.001));
      }
    });

    test('minimize hover glow follows the supplied palette, not the default', () {
      const custom = GuiPalette(glowSoft: Color(0xFF112233));
      final style = WindowCaptionButton.styleFor(
        custom,
        WindowCaptionButtonKind.minimize,
        hovered: true,
      );
      expect(style.background, custom.bgHover);
      final colors = style.glowShadows.map((s) => s.color).toSet();
      expect(colors, contains(custom.glowSoft));
      expect(colors, isNot(contains(const GuiPalette().glowSoft)));
    });

    test('close hover is stFault with a near-white symbol and red glow', () {
      final style = WindowCaptionButton.styleFor(
        palette,
        WindowCaptionButtonKind.close,
        hovered: true,
      );
      expect(style.background, palette.stFault);
      expect(style.color, palette.fgStrong);
      expect(style.glowShadows, isNotEmpty);
      for (final shadow in style.glowShadows) {
        expect(shadow.color.r, closeTo(palette.stFault.r, 0.001));
        expect(shadow.color.g, closeTo(palette.stFault.g, 0.001));
        expect(shadow.color.b, closeTo(palette.stFault.b, 0.001));
      }
    });
  });

  testWidgets('close gains its fault-hover style under a real pointer', (
    tester,
  ) async {
    await _pump(tester, const WindowCaptionBar());

    final gesture = await tester.createGesture(kind: PointerDeviceKind.mouse);
    await gesture.addPointer(location: Offset.zero);
    addTearDown(gesture.removePointer);
    await gesture.moveTo(
      tester.getCenter(find.byKey(const Key('window_caption_close'))),
    );
    await tester.pumpAndSettle();

    final painter = _painterFor(tester, const Key('window_caption_close'));
    expect(painter.color, const GuiPalette().fgStrong);
    expect(painter.glowShadows, isNotEmpty);
    for (final shadow in painter.glowShadows) {
      expect(shadow.color.r, closeTo(const GuiPalette().stFault.r, 0.001));
    }
  });
}
