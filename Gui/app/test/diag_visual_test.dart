import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/toast_overlay.dart';
import 'package:g6618_gui/widgets/gui_button.dart';
import 'package:g6618_gui/widgets/gui_text_field.dart';
import 'package:g6618_gui/widgets/param_controls.dart';

/// Diagnostic goldens (temp): render the suspicious surfaces so the
/// "double yellow line under glyphs" defect can be inspected pixel by pixel.
/// Regenerate with `flutter test --update-goldens test/diag_visual_test.dart`.
Future<void> _loadFonts() async {
  final families = <String, List<String>>{
    'VT323': <String>['assets/fonts/VT323-Regular.ttf'],
    'Fusion Pixel 12': <String>['assets/fonts/FusionPixel12-zh_hans.ttf'],
    'Silkscreen': <String>['assets/fonts/Silkscreen-Regular.ttf'],
    'Share Tech Mono': <String>['assets/fonts/ShareTechMono-Regular.ttf'],
  };
  for (final entry in families.entries) {
    final loader = FontLoader(entry.key);
    for (final path in entry.value) {
      final bytes = await File(path).readAsBytes();
      loader.addFont(Future<ByteData>.value(ByteData.view(bytes.buffer)));
    }
    await loader.load();
  }
}

void main() {
  setUpAll(_loadFonts);

  final state = AppState.test(
    metas: <ParamMetaDto>[
      const ParamMetaDto(
        index: 0,
        name: 'software.fault.vbus_ov_v',
        ty: 0,
        apply: 0,
        unit: 'V',
        min: 0,
        max: 80,
      ),
    ],
  );
  state.values['software.fault.vbus_ov_v'] = ParamValueDto(
    ty: 0,
    raw: (ByteData(4)..setFloat32(0, 48.2, Endian.little)).getInt32(
      0,
      Endian.little,
    ),
  );

  tearDownAll(state.dispose);

  testWidgets('diag: param edit dialog (title/body/input/SAFETY/buttons)', (
    tester,
  ) async {
    final ctrl = TextEditingController(text: '48.20');
    addTearDown(ctrl.dispose);
    await tester.pumpWidget(
      RepaintBoundary(
        key: const Key('diag_param_dialog'),
        child: MaterialApp(
          debugShowCheckedModeBanner: false,
          theme: buildGuiTheme(),
          home: Scaffold(
            backgroundColor: const Color(0xFF04080C),
            body: Center(
              child: AlertDialog(
                title: const Text('software.fault.vbus_ov_v'),
                content: Column(
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: <Widget>[
                    Text(
                      'f32 · 0.0..80.0 V',
                      style: const GuiTypeScale(GuiPalette()).label.copyWith(
                        color: const GuiPalette().fgDim,
                      ),
                    ),
                    const SizedBox(height: 8),
                    GuiTextField(controller: ctrl, labelText: '新值'),
                    const SizedBox(height: 10),
                    Row(
                      children: <Widget>[
                        Text(
                          '48.20 V',
                          style: GuiGlowStyles.softWith(
                            const GuiPalette(),
                            const GuiTypeScale(GuiPalette()).data.copyWith(
                              fontSize: 15,
                              color: const GuiPalette().valueAmber,
                            ),
                          ),
                        ),
                        const SizedBox(width: 10),
                        const SafetyBadge(),
                      ],
                    ),
                  ],
                ),
                actions: <Widget>[
                  GuiButton(label: '取消', onPressed: () {}),
                  GuiButton(
                    label: '写入',
                    variant: GuiButtonVariant.primary,
                    onPressed: () {},
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
    await expectLater(
      find.byKey(const Key('diag_param_dialog')),
      matchesGoldenFile('goldens/diag_param_dialog.png'),
    );
  });

  testWidgets('diag: toast', (tester) async {
    await tester.pumpWidget(
      RepaintBoundary(
        key: const Key('diag_toast'),
        child: MaterialApp(
          debugShowCheckedModeBanner: false,
          theme: buildGuiTheme(),
          home: Scaffold(
            backgroundColor: const Color(0xFF04080C),
            body: Builder(
              builder: (context) => Center(
                child: ElevatedButton(
                  onPressed: () =>
                      showGuiToast(context, '参数已写入', tone: ToastTone.success),
                  child: const Text('go'),
                ),
              ),
            ),
          ),
        ),
      ),
    );
    await tester.tap(find.text('go'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 240));
    await expectLater(
      find.byKey(const Key('diag_toast')),
      matchesGoldenFile('goldens/diag_toast.png'),
    );
    await tester.pump(const Duration(milliseconds: 3000));
  });

  testWidgets('diag: param row (amber value + safety badge)', (tester) async {
    await tester.pumpWidget(
      RepaintBoundary(
        key: const Key('diag_param_row'),
        child: MaterialApp(
          debugShowCheckedModeBanner: false,
          theme: buildGuiTheme(),
          home: Scaffold(
            backgroundColor: const Color(0xFF04080C),
            body: Center(
              child: SizedBox(
                width: 320,
                child: ParamField(
                  state: state,
                  name: 'software.fault.vbus_ov_v',
                  label: '过压门限',
                  safety: true,
                ),
              ),
            ),
          ),
        ),
      ),
    );
    await expectLater(
      find.byKey(const Key('diag_param_row')),
      matchesGoldenFile('goldens/diag_param_row.png'),
    );
  });
}
