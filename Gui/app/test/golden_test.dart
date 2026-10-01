import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/widgets/hairline_frame.dart';
import 'package:g6618_gui/widgets/section_title.dart';

/// Rendering-level regression tests for the hairline motifs: a single bright
/// cyan frame with a dark inner seam, and a single low-bright violet group
/// line beneath the section title. Regenerate with `flutter test
/// --update-goldens`.
void main() {
  testWidgets('golden: HairlineFrame main is a bright frame with a dark seam', (
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

  testWidgets('golden: SectionTitle underline is a single low-bright violet line', (
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
