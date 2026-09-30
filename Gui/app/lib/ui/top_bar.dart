import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_theme.dart';
import '../widgets/device_ascii.dart';
import '../widgets/stale_gate.dart';
import '../widgets/status_badge.dart';

/// Formats a session duration as `HH:MM:SS` (hours may exceed two digits).
String formatUptime(Duration d) {
  String two(int v) => v.toString().padLeft(2, '0');
  return '${two(d.inHours)}:${two(d.inMinutes % 60)}:${two(d.inSeconds % 60)}';
}

/// Persistent top bar for the single-page layout (design-system §4.2/§4.3).
///
/// Left: the connection tab, which expands a detail panel below the whole bar
/// (never over the device identity). Middle: the device identity read from the
/// `PROTO_INFO` handshake, or the `DEVICE ID PENDING` placeholder. Right: the
/// poll/uptime/SEQ/frame telemetry, honestly showing `—` for anything the
/// protocol does not provide.
class TopBar extends StatefulWidget {
  const TopBar({super.key, required this.state});

  final AppState state;

  @override
  State<TopBar> createState() => _TopBarState();
}

class _TopBarState extends State<TopBar> {
  final TextEditingController _addrCtrl = TextEditingController(
    text: '127.0.0.1:9000',
  );
  bool _expanded = false;
  bool _busy = false;

  @override
  void dispose() {
    _addrCtrl.dispose();
    super.dispose();
  }

  Future<void> _doConnect() async {
    final messenger = ScaffoldMessenger.of(context);
    setState(() => _busy = true);
    final ok = await widget.state.connect(_addrCtrl.text.trim());
    if (!mounted) return;
    setState(() => _busy = false);
    messenger.showSnackBar(
      SnackBar(
        content: Text(
          ok ? '已连接 ${widget.state.deviceName}' : '连接失败：${widget.state.lastError}',
        ),
      ),
    );
  }

