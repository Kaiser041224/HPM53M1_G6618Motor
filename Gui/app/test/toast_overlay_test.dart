import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/toast_overlay.dart';

/// Pumps a minimal host with a single button that raises one toast, then taps
/// it so exactly one overlay entry is live.
Future<void> _pumpAndShow(
  WidgetTester tester, {
  String message = '提示信息',
  ToastTone tone = ToastTone.info,
}) async {
  await tester.pumpWidget(
    MaterialApp(
      theme: buildGuiTheme(),
      home: Scaffold(
        body: Builder(
          builder: (context) => Center(
            child: ElevatedButton(
              onPressed: () => showGuiToast(context, message, tone: tone),
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

/// Finishes the toast's lifetime so no transient callback survives the test.
Future<void> _drain(WidgetTester tester) async {
  await tester.pump(const Duration(milliseconds: 3000));
}

void main() {
  group('GuiToast 结构', () {
    testWidgets('双层青色锐利矩形 + 图标 + fgStrong 文字', (tester) async {
      const palette = GuiPalette();
      await _pumpAndShow(tester, message: '已连接');

      expect(find.byKey(const Key('gui_toast')), findsOneWidget);
      final frame = tester.widget<Container>(
        find.byKey(const Key('gui_toast_frame')),
      );
      final frameDecoration = frame.decoration! as BoxDecoration;
      final border = frameDecoration.border! as Border;
      expect(border.top.color, palette.accent);
      expect(border.top.width, inInclusiveRange(1.0, 1.5));
      expect(frameDecoration.color, palette.bgBase);
      // 直角：不得出现圆角。
      expect(frameDecoration.borderRadius, isNull);

      final inner = tester.widget<Container>(
        find.byKey(const Key('gui_toast_inner')),
      );
      final innerBorder =
          (inner.decoration! as BoxDecoration).border! as Border;
      expect(innerBorder.top.color, palette.lineDim);
      expect(innerBorder.top.width, 1.0);

      final text = tester.widget<Text>(find.byKey(const Key('gui_toast_text')));
      expect(text.data, '已连接');
      expect(text.style?.color, palette.fgStrong);

      expect(find.byKey(const Key('gui_toast_icon')), findsOneWidget);

      await _drain(tester);
    });

    testWidgets('动画起始位在落点下方，随后向上滑入定格', (tester) async {
      await _pumpAndShow(tester);
      final startTop = tester
          .getTopLeft(find.byKey(const Key('gui_toast_frame')))
          .dy;

      await tester.pump(const Duration(milliseconds: 320));
      final settledTop = tester
          .getTopLeft(find.byKey(const Key('gui_toast_frame')))
          .dy;

      expect(
        settledTop,
        lessThan(startTop),
        reason: 'toast 应从下方弹出（起始 dy 更大）并向上定格',
      );
      expect(tester.takeException(), isNull);

      await _drain(tester);
    });

    testWidgets('~2.2s 后自动消失', (tester) async {
      await _pumpAndShow(tester);
      expect(find.byKey(const Key('gui_toast_frame')), findsOneWidget);

      await tester.pump(const Duration(milliseconds: 2600));
      expect(find.byKey(const Key('gui_toast_frame')), findsNothing);
    });

    testWidgets('底部水平居中锚定：底缘落在状态栏上方 inset 处', (tester) async {
      await _pumpAndShow(tester);
      // 入场完成、进入定格段。
      await tester.pump(const Duration(milliseconds: 400));

      final size = tester.view.physicalSize / tester.view.devicePixelRatio;
      final rect = tester.getRect(find.byKey(const Key('gui_toast_frame')));

      expect(
        rect.bottom,
        closeTo(size.height - kGuiToastBottomInset, 1.0),
        reason: '底缘应锚定在状态栏顶缘上方 ~12px（不再居中悬浮）',
      );
      expect(
        rect.center.dx,
        closeTo(size.width / 2, 1.0),
        reason: '水平取底部居中',
      );
      expect(
        rect.center.dy,
        greaterThan(size.height / 2),
        reason: '应贴底，不得居中偏下悬浮',
      );

      await _drain(tester);
    });

    testWidgets('无下划 / 黄线强调装饰：双层框四边等色等宽', (tester) async {
      const palette = GuiPalette();
      await _pumpAndShow(tester);

      final frame = tester.widget<Container>(
        find.byKey(const Key('gui_toast_frame')),
      );
      final frameBorder =
          (frame.decoration! as BoxDecoration).border! as Border;
      // 四边等色等宽 —— 不允许出现「仅底边」的下划强调线。
      expect(frameBorder.top, frameBorder.bottom);
      expect(frameBorder.top, frameBorder.left);
      expect(frameBorder.top, frameBorder.right);

      // 提示框内不得有黄线（accent2 / valueAmber）或仅底边的装饰条。
      final containers = tester.widgetList<Container>(
        find.descendant(
          of: find.byKey(const Key('gui_toast')),
          matching: find.byType(Container),
        ),
      );
      for (final container in containers) {
        final decoration = container.decoration;
        if (decoration is! BoxDecoration) continue;
        final border = decoration.border;
        if (border is Border) {
          expect(
            border.bottom.width,
            border.top.width,
            reason: '不得出现仅底边的下划强调线',
          );
        }
        expect(decoration.color, isNot(palette.accent2));
        expect(decoration.color, isNot(palette.valueAmber));
      }

      await _drain(tester);
    });
  });

  group('GuiToast 语气三重编码（icon + 色带 + 颜色）', () {
    const palette = GuiPalette();

    for (final entry in <(ToastTone, Color)>[
      (ToastTone.info, palette.accent),
      (ToastTone.success, palette.stNormal),
      (ToastTone.warn, palette.stWarn),
      (ToastTone.error, palette.stFault),
    ]) {
      testWidgets('${entry.$1.name} → 图标按语气着色', (tester) async {
        await _pumpAndShow(tester, tone: entry.$1);
        final icon = tester.widget<Icon>(
          find.byKey(const Key('gui_toast_icon')),
        );
        expect(icon.color, entry.$2);
        await _drain(tester);
      });
    }
  });
}
