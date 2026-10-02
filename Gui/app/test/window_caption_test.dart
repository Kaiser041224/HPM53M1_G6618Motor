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

    test('rest：低饱和半透灯面、浅色描线、无辉光、青勾线', () {
      for (final kind in WindowCaptionButtonKind.values) {
        final style = WindowCaptionButton.styleFor(
          palette,
          kind,
          hovered: false,
        );
        final face = style.background;
        expect(face, WindowCaptionButton.lampFaceFor(palette, kind));
        expect(
          face.a,
          inInclusiveRange(0.45, 0.55),
          reason: '灯面降饱和 + 提高透明 (α≈0.45~0.55)',
        );
        expect(face, isNot(WindowCaptionButton.fillFor(palette, kind)));
        expect(style.color, WindowCaptionButton.glyphFor(palette, kind));
        expect(
          _luminance(style.color),
          greaterThan(_luminance(face)),
          reason: '浅描线压在深色格栅灯面上',
        );
        expect(style.glowShadows, isEmpty);
        // 灯座圈：1px accent 青勾线。
        expect(style.border.color, palette.accent);
        expect(style.border.width, 1);
      }
    });

    test('hover 提亮一档 + 同色极弱辉光；press 压暗', () {
      const kind = WindowCaptionButtonKind.maximize;
      final base = WindowCaptionButton.lampFaceFor(palette, kind);

      final hover = WindowCaptionButton.styleFor(
        palette,
        kind,
        hovered: true,
      );
      expect(_luminance(hover.background), greaterThan(_luminance(base)));
      expect(
        hover.background.a,
        greaterThan(base.a),
        reason: 'hover 灯面同时提亮一档',
      );
      expect(hover.glowShadows, isNotEmpty);
      final hue = WindowCaptionButton.fillFor(palette, kind);
      for (final shadow in hover.glowShadows) {
        expect(shadow.color.r, closeTo(hue.r, 0.02));
        expect(shadow.color.g, closeTo(hue.g, 0.02));
        expect(shadow.color.b, closeTo(hue.b, 0.02));
      }

      final pressed = WindowCaptionButton.styleFor(
        palette,
        kind,
        hovered: true,
        pressed: true,
      );
      expect(_luminance(pressed.background), lessThan(_luminance(base)));
      expect(pressed.background.a, lessThan(base.a), reason: 'press 灯面压暗');
    });
  });

  testWidgets('三键灯面为直角、半透、1px 青勾线，尺寸等宽 44×30', (tester) async {
    await _pump(tester, const WindowCaptionBar());
    const palette = GuiPalette();

    for (final kind in WindowCaptionButtonKind.values) {
      final deco = _surfaceDecoration(tester, kind.name);
      expect(deco.color, WindowCaptionButton.lampFaceFor(palette, kind));
      expect(
        deco.color!.a,
        inInclusiveRange(0.45, 0.55),
        reason: '半透灯面',
      );
      expect(deco.borderRadius, isNull, reason: '直角');
      final border = deco.border! as Border;
      expect(border.top.color, palette.accent, reason: '青勾线外圈');
      expect(border.top.width, 1);
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

  testWidgets('格栅罩：双向 1px 暗线覆盖灯面，格距 4px', (tester) async {
    await _pump(tester, const WindowCaptionBar());

    expect(WindowCaptionLatticePainter.spacing, 4.0);
    expect(WindowCaptionLatticePainter.lineWidth, 1.0);
    expect(
      WindowCaptionLatticePainter.latticeLines(9),
      <double>[0, 4, 8],
      reason: '双向格栅线按 4px 均布',
    );

    for (final kind in WindowCaptionButtonKind.values) {
      final lattice = tester
          .widgetList<CustomPaint>(
            find.descendant(
              of: find.byKey(Key('window_caption_surface_${kind.name}')),
              matching: find.byType(CustomPaint),
            ),
          )
          .map((c) => c.painter)
          .whereType<WindowCaptionLatticePainter>()
          .first;
      expect(
        lattice.color.a,
        greaterThan(0.3),
        reason: '${kind.name} 格栅暗线可见',
      );
    }
  });

  testWidgets('灯座：外扩发暗光晕 vignette + 1px accent 青勾线', (tester) async {
    await _pump(tester, const WindowCaptionBar());
    const palette = GuiPalette();

    for (final kind in WindowCaptionButtonKind.values) {
      final deco = _surfaceDecoration(tester, kind.name);
      final border = deco.border! as Border;
      expect(border.top.color, palette.accent);
      expect(border.top.width, 1);

      final vignette = deco.boxShadow!.first;
      expect(vignette.color.a, greaterThan(0.45), reason: '发暗光晕 α≈.55');
      expect(vignette.color.r, lessThan(0.2));
      expect(vignette.color.g, lessThan(0.2));
      expect(vignette.color.b, lessThan(0.2));
      expect(vignette.blurRadius, greaterThanOrEqualTo(8));
      expect(vignette.spreadRadius, greaterThanOrEqualTo(1));
    }
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
