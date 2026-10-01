import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/glow_value.dart';
import '../widgets/gui_button.dart';
import '../widgets/section_title.dart';
import 'panel_title.dart';

/// Semantic label for a single fault bit, extracted read-only from
/// `App/Control/Inc/app_fault.h` (bit0..bit9). Unknown bits fall back to
/// `bit N (0x…)` so an undecoded condition is still shown honestly.
String faultBitLabel(int bit) => switch (bit) {
  0 => '过流 L1 · U 相',
  1 => '过流 L1 · V 相',
  2 => '过流 L1 · W 相',
  3 => '过流 L2 · U 相',
  4 => '过流 L2 · V 相',
  5 => '过流 L2 · W 相',
  6 => '母线过压',
  7 => '母线欠压',
  8 => 'ADC 采集超时',
  9 => '编码器读取错误',
  _ =>
    'bit $bit (0x${(1 << bit).toRadixString(16).toUpperCase().padLeft(8, '0')})',
};

/// Maps a device error code to its user-facing interlock message (migrated
/// verbatim from the retired dashboard page). Shared by the fault-clear action
/// here and the start / stop actions in `start_adjust_zone.dart`.
String ctrlMessage(int code) => switch (code) {
  4 => '联锁拒绝：若故障锁存请先 FAULT_CLEAR；若运行中请先停止',
  6 => '设备判定越界',
  0x100 => '请求超时',
  0x101 => '通讯错误/连接中断',
  0x102 => '连接已断开',
  _ => '操作失败（code=$code）',
};

/// Combined current + latched fault bitmask, preferring the latched set.
int _faultMask(AppState state) => state.latchedFlags != 0
    ? state.latchedFlags
    : (state.status?.faultFlags ?? 0);

/// 故障区（「监视 · 控制」页右栏第 1 段）：故障构成 + 清除故障按钮。
///
/// 从原「控制」面板瘦身而来 —— 启停按钮迁入 `start_adjust_zone.dart`，
/// FREQ / MOD 滑条与设备回读面板删除；本区只保留**故障**语义：
/// 逐位拆解当前 / 锁存故障位，并提供唯一的清除动作。
class ControlZone extends StatelessWidget {
  const ControlZone({super.key, required this.state});

  final AppState state;

  /// Clears latched faults; surfaces a classified failure as a SnackBar.
  Future<void> _clearFault(BuildContext context) async {
    final messenger = ScaffoldMessenger.of(context);
    final ok = await state.faultClear();
    if (ok) return;
    messenger.showSnackBar(
      SnackBar(content: Text(ctrlMessage(state.lastErrorCode))),
    );
  }

  @override
  Widget build(BuildContext context) {
    final scale = GuiTypeScale.of(context);
    return ListenableBuilder(
      listenable: state,
      builder: (context, _) => Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          SectionTitle(
            title: '故障',
            en: 'Fault',
            trailing: Text('故障构成 · 清除锁存', style: scale.label),
          ),
          const SizedBox(height: 14),
          PanelBox(
            frameKey: const Key('fault_frame'),
            // `_faultMask` prefers the latched set; label it accordingly so a
            // latched bit is never captioned `CURRENT`.
            title: '故障构成与指令',
            en: state.latchedFlags != 0 ? 'LATCHED' : 'CURRENT',
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: <Widget>[
                FaultComposition(
                  flags: _faultMask(state),
                  latched: state.latchedFlags != 0,
                ),
                const SizedBox(height: 12),
                Align(
                  alignment: Alignment.centerLeft,
                  child: GuiButton(
                    label: '清除故障',
                    variant: GuiButtonVariant.danger,
                    onPressed: () => _clearFault(context),
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

/// Decodes a fault bitmask into per-bit entries (color + icon + text).
///
/// Bits follow `app_fault.h`; a bit with no known meaning is still listed as
/// `bit N (0x…)` rather than being silently dropped.
class FaultComposition extends StatelessWidget {
  const FaultComposition({super.key, required this.flags, this.latched = false});

  /// Fault bitmask to decompose.
  final int flags;

  /// Whether [flags] is the latched set (drives the right-hand caption).
  final bool latched;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final bits = <int>[
      for (var i = 0; i < 32; i++)
        if ((flags & (1 << i)) != 0) i,
    ];
    // v3 tone mapping: fault entries are flat semantic red; a latched set uses
    // the strengthened latched hue so the two are never confused.
    final color = latched
        ? palette.stLatched
        : GlowValue.colorFor(palette, Tone.red);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        if (bits.isEmpty)
          Text('无故障锁存', style: scale.body.copyWith(color: palette.fgMute))
        else
          for (final bit in bits)
            Padding(
              padding: const EdgeInsets.only(bottom: 8),
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.center,
                children: <Widget>[
                  Text('■', style: scale.label.copyWith(color: color)),
                  const SizedBox(width: 8),
                  Expanded(
                    child: Text(
                      faultBitLabel(bit),
                      style: scale.body.copyWith(color: color),
                    ),
                  ),
                ],
              ),
            ),
      ],
    );
  }
}
