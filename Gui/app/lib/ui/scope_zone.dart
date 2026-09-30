import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/glow_value.dart';
import '../widgets/gui_button.dart';
import '../widgets/hairline_frame.dart';
import '../widgets/section_title.dart';
import '../widgets/segment_bar.dart';
import '../widgets/trend_chart.dart';
import 'panel_title.dart';
import 'status_zone.dart' show HeatLoadBar;

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
/// verbatim from the retired dashboard page).
String _ctrlMessage(int code) => switch (code) {
  4 => '联锁拒绝：若故障锁存请先 FAULT_CLEAR；若运行中请先停止',
  6 => '设备判定越界',
  0x100 => '请求超时',
  0x101 => '通讯错误/连接中断',
  0x102 => '连接已断开',
  _ => '操作失败（code=$code）',
};

/// The 曲线 zone: a frame-only scope placeholder (the live waveform is a
/// later `STREAM_SUB` capability) plus the control bar and fault composition.
class ScopeZone extends StatelessWidget {
  const ScopeZone({super.key, required this.state});

  final AppState state;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: state,
      builder: (context, _) => _build(context),
    );
  }

  Widget _build(BuildContext context) {
    final scale = GuiTypeScale.of(context);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        SectionTitle(
          title: '曲线',
          en: 'Scope',
          trailing: Text(
            '一级流预留 · STREAM_SUB 返回 UNSUPPORTED',
            style: scale.label,
          ),
        ),
        const SizedBox(height: 14),
        Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Expanded(
              flex: 2,
              child: TrendChart(
                key: const Key('scope_curve_frame'),
                history: state.chartHistory,
                freshness: state.freshnessOf(AppState.statusTileKey('I_U')),
              ),
            ),
            const SizedBox(width: 34),
            Expanded(
              flex: 1,
              child: HairlineFrame(
                padding: const EdgeInsets.all(10),
                child: _HeatLoadPanel(
                  key: const Key('scope_heat_panel'),
                  state: state,
                ),
              ),
            ),
          ],
        ),
        const SizedBox(height: 26),
        Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Expanded(
              child: HairlineFrame(
                key: const Key('fault_frame'),
                padding: const EdgeInsets.all(10),
                child: FaultComposition(
                  flags: _faultMask(state),
                  // `_faultMask` prefers the latched set; label it accordingly
                  // so a latched bit is never captioned `CURRENT`.
                  latched: state.latchedFlags != 0,
                ),
              ),
            ),
            const SizedBox(width: 34),
            Expanded(
              child: HairlineFrame(
                key: const Key('control_frame'),
                padding: const EdgeInsets.all(10),
                child: ControlPanel(state: state),
              ),
            ),
          ],
        ),
      ],
    );
  }
}

/// Combined current + latched fault bitmask, preferring the latched set.
int _faultMask(AppState state) => state.latchedFlags != 0
    ? state.latchedFlags
    : (state.status?.faultFlags ?? 0);

/// Relative load against a threshold, in the right-hand column once occupied by
/// the retired 相电流占比 pie.
///
/// Six rows (NTC / U / V / W / VBUS / 模组) reuse [HeatLoadBar]: the fill and
/// the right cell take the three-band semantic tone from the ratio, and a row
/// with no threshold (or no value) stays an empty grey bar with an honest `—`
/// — never a fabricated `0 %` (design-system §6 rule 2).
class _HeatLoadPanel extends StatelessWidget {
  const _HeatLoadPanel({super.key, required this.state});

  final AppState state;

