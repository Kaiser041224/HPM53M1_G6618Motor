import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/control_zone.dart';
import 'package:g6618_gui/ui/panel_title.dart';
import 'package:g6618_gui/ui/pid_zone.dart';
import 'package:g6618_gui/ui/start_adjust_zone.dart';
import 'package:g6618_gui/ui/tuning_blocks.dart';
import 'package:g6618_gui/widgets/section_title.dart';

/// v6 标题分色：渐变结构由色相参数派生，相邻标题相异。
void main() {
  const palette = GuiPalette();

  Color hueColor(TitleHue hue) => titleHueColor(palette, hue);

  group('A. 渐变色相派生', () {
    test('每个色相：同色 38% → 12% → 透明，三停靠点 0/0.5/1', () {
      for (final hue in TitleHue.values) {
        final g = titleGradientFor(palette, hue);
        expect(g.begin, Alignment.centerLeft);
        expect(g.end, Alignment.centerRight);
        expect(g.stops, const <double>[0.0, 0.5, 1.0]);
        expect(g.colors.length, 3);
        expect(g.colors[0].a, closeTo(0.38, 0.005), reason: '$hue 首档 38%');
        expect(g.colors[1].a, closeTo(0.12, 0.005), reason: '$hue 次档 12%');
        expect(g.colors[2].a, closeTo(0.0, 0.005), reason: '$hue 末档透明');
        for (final color in <Color>[g.colors[0], g.colors[1], g.colors[2]]) {
          expect(color.r, closeTo(hueColor(hue).r, 0.002));
          expect(color.g, closeTo(hueColor(hue).g, 0.002));
          expect(color.b, closeTo(hueColor(hue).b, 0.002));
        }
      }
    });

    test('五个色相彼此不同（相邻标题才有可分辨的色相）', () {
      final argbs = TitleHue.values.map(hueColor).map((c) => c.toARGB32()).toSet();
      expect(argbs.length, TitleHue.values.length);
    });

    test('默认色相为 violet，且与 kTitleGradient 同色', () {
      expect(kDefaultTitleHue, TitleHue.violet);
      final g = titleGradientFor(palette, kDefaultTitleHue);
      expect(
        g.colors.map((c) => c.toARGB32()).toList(),
        kTitleGradient.colors.map((c) => c.toARGB32()).toList(),
      );
    });
  });

  group('B. 控件把色相落到标题条渐变上', () {
    testWidgets('SectionTitle / PanelTitle 的 hue 决定标题条首色', (tester) async {
      await tester.pumpWidget(
        MaterialApp(
          theme: buildGuiTheme(),
          home: Scaffold(
            body: Column(
              children: <Widget>[
                const SectionTitle(title: '青', hue: TitleHue.cyan),
                const PanelTitle(title: '绿', hue: TitleHue.green),
              ],
            ),
          ),
        ),
      );
      final bars = tester.widgetList<Container>(
        find.byKey(const Key('gui_title_bar')),
      ).toList();
      expect(bars.length, 2);
      final cyan = ((bars[0].decoration! as BoxDecoration).gradient!
              as LinearGradient)
          .colors
          .first;
      final green = ((bars[1].decoration! as BoxDecoration).gradient!
              as LinearGradient)
          .colors
          .first;
      expect(cyan.r, closeTo(palette.accent.r, 0.002));
      expect(green.g, closeTo(palette.valueGreen.g, 0.002));
    });
  });

  group('C. 监视·控制右栏六段相邻相异', () {
    testWidgets('故障/启动调整/电流环/速度环/角度环/保护门限 = 洋红/青/紫/翠绿/洋红/紫', (
      tester,
    ) async {
      tester.view.physicalSize = const Size(1200, 3200);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.reset);

      final state = AppState.test();
      addTearDown(state.dispose);

      await tester.pumpWidget(
        MaterialApp(
          theme: buildGuiTheme(),
          home: Scaffold(
            body: SingleChildScrollView(
              child: Column(
                children: <Widget>[
                  ControlZone(state: state),
                  const SizedBox(height: 24),
                  StartAdjustZone(state: state),
                  const SizedBox(height: 24),
                  CurrentLoopPanel(state: state),
                  const SizedBox(height: 24),
                  PidZone(state: state),
                  const SizedBox(height: 24),
                  ProtectionLimitsPanel(state: state),
                ],
              ),
            ),
          ),
        ),
      );

      TitleHue sectionHue(String title) =>
          tester.widget<SectionTitle>(
            find.widgetWithText(SectionTitle, title),
          ).hue;
      TitleHue panelHue(String title) =>
          tester.widget<PanelBox>(find.widgetWithText(PanelBox, title)).hue;

      final segments = <TitleHue>[
        sectionHue('故障'),
        panelHue('启动 / 调整'),
        panelHue('电流环 PI'),
        panelHue('速度环'),
        panelHue('角度环'),
        panelHue('保护门限'),
      ];
      expect(segments, <TitleHue>[
        TitleHue.magenta,
        TitleHue.cyan,
        TitleHue.violet,
        TitleHue.green,
        TitleHue.magenta,
        TitleHue.violet,
      ]);

      for (var i = 1; i < segments.length; i++) {
        expect(
          segments[i],
          isNot(segments[i - 1]),
          reason: '第 $i 段与上一段标题不得同色相',
        );
      }
    });
  });
}
