import 'package:flutter/material.dart';
import 'package:window_manager/window_manager.dart';

import '../state/app_state.dart';
import '../theme/gui_theme.dart';
import '../widgets/device_ascii.dart';
import '../widgets/gui_button.dart';
import '../widgets/hairline_frame.dart';
import '../widgets/stable_digits.dart';
import '../widgets/stale_gate.dart';
import '../widgets/status_badge.dart';
import '../widgets/window_caption.dart';

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
/// poll/uptime/SEQ/frame telemetry, then the window caption cluster.
///
/// The middle identity area doubles as the custom title bar: dragging it moves
/// the window and double-clicking it toggles maximize (native chrome is hidden
/// in `main.dart`). The window callbacks are injectable so the bar is testable
/// without a native window; when absent they fall back to `window_manager`.
class TopBar extends StatefulWidget {
  const TopBar({
    super.key,
    required this.state,
    this.onMinimize,
    this.onToggleMaximize,
    this.onClose,
    this.onDragStart,
  });

  final AppState state;

  /// Overrides the minimize button; defaults to `windowManager.minimize()`.
  final VoidCallback? onMinimize;

  /// Overrides the maximize/restore button and the blank-area double-click;
  /// defaults to toggling `windowManager`.
  final VoidCallback? onToggleMaximize;

  /// Overrides the close button; defaults to `windowManager.close()`.
  final VoidCallback? onClose;

  /// Overrides blank-area dragging; defaults to `windowManager.startDragging()`.
  final VoidCallback? onDragStart;

  @override
  State<TopBar> createState() => _TopBarState();
}

class _TopBarState extends State<TopBar> with WindowListener {
  final TextEditingController _addrCtrl = TextEditingController(
    text: '127.0.0.1:9000',
  );
  bool _expanded = false;
  bool _busy = false;
  bool _maximized = false;
  bool _connectTabHovered = false;

  @override
  void initState() {
    super.initState();
    windowManager.addListener(this);
    _syncMaximizedState();
  }

  @override
  void dispose() {
    windowManager.removeListener(this);
    _addrCtrl.dispose();
    super.dispose();
  }

  @override
  void onWindowMaximize() {
    if (mounted) setState(() => _maximized = true);
  }

  @override
  void onWindowUnmaximize() {
    if (mounted) setState(() => _maximized = false);
  }

  /// Seeds [_maximized] from the native window at startup. Any failure (widget
  /// tests with no platform channel, unsupported platform) leaves the button in
  /// its restored shape instead of surfacing an error.
  Future<void> _syncMaximizedState() async {
    try {
      final maximized = await windowManager.isMaximized();
      if (mounted && maximized != _maximized) {
        setState(() => _maximized = maximized);
      }
    } catch (_) {
      // No native window; stay restored.
    }
  }

  void _minimize() {
    final injected = widget.onMinimize;
    if (injected != null) {
      injected();
      return;
    }
    windowManager.minimize();
  }

  void _toggleMaximize() {
    final injected = widget.onToggleMaximize;
    if (injected != null) {
      injected();
      return;
    }
    _toggleMaximizeWindow();
  }

  Future<void> _toggleMaximizeWindow() async {
    if (await windowManager.isMaximized()) {
      await windowManager.unmaximize();
    } else {
      await windowManager.maximize();
    }
  }

  void _close() {
    final injected = widget.onClose;
    if (injected != null) {
      injected();
      return;
    }
    windowManager.close();
  }

