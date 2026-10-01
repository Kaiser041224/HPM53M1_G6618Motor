/// 调参板块 —— 监视·控制页右栏的「常用调参」区。
///
/// 只放**调试过程中最常改**的参数组，全量表仍在参数页。
/// 所有参数名均为设备元数据里的域限定全名；取不到即由 [ParamField]
/// 如实渲染为「设备未提供」，不做猜测兜底。
///
/// 电流环 / 速度环 / 角度环 PID 已合并进 `pid_panel.dart` 的单个面板，
/// 本文件只保留「保护门限」。
///
/// Copyright (c) 2026 Alliance HardwareGroup
/// SPDX-License-Identifier: BSD-3-Clause
library;

import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/param_controls.dart';
import 'panel_title.dart';

/// 保护门限 —— 固件把这几个参数列为**安全相关**
/// （`app_terminal_cmd_param.c` 的 `s_safety_params`），终端 set 时会警告；
/// 界面上同样给 SAFETY 徽标，避免随手改动。
///
/// 这三个阈值直接决定过流 / 母线过压 / 欠压保护是否按预期动作。
class ProtectionLimitsPanel extends StatelessWidget {
  const ProtectionLimitsPanel({super.key, required this.state});

  final AppState state;

  /// 固件 `s_safety_params` 中的保护阈值（域限定全名）。
  static const List<(String, String)> fields = <(String, String)>[
    ('software.fault.oc_trip_a', '过流阈值'),
    ('software.fault.vbus_ov_v', '母线过压'),
    ('software.fault.vbus_uv_v', '母线欠压'),
  ];

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return PanelBox(
      frameKey: const Key('protection_limits_frame'),
      title: '保护门限',
      en: 'Protection',
      hue: TitleHue.violet,
      trailing: Text(
        'SAFETY · 改动即影响保护动作',
        style: scale.label.copyWith(color: palette.stWarn),
      ),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          for (final (name, label) in fields) ...<Widget>[
            Expanded(
              child: ParamField(
                state: state,
                name: name,
                label: label,
                safety: true,
                compact: true,
              ),
            ),
            if (name != fields.last.$1) const SizedBox(width: 8),
          ],
        ],
      ),
    );
  }
}
