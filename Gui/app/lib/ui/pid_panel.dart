/// PID 三环整定面板 —— 「监视 · 控制」页右栏的**电流环 / 速度环 / 角度环**入口。
///
/// 三个环曾各占一个独立 [PanelBox]（电流环在 `tuning_blocks.dart`，速度 / 角度
/// 环在 `pid_zone.dart`）。合并进**一个外框面板**：主标题 + 一条共用按钮行
/// （读取 / 写入 / 恢复出厂，作用于三环参数名集合），其下三节以副标题 + 细分
/// 割线区隔。暂存 / 写入 / 恢复出厂语义与参数页、驱动页完全一致 —— 一律元数据
/// 驱动，缺参如实「设备未提供」，0 猜测兜底。
///
/// Copyright (c) 2026 Alliance HardwareGroup
/// SPDX-License-Identifier: BSD-3-Clause
library;

import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/gui_button.dart';
import '../widgets/param_controls.dart';
import 'panel_title.dart';
import 'toast_overlay.dart';

/// 电流环 PI 参数位（全限定名）。`software.control.current_loop.*`。
const List<(String, String)> kCurrentLoopFields = <(String, String)>[
  ('software.control.current_loop.kp', '比例增益 kp'),
  ('software.control.current_loop.ki', '积分增益 ki'),
];

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

/// 三环参数位小节：(节 key, 副标题, 英文副标, 参数位, 诚实标注)。
const List<(String, String, String, List<(String, String)>, (String, String)?)>
kPidSections =
    <(String, String, String, List<(String, String)>, (String, String)?)>[
      (
        'pid_current_section',
        '电流环',
        'Current Loop',
        kCurrentLoopFields,
        ('▲ FOC 预留 · 当前无消费者，写入暂不改变运行行为', 'current_loop_note'),
      ),
      ('pid_speed_section', '速度环', 'Speed Loop', kSpeedLoopFields, null),
      ('pid_angle_section', '角度环', 'Angle Loop', kAngleLoopFields, null),
    ];

/// 电流环 / 速度环 / 角度环合并面板。
///
/// 一个外框 [PanelBox]（青 1.5px）+ 一条主标题；标题下方一条**共用按钮行**
/// （读取 / 写入 / 恢复出厂）与行尾的待写入计数徽标；其下三节以小号青色副标
/// 题 + [GuiPalette.lineDim] 1px 分割线区隔，每节内为 [ParamField] 行。
class PidPanel extends StatelessWidget {
  const PidPanel({super.key, required this.state});

  final AppState state;

  /// 恢复出厂名单：三环参数全名，顺序为电流 → 速度 → 角度。
  static final List<String> resetNames = <String>[
    for (final (_, _, _, fields, _) in kPidSections)
      for (final (name, _) in fields) name,
  ];

  /// 本面板写入范围（`writeStaged(only:)` 的名单）—— 三环参数全名集合。
  static final Set<String> writeNames = resetNames.toSet();

  /// 待写入计数：三环暂存项数。
  int get _pending => state.staged.keys.where(writeNames.contains).length;

  /// 暂存改动 → 确认丢弃 → 重读全部设备参数。
  Future<void> _onRead(BuildContext context) async {
    if (state.staged.isNotEmpty) {
      final confirmed = await showDialog<bool>(
        context: context,
        builder: (dialogContext) => AlertDialog(
          title: const Text('读取 PID 参数'),
          content: const Text('暂存改动将被丢弃，继续？'),
          actions: <Widget>[
            GuiButton(
              label: '取消',
              onPressed: () => Navigator.pop(dialogContext, false),
            ),
            GuiButton(
              label: '继续',
              variant: GuiButtonVariant.primary,
              onPressed: () => Navigator.pop(dialogContext, true),
            ),
          ],
        ),
      );
      if (confirmed != true || !context.mounted) return;
    }
    final overlay = Overlay.maybeOf(context);
    final hadStaged = state.staged.isNotEmpty;
    await state.readAll();
    if (overlay == null) return;
    showGuiToastIn(
      overlay,
      hadStaged ? '已读取设备参数，暂存改动已丢弃' : '已读取设备参数',
      tone: ToastTone.info,
    );
  }

  /// 下发**本面板**（三环）暂存并 commit，随后 toast 结果。
  Future<void> _onWrite(BuildContext context) async {
    final names = writeNames;
    final overlay = Overlay.maybeOf(context);
    final count = state.staged.keys.where(names.contains).length;
    final ok = await state.writeStaged(only: names);
    if (overlay == null) return;
    showGuiToastIn(
      overlay,
      ok ? '已写入 $count 项' : '写入失败：${faultText(state)}',
      tone: ok ? ToastTone.success : ToastTone.error,
    );
  }

