/// 电机 · 驱动配置页（L1 第 4 页）—— 涉及驱动板或电机的参数单列一处。
///
/// 左右分栏：
/// - 左：标定 / 测试程序启动按钮。这些例程在固件里**已经实现**
///   （`cal current` / `enc zero` / `inv` / `param reset`），但
///   **协议 `CTRL_CMD` 未暴露**（只有 START/STOP/SET_FREQ/SET_MOD），
///   故按钮一律禁用并如实标注原因 —— 不做点了没反应的假按钮。
/// - 右：参数对照表（当前值 vs 出厂预设）。当前值来自 `PARAM_READ`；
///   出厂预设当前**取不到**（协议元数据只带 `index/ty/apply/min/max/name/unit`，
///   不含默认值），故该列显示 `—` 并在表头标明。
///
/// Copyright (c) 2026 Alliance HardwareGroup
/// SPDX-License-Identifier: BSD-3-Clause
library;

import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_glow.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/hairline_frame.dart';
import '../widgets/param_controls.dart';
import '../widgets/section_title.dart';
import '../widgets/vfd_bar_painter.dart';
import 'label_style.dart';
import 'panel_title.dart';

/// 一条标定 / 测试程序的描述。
class CalibProgram {
  const CalibProgram({
    required this.title,
    required this.command,
    required this.precondition,
    this.risk = false,
  });

  /// 中文名。
  final String title;

  /// 固件终端命令（原样展示，便于对照终端使用）。
  final String command;

  /// 前置条件。
  final String precondition;

  /// 是否直接驱动功率级（风险较高）。
  final bool risk;
}

/// 固件已实现但协议未暴露的标定 / 测试程序。
const List<CalibProgram> kCalibPrograms = <CalibProgram>[
  CalibProgram(
    title: '电流零点标定',
    command: 'cal current',
    precondition: '电机停止 · 无故障锁存',
  ),
  CalibProgram(
    title: '编码器零点 · 转子',
    command: 'enc zero rotor',
    precondition: '电机停止 · 无故障锁存',
  ),
  CalibProgram(
    title: '编码器零点 · 出轴',
    command: 'enc zero output',
    precondition: '电机停止 · 无故障锁存',
  ),
  CalibProgram(
    title: '逆变器相测试',
    command: 'inv <u|v|w|all|off>',
    precondition: '直接驱动功率级 · 需接限流',
    risk: true,
  ),
  CalibProgram(
    title: '恢复出厂参数',
    command: 'param reset',
    precondition: '停机窗口 · 会覆盖全部已存参数',
  ),
];

/// 右栏一组待对照的参数。
class ConfigGroup {
  const ConfigGroup(this.title, this.params);

  final String title;

  /// (域限定全名, 中文名, 是否安全相关)
  final List<(String, String, bool)> params;
}

/// 驱动板 / 电机侧的配置参数分组（名称为设备元数据里的全名）。
const List<ConfigGroup> kConfigGroups = <ConfigGroup>[
  ConfigGroup('逆变器', <(String, String, bool)>[
    ('hardware.inverter.pwm_freq_hz', '开关频率', true),
    ('hardware.inverter.deadtime_ns', '死区时间', true),
    ('hardware.adc.sample_cycle', '采样窗口', false),
    ('hardware.adc.trigger_delay_ns', '触发延时', false),
  ]),
  ConfigGroup('电机铭牌', <(String, String, bool)>[
    ('motor.pole_pairs', '极对数', false),
    ('motor.rs_ohm', '相电阻', false),
    ('motor.ls_h', '相电感', false),
    ('motor.ke_vs_per_rad', '反电动势系数', false),
    ('motor.kt_nm_per_a', '转矩系数', false),
    ('motor.i_rated_a', '额定电流', false),
    ('motor.rpm_max', '最高转速', false),
  ]),
  ConfigGroup('电流采样标定', <(String, String, bool)>[
    ('hardware.current_sense.shunt_ohm', '采样电阻', false),
    ('hardware.current_sense.amp_gain', '运放增益', false),
    ('hardware.current_sense.a_per_volt', '电流标度（派生）', false),
    ('hardware.current_sense.bias_v', '零电流偏置', false),
  ]),
  ConfigGroup('母线采样', <(String, String, bool)>[
    ('hardware.vbus_sense.divider_high_ohm', '分压上臂', false),
    ('hardware.vbus_sense.divider_low_ohm', '分压下臂', false),
    ('hardware.vbus_sense.v_per_volt', '母线标度（派生）', false),
  ]),
  ConfigGroup('编码器配置', <(String, String, bool)>[
    ('motor.encoder.resolution_bits', '单圈分辨率', false),
    ('motor.encoder.rotor_ring_teeth', '转子外齿圈齿数', false),
    ('motor.encoder.rotor_pinion_teeth', '转子小齿轮齿数', false),
    ('motor.encoder.output_pinion_teeth', '出轴小齿轮齿数', false),
    ('motor.encoder.rotor_ratio', '转子比值（派生）', false),
    ('motor.encoder.output_ratio', '出轴比值（派生）', false),
  ]),
];

