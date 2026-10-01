import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/toast_overlay.dart';
import 'package:g6618_gui/widgets/param_controls.dart';

/// Task W42 弹窗防御：切断 Material 默认色相渗入、单层辉光、徽标整框。
void main() {
  const palette = GuiPalette();

  group('A. 弹窗主题切断琥珀 / 显式无下划线', () {
    test('dialogTheme 标题/正文显式 TextDecoration.none', () {
      final d = buildGuiTheme().dialogTheme;
      expect(d.titleTextStyle?.decoration, TextDecoration.none);
      expect(d.contentTextStyle?.decoration, TextDecoration.none);
    });

    test('dialogTheme.iconColor 显式绑 accent，不再回落 amber secondary', () {
      final d = buildGuiTheme().dialogTheme;
      expect(d.iconColor, palette.accent);
      expect(d.iconColor, isNot(palette.accent2));
    });

    test('dialogTheme.surfaceTintColor 关闭（无 M3 叠色）', () {
      expect(buildGuiTheme().dialogTheme.surfaceTintColor, Colors.transparent);
    });

    testWidgets('对话框内未着色 Icon 继承 accent，而非 amber secondary', (tester) async {
      await tester.pumpWidget(
        MaterialApp(
          theme: buildGuiTheme(),
          home: Scaffold(
            body: AlertDialog(
              icon: const Icon(Icons.warning_amber_outlined),
              title: const Text('标题'),
              content: const Text('正文'),
              actions: <Widget>[GuiButtonProbe()],
            ),
          ),
        ),
      );
      final iconElement = tester.element(find.byType(Icon).first);
      expect(IconTheme.of(iconElement).color, palette.accent);
      expect(IconTheme.of(iconElement).color, isNot(palette.accent2));
    });
  });

  group('B. 小字号琥珀文本：单层同色相辉光', () {
    final base = const GuiTypeScale(GuiPalette()).data.copyWith(fontSize: 15);

    test('softTint 只有一层，alpha 0.12，且取文本自身色相', () {
      final style = GuiGlowStyles.softTint(palette.valueAmber, base);
      expect(style.shadows!.length, 1);
      final s = style.shadows!.single;
      expect(s.blurRadius, GuiGlowStyles.softBlur);
      expect(s.color.a, closeTo(0.12, 0.005));
      expect(s.color.r, closeTo(palette.valueAmber.r, 0.002));
      expect(s.color.g, closeTo(palette.valueAmber.g, 0.002));
      expect(s.color.b, closeTo(palette.valueAmber.b, 0.002));
    });

    testWidgets('ParamField 琥珀值用单层同色相辉光（不再拾取青色光晕）', (tester) async {
      final state = AppState.test(
        metas: <ParamMetaDto>[
          const ParamMetaDto(
            index: 0,
            name: 'software.control.speed_loop.kp',
            ty: 0,
            apply: 0,
            unit: '',
            min: 0,
            max: 10,
          ),
        ],
      );
      addTearDown(state.dispose);
      state.values['software.control.speed_loop.kp'] = ParamValueDto(
        ty: 0,
        raw: (ByteData(4)..setFloat32(0, 1.5, Endian.little)).getInt32(
          0,
          Endian.little,
        ),
      );

      await tester.pumpWidget(
        MaterialApp(
          theme: buildGuiTheme(),
          home: Scaffold(
            body: ParamField(
              state: state,
              name: 'software.control.speed_loop.kp',
              label: 'kp',
            ),
          ),
        ),
      );

      final value = tester.widget<Text>(find.text('1.50'));
      expect(value.style?.shadows?.length, 1);
      final s = value.style!.shadows!.single;
      expect(s.color.r, closeTo(palette.valueAmber.r, 0.002));
      expect(s.color.g, closeTo(palette.valueAmber.g, 0.002));
      expect(s.color.b, closeTo(palette.valueAmber.b, 0.002));
    });
  });

  group('C. 徽标整框（四边等宽等色，非底线双画）', () {
    testWidgets('SafetyBadge 为完整边框，四边同色 0.55，文字无下划线', (tester) async {
      await tester.pumpWidget(
        MaterialApp(
          theme: buildGuiTheme(),
          home: const Scaffold(body: Center(child: SafetyBadge())),
        ),
      );

      final container = tester.widget<Container>(
        find
            .descendant(of: find.byType(ApplyBadge), matching: find.byType(Container))
            .first,
      );
      final border = (container.decoration! as BoxDecoration).border! as Border;
      for (final side in <BorderSide>[
        border.top,
        border.bottom,
        border.left,
        border.right,
      ]) {
        expect(side.width, 1);
        expect(side.color.a, closeTo(0.55, 0.01));
      }
      expect(border.top, border.bottom);

      final text = tester.widget<Text>(find.text('SAFETY'));
      expect(text.style?.decoration, TextDecoration.none);
    });
  });

  group('D. MaterialApp 兜底样式（黄色双下划线）不得渗入浮层', () {
    test('GuiTypeScale 每个样式显式 decoration: none', () {
      final s = GuiTypeScale(const GuiPalette());
      for (final style in <TextStyle>[
        s.title,
        s.section,
        s.label,
        s.sectionTitle,
        s.sectionTitleLarge,
        s.data,
        s.body,
        s.emph,
        s.dense,
      ]) {
        expect(style.decoration, TextDecoration.none);
      }
    });

    testWidgets('Overlay 中的 Toast 文本实际解析为 TextDecoration.none', (
      tester,
    ) async {
      await tester.pumpWidget(
        MaterialApp(
          theme: buildGuiTheme(),
          home: Scaffold(
            body: Builder(
              builder: (context) => Center(
                child: ElevatedButton(
                  onPressed: () => showGuiToast(context, '参数已写入'),
                  child: const Text('go'),
                ),
              ),
            ),
          ),
        ),
      );
      await tester.tap(find.text('go'));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 240));

      final para = tester.renderObject<RenderParagraph>(
        find.byKey(const Key('gui_toast_text')),
      );
      expect(para.text.style?.decoration, TextDecoration.none);
      await tester.pump(const Duration(milliseconds: 3000));
    });
  });
}

/// 占位按钮，避免 AlertDialog actions 为空触发 layout 边界用例。
class GuiButtonProbe extends StatelessWidget {
  const GuiButtonProbe({super.key});

  @override
  Widget build(BuildContext context) => const SizedBox.shrink();
}