  /// 按三环名单恢复出厂参数（设备持久化），随后重读基线。
  Future<void> _onResetFactory(BuildContext context) async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (dialogContext) => AlertDialog(
        title: const Text('恢复出厂参数'),
        content: const Text('将三环 PID 参数恢复出厂值并写入设备。'),
        actions: <Widget>[
          GuiButton(
            label: '取消',
            onPressed: () => Navigator.pop(dialogContext, false),
          ),
          GuiButton(
            label: '确认恢复',
            variant: GuiButtonVariant.primary,
            onPressed: () => Navigator.pop(dialogContext, true),
          ),
        ],
      ),
    );
    if (confirmed != true || !context.mounted) return;
    final overlay = Overlay.maybeOf(context);
    final ok = await state.resetFactory(resetNames);
    if (overlay == null) return;
    showGuiToastIn(
      overlay,
      ok ? '已恢复三环出厂参数' : '恢复失败：${faultText(state)}',
      tone: ok ? ToastTone.success : ToastTone.error,
    );
  }

  @override
  Widget build(BuildContext context) {
    // 监听：参数值与脏标记随时变化，需随 state 通知重建。
    return ListenableBuilder(
      listenable: state,
      builder: (context, _) => PanelBox(
        frameKey: const Key('pid_panel_frame'),
        title: 'PID 参数',
        en: 'PID',
        hue: TitleHue.green,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            _buttonRow(context),
            const SizedBox(height: 12),
            for (final (
                  sectionKey,
                  title,
                  en,
                  fields,
                  note,
                )
                in kPidSections) ...<Widget>[
              _PidSection(
                sectionKey: Key(sectionKey),
                dividerKey: Key('${sectionKey.replaceFirst('_section', '')}_divider'),
                title: title,
                en: en,
                fields: fields,
                state: state,
                note: note == null ? null : (note.$1, Key(note.$2)),
              ),
              if (sectionKey != kPidSections.last.$1)
                const SizedBox(height: 12),
            ],
          ],
        ),
      ),
    );
  }

  /// 共用按钮行：读取 | 写入 | 恢复出厂，行尾为待写入计数徽标。
  Widget _buttonRow(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final pending = _pending;
    return Row(
      mainAxisAlignment: MainAxisAlignment.end,
      children: <Widget>[
        GuiButton(
          label: '读取',
          icon: Icons.refresh,
          onPressed: () => _onRead(context),
        ),
        const SizedBox(width: 8),
        GuiButton(
          label: '写入',
          variant: GuiButtonVariant.primary,
          icon: Icons.save,
          onPressed: pending > 0 ? () => _onWrite(context) : null,
        ),
        const SizedBox(width: 8),
        GuiButton(
          label: '恢复出厂',
          icon: Icons.settings_backup_restore,
          onPressed: () => _onResetFactory(context),
        ),
        if (pending > 0) ...<Widget>[
          const SizedBox(width: 8),
          Container(
            key: const Key('pid_pending_badge'),
            padding: const EdgeInsets.symmetric(horizontal: 7, vertical: 2),
            decoration: BoxDecoration(
              border: Border.all(
                color: palette.stPending.withValues(alpha: 0.7),
              ),
            ),
            child: Text(
              '$pending',
              style: scale.label.copyWith(color: palette.stPending),
            ),
          ),
        ],
      ],
    );
  }
}

/// 一个环的子节：小号青色副标题 + [GuiPalette.lineDim] 1px 分割线 + 参数位行，
/// 末尾可附一条诚实标注（如电流环的「FOC 预留」）。
class _PidSection extends StatelessWidget {
  const _PidSection({
    required this.sectionKey,
    required this.dividerKey,
    required this.title,
    required this.en,
    required this.fields,
    required this.state,
    this.note,
  });

  /// 子节整体（副标题 + 分割线 + 参数位）的键。
  final Key sectionKey;

  /// 分割线的键。
  final Key dividerKey;

  /// 中文副标题。
  final String title;

  /// 英文副标。
  final String en;

  /// 本节的参数位。
  final List<(String, String)> fields;

  final AppState state;

  /// 可选诚实标注 `(文本, 键)`。
  final (String, Key)? note;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return Column(
      key: sectionKey,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        Row(
          children: <Widget>[
            Text(title, style: scale.label.copyWith(color: palette.accent)),
            const SizedBox(width: 8),
            Text(en.toUpperCase(), style: scale.label),
          ],
        ),
        const SizedBox(height: 4),
        Container(
          key: dividerKey,
          height: palette.lineWidth,
          color: palette.lineDim,
        ),
        const SizedBox(height: 8),
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
        if (note != null) ...<Widget>[
          const SizedBox(height: 6),
          Text(
            note!.$1,
            key: note!.$2,
            style: scale.label.copyWith(color: palette.stWarn),
          ),
        ],
      ],
    );
  }
}