  Future<void> _doDisconnect() async {
    final messenger = ScaffoldMessenger.of(context);
    setState(() => _busy = true);
    await widget.state.disconnect();
    if (!mounted) return;
    setState(() => _busy = false);
    messenger.showSnackBar(const SnackBar(content: Text('已断开连接')));
  }

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    return Container(
      color: palette.bgPanel,
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          _bar(context),
          if (_expanded) _panel(context),
        ],
      ),
    );
  }

  // ── top bar ─────────────────────────────────────────────────────────────

  Widget _bar(BuildContext context) {
    final palette = GuiPalette.of(context);
    return Container(
      padding: const EdgeInsets.fromLTRB(16, 12, 16, 12),
      decoration: BoxDecoration(
        border: Border(
          bottom: BorderSide(color: palette.lineHi, width: palette.lineWidth),
        ),
      ),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          _connectTab(context),
          const SizedBox(width: 20),
          Expanded(child: _identity(context)),
          const SizedBox(width: 20),
          _telemetry(context),
        ],
      ),
    );
  }

  Widget _connectTab(BuildContext context) {
    final s = widget.state;
    final palette = GuiPalette.of(context);
    return InkWell(
      key: const Key('top_bar_connect_tab'),
      onTap: () => setState(() => _expanded = !_expanded),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
        decoration: BoxDecoration(
          border: Border.all(
            color: _expanded ? palette.lineHi : palette.line,
            width: palette.lineWidth,
          ),
        ),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            Text(
              _expanded ? '▾' : '▸',
              style: GuiTypeScale.of(context).label,
            ),
            const SizedBox(width: 6),
            StatusBadge(
              state: s.connected ? ItemState.normal : ItemState.stale,
              label: s.connected ? '连接 · TCP' : '连接 · 未建立',
            ),
          ],
        ),
      ),
    );
  }

  Widget _identity(BuildContext context) {
    final s = widget.state;
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final connected = s.connected && s.deviceName.isNotEmpty;
    if (!connected) {
      return Row(
        children: <Widget>[
          const DeviceAscii(connected: false),
          const SizedBox(width: 16),
          Expanded(
            child: Text(
              '设备型号由 PROTO_INFO 握手取得，连接前不作假定；展开左侧选项卡可选链路与地址。',
              style: scale.body.copyWith(color: palette.fgMute),
            ),
          ),
        ],
      );
    }
    return FittedBox(
      fit: BoxFit.scaleDown,
      alignment: Alignment.centerLeft,
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          DeviceAscii(connected: true, modelName: s.deviceName),
          const SizedBox(width: 24),
          Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              Text(
                'MCU HPM53M1 · PROTO v${s.protoMajor}.${s.protoMinor}',
                style: scale.label.copyWith(color: palette.fgDim),
              ),
              const SizedBox(height: 6),
              Text(
                'FW v0.1.0 · SERIAL 未提供',
                style: scale.label.copyWith(color: palette.fgMute),
              ),
            ],
          ),
        ],
      ),
    );
  }

  Widget _telemetry(BuildContext context) {
    final s = widget.state;
    final connected = s.connected;
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.end,
      children: <Widget>[
        _telemetryRow(
          context,
          'POLL',
          connected ? '500 ms' : null,
          const Key('top_bar_poll_value'),
        ),
        const SizedBox(height: 4),
        _telemetryRow(
          context,
          'UPTIME',
          connected && s.sessionUptime != null
              ? formatUptime(s.sessionUptime!)
              : null,
          const Key('top_bar_uptime_value'),
        ),
        const SizedBox(height: 4),
        _telemetryRow(
          context,
          'SEQ',
          null,
          const Key('top_bar_seq_value'),
        ),
        const SizedBox(height: 4),
        _telemetryRow(
          context,
          '帧',
          connected ? '${s.attempts}' : null,
          const Key('top_bar_frames_value'),
        ),
      ],
    );
  }

  Widget _telemetryRow(BuildContext context, String label, String? value, Key key) {
    final scale = GuiTypeScale.of(context);
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: <Widget>[
        Text(label, style: scale.label),
        const SizedBox(width: 10),
        StaleGate(
          key: key,
          freshness: value == null ? Freshness.unknown : Freshness.live,
          live: Text(
            value ?? '',
            style: GuiGlowStyles.soft(scale.data.copyWith(fontSize: 13)),
          ),
        ),
      ],
    );
  }

  // ── expanded connection panel ────────────────────────────────────────────

  Widget _panel(BuildContext context) {
    final palette = GuiPalette.of(context);
    return Container(
      width: double.infinity,
      padding: const EdgeInsets.fromLTRB(16, 14, 16, 16),
      decoration: BoxDecoration(
        color: palette.bgBase,
        border: Border(
          bottom: BorderSide(color: palette.line, width: palette.lineWidth),
        ),
      ),
      child: Wrap(
        spacing: 40,
        runSpacing: 18,
        crossAxisAlignment: WrapCrossAlignment.start,
        children: <Widget>[
          _transportSection(context),
          _addressSection(context),
          _handshakeSection(context),
          _linkQualitySection(context),
        ],
      ),
    );
  }

  Widget _sectionHeading(BuildContext context, String zh, String en) {
    final scale = GuiTypeScale.of(context);
    final palette = GuiPalette.of(context);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        Text(zh, style: scale.body.copyWith(color: palette.fgDim)),
        const SizedBox(height: 2),
        Text(
          en.toUpperCase(),
          style: scale.label.copyWith(color: palette.accent),
        ),
      ],
    );
  }

  Widget _transportSection(BuildContext context) {
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        _sectionHeading(context, '链路选择', 'transport'),
        const SizedBox(height: 10),
        SegmentedButton<String>(
          segments: const <ButtonSegment<String>>[
            ButtonSegment(value: 'tcp', label: Text('TCP / 无线')),
            ButtonSegment(
              value: 'usb',
              label: Text('USB BULK'),
              enabled: false,
            ),
            ButtonSegment(
              value: 'serial',
              label: Text('UART · CDC'),
              enabled: false,
            ),
          ],
          selected: const <String>{'tcp'},
          onSelectionChanged: (_) {},
        ),
      ],
    );
  }

  Widget _addressSection(BuildContext context) {
    final s = widget.state;
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        _sectionHeading(context, '地址', 'address'),
        const SizedBox(height: 10),
        Row(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            SizedBox(
              width: 220,
              child: TextField(
                controller: _addrCtrl,
                decoration: const InputDecoration(
                  labelText: '地址',
                  hintText: '127.0.0.1:9000',
                  isDense: true,
                  border: OutlineInputBorder(),
                ),
              ),
            ),
            const SizedBox(width: 10),
            FilledButton.icon(
              onPressed: _busy ? null : _doConnect,
              icon: const Icon(Icons.link, size: 16),
              label: const Text('连接'),
            ),
            const SizedBox(width: 8),
            OutlinedButton.icon(
              onPressed: (_busy || !s.connected) ? null : _doDisconnect,
              icon: const Icon(Icons.link_off, size: 16),
              label: const Text('断开'),
            ),
          ],
        ),
      ],
    );
  }

  Widget _handshakeSection(BuildContext context) {
    final s = widget.state;
    final connected = s.connected;
    final caps = '0x${s.capabilities.toRadixString(16).padLeft(2, '0')}';
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        _sectionHeading(context, '握手与能力', 'handshake'),
        const SizedBox(height: 10),
        _kvRow(
          context,
          'PROTOCOL',
          connected ? 'v${s.protoMajor}.${s.protoMinor}' : '—',
        ),
        _kvRow(context, 'CAPABILITIES', connected ? caps : '—'),
        _kvRow(context, 'PARAM META', '${s.metas.length} / ${s.metas.length}'),
      ],
    );
  }

  Widget _linkQualitySection(BuildContext context) {
    final s = widget.state;
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        _sectionHeading(context, '链路质量', 'link quality'),
        const SizedBox(height: 10),
        _kvRow(
          context,
          'RTT',
          s.rttMs == null ? '—' : '${s.rttMs!.toStringAsFixed(1)} ms',
        ),
        _kvRow(context, 'CRC · 丢包', '${s.crcErrors}'),
        _kvRow(context, '重连次数', '${s.reconnects}'),
      ],
    );
  }

  Widget _kvRow(BuildContext context, String label, String value) {
    final scale = GuiTypeScale.of(context);
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 2),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: <Widget>[
          SizedBox(width: 110, child: Text(label, style: scale.label)),
          const SizedBox(width: 8),
          Text(
            value,
            style: GuiGlowStyles.soft(scale.data.copyWith(fontSize: 12)),
          ),
        ],
      ),
    );
  }
}