  @override
  Widget build(BuildContext context) {
    final status = state.status;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        const PanelTitle(title: '热与负载 · 相对门限', en: '%'),
        const SizedBox(height: 12),
        for (final bar in _bars(status)) ...<Widget>[
          bar,
          const SizedBox(height: 10),
        ],
      ],
    );
  }

  List<Widget> _bars(StatusDto? status) {
    return <Widget>[
      HeatLoadBar(
        name: 'NTC',
        value: status?.ntcTemp,
        threshold: state.thresholdNtc,
      ),
      HeatLoadBar(
        name: 'U 相',
        value: status?.iU,
        threshold: state.thresholdCurrent,
      ),
      HeatLoadBar(
        name: 'V 相',
        value: status?.iV,
        threshold: state.thresholdCurrent,
      ),
      HeatLoadBar(
        name: 'W 相',
        value: status?.iW,
        threshold: state.thresholdCurrent,
      ),
      HeatLoadBar(
        name: 'VBUS',
        value: status?.vbus,
        threshold: state.thresholdVbus,
      ),
      const HeatLoadBar(name: '模组', value: null, threshold: null),
    ];
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
        PanelTitle(
          title: '故障构成与指令',
          en: latched ? 'LATCHED' : 'CURRENT',
        ),
        const SizedBox(height: 12),
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

/// Motor control bar: start / stop / clear-fault buttons plus FREQ / MOD LED
/// segment sliders with their right-aligned setpoints.
///
/// Control failures surface the same classified interlock message as before
/// (see [_ctrlMessage]); the message table is preserved verbatim in the UI
/// migration.
class ControlPanel extends StatefulWidget {
  const ControlPanel({super.key, required this.state});

  final AppState state;

  @override
  State<ControlPanel> createState() => _ControlPanelState();
}

class _ControlPanelState extends State<ControlPanel> {
  late double _freq;
  late double _mod;

  @override
  void initState() {
    super.initState();
    _freq = widget.state.status?.freqHz ?? 0;
    _mod = widget.state.status?.modPct ?? 0;
  }

  /// Runs a control [op] and surfaces its classified failure as a SnackBar.
  Future<void> _run(Future<bool> Function() op) async {
    final messenger = ScaffoldMessenger.of(context);
    final ok = await op();
    if (!mounted || ok) return;
    messenger.showSnackBar(
      SnackBar(content: Text(_ctrlMessage(widget.state.lastErrorCode))),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        Wrap(
          spacing: 8,
          runSpacing: 8,
          children: <Widget>[
            GuiButton(
              label: '启动',
              variant: GuiButtonVariant.primary,
              onPressed: () => _run(widget.state.ctrlStart),
            ),
            GuiButton(
              label: '停止',
              onPressed: () => _run(widget.state.ctrlStop),
            ),
            GuiButton(
              label: '清除故障',
              variant: GuiButtonVariant.danger,
              onPressed: () => _run(widget.state.faultClear),
            ),
          ],
        ),
        const SizedBox(height: 18),
        _ledRow(
          label: 'FREQ',
          value: _freq,
          max: 1000,
          precision: 2,
          unit: 'Hz',
          onChanged: (v) => setState(() => _freq = v),
          onSend: () => _run(() => widget.state.ctrlSetFreq(_freq)),
        ),
        const SizedBox(height: 12),
        _ledRow(
          label: 'MOD',
          value: _mod,
          max: 100,
          precision: 1,
          unit: '%',
          onChanged: (v) => setState(() => _mod = v),
          onSend: () => _run(() => widget.state.ctrlSetMod(_mod)),
        ),
      ],
    );
  }

  Widget _ledRow({
    required String label,
    required double value,
    required double max,
    required int precision,
    required String unit,
    required ValueChanged<double> onChanged,
    required VoidCallback onSend,
  }) {
    final scale = GuiTypeScale.of(context);
    final value01 = (value / max).clamp(0.0, 1.0).toDouble();
    return Row(
      children: <Widget>[
        SizedBox(width: 42, child: Text(label, style: scale.label)),
        Expanded(
          child: LayoutBuilder(
            builder: (context, constraints) {
              final width = constraints.maxWidth;
              void setFrom(double dx) {
                final next = (dx / width * max).clamp(0.0, max).toDouble();
                onChanged(next);
              }

              return GestureDetector(
                behavior: HitTestBehavior.opaque,
                onTapDown: (details) => setFrom(details.localPosition.dx),
                onHorizontalDragUpdate: (details) =>
                    setFrom(details.localPosition.dx),
                onHorizontalDragEnd: (_) => onSend(),
                child: SegmentBar(value01: value01, warn: value01 > 0.8),
              );
            },
          ),
        ),
        const SizedBox(width: 12),
        SizedBox(
          width: 70,
          child: FittedBox(
            fit: BoxFit.scaleDown,
            alignment: Alignment.centerRight,
            // v3 tone mapping: the setpoint readout is amber while the LED bar
            // itself keeps the cyan accent. `glow: true` stacks the single soft
            // layer the design reserves for bar / value readouts.
            child: GlowValue(
              value: value,
              unit: unit,
              precision: precision,
              tone: Tone.amber,
              glow: true,
            ),
          ),
        ),
      ],
    );
  }
}