/// 电机 · 驱动配置页。
class MotorDriveZone extends StatelessWidget {
  const MotorDriveZone({super.key, required this.state});

  final AppState state;

  @override
  Widget build(BuildContext context) {
    final scale = GuiTypeScale.of(context);
    return ListenableBuilder(
      listenable: state,
      builder: (context, _) => Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          SectionTitle(
            title: '电机 · 驱动',
            en: 'Motor / Drive',
            trailing: Text('标定程序 · 驱动板与电机参数', style: scale.label),
          ),
          const SizedBox(height: 14),
          Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              Expanded(flex: 4, child: _programColumn(context)),
              const SizedBox(width: 34),
              Expanded(flex: 6, child: _configColumn(context)),
            ],
          ),
        ],
      ),
    );
  }

  /// 左栏：标定 / 测试程序。
  Widget _programColumn(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return HairlineFrame(
      key: const Key('calib_programs_frame'),
      padding: const EdgeInsets.all(10),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          PanelTitle(
            title: '标定程序',
            en: 'Calibration',
            trailing: Text('${kCalibPrograms.length} 项', style: scale.label),
          ),
          const SizedBox(height: 6),
          // 协议未暴露的总说明 —— 放在最前，避免逐条重复。
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 6),
            decoration: BoxDecoration(
              border: Border.all(color: palette.stWarn.withValues(alpha: 0.5)),
            ),
            child: Text(
              '▲ 以下例程固件已实现，但协议 CTRL_CMD 未暴露\n'
              '   （仅 START/STOP/SET_FREQ/SET_MOD），上位机暂无法触发。\n'
              '   当前可经固件 USB 终端执行。',
              key: const Key('calib_unavailable_note'),
              style: scale.label.copyWith(color: palette.stWarn),
            ),
          ),
          const SizedBox(height: 10),
          for (final p in kCalibPrograms) ...<Widget>[
            _ProgramCard(program: p),
            if (p != kCalibPrograms.last) const SizedBox(height: 8),
          ],
        ],
      ),
    );
  }

  /// 右栏：参数对照表。
  Widget _configColumn(BuildContext context) {
    final scale = GuiTypeScale.of(context);
    return HairlineFrame(
      key: const Key('config_compare_frame'),
      padding: const EdgeInsets.all(10),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          PanelTitle(
            title: '参数对照',
            en: 'Configuration',
            trailing: Text('当前值 vs 出厂预设', style: scale.label),
          ),
          const SizedBox(height: 10),
          const _CompareHeader(),
          for (final g in kConfigGroups) ...<Widget>[
            _groupHeader(context, g.title, g.params.first.$1),
            for (final (name, label, safety) in g.params)
              _CompareRow(
                state: state,
                name: name,
                label: label,
                safety: safety,
              ),
          ],
        ],
      ),
    );
  }

  /// 分组头按**参数域**着色（motor 绿 / hardware 青），与全量参数页同一套映射，
  /// 让两处的颜色语言一致。
  Widget _groupHeader(BuildContext context, String title, String anyParam) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final hue = domainColor(palette, groupKeyOf(anyParam));
    return Padding(
      padding: const EdgeInsets.only(top: 12, bottom: 4),
      child: Row(
        children: <Widget>[
          Text(
            title,
            style: scale.sectionTitle.copyWith(
              fontSize: GuiTypeScale.pixelSize,
              color: hue,
            ),
          ),
          const SizedBox(width: 8),
          // 组线归一：低亮紫 groupLine（§2.6.2）。
          Expanded(child: Container(height: 1, color: palette.groupLine)),
        ],
      ),
    );
  }
}

/// 对照表表头。出厂预设列标明「待协议暴露」。
class _CompareHeader extends StatelessWidget {
  const _CompareHeader();

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    // 数据列标签统一青色 + 弱青辉光（§2.6.3）。
    final style = guiLabelStyle(palette, scale);
    return Container(
      padding: const EdgeInsets.only(bottom: 4),
      decoration: BoxDecoration(
        border: Border(
          bottom: BorderSide(color: palette.line, width: palette.lineWidth),
        ),
      ),
      child: Row(
        children: <Widget>[
          Expanded(child: Text('参数', style: style)),
          SizedBox(
            width: 76,
            child: Text('当前值', textAlign: TextAlign.right, style: style),
          ),
          SizedBox(
            width: 72,
            child: Text(
              '出厂预设',
              key: const Key('factory_col_header'),
              textAlign: TextAlign.right,
              style: style,
            ),
          ),
          SizedBox(
            width: 96,
            child: Text('范围', textAlign: TextAlign.right, style: style),
          ),
          SizedBox(
            width: 54,
            child: Text('生效', textAlign: TextAlign.right, style: style),
          ),
        ],
      ),
    );
  }
}

