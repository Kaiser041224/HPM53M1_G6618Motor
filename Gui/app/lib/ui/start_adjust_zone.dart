/// 启动 / 调整 —— 启停切换 + 快捷控制模式 + 设定值下发。
///
/// - 启停：**单键切换**（停机态「▶ 启动」/ 运行态「■ 停止」），三重编码：
///   符号（▶/■）+ 文字（启动/停止）+ 颜色（停止=灰青 / 运行=翠绿）。
/// - 控制模式：三枚模式按钮（力矩 / 速度 / 角度），协议 v1.1 `CTRL_CMD SET_MODE`。
///   当前模式**无设备回读**，仅按最近一次成功写入在本地记录并高亮。
/// - 设定值：协议 v1.2 `SET_TORQUE / SET_SPEED / SET_ANGLE`，单位随模式切换
///   （力矩 N·m / 速度 rpm / 角度 °）；下发前做有限值与速度量程校验。
///
/// 从 `motor_drive_zone.dart` 迁入「监视 · 控制」页（调参时需与读数同屏）。
///
/// Copyright (c) 2026 Alliance HardwareGroup
/// SPDX-License-Identifier: BSD-3-Clause
library;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../state/app_state.dart';
import '../theme/gui_glow.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/gui_button.dart';
import '../widgets/gui_text_field.dart';
import '../widgets/param_controls.dart' show faultText, numText;
import 'control_zone.dart' show ctrlMessage;
import 'panel_title.dart';
import 'toast_overlay.dart';

/// 快捷控制模式（协议 v1.1 `CTRL_CMD SET_MODE`）：(mode, 中文名, 单位)。
const List<(int, String, String)> kControlModes = <(int, String, String)>[
  (1, '力矩', 'N·m'),
  (2, '速度', 'rpm'),
  (3, '角度', '°'),
];

/// 速度设定量程上限 [rpm]（协议 v1.2：`|value| > 20000` → `RANGE_ERR`）。
const double kSpeedSetpointMaxRpm = 20000;

/// 启动 / 调整面板。
class StartAdjustZone extends StatefulWidget {
  const StartAdjustZone({super.key, required this.state});

  final AppState state;

  @override
  State<StartAdjustZone> createState() => _StartAdjustZoneState();
}

class _StartAdjustZoneState extends State<StartAdjustZone> {
  /// 设定值输入框控制器（跨模式保留）。
  final TextEditingController _controller = TextEditingController();

  /// 本地校验错误（空串 = 无错误）。
  String _error = '';

  AppState get _state => widget.state;

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  /// 当前模式对应的单位标识；模式未定时为 `—`。
  String get _unit {
    for (final (mode, _, unit) in kControlModes) {
      if (mode == _state.ctrlMode) return unit;
    }
    return '—';
  }

  /// 量程提示（随模式变化）。
  String get _hint => switch (_state.ctrlMode) {
    1 => '有限值 · 负值允许',
    2 => '|v| ≤ ${numText(kSpeedSetpointMaxRpm)}',
    3 => '有限值 · 负值允许',
    _ => '请先选择控制模式',
  };

  /// 启停切换：按当前运行态调用 [AppState.ctrlStart] / [AppState.ctrlStop]；
  /// 失败（含故障联锁 code 4）走既有 [ctrlMessage] 文案。
  Future<void> _toggleRun() async {
    final overlay = Overlay.maybeOf(context);
    final ok = _state.motorRunning
        ? await _state.ctrlStop()
        : await _state.ctrlStart();
    if (ok || overlay == null) return;
    showGuiToastIn(
      overlay,
      ctrlMessage(_state.lastErrorCode),
      tone: ToastTone.error,
    );
  }

  /// 下发控制模式；成功提示中文名，失败走既有 [faultText] 文案表
  /// （运行中切换 → LOCKED code 4；UI 已预禁用，此处为兜底）。
  Future<void> _selectMode(int mode, String label) async {
    final overlay = Overlay.maybeOf(context);
    final ok = await _state.setCtrlMode(mode);
    if (overlay == null) return;
    showGuiToastIn(
      overlay,
      ok ? '控制模式：$label' : faultText(_state),
      tone: ok ? ToastTone.success : ToastTone.error,
    );
  }

