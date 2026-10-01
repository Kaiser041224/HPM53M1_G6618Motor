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
      expect(palette.lineHi, const Color(0xFF3F9E98));
      expect(palette.line, const Color(0xFF1E5458));
      expect(palette.lineDim, const Color(0xFF123234));
      expect(palette.lineWidth, 1.0);
    });

    test('text tiers are solid v4 colors, not translucent', () {
      expect(palette.fg, const Color(0xFFA6BAB8));
      expect(palette.fgStrong, const Color(0xFFCFDCDA));
      expect(palette.fgDim, const Color(0xFF6E908E));
      expect(palette.fgMute, const Color(0xFF5B8280));
      for (final tier in <Color>[
        palette.fg,
        palette.fgStrong,
        palette.fgDim,
        palette.fgMute,
      ]) {
        expect(tier.a, 1.0);
      }
    });

    test('accents: muted cyan primary, olive secondary (not magenta)', () {
      expect(palette.accent, const Color(0xFF3F9E98));
      expect(palette.accent2, const Color(0xFFBFA84C));
      expect(palette.accentBlue, const Color(0xFF085890));
      expect(palette.accentGlow, const Color.fromRGBO(63, 158, 152, 0.45));
    });

    test('status semantics', () {
      expect(palette.stNormal, const Color(0xFF4E9454));
      expect(palette.stWarn, const Color(0xFFB0A038));
      expect(palette.stFault, const Color(0xFFC06858));
      expect(palette.stLatched, const Color(0xFFD08880));
      expect(palette.stStale, const Color(0xFF567476));
      expect(palette.stPending, const Color(0xFFA0A048));
    });

    test('flat semantic value colors (v4)', () {
      expect(palette.valueAmber, const Color(0xFFBFA84C));
      expect(palette.valueGreen, const Color(0xFF589E5E));
      expect(palette.valueRed, const Color(0xFFB86858));
      expect(palette.valuePink, const Color(0xFFC07898));
      expect(palette.valueViolet, const Color(0xFF8878B8));
      expect(palette.white, const Color(0xFFCFDCDA));
    });

    test('three-tier button colors (v4)', () {
      expect(palette.btnFill, const Color(0xFF222C2E));
      expect(palette.btnBorder, const Color(0xFF3E6466));
      expect(palette.btnText, const Color(0xFFA6BAB8));
      expect(palette.btnPrimaryBorder, const Color(0xFF3F9E98));
      expect(palette.btnPrimaryText, const Color(0xFF6FB8B2));
      expect(palette.btnDangerBorder, const Color(0xFFC06858));
      expect(palette.btnDangerText, const Color(0xFFD08880));
    });

    test('data series six', () {
      expect(palette.series1, const Color(0xFF3F9E98));
      expect(palette.series2, const Color(0xFFBFA84C));
      expect(palette.series3, const Color(0xFFC07898));
      expect(palette.series4, const Color(0xFF8878B8));
      expect(palette.series5, const Color(0xFF589E90));
      expect(palette.series6, const Color(0xFFB86858));
    });

    test('glow layers and stroke come from tokens.css; glowSoft is v4', () {
      expect(palette.glow1, const Color.fromRGBO(127, 227, 219, 0.85));
      expect(palette.glow2, const Color.fromRGBO(88, 200, 192, 0.48));
      expect(palette.glow3, const Color.fromRGBO(88, 200, 192, 0.24));
      expect(palette.glow4, const Color.fromRGBO(88, 200, 192, 0.11));
      expect(palette.stroke, const Color.fromRGBO(127, 227, 219, 0.46));
      expect(palette.glowSoft, const Color.fromRGBO(63, 158, 152, 0.12));
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
    expect(resolved.accent, const Color(0xFF3F9E98));
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

  test('GuiTypeScale 采用 CRT 终端字体栈与字距', () {
    const palette = GuiPalette();
    final scale = GuiTypeScale(const GuiPalette());
    expect(scale.section.fontFamily, 'VT323');
    expect(scale.section.color, palette.accent);
    expect(scale.section.letterSpacing, closeTo(13 * 0.16, 0.001));
    expect(scale.label.fontFamily, 'VT323');
    expect(scale.label.color, palette.fgMute);
    expect(scale.data.fontFamily, 'VT323');
    expect(scale.data.color, palette.fgStrong);
    expect(scale.data.fontFeatures, contains(const FontFeature.tabularFigures()));
  });

  test('汉字落在像素网格上：Fusion Pixel 12，尺寸为 12 的整数倍', () {
    const palette = GuiPalette();
    final scale = GuiTypeScale(const GuiPalette());
    expect(scale.sectionTitle.fontFamily, 'Fusion Pixel 12');
    expect(scale.sectionTitle.fontSize, GuiTypeScale.pixelSize);
    expect(scale.sectionTitle.color, palette.fgStrong);
    expect(scale.body.fontFamily, 'Fusion Pixel 12');
    expect(scale.body.fontSize, GuiTypeScale.pixelSize);
    // 放大档必须是整数倍，否则位图字体被平滑、失去锐利。
    expect(
      GuiTypeScale.pixelSizeLarge % GuiTypeScale.pixelSize,
      0,
      reason: '放大档须为像素网格的整数倍',
    );
  });

  test('区分/增强用 Silkscreen，密集表格用 Share Tech Mono', () {
    final scale = GuiTypeScale(const GuiPalette());
    expect(scale.emph.fontFamily, 'Silkscreen');
    expect(scale.dense.fontFamily, 'Share Tech Mono');
  });

  test('设备块字 █ 只在汉字像素体里 —— 艺术字不得改用 VT323', () {
    // VT323 不含 U+2588，艺术字整幅由块字符构成，用错字体即变豆腐块。
    expect(GuiTypeScale.zhFamily, isNot(GuiTypeScale.displayFamily));
  });

  group('GuiGlowStyles', () {
    final base = GuiTypeScale(const GuiPalette()).data;

    test('strong is a single soft layer with no stroke (v3)', () {
      final style = GuiGlowStyles.strong(base);
      expect(style.shadows!.length, 1);
      expect(style.foreground, isNull);
    });

    test('soft is a single soft layer too', () {
      expect(GuiGlowStyles.soft(base).shadows!.length, 1);
    });

    test('none clears the shadows', () {
      expect(GuiGlowStyles.none(base).shadows, isEmpty);
    });

    test('weak is a single faint halo without stroke', () {
      expect(GuiGlowStyles.weak(base).shadows!.length, 1);
      expect(GuiGlowStyles.weak(base).foreground, isNull);
    });

    test('status glow is one layer and keeps the status hue', () {
      const fault = Color(0xFFC06858);
      final shadows = GuiGlowStyles.status(fault, base).shadows!;
      expect(shadows.length, 1);
      for (final shadow in shadows) {
        expect(shadow.color.r, closeTo(fault.r, 0.001));
        expect(shadow.color.g, closeTo(fault.g, 0.001));
        expect(shadow.color.a, closeTo(0.18, 0.001));
      }
    });

    test('ascii keeps the multi-layer device glow, same hue', () {
      const stop = Color(0xFF00D7FF);
      final shadows = GuiGlowStyles.ascii(stop, base).shadows!;
      expect(shadows.length, greaterThan(1));
      for (final shadow in shadows) {
        expect(shadow.color.r, closeTo(stop.r, 0.001));
        expect(shadow.color.g, closeTo(stop.g, 0.001));
        expect(shadow.color.b, closeTo(stop.b, 0.001));
      }
    });
  });

  testWidgets('GlowText strong renders a single soft layer', (
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
    expect(maxShadows, 1);
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

  testWidgets('HairlineDivider is a single bright line (v3)', (
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
    expect(containers, findsOneWidget);
    final line = tester.widget<Container>(containers);
    expect(line.color, const GuiPalette().lineHi);
  });

  testWidgets('HairlineFrame main wraps its child', (
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

  testWidgets('GlowValue is flat by default (no glow)', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildGuiTheme(),
        home: const Scaffold(body: GlowValue(value: 24.0)),
      ),
    );
    final value = tester.widget<Text>(find.text('24.000'));
    expect(value.style?.shadows, anyOf(isNull, isEmpty));
  });

  testWidgets('GlowValue tone colors the value flat (v3)', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildGuiTheme(),
        home: const Scaffold(
          body: GlowValue(value: 1.0, tone: Tone.amber),
        ),
      ),
    );
    final value = tester.widget<Text>(find.text('1.000'));
    expect(value.style?.color, const GuiPalette().valueAmber);
  });

  testWidgets('GlowValue glow:true adds the soft layer', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildGuiTheme(),
        home: const Scaffold(
          body: GlowValue(value: 1.0, tone: Tone.green, glow: true),
        ),
      ),
    );
    final value = tester.widget<Text>(find.text('1.000'));
    expect(value.style?.shadows?.length, 1);
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

  // ── 对比度底线 ────────────────────────────────────────────────────────────
  //
  // 显示效果的可读性下限。这套配色刻意压暗（v4 muted），但压暗不能压到
  // 读不出来 —— 尤其 `stStale`：失效态是**安全提示**，看不见就失去意义。
  //
  // 阈值取 WCAG 2.1：正文 4.5:1、大字/次要 3.0:1。
  // 这里只对**文字类** token 设卡；线框、填充块不适用。
  group('对比度底线（bgBase 之上）', () {
    const palette = GuiPalette();

    /// WCAG 相对亮度。
    double luminance(Color c) {
      double channel(double v) =>
          v <= 0.03928 ? v / 12.92 : math.pow((v + 0.055) / 1.055, 2.4).toDouble();
      return 0.2126 * channel(c.r) +
          0.7152 * channel(c.g) +
          0.0722 * channel(c.b);
    }

    /// 两色对比度（1..21）。
    double contrast(Color a, Color b) {
      final double la = luminance(a);
      final double lb = luminance(b);
      final double hi = math.max(la, lb);
      final double lo = math.min(la, lb);
      return (hi + 0.05) / (lo + 0.05);
    }

    test('正文级文字 >= 4.5:1', () {
      final Color bg = palette.bgBase;
      for (final MapEntry<String, Color> e in <String, Color>{
        'fg': palette.fg,
        'fgStrong': palette.fgStrong,
        'fgDim': palette.fgDim,
        // fgMute 承载 9px 小字，同样按正文标准卡。
        'fgMute': palette.fgMute,
      }.entries) {
        expect(
          contrast(e.value, bg),
          greaterThanOrEqualTo(4.5),
          reason: '${e.key} 在 bgBase 上的对比度不足',
        );
      }
    });

    test('状态色 >= 4.5:1（都是要读出来的读数）', () {
      final Color bg = palette.bgBase;
      for (final MapEntry<String, Color> e in <String, Color>{
        'stNormal': palette.stNormal,
        'stWarn': palette.stWarn,
        'stFault': palette.stFault,
        'stLatched': palette.stLatched,
      }.entries) {
        expect(
          contrast(e.value, bg),
          greaterThanOrEqualTo(4.5),
          reason: '${e.key} 在 bgBase 上的对比度不足',
        );
      }
    });

    test('stStale 达大字下限，且明显暗于 stNormal（保住"退后"语义）', () {
      final double stale = contrast(palette.stStale, palette.bgBase);
      expect(stale, greaterThanOrEqualTo(3.0), reason: '失效态低到看不见');
      expect(
        stale,
        lessThan(contrast(palette.stNormal, palette.bgBase)),
        reason: '失效态必须比正常态暗，否则失去"退后"语义',
      );
    });
  });
}
