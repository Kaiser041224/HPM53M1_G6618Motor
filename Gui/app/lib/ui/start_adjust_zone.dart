/// 启动 / 调整 —— 快捷控制模式（协议 v1.1 `CTRL_CMD SET_MODE`）。
///
/// 三枚模式按钮（力矩 / 速度 / 角度）协议已暴露，故**可点**；与「电机 · 驱动」
/// 页里协议未暴露的标定例程禁用按钮区分：本面板 trailing 注明 v1.1。
/// 当前模式**无设备回读**，仅按最近一次成功写入在本地记录并高亮，
/// 故小字如实说明。
///
/// 从 `motor_drive_zone.dart` 迁入「监视 · 控制」页（调参时需与读数同屏）。
///
/// Copyright (c) 2026 Alliance HardwareGroup
/// SPDX-License-Identifier: BSD-3-Clause
library;

import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/gui_button.dart';
import '../widgets/hairline_frame.dart';
import '../widgets/param_controls.dart' show faultText;
import 'panel_title.dart';

/// 快捷控制模式（协议 v1.1 `CTRL_CMD SET_MODE`）：(mode, 中文名)。
const List<(int, String)> kControlModes = <(int, String)>[
  (1, '力矩'),
  (2, '速度'),
  (3, '角度'),
];

/// 启动 / 调整面板：快捷控制模式测试。
class StartAdjustZone extends StatelessWidget {
  const StartAdjustZone({super.key, required this.state});

  final AppState state;

  @override
  Widget build(BuildContext context) {
    // 监听：模式高亮由最近一次成功写入的本地记录驱动，需随 state 通知重建。
    return ListenableBuilder(
      listenable: state,
      builder: (context, _) {
        final palette = GuiPalette.of(context);
        final scale = GuiTypeScale.of(context);
        return HairlineFrame(
          key: const Key('quick_control_frame'),
          padding: const EdgeInsets.all(10),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: <Widget>[
              PanelTitle(
                title: '启动 / 调整',
                en: 'Quick Control',
                trailing: Text(
                  '协议 v1.1',
                  key: const Key('ctrl_mode_v11_note'),
                  style: scale.label.copyWith(color: palette.accent),
                ),
              ),
              const SizedBox(height: 10),
              Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: <Widget>[
                  for (final (mode, label) in kControlModes) ...<Widget>[
                    Expanded(
                      child: _ModeButton(
                        mode: mode,
                        label: '$label控制',
                        selected: state.ctrlMode == mode,
                        onPressed: () => _selectMode(context, mode, label),
                      ),
                    ),
                    if (mode != kControlModes.last.$1)
                      const SizedBox(width: 8),
                  ],
                ],
              ),
              const SizedBox(height: 9),
              Text(
                '▲ 模式无回读（协议 v1.1）：高亮为最近一次成功写入的本地记录，非设备回读',
                key: const Key('ctrl_mode_readback_note'),
                style: scale.label.copyWith(color: palette.fgMute),
              ),
            ],
          ),
        );
      },
    );
  }

  /// 下发控制模式；成功提示中文名，失败走既有 [faultText] 文案表
  /// （运行中切换 → LOCKED code 4）。
  Future<void> _selectMode(BuildContext context, int mode, String label) async {
    final messenger = ScaffoldMessenger.of(context);
    final ok = await state.setCtrlMode(mode);
    messenger.showSnackBar(
      SnackBar(content: Text(ok ? '控制模式：$label' : faultText(state))),
    );
  }
}

/// 一枚控制模式按钮：选中态用「primary 边框 + ◆ 当前徽标」三重编码
/// （符号 + 文字 + 强调色）标注，未选中位保留等高。
class _ModeButton extends StatelessWidget {
  const _ModeButton({
    required this.mode,
    required this.label,
    required this.selected,
    required this.onPressed,
  });

  final int mode;
  final String label;
  final bool selected;
  final VoidCallback onPressed;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        GuiButton(
          key: Key('ctrl_mode_$mode'),
          label: label,
          variant: selected
              ? GuiButtonVariant.primary
              : GuiButtonVariant.normal,
          onPressed: onPressed,
        ),
        const SizedBox(height: 4),
        SizedBox(
          height: 18,
          child: selected
              ? Center(
                  child: Container(
                    key: Key('ctrl_mode_badge_$mode'),
                    padding: const EdgeInsets.symmetric(
                      horizontal: 6,
                      vertical: 1,
                    ),
                    decoration: BoxDecoration(
                      border: Border.all(
                        color: palette.accent.withValues(alpha: 0.55),
                      ),
                    ),
                    child: Text(
                      '◆ 当前',
                      style: scale.label.copyWith(color: palette.accent),
                    ),
                  ),
                )
              : null,
        ),
      ],
    );
  }
}
