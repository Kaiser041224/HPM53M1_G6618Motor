/// 速度环 / 角度环 PID 调整入口 —— 「监视 · 控制」页的两栏并排面板。
///
/// 排版同「保护门限 / 电流环 PI」：面板标题 + 一行 [ParamField]；参数一律
/// 元数据驱动，缺参如实「设备未提供」，0 猜测兜底。速度环字段
/// `software.control.speed_loop.kp/ki`（设备 schema 无 kd）；角度环
/// `software.control.angle_loop.kp/ki`（schema 暂无，按 speed_loop 命名约定预留）。
///
/// 从 `motor_drive_zone.dart` 迁入「监视 · 控制」页（调参时需与读数同屏）。
///
/// Copyright (c) 2026 Alliance HardwareGroup
/// SPDX-License-Identifier: BSD-3-Clause
library;

import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_type.dart';
import '../widgets/hairline_frame.dart';
import '../widgets/param_controls.dart';
import 'panel_title.dart';

/// 速度环 PID 参数位（全限定名）。设备 schema 无 kd。
const List<(String, String)> kSpeedLoopFields = <(String, String)>[
  ('software.control.speed_loop.kp', '比例增益 kp'),
  ('software.control.speed_loop.ki', '积分增益 ki'),
];

/// 角度环 PID 参数位（全限定名）。schema 暂无，按 speed_loop 命名约定预留；
/// 设备未提供时如实显示「设备未提供」。
const List<(String, String)> kAngleLoopFields = <(String, String)>[
  ('software.control.angle_loop.kp', '比例增益 kp'),
  ('software.control.angle_loop.ki', '积分增益 ki'),
];

/// 速度环 / 角度环两组 PID 面板，在右栏**上下排列**（各占独立 PanelTitle）。
class PidZone extends StatelessWidget {
  const PidZone({super.key, required this.state});

  final AppState state;

  /// 两组面板间距 [logical px]，与右栏各段间距一致（`sp-5`, 24px）。
  static const double _panelGap = 24;

  @override
  Widget build(BuildContext context) {
    // 监听：参数值与脏标记随时变化，需随 state 通知重建。
    return ListenableBuilder(
      listenable: state,
      builder: (context, _) => Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          _PidLoopPanel(
            frameKey: const Key('speed_loop_frame'),
            title: '速度环',
            en: 'Speed Loop',
            fields: kSpeedLoopFields,
            state: state,
          ),
          const SizedBox(height: _panelGap),
          _PidLoopPanel(
            frameKey: const Key('angle_loop_frame'),
            title: '角度环',
            en: 'Angle Loop',
            fields: kAngleLoopFields,
            state: state,
          ),
        ],
      ),
    );
  }
}

/// 一组 PID 参数位面板（速度环 / 角度环），排版同「保护门限 / 电流环」：
/// 面板标题 + 一行 [ParamField]。参数一律元数据驱动，缺参如实「设备未提供」。
class _PidLoopPanel extends StatelessWidget {
  const _PidLoopPanel({
    required this.frameKey,
    required this.title,
    required this.en,
    required this.fields,
    required this.state,
  });

  final Key frameKey;
  final String title;
  final String en;
  final List<(String, String)> fields;
  final AppState state;

  @override
  Widget build(BuildContext context) {
    final scale = GuiTypeScale.of(context);
    return HairlineFrame(
      key: frameKey,
      padding: const EdgeInsets.all(10),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          PanelTitle(
            title: title,
            en: en,
            trailing: Text('由设备元数据提供', style: scale.label),
          ),
          const SizedBox(height: 10),
          Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              for (final (name, label) in fields) ...<Widget>[
                Expanded(
                  child: ParamField(
                    state: state,
                    name: name,
                    label: label,
                    compact: true,
                  ),
                ),
                if (name != fields.last.$1) const SizedBox(width: 8),
              ],
            ],
          ),
        ],
      ),
    );
  }
}