  /// 校验并下发改定值；成功提示「已下发：…」，失败走既有文案表
  /// （运行中 → LOCKED code 4 / 越界 → RANGE_ERR code 6）。
  Future<void> _sendSetpoint() async {
    final overlay = Overlay.maybeOf(context);
    final mode = _state.ctrlMode;
    final unit = _unit;
    final text = _controller.text.trim();
    final value = double.tryParse(text);

    if (mode == 0) {
      setState(() => _error = '请先选择控制模式');
      return;
    }
    if (value == null || !value.isFinite) {
      setState(() => _error = '请输入有效数值（有限值）');
      return;
    }
    if (mode == 2 && value.abs() > kSpeedSetpointMaxRpm) {
      setState(
        () => _error = '速度越界：|v| ≤ ${numText(kSpeedSetpointMaxRpm)} rpm',
      );
      return;
    }
    setState(() => _error = '');

    final ok = switch (mode) {
      1 => await _state.setTorque(value),
      2 => await _state.setSpeed(value),
      3 => await _state.setAngle(value),
      _ => false,
    };
    if (overlay == null) return;
    showGuiToastIn(
      overlay,
      ok ? '已下发：${numText(value)} $unit' : faultText(_state),
      tone: ok ? ToastTone.success : ToastTone.error,
    );
  }

