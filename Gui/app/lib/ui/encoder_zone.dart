import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/encoder_schematic.dart';
import '../widgets/glow_value.dart';
import '../widgets/gui_button.dart';
import '../widgets/gui_text_field.dart';
import '../widgets/hairline_frame.dart';
import '../widgets/param_controls.dart' show faultText;
import '../widgets/section_title.dart';
import '../widgets/stable_digits.dart';
import '../widgets/stale_gate.dart';
import '../widgets/status_badge.dart';
import '../widgets/vfd_bar_painter.dart';
import 'label_style.dart';
import 'toast_overlay.dart';

/// The 编码器 (encoder) zone: a thin-line schematic beside the two encoder
/// readings, plus the v1.1 output-shaft zero-point shortcuts.
///
/// The rotor (`ENC R`) and output-shaft (`ENC O`) angles are shown with a
/// [StatusBadge] and gated by their own [Freshness] — a stale or never-read
/// angle degrades to `—` rather than passing an old reading off as current
/// (design-system §6 rule 2). The two readings are split into a rotor column
/// (left) and an output-shaft column (right); the computed reduction ratio
/// `i = rotor_ratio / output_ratio` is annotated below the gear pair. A
/// mechanical constant the protocol does not expose (resolution) stays an
/// honest `—`.
///
/// v1.1 adds the **output-shaft** zero-point shortcuts (protocol opcodes
/// `SET_OUTPUT_ZERO_NOW` / `SET_OUTPUT_ZERO_ABS`); the rotor zero point is out
/// of this panel's scope and belongs to the 电机·驱动 domain.
class EncoderZone extends StatelessWidget {
  const EncoderZone({super.key, required this.state});

