/// 文本截断守卫 —— 「尽量不要有文字显示不全」的可执行版本。
///
/// 目视检查靠不住：`TextOverflow.ellipsis` 是**静默**的，不报错也不抛异常。
/// 这里直接遍历渲染树，找出 `RenderParagraph.didExceedMaxLines == true`
/// 的段落 —— 那正是「被省略号截掉」的判定性证据。
///
/// 覆盖最窄的可用窗口（app 的 `minimumSize` 1024x640）与默认窗口，
/// 因为截断几乎总在窄屏暴露。
///
/// Copyright (c) 2026 Alliance HardwareGroup
/// SPDX-License-Identifier: BSD-3-Clause
library;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/single_page.dart';

/// 把随包字体喂给测试渲染器。
///
/// **不加载字体这个测试就是失真的** —— `flutter_test` 默认用一套很宽的
/// 兜底字体，会把所有文本都渲染成截断，于是守卫永远报警、毫无意义。
/// 必须用真实字体度量，测出来的才是用户看到的结果。
Future<void> _loadBundledFonts() async {
  for (final MapEntry<String, String> e in <String, String>{
    'VT323': 'assets/fonts/VT323-Regular.ttf',
    'Fusion Pixel 12': 'assets/fonts/FusionPixel12-zh_hans.ttf',
    'Silkscreen': 'assets/fonts/Silkscreen-Regular.ttf',
    'Share Tech Mono': 'assets/fonts/ShareTechMono-Regular.ttf',
  }.entries) {
    final loader = FontLoader(e.key);
    loader.addFont(rootBundle.load(e.value));
    await loader.load();
  }
}

/// 收集**当前可见**页面里所有「因超行而被截断」的文本及其内容。
///
/// 必须走 `find.byType(..., skipOffstage: true)`（默认值）—— 直接遍历
/// 渲染树会把 `Offstage` 里未显示的页面一并算上，误报成当前页的问题。
List<String> _truncatedTexts(WidgetTester tester) {
  final out = <String>[];
  for (final RenderParagraph para in tester.renderObjectList<RenderParagraph>(
    find.byType(Text, skipOffstage: true),
  )) {
    if (para.didExceedMaxLines) out.add(para.text.toPlainText());
  }
  return out;
}

StatusDto _status() => const StatusDto(
  faultFlags: 0,
  motorRunning: true,
  motorState: 1,
  freqHz: 50.0,
  modPct: 72.0,
  iU: 1.82,
  iV: 1.75,
  iW: 1.80,
  vbus: 24.0,
  ntcTemp: 78.4,
  encRotorDeg: 123.4,
  encOutputDeg: 120.9,
);

/// 用**固件真实参数名**构造元数据（含最容易撑破的长名）。
List<ParamMetaDto> _metas() {
  const names = <(String, int, String)>[
    ('motor.rs_ohm', 0, 'Ω'),
    ('motor.ke_vs_per_rad', 0, ''),
    ('hardware.inverter.pwm_freq_hz', 1, 'Hz'),
    ('hardware.inverter.deadtime_ns', 1, 'ns'),
    ('hardware.current_sense.a_per_volt', 0, 'A/V'),
    ('hardware.current_sense.shunt_ohm', 0, 'Ω'),
    ('hardware.vbus_sense.divider_high_ohm', 1, 'Ω'),
    ('software.fault.oc_trip_a', 0, 'A'),
    ('software.fault.vbus_ov_v', 0, 'V'),
    ('software.control.current_loop.kp', 0, ''),
    ('motor.encoder.output_pinion_teeth', 1, ''),
    ('motor.encoder.resolution_bits', 1, 'bit'),
  ];
  return <ParamMetaDto>[
    for (var i = 0; i < names.length; i++)
      ParamMetaDto(
        index: i,
        name: names[i].$1,
        ty: names[i].$2,
        apply: names[i].$1.contains('inverter') ? 1 : 0,
        unit: names[i].$3,
        min: 0,
        max: names[i].$2 == 1 ? 100000 : 100,
      ),
  ];
}

Future<void> _pumpAt(WidgetTester tester, Size size) async {
  tester.view.physicalSize = size;
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);

  final state = AppState.test(status: _status(), metas: _metas());
  addTearDown(state.dispose);
  for (final m in state.metas) {
    state.values[m.name] = ParamValueDto(
      ty: m.ty,
      raw: m.ty == 0 ? 1065353216 : 12345,
    );
  }

  await tester.pumpWidget(
    MaterialApp(theme: buildGuiTheme(), home: SinglePage(state: state)),
  );
  // 监视页有常驻动画，pumpAndSettle 永不收敛。
  await tester.pump(const Duration(milliseconds: 400));
}

void main() {
  setUpAll(_loadBundledFonts);

  // app 的 window_manager minimumSize 是 1024x640 —— 界面必须在此成立。
  for (final Size size in const <Size>[Size(1024, 640), Size(1280, 800)]) {
    testWidgets('${size.width.toInt()}x${size.height.toInt()}：所有页面无文字截断', (
      tester,
    ) async {
      await _pumpAt(tester, size);

      for (final nav in kNavItems) {
        await tester.tap(find.byKey(Key(nav.key)));
        await tester.pump(const Duration(milliseconds: 400));
        expect(
          _truncatedTexts(tester),
          isEmpty,
          reason: '${nav.zh} 页存在被截断的文本',
        );
      }
    });
  }
}