  void _startDrag() {
    final injected = widget.onDragStart;
    if (injected != null) {
      injected();
      return;
    }
    windowManager.startDragging();
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
    // v3 fills the screen with bright hairline panels: the whole top bar is one
    // `HairlineFrame(main)` (bright cyan 1.5px) instead of a bare strip.
    return HairlineFrame(
      child: Container(
        color: palette.bgPanel,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            _bar(context),
            if (_expanded) _panel(context),
          ],
        ),
      ),
    );
  }

  // ── top bar ─────────────────────────────────────────────────────────────

  Widget _bar(BuildContext context) {
    final palette = GuiPalette.of(context);
    return Container(
      padding: const EdgeInsets.fromLTRB(16, 12, 16, 12),
      decoration: BoxDecoration(
        // The surrounding frame already draws the bright outer edge; only add
        // an inner seam when the details panel is expanded below the bar.
        border: _expanded
            ? Border(
                bottom: BorderSide(
                  color: palette.line,
                  width: palette.lineWidth,
                ),
              )
            : null,
      ),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          _connectTab(context),
          const SizedBox(width: 20),
          Expanded(child: _dragRegion(_identity(context))),
          const SizedBox(width: 20),
          _telemetry(context),
          const SizedBox(width: 12),
          WindowCaptionBar(
            maximized: _maximized,
            onMinimize: _minimize,
            onToggleMaximize: _toggleMaximize,
            onClose: _close,
          ),
        ],
      ),
    );
  }

  /// The blank middle of the bar acts as the custom title bar: drag to move the
  /// window, double-click to toggle maximize (design-system §4.2 keeps the
  /// connect tab at the far left and the identity beside it; the empty space
  /// around the identity is the only free area).
  Widget _dragRegion(Widget child) {
    return GestureDetector(
      key: const Key('top_bar_drag_region'),
      behavior: HitTestBehavior.opaque,
      onPanStart: (_) => _startDrag(),
      onDoubleTap: _toggleMaximize,
      child: child,
    );
  }

  Widget _connectTab(BuildContext context) {
    final s = widget.state;
    final palette = GuiPalette.of(context);
    final highlighted = _expanded || _connectTabHovered;
    return MouseRegion(
      cursor: SystemMouseCursors.click,
      onEnter: (_) => setState(() => _connectTabHovered = true),
      onExit: (_) => setState(() => _connectTabHovered = false),
      child: InkWell(
        key: const Key('top_bar_connect_tab'),
        onTap: () => setState(() => _expanded = !_expanded),
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
          decoration: BoxDecoration(
            // v3 button vocabulary: idle hairline, bright cyan on hover/expand
            // with one very faint same-hue glow.
            border: Border.all(
              color: highlighted ? palette.lineHi : palette.line,
              width: palette.lineWidth,
            ),
            boxShadow: highlighted
                ? <BoxShadow>[
                    BoxShadow(
                      color: palette.glowSoft,
                      blurRadius: GuiGlowStyles.softBlur,
                    ),
                  ]
                : const <BoxShadow>[],
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
          const DeviceAscii(connected: true),
          const SizedBox(width: 24),
          Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              Text(
                s.deviceName,
                style: scale.sectionTitle.copyWith(fontSize: 14),
              ),
              const SizedBox(height: 6),
              Text(
                'MCU HPM53M1 · PROTO v${s.protoMajor}.${s.protoMinor}',
                style: scale.label.copyWith(color: palette.fgDim),
              ),
              const SizedBox(height: 6),
              Text(
                'FW — · SERIAL 未提供',
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
    final palette = GuiPalette.of(context);
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: <Widget>[
        Text(label, style: scale.label),
        const SizedBox(width: 10),
        StaleGate(
          key: key,
          freshness: value == null ? Freshness.unknown : Freshness.live,
          // Telemetry is a monitor value: sample it at 5 Hz. Telemetry is quiet
          // second-tier text (v3).
          live: StableDigits(
            text: value ?? '',
            style: scale.data.copyWith(fontSize: 13, color: palette.fgDim),
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
            GuiButton(
              label: '连接',
              variant: GuiButtonVariant.primary,
              icon: Icons.link,
              onPressed: _busy ? null : _doConnect,
            ),
            const SizedBox(width: 8),
            GuiButton(
              label: '断开',
              icon: Icons.link_off,
              onPressed: (_busy || !s.connected) ? null : _doDisconnect,
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
        _kvRow(context, 'PARAM META', '${s.metas.length} 项'),
      ],
    );
  }

  Widget _linkQualitySection(BuildContext context) {
    final s = widget.state;
    final connected = s.connected;
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
        _kvRow(context, 'CRC · 丢包', connected ? '${s.crcErrors}' : '—'),
        _kvRow(context, '重连次数', connected ? '${s.reconnects}' : '—'),
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
          StableDigits(
            text: value,
            style: GuiGlowStyles.soft(scale.data.copyWith(fontSize: 12)),
          ),
        ],
      ),
    );
  }
}
