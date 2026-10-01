/// 调参板块 —— 监视·控制页右栏的「常用调参」区。
///
/// 只放**调试过程中最常改**的参数组，全量表仍在参数页。
/// 所有参数名均为设备元数据里的域限定全名；取不到即由 [ParamField]
/// 如实渲染为「设备未提供」，不做猜测兜底。
///
/// Copyright (c) 2026 Alliance HardwareGroup
/// SPDX-License-Identifier: BSD-3-Clause
library;

import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/hairline_frame.dart';
import '../widgets/param_controls.dart';
import '../widgets/vfd_bar_painter.dart';
import 'label_style.dart';
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
    return HairlineFrame(
      key: const Key('protection_limits_frame'),
      padding: const EdgeInsets.all(10),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          PanelTitle(
            title: '保护门限',
            en: 'Protection',
            trailing: Text(
              'SAFETY · 改动即影响保护动作',
              style: scale.label.copyWith(color: palette.stWarn),
            ),
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
                    safety: true,
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

/// 电流环 PI —— kp / ki。
///
/// **诚实标注**：固件 `app_software_params.h` 把 `current_loop` 标为
/// 「FOC 预留」，且当前**没有任何消费者**（`algo_pid.c` 用的是自身默认值）。
/// 写入会正常落到设备并持久化，但**暂不改变运行行为** ——
/// 界面必须说清这一点，否则会让人以为调了它电机就变了。
class CurrentLoopPanel extends StatelessWidget {
  const CurrentLoopPanel({super.key, required this.state});

  final AppState state;

  /// `software.control.current_loop.*`（域限定全名）。
  static const List<(String, String)> fields = <(String, String)>[
    ('software.control.current_loop.kp', '比例增益 kp'),
    ('software.control.current_loop.ki', '积分增益 ki'),
  ];

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return HairlineFrame(
      key: const Key('current_loop_frame'),
      padding: const EdgeInsets.all(10),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          PanelTitle(
            title: '电流环 PI',
            en: 'Current Loop',
            trailing: Text(
              '目标带宽 1 kHz',
              style: scale.label.copyWith(color: palette.valuePurple),
            ),
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
          const SizedBox(height: 9),
          Text(
            '▲ FOC 预留 · 当前无消费者，写入暂不改变运行行为',
            key: const Key('current_loop_note'),
            style: scale.label.copyWith(color: palette.stWarn),
          ),
        ],
      ),
    );
  }
}

/// 给定设定 —— FREQ / MOD 的当前下发值（只读回显）。
///
/// 真正的滑块在 [ControlPanel] 里；这里只把数值与设备回读对照，
/// 让「我设的」和「设备认的」一眼可比。
class SetpointReadbackPanel extends StatelessWidget {
  const SetpointReadbackPanel({super.key, required this.state});

  final AppState state;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final status = state.status;
    final freq = status?.freqHz;
    final mod = status?.modPct;

    return HairlineFrame(
      key: const Key('setpoint_readback_frame'),
      padding: const EdgeInsets.all(10),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          PanelTitle(
            title: '设备回读',
            en: 'Setpoint',
            trailing: Text(
              'STATUS_GET 快照',
              style: scale.label.copyWith(color: palette.accent),
            ),
          ),
          const SizedBox(height: 10),
          Row(
            children: <Widget>[
              Expanded(child: _readback(context, '频率', 'Hz', freq, 2)),
              const SizedBox(width: 8),
              Expanded(child: _readback(context, '调制比', '%', mod, 1)),
            ],
          ),
        ],
      ),
    );
  }

  Widget _readback(
    BuildContext context,
    String label,
    String unit,
    double? value,
    int precision,
  ) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    // 无状态快照时如实显示「—」，不用 0 冒充。
    final text = value == null ? '—' : value.toStringAsFixed(precision);
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 7),
      decoration: BoxDecoration(
        color: palette.bgVoid,
        border: Border.all(color: palette.lineDim, width: palette.lineWidth),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        mainAxisSize: MainAxisSize.min,
        children: <Widget>[
          Text(label, style: guiLabelStyle(palette, scale)),
          const SizedBox(height: 5),
          FittedBox(
            fit: BoxFit.scaleDown,
            alignment: Alignment.centerLeft,
            child: Row(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.baseline,
              textBaseline: TextBaseline.alphabetic,
              children: <Widget>[
                Text(
                  text,
                  maxLines: 1,
                  style: scale.data.copyWith(
                    fontSize: 21,
                    color: value == null
                        ? palette.fgMute
                        : tintForUnit(palette, unit),
                  ),
                ),
                // 单位保持最暗档，专色只赋数值（§2.6.3）。
                if (value != null) ...<Widget>[
                  const SizedBox(width: 4),
                  Text(unit, style: scale.label),
                ],
              ],
            ),
          ),
        ],
      ),
    );
  }
}