  @override
  Widget build(BuildContext context) {
    // 监听：模式 / 运行态 / 启停高亮由 state 通知驱动重建。
    return ListenableBuilder(
      listenable: _state,
      builder: (context, _) {
        final palette = GuiPalette.of(context);
        final scale = GuiTypeScale.of(context);
        final mode = _state.ctrlMode;
        final modeValid = _unit != '—';
        // 运行中：模式切换与设定值下发一律禁用（协议侧本就 LOCKED，此处预禁用）。
        final running = _state.motorRunning;
        final modeButtons = Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            for (final (m, label, _) in kControlModes) ...<Widget>[
              Expanded(
                child: _ModeButton(
                  mode: m,
                  label: '$label控制',
                  selected: mode == m,
                  enabled: !running,
                  onPressed: () => _selectMode(m, label),
                ),
              ),
              if (m != kControlModes.last.$1) const SizedBox(width: 8),
            ],
          ],
        );
        return PanelBox(
          frameKey: const Key('quick_control_frame'),
          title: '启动 / 调整',
          en: 'Quick Control',
          trailing: Text(
            '协议 v1.1 / v1.2',
            key: const Key('ctrl_mode_v11_note'),
            style: scale.label.copyWith(color: palette.accent),
          ),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: <Widget>[
              // 启停合并为单键切换（三重编码：符号 + 文字 + 颜色）。
              _StartStopToggle(
                running: _state.motorRunning,
                onPressed: _toggleRun,
              ),
              const SizedBox(height: 12),
              if (running)
                Tooltip(message: '请先停止后切换', child: modeButtons)
              else
                modeButtons,
              if (running) ...<Widget>[
                const SizedBox(height: 6),
                Text(
                  '请先停止后切换',
                  key: const Key('ctrl_locked_note'),
                  style: scale.label.copyWith(color: palette.stWarn),
                ),
              ],
              const SizedBox(height: 9),
              // 控制模式设定输入：数值 + 单位标识（随模式切换）+ 下发。
              Row(
                crossAxisAlignment: CrossAxisAlignment.center,
                children: <Widget>[
                  Expanded(
                    child: _SetpointField(
                      controller: _controller,
                      enabled: modeValid && !running,
                      hint: running ? '请先停止后切换' : _hint,
                    ),
                  ),
                  const SizedBox(width: 8),
                  SizedBox(
                    width: 44,
                    child: Text(
                      _unit,
                      key: const Key('ctrl_setpoint_unit'),
                      textAlign: TextAlign.center,
                      style: scale.body.copyWith(
                        fontSize: 13,
                        color: (modeValid && !running)
                            ? palette.accent
                            : palette.fgMute,
                      ),
                    ),
                  ),
                  const SizedBox(width: 8),
                  GuiButton(
                    key: const Key('ctrl_setpoint_send'),
                    label: '下发',
                    variant: GuiButtonVariant.primary,
                    onPressed: (modeValid && !running) ? _sendSetpoint : null,
                  ),
                ],
              ),
              if (_error.isNotEmpty) ...<Widget>[
                const SizedBox(height: 6),
                Text(
                  _error,
                  key: const Key('ctrl_setpoint_error'),
                  style: scale.label.copyWith(color: palette.stFault),
                ),
              ],
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
}

/// 启停切换按钮：停机态「▶ 启动」（灰青）/ 运行态「■ 停止」（翠绿）。
///
/// 三重编码 —— 符号（▶/■）+ 文字（启动/停止）+ 颜色（灰青/翠绿），
/// 即使配色被忽略，状态仍可从符号与文字读出。
class _StartStopToggle extends StatefulWidget {
  const _StartStopToggle({required this.running, required this.onPressed});

  final bool running;
  final VoidCallback onPressed;

  @override
  State<_StartStopToggle> createState() => _StartStopToggleState();
}

class _StartStopToggleState extends State<_StartStopToggle> {
  bool _hovered = false;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    // 运行 = 翠绿（stNormal）；停止 = 灰青（fgDim）。
    final color = widget.running ? palette.stNormal : palette.fgDim;
    final symbol = widget.running ? '■' : '▶';
    final label = widget.running ? '停止' : '启动';
    return MouseRegion(
      cursor: SystemMouseCursors.click,
      onEnter: (_) => setState(() => _hovered = true),
      onExit: (_) => setState(() => _hovered = false),
      child: GestureDetector(
        onTap: widget.onPressed,
        child: Container(
          key: const Key('ctrl_start_stop_toggle'),
          height: GuiButton.height,
          alignment: Alignment.center,
          decoration: BoxDecoration(
            color: palette.btnFill,
            border: Border.all(
              color: _hovered ? palette.lineHi : color,
              width: 1,
            ),
            boxShadow: _hovered
                ? <BoxShadow>[
                    BoxShadow(
                      color: palette.glowSoft,
                      blurRadius: GuiGlowStyles.softBlur,
                    ),
                  ]
                : const <BoxShadow>[],
          ),
          child: Text(
            '$symbol $label',
            style: scale.body.copyWith(fontSize: 14, color: color),
          ),
        ),
      ),
    );
  }
}

/// 设定值输入框：等宽数字键盘、有界、随模式启用 / 禁用。
class _SetpointField extends StatelessWidget {
  const _SetpointField({
    required this.controller,
    required this.enabled,
    required this.hint,
  });

  final TextEditingController controller;
  final bool enabled;
  final String hint;

  @override
  Widget build(BuildContext context) {
    return GuiTextField(
      key: const Key('ctrl_setpoint_input'),
      controller: controller,
      enabled: enabled,
      keyboardType: const TextInputType.numberWithOptions(
        decimal: true,
        signed: true,
      ),
      inputFormatters: <TextInputFormatter>[
        FilteringTextInputFormatter.allow(RegExp(r'[0-9.\-+eE]')),
      ],
      hintText: hint,
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
    this.enabled = true,
  });

  final int mode;
  final String label;
  final bool selected;
  final VoidCallback onPressed;

  /// 运行中为 false：按钮禁用（GuiButton 禁用样式 + 工具提示 + 小字）。
  final bool enabled;

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
          onPressed: enabled ? onPressed : null,
        ),
        const SizedBox(height: 4),
        // 徽标槽位等高；容器自适内容宽度，文字不裁（含 ◆ 与「当前」）。
        SizedBox(
          height: 22,
          child: selected
              ? Center(
                  child: Container(
                    key: Key('ctrl_mode_badge_$mode'),
                    padding: const EdgeInsets.symmetric(
                      horizontal: 8,
                      vertical: 2,
                    ),
                    decoration: BoxDecoration(
                      border: Border.all(
                        color: palette.accent.withValues(alpha: 0.55),
                      ),
                    ),
                    child: Text(
                      '◆ 当前',
                      maxLines: 1,
                      softWrap: false,
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