  /// Observable telemetry + parameter table.
  final AppState state;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: state,
      builder: (context, _) => _build(context),
    );
  }

  /// 齿轮示意图的最大宽度 [logical px] —— 居中限宽，避免大窗口下过度拉伸。
  static const double _schematicMaxWidth = 460;

  /// 分辨率中列小字的最大宽度 [logical px]。
  static const double _resolutionMaxWidth = 260;

  /// 两个读数列之间的栏间距 [logical px]。
  static const double _columnGap = 24;

  /// 数据行标签列宽 [logical px]（「角度」「转速」）。
  static const double _labelWidth = 40;

  /// 栏标题行固定高度 [logical px] —— 保证左右两栏标题 / 数据行跨栏对齐。
  static const double _headerHeight = 24;

  /// 数据行固定高度 [logical px] —— 角度 / 转速两行跨栏同行基线。
  static const double _dataRowHeight = 32;

  Widget _build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final status = state.status;
    final Freshness rotorFreshness = status == null
        ? Freshness.unknown
        : state.freshnessOf(AppState.statusTileKey('ENC_R'));
    final Freshness outputFreshness = status == null
        ? Freshness.unknown
        : state.freshnessOf(AppState.statusTileKey('ENC_O'));

    final double? reduction = _reductionRatio();
    final String ratioLabel = reduction == null
        ? '—'
        : reduction.toStringAsFixed(2);

    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        SectionTitle(
          title: '编码器',
          en: 'ENCODERS',
          trailing: Text('KTH7823 ×2 · 转子 / 出轴', style: scale.label),
        ),
        const SizedBox(height: 14),
        HairlineFrame(
          key: const Key('encoder_frame'),
          padding: const EdgeInsets.all(14),
          // **上下排布**（动画在上、读数在下），读数区再由两列左右分列。
          //
          // 并排时读数区只分到约 40% 宽（合并页左栏里约 280px），
          // 「标签 + 中文注记 + 数值 + 状态徽标」这一行必然被截断。
          // 改为上下后读数区拿到整幅宽度，再由两列承载转子 / 出轴。
          // 动画本身也不需要横向铺满，故居中并限宽。
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: <Widget>[
              Center(
                child: ConstrainedBox(
                  constraints: const BoxConstraints(
                    maxWidth: _schematicMaxWidth,
                  ),
                  child: EncoderSchematic(
                    key: const Key('encoder_schematic'),
                    rotorDeg: rotorFreshness == Freshness.live
                        ? state.status?.encRotorDeg
                        : null,
                    outputDeg: outputFreshness == Freshness.live
                        ? state.status?.encOutputDeg
                        : null,
                    ratioLabel: ratioLabel,
                    // v1.1：指针（含辉光）与减速比数值用洋红强调。
                    pointerColor: palette.valueMagenta,
                    ratioValueColor: palette.valueMagenta,
                  ),
                ),
              ),
              const SizedBox(height: 16),
              _reductionRow(context, ratioLabel),
              const SizedBox(height: 12),
              _resolutionRow(context),
              const SizedBox(height: 16),
              _divider(context),
              const SizedBox(height: 16),
              // 下部：左右两栏 + 中央竖分割线；角度 / 转速两行跨栏同行对齐。
              IntrinsicHeight(
                child: Row(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: <Widget>[
                    Expanded(
                      child: _encoderColumn(
                        context,
                        columnKey: const Key('encoder_column_rotor'),
                        headerKey: const Key('encoder_header_rotor'),
                        title: '转子编码器',
                        freshness: rotorFreshness,
                        angleRow: _angleRow(
                          context,
                          key: const Key('encoder_row_rotor'),
                          value: state.status?.encRotorDeg,
                          freshness: rotorFreshness,
                        ),
                        rpmRow: _rpmRow(
                          context,
                          key: const Key('encoder_rpm_rotor'),
                          note: '角差分 EST',
                          value: state.rpmRotor,
                          freshness: rotorFreshness,
                        ),
                      ),
                    ),
                    const SizedBox(width: _columnGap),
                    _verticalDivider(context),
                    const SizedBox(width: _columnGap),
                    Expanded(
                      child: _encoderColumn(
                        context,
                        columnKey: const Key('encoder_column_output'),
                        headerKey: const Key('encoder_header_output'),
                        title: '出轴编码器',
                        freshness: outputFreshness,
                        angleRow: _angleRow(
                          context,
                          key: const Key('encoder_row_output'),
                          value: state.status?.encOutputDeg,
                          freshness: outputFreshness,
                        ),
                        rpmRow: _rpmRow(
                          context,
                          key: const Key('encoder_rpm_output'),
                          note: '磁编 · 角差分 EST',
                          value: state.rpmOutput,
                          freshness: outputFreshness,
                        ),
                        footer: _zeroSetRow(context),
                      ),
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ],
    );
  }

  /// `i = rotor_ratio / output_ratio`; null when either parameter is unloaded or
  /// the divisor is zero (an honest `—`, never `Inf` / `NaN`).
  double? _reductionRatio() {
    final rotor = state.rotorRatio;
    final output = state.outputRatio;
    if (rotor == null || output == null || output == 0) return null;
    final ratio = rotor / output;
    return ratio.isFinite ? ratio : null;
  }

  /// 一栏编码器读数：标题 + 状态徽标（紧邻标题右侧），下方角度 / 转速两行。
  ///
  /// 标题行与数据行均取固定高度，使左右两栏的角度、转速行落在同一基线
  /// （跨栏对齐）。右栏通过 [footer] 额外承载出轴零点按钮组。
  Widget _encoderColumn(
    BuildContext context, {
    required Key columnKey,
    required Key headerKey,
    required String title,
    required Freshness freshness,
    required Widget angleRow,
    required Widget rpmRow,
    Widget? footer,
  }) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final (ItemState item, String text) = _badgeFor(freshness);
    return KeyedSubtree(
      key: columnKey,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          SizedBox(
            height: _headerHeight,
            child: Row(
              key: headerKey,
              crossAxisAlignment: CrossAxisAlignment.center,
              children: <Widget>[
                Text(title, style: guiLabelStyle(palette, scale)),
                const SizedBox(width: 10),
                StatusBadge(state: item, label: text),
              ],
            ),
          ),
          const SizedBox(height: 10),
          angleRow,
          const SizedBox(height: 8),
          rpmRow,
          if (footer != null) ...<Widget>[
            const SizedBox(height: 12),
            footer,
          ],
        ],
      ),
    );
  }

  /// 中央竖分割线：1px 低亮青 [GuiPalette.lineDim]。
  Widget _verticalDivider(BuildContext context) {
    final palette = GuiPalette.of(context);
    return Container(
      key: const Key('encoder_column_divider'),
      width: palette.lineWidth,
      color: palette.lineDim,
    );
  }

  /// 减速比数据行（居中，位于齿轮对下方）：`i = X.XX : 1`，越界 → `i = —`。
  Widget _reductionRow(BuildContext context, String ratioLabel) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final prefixStyle = scale.label.copyWith(
      color: palette.fgDim,
      fontSize: 12,
    );
    final valueStyle = scale.label.copyWith(
      color: ratioLabel == '—' ? palette.fgMute : palette.valueMagenta,
      fontSize: 12,
    );
    final value = ratioLabel == '—' ? '—' : '$ratioLabel : 1';
    return Row(
      key: const Key('encoder_reduction_ratio'),
      mainAxisAlignment: MainAxisAlignment.center,
      children: <Widget>[
        Text(
          '减速比',
          style: scale.body.copyWith(
            fontSize: GuiTypeScale.pixelSize,
            color: palette.fgDim,
          ),
        ),
        const SizedBox(width: 8),
        Text.rich(
          key: const Key('encoder_reduction_ratio_value'),
          TextSpan(
            children: <TextSpan>[
              TextSpan(text: 'i = ', style: prefixStyle),
              TextSpan(text: value, style: valueStyle),
            ],
          ),
        ),
      ],
    );
  }

  /// 分辨率中列小字（无源项 → `—`）。
  Widget _resolutionRow(BuildContext context) {
    return Center(
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: _resolutionMaxWidth),
        child: _plainRow(
          context,
          key: const Key('encoder_resolution'),
          label: '分辨率',
          zh: 'RES',
          value: state.encoderResolutionBits == null
              ? null
              : '${state.encoderResolutionBits} bit',
        ),
      ),
    );
  }

  /// 出轴零点快捷设置（**右栏**出轴侧下方）；转子零点不在此处。
  Widget _zeroSetRow(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.end,
      children: <Widget>[
        Wrap(
          spacing: 8,
          runSpacing: 8,
          alignment: WrapAlignment.end,
          children: <Widget>[
            GuiButton(
              key: const Key('encoder_zero_set_now'),
              label: '当前角设零点',
              variant: GuiButtonVariant.primary,
              onPressed: () => _setZeroNow(context),
            ),
            GuiButton(
              key: const Key('encoder_zero_set_abs'),
              label: '指定角度设零点',
              onPressed: () => _setZeroAbs(context),
            ),
          ],
        ),
        const SizedBox(height: 6),
        Text(
          '转子零点由 电机·驱动 负责',
          style: scale.label.copyWith(color: palette.fgMute),
        ),
      ],
    );
  }

  /// 按钮 A：把当前出轴角设为零点。
  Future<void> _setZeroNow(BuildContext context) async {
    final overlay = Overlay.maybeOf(context);
    final ok = await state.setOutputZeroNow();
    if (overlay == null) return;
    showGuiToastIn(
      overlay,
      ok ? '出轴零点已设置' : faultText(state),
      tone: ok ? ToastTone.success : ToastTone.error,
    );
  }

  /// 按钮 B：输入指定角度后设为出轴零点。
  Future<void> _setZeroAbs(BuildContext context) async {
    final overlay = Overlay.maybeOf(context);
    final deg = await showDialog<double>(
      context: context,
      builder: (_) => const _AngleZeroDialog(),
    );
    if (deg == null) return;
    final ok = await state.setOutputZeroAbs(deg);
    if (overlay == null) return;
    showGuiToastIn(
      overlay,
      ok ? '出轴零点已设置' : faultText(state),
      tone: ok ? ToastTone.success : ToastTone.error,
    );
  }

  /// 角度数据行：`角度` 标签 + 1 位小数 `°`（量纲青）。固定行高，
  /// 与对栏角度行同行基线。
  Widget _angleRow(
    BuildContext context, {
    required Key key,
    required double? value,
    required Freshness freshness,
  }) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return SizedBox(
      key: key,
      height: _dataRowHeight,
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.center,
        children: <Widget>[
          SizedBox(
            width: _labelWidth,
            child: Text('角度', style: guiLabelStyle(palette, scale)),
          ),
          const Spacer(),
          StaleGate(
            freshness: freshness,
            fallback: '—',
            // Angle readout is a monitor value: sample it at 5 Hz.
            live: StableDigits(
              text: value == null ? '—' : value.toStringAsFixed(1),
              builder: (context) => GlowValue(
                value: value,
                unit: '°',
                precision: 1,
                tint: tintForUnit(palette, '°'),
              ),
            ),
          ),
        ],
      ),
    );
  }

  /// One angular-speed estimate row: a `转速` label, the derivation note, and
  /// the value in rpm (1 decimal, dimension-tinted). Gated by the same encoder
  /// [freshness] as the angle above it, so a stalled feed degrades the estimate
  /// to `—` (design-system §6 rule 2). Fixed row height keeps it on the same
  /// baseline as the opposite column's row.
  Widget _rpmRow(
    BuildContext context, {
    required Key key,
    required String note,
    required double? value,
    required Freshness freshness,
  }) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return SizedBox(
      key: key,
      height: _dataRowHeight,
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.center,
        children: <Widget>[
          SizedBox(
            width: _labelWidth,
            child: Text('转速', style: guiLabelStyle(palette, scale)),
          ),
          const SizedBox(width: 6),
          Expanded(
            child: Text(
              note,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: scale.label.copyWith(color: palette.fgDim),
            ),
          ),
          StaleGate(
            freshness: freshness,
            fallback: '—',
            // Speed estimate is a monitor value: sample it at 5 Hz.
            live: StableDigits(
              text: value == null ? '—' : value.toStringAsFixed(1),
              builder: (context) => GlowValue(
                value: value,
                precision: 1,
                tint: tintForUnit(palette, 'RPM'),
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _plainRow(
    BuildContext context, {
    required Key key,
    required String label,
    required String zh,
    String? value,
  }) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return Row(
      key: key,
      crossAxisAlignment: CrossAxisAlignment.center,
      children: <Widget>[
        SizedBox(
          width: 56,
          child: Text(label, style: guiLabelStyle(palette, scale)),
        ),
        Expanded(
          child: Text(
            zh,
            overflow: TextOverflow.ellipsis,
            style: scale.body.copyWith(
              fontSize: GuiTypeScale.pixelSize,
              color: palette.fgDim,
            ),
          ),
        ),
        // Resolution readout is a monitor value: sample at 5 Hz.
        // Unitless → the neutral white-cyan dimension tint (§2.6.3).
        StableDigits(
          text: value ?? '—',
          style: scale.label.copyWith(
            color: value == null ? palette.fgMute : tintForUnit(palette, 'bit'),
          ),
        ),
      ],
    );
  }

  Widget _divider(BuildContext context) {
    final palette = GuiPalette.of(context);
    // 行分割归一：低亮青 line（#173E44）。
    return Container(height: palette.lineWidth, color: palette.line);
  }

  /// Triple-encoded status badge for an encoder [freshness].
  (ItemState, String) _badgeFor(Freshness freshness) => switch (freshness) {
    Freshness.live => (ItemState.normal, '正常'),
    Freshness.stale => (ItemState.stale, '失效'),
    Freshness.unknown => (ItemState.pending, '未读'),
  };
}

/// 输入「指定角度」的对话框：校验 `−360..360`，合法则 `pop` 回该角度。
class _AngleZeroDialog extends StatefulWidget {
  const _AngleZeroDialog();

  @override
  State<_AngleZeroDialog> createState() => _AngleZeroDialogState();
}

class _AngleZeroDialogState extends State<_AngleZeroDialog> {
  final TextEditingController _controller = TextEditingController();
  String _error = '';

  /// 允许的零点角度闭区间上界 / 下界 [°]。
  static const double _maxDeg = 360;
  static const double _minDeg = -360;

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  void _submit() {
    final deg = double.tryParse(_controller.text.trim());
    if (deg == null) {
      setState(() => _error = '请输入有效角度');
      return;
    }
    if (deg < _minDeg || deg > _maxDeg) {
      setState(() => _error = '角度须在 −360..360 之间');
      return;
    }
    Navigator.pop(context, deg);
  }

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return AlertDialog(
      title: const Text('指定角度设零点'),
      content: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          GuiTextField(
            key: const Key('encoder_zero_angle_field'),
            controller: _controller,
            autofocus: true,
            keyboardType: const TextInputType.numberWithOptions(
              decimal: true,
              signed: true,
            ),
            labelText: '角度 (°)',
          ),
          if (_error.isNotEmpty)
            Padding(
              padding: const EdgeInsets.only(top: 12),
              child: Text(
                _error,
                style: scale.label.copyWith(color: palette.stFault),
              ),
            ),
        ],
      ),
      actions: <Widget>[
        GuiButton(
          key: const Key('encoder_zero_angle_cancel'),
          label: '取消',
          onPressed: () => Navigator.pop(context),
        ),
        GuiButton(
          key: const Key('encoder_zero_angle_confirm'),
          label: '设零点',
          variant: GuiButtonVariant.primary,
          onPressed: _submit,
        ),
      ],
    );
  }
}
