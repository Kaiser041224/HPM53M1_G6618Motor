import 'package:flutter/material.dart';

import '../bridge/frb/api.dart';
import '../state/app_state.dart';

/// Formats a 32-bit bitmask as a fixed-width hex literal.
String _hex(int value) =>
    '0x${value.toRadixString(16).toUpperCase().padLeft(8, '0')}';

/// Maps a device error code to its user-facing interlock message.
String _ctrlMessage(int code) => switch (code) {
  4 => '联锁拒绝：若故障锁存请先 FAULT_CLEAR；若运行中请先停止',
  6 => '设备判定越界',
  0x100 => '请求超时',
  0x102 => '连接已断开',
  _ => '操作失败（code=$code）',
};

/// Live telemetry, motor control and interlock feedback.
///
/// Left two thirds: fault banner plus the status-card grid. Right third:
/// RUN/STOP state, start/stop buttons and the frequency/modulation sliders.
class DashboardPage extends StatefulWidget {
  const DashboardPage({super.key, required this.state});

  final AppState state;

  @override
  State<DashboardPage> createState() => _DashboardPageState();
}

class _DashboardPageState extends State<DashboardPage> {
  double _freq = 0;
  double _mod = 0;
  final TextEditingController _freqCtrl = TextEditingController(text: '0');
  final TextEditingController _modCtrl = TextEditingController(text: '0');

  @override
  void dispose() {
    _freqCtrl.dispose();
    _modCtrl.dispose();
    super.dispose();
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
    return ListenableBuilder(
      listenable: widget.state,
      builder: (context, _) => _build(context, widget.state),
    );
  }

  Widget _build(BuildContext context, AppState s) {
    if (!s.connected && s.status == null) {
      return const Center(child: Text('未连接设备'));
    }
    final status = s.status;
    return Padding(
      padding: const EdgeInsets.all(16),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Expanded(flex: 2, child: _statusArea(context, s, status)),
          const SizedBox(width: 16),
          Expanded(flex: 1, child: _controlArea(context, s, status)),
        ],
      ),
    );
  }

  Widget _statusArea(BuildContext context, AppState s, StatusDto? status) {
    final showBanner =
        status != null && (status.faultFlags != 0 || s.latchedFlags != 0);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        if (showBanner) _faultBanner(context, s, status),
        Expanded(
          child: SingleChildScrollView(
            child: status == null
                ? const Padding(
                    padding: EdgeInsets.all(24),
                    child: Text('等待设备状态…'),
                  )
                : Wrap(
                    spacing: 12,
                    runSpacing: 12,
                    children: [
                      _card(context, '母线电压', status.vbus, 'V'),
                      _card(context, 'NTC', status.ntcTemp, '℃'),
                      _card(context, 'U 相电流', status.iU, 'A'),
                      _card(context, 'V 相电流', status.iV, 'A'),
                      _card(context, 'W 相电流', status.iW, 'A'),
                      _card(context, '转子角', status.encRotorDeg, '°'),
                      _card(context, '出轴角', status.encOutputDeg, '°'),
                      _card(context, '频率', status.freqHz, 'Hz'),
                      _card(context, '调制度', status.modPct, '%'),
                    ],
                  ),
          ),
        ),
      ],
    );
  }

  Widget _faultBanner(BuildContext context, AppState s, StatusDto status) {
    final scheme = Theme.of(context).colorScheme;
    return Container(
      width: double.infinity,
      margin: const EdgeInsets.only(bottom: 12),
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
      decoration: BoxDecoration(
        color: scheme.errorContainer,
        borderRadius: BorderRadius.circular(8),
      ),
      child: Row(
        children: [
          Icon(Icons.warning_amber, color: scheme.onErrorContainer),
          const SizedBox(width: 12),
          Expanded(
            child: Text(
              '故障 flags=${_hex(status.faultFlags)} '
              'latched=${_hex(s.latchedFlags)}',
              style: TextStyle(color: scheme.onErrorContainer),
            ),
          ),
          TextButton(
            onPressed: () => _run(s.faultClear),
            child: const Text('FAULT_CLEAR'),
          ),
        ],
      ),
    );
  }

  Widget _card(BuildContext context, String title, double value, String unit) {
    return SizedBox(
      width: 150,
      child: Card(
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(title, style: Theme.of(context).textTheme.bodySmall),
              const SizedBox(height: 4),
              Text(
                value.toStringAsFixed(1),
                style: Theme.of(context).textTheme.headlineSmall,
              ),
              Text(unit, style: Theme.of(context).textTheme.bodySmall),
            ],
          ),
        ),
      ),
    );
  }

  Widget _controlArea(BuildContext context, AppState s, StatusDto? status) {
    final running = status?.motorRunning ?? false;
    return SingleChildScrollView(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Card(
            child: Column(
              children: [
                ListTile(
                  title: const Text('电机状态'),
                  trailing: Text(
                    running ? 'RUN' : 'STOP',
                    style: TextStyle(
                      fontWeight: FontWeight.bold,
                      color: running ? Colors.green : null,
                    ),
                  ),
                ),
                Padding(
                  padding: const EdgeInsets.all(8),
                  child: Row(
                    children: [
                      Expanded(
                        child: FilledButton(
                          onPressed: () => _run(s.ctrlStart),
                          child: const Text('启动'),
                        ),
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: OutlinedButton(
                          onPressed: () => _run(s.ctrlStop),
                          child: const Text('停止'),
                        ),
                      ),
                    ],
                  ),
                ),
              ],
            ),
          ),
          const SizedBox(height: 12),
          _sliderField(
            context,
            label: '频率',
            unit: 'Hz',
            value: _freq,
            max: 1000,
            controller: _freqCtrl,
            onChanged: (v) {
              setState(() => _freq = v);
              _freqCtrl.text = v.toStringAsFixed(0);
            },
            onSend: () => _run(() => s.ctrlSetFreq(_freq)),
          ),
          const SizedBox(height: 12),
          _sliderField(
            context,
            label: '调制度',
            unit: '%',
            value: _mod,
            max: 100,
            controller: _modCtrl,
            onChanged: (v) {
              setState(() => _mod = v);
              _modCtrl.text = v.toStringAsFixed(0);
            },
            onSend: () => _run(() => s.ctrlSetMod(_mod)),
          ),
        ],
      ),
    );
  }

  Widget _sliderField(
    BuildContext context, {
    required String label,
    required String unit,
    required double value,
    required double max,
    required TextEditingController controller,
    required ValueChanged<double> onChanged,
    required Future<void> Function() onSend,
  }) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              '$label ($unit)',
              style: Theme.of(context).textTheme.bodyMedium,
            ),
            Row(
              children: [
                Expanded(
                  child: Slider(
                    value: value,
                    max: max,
                    divisions: max.round(),
                    label: value.toStringAsFixed(0),
                    onChanged: onChanged,
                    onChangeEnd: (_) => onSend(),
                  ),
                ),
                SizedBox(
                  width: 64,
                  child: TextField(
                    controller: controller,
                    keyboardType: TextInputType.number,
                    textAlign: TextAlign.end,
                    decoration: const InputDecoration(isDense: true),
                    onSubmitted: (_) {
                      final parsed = double.tryParse(controller.text);
                      if (parsed == null) return;
                      onChanged(parsed.clamp(0.0, max).toDouble());
                      onSend();
                    },
                  ),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}
