import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/widgets/hairline_frame.dart';
import 'package:g6618_gui/widgets/section_title.dart';

/// Rendering-level regression tests for the hairline motifs. These guard the
/// design-system §3.2 "一亮两暗" lines against the transparent-container
/// `boxShadow` defect, where an unfilled box shadow bleeds into the interior.
/// Regenerate with `flutter test --update-goldens`.
void main() {
  testWidgets('golden: HairlineFrame main keeps its interior transparent', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        debugShowCheckedModeBanner: false,
        theme: buildGuiTheme(),
        home: const ColoredBox(
          color: Color(0xFF081018),
          child: Center(
            child: RepaintBoundary(
              key: ValueKey('hairline_frame_main'),
              child: SizedBox(
                width: 220,
                child: HairlineFrame(
                  child: Padding(
                    padding: EdgeInsets.all(12),
                    child: Text('G6618', style: TextStyle(fontSize: 14)),
                  ),
                ),
              ),
            ),
          ),
        ),
      ),
    );
    await expectLater(
      find.byKey(const ValueKey('hairline_frame_main')),
      matchesGoldenFile('goldens/hairline_frame_main.png'),
    );
  });

  testWidgets('golden: SectionTitle underline is a stack of thin lines', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        debugShowCheckedModeBanner: false,
        theme: buildGuiTheme(),
        home: const ColoredBox(
          color: Color(0xFF081018),
          child: Center(
            child: RepaintBoundary(
              key: ValueKey('section_title'),
              child: SizedBox(
                width: 260,
                child: SectionTitle(title: '状态监视', en: 'Status'),
              ),
            ),
          ),
        ),
      ),
    );
    await expectLater(
      find.byKey(const ValueKey('section_title')),
      matchesGoldenFile('goldens/section_title.png'),
    );
  });
}