/// 一行参数对照。
class _CompareRow extends StatelessWidget {
  const _CompareRow({
    required this.state,
    required this.name,
    required this.label,
    required this.safety,
  });

  final AppState state;
  final String name;
  final String label;
  final bool safety;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);

    final meta = metaOf(state, name);
    final value = state.values[name];
    final dirty = state.dirty.contains(name);
    // 数值按**量纲**着色（§2.6.3），无单位参数回落白青中性。
    final tint = tintForUnit(palette, meta?.unit ?? '');

    final currentText = value == null
        ? '—'
        : '${formatValue(value.ty, value.raw)}'
              '${meta == null || meta.unit.isEmpty ? '' : ' ${meta.unit}'}';

    return InkWell(
      onTap: meta == null ? null : () => openParamEditor(context, state, meta),
      child: Container(
        padding: const EdgeInsets.symmetric(vertical: 3),
        decoration: BoxDecoration(
          // 行分割归一：低亮青 line（#173E44）。
          border: Border(
            bottom: BorderSide(color: palette.line, width: palette.lineWidth),
          ),
        ),
        child: Row(
          children: <Widget>[
            if (safety) ...<Widget>[
              const SafetyBadge(),
              const SizedBox(width: 6),
            ],
            Expanded(
              child: Text(
                label,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: meta == null
                    ? scale.body.copyWith(
                        fontSize: GuiTypeScale.pixelSize,
                        color: palette.fgMute,
                      )
                    : GuiGlowStyles.weak(
                        scale.body.copyWith(
                          fontSize: GuiTypeScale.pixelSize,
                          color: palette.accent,
                        ),
                      ),
              ),
            ),
            SizedBox(
              width: 76,
              child: FittedBox(
                fit: BoxFit.scaleDown,
                alignment: Alignment.centerRight,
                child: Text(
                  currentText,
                  maxLines: 1,
                  style: scale.data.copyWith(
                    fontSize: 15,
                    color: value == null
                        ? palette.fgMute
                        : (dirty ? palette.stWarn : tint),
                  ),
                ),
              ),
            ),
            SizedBox(
              width: 72,
              child: Text(
                // 协议元数据不含默认值 —— 如实留空，不猜。
                '—',
                key: Key('factory_$name'),
                textAlign: TextAlign.right,
                style: scale.label.copyWith(color: palette.fgMute),
              ),
            ),
            SizedBox(
              width: 96,
              child: Text(
                meta == null ? '—' : rangeText(meta),
                textAlign: TextAlign.right,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: scale.label,
              ),
            ),
            SizedBox(
              width: 54,
              child: Text(
                meta == null ? '—' : (meta.apply == 1 ? 'REBOOT' : 'LIVE'),
                textAlign: TextAlign.right,
                style: scale.label.copyWith(
                  color: meta == null
                      ? palette.fgMute
                      : (meta.apply == 1 ? palette.accent2 : palette.accent),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

/// 一张标定程序卡片：名称 + 命令 + 前置条件，按钮禁用。
class _ProgramCard extends StatelessWidget {
  const _ProgramCard({required this.program});

  final CalibProgram program;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final accent = program.risk ? palette.stFault : palette.accent;

    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 8),
      decoration: BoxDecoration(
        color: palette.bgVoid,
        border: Border.all(color: palette.lineDim, width: palette.lineWidth),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Row(
            children: <Widget>[
              Expanded(
                child: Text(
                  program.title,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: scale.sectionTitle.copyWith(
                    fontSize: GuiTypeScale.pixelSize,
                    color: palette.fgStrong,
                  ),
                ),
              ),
              ApplyBadge(label: '未暴露', color: palette.fgMute),
            ],
          ),
          const SizedBox(height: 5),
          Text(
            program.command,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: scale.data.copyWith(fontSize: 13, color: accent),
          ),
          const SizedBox(height: 3),
          Text(
            program.precondition,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: scale.label.copyWith(
              color: program.risk ? palette.stFault : palette.fgMute,
            ),
          ),
          const SizedBox(height: 7),
          // 禁用按钮：保留外观但不接受点击，配合上方说明表达「暂不可用」。
          Opacity(
            opacity: 0.45,
            child: Container(
              width: double.infinity,
              padding: const EdgeInsets.symmetric(vertical: 5),
              decoration: BoxDecoration(
                color: palette.btnFill,
                border: Border.all(color: palette.btnBorder),
              ),
              child: Text(
                '启动',
                textAlign: TextAlign.center,
                style: scale.label.copyWith(color: palette.fgMute),
              ),
            ),
          ),
        ],
      ),
    );
  }
}
