import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/toast_overlay.dart';
import 'package:g6618_gui/widgets/gui_button.dart';
import 'package:g6618_gui/widgets/gui_text_field.dart';

void main() {
  const palette = GuiPalette();

  group('A. 输入框全局风格：整框 Outline，无下划线 / 无琥珀', () {
    test('inputDecorationTheme 全状态为 OutlineInputBorder（青边 / 直角 / bgRaise 填充）', () {
      final t = buildGuiTheme().inputDecorationTheme;

      expect(t.filled, isTrue, reason: '深填充');
      expect(t.fillColor, palette.bgRaise);

      InputBorder borderOf(InputBorder? b) => b!;

      for (final InputBorder b in <InputBorder>[
        borderOf(t.border),
        borderOf(t.enabledBorder),
        borderOf(t.focusedBorder),
        borderOf(t.errorBorder),
        borderOf(t.focusedErrorBorder),
        borderOf(t.disabledBorder),
      ]) {
        expect(b, isA<OutlineInputBorder>(), reason: '不得出现 Material 默认下划线');
        expect(b, isNot(isA<UnderlineInputBorder>()));
        expect((b as OutlineInputBorder).borderRadius, BorderRadius.zero);
      }

      BorderSide side(InputBorder? b) => (b! as OutlineInputBorder).borderSide;
      expect(side(t.enabledBorder).color, palette.accent);
      expect(side(t.enabledBorder).width, 1.0);
      expect(side(t.focusedBorder).color, palette.lineHi);
      expect(side(t.focusedBorder).width, 1.5);
      expect(side(t.errorBorder).color, palette.stFault);
      expect(side(t.disabledBorder).color, palette.lineDim);

      expect(t.labelStyle?.color, palette.fgDim);
      expect(t.hintStyle?.color, palette.fgDim);
    });

    test('聚焦色 / 悬停色为 token 青，不得沿用 Material 默认（白/琥珀）', () {
      final theme = buildGuiTheme();
      expect(theme.focusColor.r, closeTo(palette.accent.r, 0.02));
      expect(theme.focusColor.g, closeTo(palette.accent.g, 0.02));
      expect(theme.focusColor.b, closeTo(palette.accent.b, 0.02));
      expect(theme.hoverColor.r, closeTo(palette.accent.r, 0.02));
      expect(theme.textSelectionTheme.cursorColor, palette.accent);
    });

    testWidgets('TextField 实际解析为 Outline（无下划线）且聚焦不移到琥珀', (tester) async {
      await tester.pumpWidget(
        MaterialApp(
          theme: buildGuiTheme(),
          home: const Scaffold(
            body: GuiTextField(hintText: '输入'),
          ),
        ),
      );
      final decorator = tester.widget<InputDecorator>(
        find.byType(InputDecorator),
      );
      expect(decorator.decoration.border, isA<OutlineInputBorder>());

      // 聚焦：EditableText 光标为青色，不是 Material 默认。
      await tester.tap(find.byType(TextField));
      await tester.pump();
      final editable = tester.widget<EditableText>(find.byType(EditableText));
      expect(editable.cursorColor, palette.accent);
    });
  });

  group('A2. GuiTextField 聚焦辉光', () {
    testWidgets('失焦无辉光；聚焦亮青外框 + 8px α.25 弱辉光', (tester) async {
      await tester.pumpWidget(
        MaterialApp(
          theme: buildGuiTheme(),
          home: const Scaffold(body: Center(child: GuiTextField())),
        ),
      );

      AnimatedContainer glow() => tester.widget<AnimatedContainer>(
        find.byKey(const Key('gui_text_field_glow')),
      );

      expect((glow().decoration! as BoxDecoration).boxShadow, isEmpty);

      await tester.tap(find.byType(TextField));
      await tester.pump(const Duration(milliseconds: 200));

      final shadows = (glow().decoration! as BoxDecoration).boxShadow!;
      expect(shadows, isNotEmpty);
      expect(shadows.first.blurRadius, 8);
      expect(shadows.first.color.a, closeTo(0.25, 0.02));
      expect(shadows.first.color.r, closeTo(palette.accent.r, 0.02));
    });
  });

  group('B. 按键统一', () {
    test('GuiButton 高度 32', () {
      expect(GuiButton.height, 32);
    });

    testWidgets('GuiButton：内容居中、字号 14', (tester) async {
      await tester.pumpWidget(
        MaterialApp(
          theme: buildGuiTheme(),
          home: Scaffold(body: GuiButton(label: '启动', onPressed: () {})),
        ),
      );

      final text = tester.widget<Text>(find.text('启动'));
      expect(text.style?.fontSize, 14);

      final box = tester.widget<Container>(
        find
            .descendant(of: find.byType(GuiButton), matching: find.byType(Container))
            .first,
      );
      expect(box.constraints?.maxHeight, 32);
      expect(
        find.descendant(
          of: find.byType(GuiButton),
          matching: find.byType(Center),
        ),
        findsOneWidget,
        reason: '内容区居中',
      );
    });
  });

  group('C. Toast 外框 / 字号', () {
    Future<void> show(WidgetTester tester) async {
      await tester.pumpWidget(
        MaterialApp(
          theme: buildGuiTheme(),
          home: Scaffold(
            body: Builder(
              builder: (context) => Center(
                child: ElevatedButton(
                  onPressed: () => showGuiToast(context, '提示信息'),
                  child: const Text('go'),
                ),
              ),
            ),
          ),
        ),
      );
      await tester.tap(find.text('go'));
      await tester.pump();
    }

    testWidgets('文字 14px、图标 18px、双层青框（外 1.5 + 内 1）间距 2px', (tester) async {
      await show(tester);

      final text = tester.widget<Text>(find.byKey(const Key('gui_toast_text')));
      expect(text.style?.fontSize, 14);

      final icon = tester.widget<Icon>(find.byKey(const Key('gui_toast_icon')));
      expect(icon.size, 18);

      final frame = tester.widget<Container>(
        find.byKey(const Key('gui_toast_frame')),
      );
      final frameDeco = frame.decoration! as BoxDecoration;
      expect((frameDeco.border! as Border).top.color, palette.accent);
      expect((frameDeco.border! as Border).top.width, 1.5);
      expect(frame.padding, const EdgeInsets.all(2));

      final inner = tester.widget<Container>(
        find.byKey(const Key('gui_toast_inner')),
      );
      final innerDeco = inner.decoration! as BoxDecoration;
      expect((innerDeco.border! as Border).top.color, palette.lineDim);
      expect((innerDeco.border! as Border).top.width, 1.0);

      await tester.pump(const Duration(milliseconds: 3000));
    });
  });

  group('D. 弹窗主题', () {
    test('dialogTheme：深底 bgBase + 青 1.5px 直角外框 + 弱辉光', () {
      final d = buildGuiTheme().dialogTheme;
      expect(d.backgroundColor, palette.bgBase);

      final shape = d.shape! as RoundedRectangleBorder;
      expect(shape.borderRadius, BorderRadius.zero);
      expect(shape.side.color, palette.accent);
      expect(shape.side.width, 1.5);

      expect(d.shadowColor?.r, closeTo(palette.accent.r, 0.02));
      expect(d.shadowColor!.a, greaterThan(0));
      expect(d.elevation, greaterThan(0));
    });
  });

  group('E. 全库无 Material 默认下划 / 琥珀残留', () {
    test(
      'lib/**/*.dart 不含 TextDecoration.underline / UnderlineInputBorder / Colors.amber',
      () {
        final files = Directory('lib')
            .listSync(recursive: true)
            .whereType<File>()
            .where((f) => f.path.endsWith('.dart'))
            .toList();
        expect(files, isNotEmpty);

        for (final file in files) {
          final src = file.readAsStringSync();
          // 只允许显式的 `TextDecoration.none`（弹窗防御），任何真正的
          // 下划线 / 删除线 / 上划线强调都必须以 Container 线实现。
          for (final banned in <String>[
            'TextDecoration.underline',
            'TextDecoration.lineThrough',
            'TextDecoration.overline',
          ]) {
            expect(
              src.contains(banned),
              isFalse,
              reason: '${file.path} 不得出现 $banned（下划强调须为 Container 线）',
            );
          }
          expect(
            src.contains('UnderlineInputBorder'),
            isFalse,
            reason: '${file.path} 不得出现 Material 下划线输入框',
          );
          expect(
            src.contains('Colors.amber') || src.contains('amberAccent'),
            isFalse,
            reason: '${file.path} 不得出现 Material 琥珀默认色',
          );
        }
      },
    );
  });
}
