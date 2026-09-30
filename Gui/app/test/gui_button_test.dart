import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/widgets/gui_button.dart';

Future<void> _pump(WidgetTester tester, Widget child) {
  return tester.pumpWidget(
    MaterialApp(theme: buildGuiTheme(), home: Scaffold(body: child)),
  );
}

void main() {
  const palette = GuiPalette();

  group('GuiButton.styleFor', () {
    test('normal idle: flat three-tier colors, no glow', () {
      final style = GuiButton.styleFor(
        palette,
        GuiButtonVariant.normal,
      );
      expect(style.fill, palette.btnFill);
      expect(style.border, palette.btnBorder);
      expect(style.text, palette.btnText);
      expect(style.glow, isEmpty);
    });

    test('primary uses the cyan border/text tier', () {
      final style = GuiButton.styleFor(palette, GuiButtonVariant.primary);
      expect(style.fill, palette.btnFill);
      expect(style.border, palette.btnPrimaryBorder);
      expect(style.text, palette.btnPrimaryText);
    });

    test('danger uses the red border/text tier', () {
      final style = GuiButton.styleFor(palette, GuiButtonVariant.danger);
      expect(style.border, palette.btnDangerBorder);
      expect(style.text, palette.btnDangerText);
    });

    test('normal hover brightens the border to lineHi with a faint same-hue glow', () {
      final style = GuiButton.styleFor(
        palette,
        GuiButtonVariant.normal,
        hovered: true,
      );
      expect(style.border, palette.lineHi);
      expect(style.glow.length, 1);
      expect(style.glow.single.color.a, closeTo(0.22, 0.001));
    });

    test('danger hover brightens to stFault, never cyan', () {
      final style = GuiButton.styleFor(
        palette,
        GuiButtonVariant.danger,
        hovered: true,
      );
      expect(style.border, palette.stFault);
    });

    test('disabled dims border + text and drops the glow', () {
      final style = GuiButton.styleFor(
        palette,
        GuiButtonVariant.primary,
        enabled: false,
        hovered: true,
      );
      expect(style.text, palette.fgMute);
      // A disabled button must not keep a bright border (minor 3).
      expect(style.border, palette.lineDim);
      expect(style.glow, isEmpty);
    });
  });

  testWidgets('renders its label, 30px tall and square', (tester) async {
    await _pump(tester, GuiButton(label: '启动', onPressed: () {}));

    expect(find.text('启动'), findsOneWidget);
    final box = tester.widget<Container>(
      find
          .descendant(
            of: find.byType(GuiButton),
            matching: find.byType(Container),
          )
          .first,
    );
    expect(box.constraints?.maxHeight, 30);
    final border = (box.decoration as BoxDecoration).border! as Border;
    expect(border.top.width, 1);
  });

  testWidgets('fires onPressed when tapped', (tester) async {
    var taps = 0;
    await _pump(tester, GuiButton(label: '停止', onPressed: () => taps++));

    await tester.tap(find.byType(GuiButton));
    await tester.pumpAndSettle();
    expect(taps, 1);
  });

  testWidgets('disabled button dims its hairline border', (tester) async {
    await _pump(tester, const GuiButton(label: '写入', onPressed: null));

    final text = tester.widget<Text>(find.text('写入'));
    expect(text.style?.color, palette.fgMute);

    final box = tester.widget<Container>(
      find
          .descendant(
            of: find.byType(GuiButton),
            matching: find.byType(Container),
          )
          .first,
    );
    final border = (box.decoration as BoxDecoration).border! as Border;
    expect(border.top.color, palette.lineDim);

    await tester.tap(find.byType(GuiButton));
    await tester.pumpAndSettle();
  });

  testWidgets('hover applies the lineHi border under a real pointer', (
    tester,
  ) async {
    await _pump(tester, GuiButton(label: '启动', onPressed: () {}));

    final gesture = await tester.createGesture(kind: PointerDeviceKind.mouse);
    await gesture.addPointer(location: Offset.zero);
    addTearDown(gesture.removePointer);
    await gesture.moveTo(tester.getCenter(find.byType(GuiButton)));
    await tester.pumpAndSettle();

    final box = tester.widget<Container>(
      find
          .descendant(
            of: find.byType(GuiButton),
            matching: find.byType(Container),
          )
          .first,
    );
    final border = (box.decoration as BoxDecoration).border! as Border;
    expect(border.top.color, palette.lineHi);
  });
}
