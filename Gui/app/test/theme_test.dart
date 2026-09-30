import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/widgets/glow_value.dart';
import 'package:g6618_gui/widgets/hairline_frame.dart';
import 'package:g6618_gui/widgets/section_title.dart';

void main() {
  group('GuiPalette matches tokens.css anchors', () {
    const palette = GuiPalette();

    test('background is a six-step blue-black -> cyan-green ramp', () {
      expect(palette.bgVoid, const Color(0xFF04080C));
      expect(palette.bgBase, const Color(0xFF081018));
      expect(palette.bgPanel, const Color(0xFF0A161A));
      expect(palette.bgRaise, const Color(0xFF102024));
      expect(palette.bgHover, const Color(0xFF162C30));
      expect(palette.bgActive, const Color(0xFF1C383C));
    });

    test('hairline borders are three colors, 1px', () {
      expect(palette.lineHi, const Color(0xFF58C8C0));
      expect(palette.line, const Color(0xFF1E5C60));
      expect(palette.lineDim, const Color(0xFF14393C));
      expect(palette.lineWidth, 1.0);
    });

    test('text tiers are translucent rgba, not solid', () {
      expect(palette.fg, const Color.fromRGBO(184, 212, 210, 0.76));
      expect(palette.fgStrong, const Color.fromRGBO(127, 227, 219, 0.94));
      expect(palette.fgDim, const Color.fromRGBO(110, 143, 142, 0.66));
      expect(palette.fgMute, const Color.fromRGBO(74, 102, 104, 0.78));
    });

    test('accents: cyan primary, yellow secondary (not magenta)', () {
      expect(palette.accent, const Color(0xFF58C8C0));
      expect(palette.accent2, const Color(0xFFC8C000));
      expect(palette.accentBlue, const Color(0xFF085890));
      expect(palette.accentGlow, const Color.fromRGBO(88, 200, 192, 0.45));
    });

    test('status semantics', () {
      expect(palette.stNormal, const Color(0xFF40B040));
      expect(palette.stWarn, const Color(0xFFC8C000));
      expect(palette.stFault, const Color(0xFFF88070));
      expect(palette.stLatched, const Color(0xFFFF9E96));
      expect(palette.stStale, const Color(0xFF4A6668));
      expect(palette.stPending, const Color(0xFFB8B848));
    });

    test('data series six', () {
      expect(palette.series1, const Color(0xFF58C8C0));
      expect(palette.series2, const Color(0xFFC8C000));
      expect(palette.series3, const Color(0xFFFF87D7));
      expect(palette.series4, const Color(0xFFAF87FF));
      expect(palette.series5, const Color(0xFF87FFAF));
      expect(palette.series6, const Color(0xFFF88070));
    });

    test('glow layers and stroke come from tokens.css', () {
      expect(palette.glow1, const Color.fromRGBO(127, 227, 219, 0.85));
      expect(palette.glow2, const Color.fromRGBO(88, 200, 192, 0.48));
      expect(palette.glow3, const Color.fromRGBO(88, 200, 192, 0.24));
      expect(palette.glow4, const Color.fromRGBO(88, 200, 192, 0.11));
      expect(palette.stroke, const Color.fromRGBO(127, 227, 219, 0.46));
    });
  });

  testWidgets('GuiPalette.of(context) resolves via the theme extension', (
    WidgetTester tester,
  ) async {
    late GuiPalette resolved;
    await tester.pumpWidget(
      MaterialApp(
        theme: buildGuiTheme(),
        home: Builder(
          builder: (context) {
            resolved = GuiPalette.of(context);
            return const SizedBox.shrink();
          },
        ),
      ),
    );
    expect(resolved.bgBase, const Color(0xFF081018));
    expect(resolved.accent, const Color(0xFF58C8C0));
  });

  test('buildGuiTheme is Material 3 dark, square, bgBase scaffold', () {
    final theme = buildGuiTheme();
    expect(theme.useMaterial3, isTrue);
    expect(theme.colorScheme.brightness, Brightness.dark);
    expect(theme.scaffoldBackgroundColor, const Color(0xFF081018));
    expect(theme.extension<GuiPalette>(), isNotNull);
    final cardShape = theme.cardTheme.shape;
    expect(cardShape, isA<RoundedRectangleBorder>());
    expect((cardShape as RoundedRectangleBorder).borderRadius, BorderRadius.zero);
  });

  test('GuiTypeScale applies display/mono families and letter spacing', () {
    const palette = GuiPalette();
    final scale = GuiTypeScale(const GuiPalette());
    expect(scale.section.fontFamily, 'Electrolize');
    expect(scale.section.color, palette.accent);
    expect(scale.section.letterSpacing, closeTo(12.5 * 0.16, 0.001));
    expect(scale.label.fontFamily, 'Electrolize');
    expect(scale.label.color, palette.fgMute);
    expect(scale.data.fontFamily, 'Cascadia Mono');
    expect(scale.data.color, palette.fgStrong);
    expect(scale.data.fontFeatures, contains(const FontFeature.tabularFigures()));
  });

  group('GuiGlowStyles', () {
    final base = GuiTypeScale(const GuiPalette()).data;

    test('strong has three layers (core + inner + outer)', () {
      expect(GuiGlowStyles.strong(base).shadows!.length, greaterThanOrEqualTo(3));
    });

    test('soft has fewer layers than strong', () {
      expect(
        GuiGlowStyles.soft(base).shadows!.length,
        lessThan(GuiGlowStyles.strong(base).shadows!.length),
      );
    });

    test('weak is a single faint inner halo without stroke', () {
      expect(GuiGlowStyles.weak(base).shadows!.length, 1);
      expect(GuiGlowStyles.weak(base).foreground, isNull);
    });

    test('status glow keeps the status hue (no cross-coloring)', () {
      const fault = Color(0xFFF88070);
      final shadows = GuiGlowStyles.status(fault, base).shadows!;
      for (final shadow in shadows) {
        expect(shadow.color.r, closeTo(fault.r, 0.001));
        expect(shadow.color.g, closeTo(fault.g, 0.001));
      }
    });
  });

  testWidgets('GlowText strong renders >=3 shadow layers', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildGuiTheme(),
        home: const Scaffold(
          body: GlowText('24.0', strength: GlowStrength.strong),
        ),
      ),
    );
    final texts = tester.widgetList<Text>(find.byType(Text));
    final maxShadows = texts
        .map((t) => t.style?.shadows?.length ?? 0)
        .reduce(math.max);
    expect(maxShadows, greaterThanOrEqualTo(3));
  });

  testWidgets('GlowText weak renders a single faint halo', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildGuiTheme(),
        home: const Scaffold(
          body: GlowText('STATUS', strength: GlowStrength.weak),
        ),
      ),
    );
    final text = tester.widget<Text>(find.text('STATUS'));
    expect(text.style?.shadows?.length, 1);
  });

  testWidgets('HairlineDivider is a bright line plus two dim lines', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildGuiTheme(),
        home: const Scaffold(body: HairlineDivider()),
      ),
    );
    final containers = find.descendant(
      of: find.byType(HairlineDivider),
      matching: find.byType(Container),
    );
    expect(containers, findsNWidgets(3));
  });

  testWidgets('HairlineFrame main variant wraps its child', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildGuiTheme(),
        home: const Scaffold(
          body: HairlineFrame(child: Text('inner')),
        ),
      ),
    );
    expect(find.text('inner'), findsOneWidget);
  });

  testWidgets('SectionTitle uppercases the english caption', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildGuiTheme(),
        home: const Scaffold(
          body: SectionTitle(title: '状态监视', en: 'Status'),
        ),
      ),
    );
    expect(find.text('状态监视'), findsOneWidget);
    expect(find.text('STATUS'), findsOneWidget);
  });

  testWidgets('GlowValue formats precision, unit and tabular figures', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildGuiTheme(),
        home: const Scaffold(
          body: GlowValue(value: 24.0, unit: 'V', precision: 1),
        ),
      ),
    );
    final value = tester.widget<Text>(find.text('24.0'));
    expect(value.style?.fontFeatures, contains(const FontFeature.tabularFigures()));
    expect(find.text('V'), findsOneWidget);
  });

  testWidgets('GlowValue shows a dash when the value is stale', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildGuiTheme(),
        home: const Scaffold(body: GlowValue(value: null, unit: 'A')),
      ),
    );
    expect(find.text('—'), findsOneWidget);
  });
}
