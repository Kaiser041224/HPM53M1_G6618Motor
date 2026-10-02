import 'package:flutter/material.dart';
import 'package:window_manager/window_manager.dart';

import '../state/app_state.dart';
import '../theme/gui_theme.dart';
import '../widgets/device_ascii.dart';
import '../widgets/gui_button.dart';
import '../widgets/gui_text_field.dart';
import '../widgets/hairline_frame.dart';
import '../widgets/stable_digits.dart';
import '../widgets/stale_gate.dart';
import '../widgets/window_caption.dart';
import 'label_style.dart';
import 'toast_overlay.dart';

/// Formats a session duration as `HH:MM:SS` (hours may exceed two digits).
String formatUptime(Duration d) {
  String two(int v) => v.toString().padLeft(2, '0');
  return '${two(d.inHours)}:${two(d.inMinutes % 60)}:${two(d.inSeconds % 60)}';
}

/// Persistent top bar for the single-page layout (design-system §4.2/§4.3).
///
/// The bar carries three resident zones and nothing collapses:
///
/// - **left** — the connection block: a compact transport segment (TCP live,
///   USB / UART disabled pending M2), the address field and the connect /
///   disconnect actions ([GuiButton] vocabulary);
/// - **middle** — the device identity read from the `PROTO_INFO` handshake:
///   the block art plus a two-column key:value grid (model / MCU / PROTO / FW /
///   CAPABILITIES). The handshake and link-quality detail lives on the bottom
///   bar, so the bar never grows a second section row;
/// - **right** — the poll / uptime / SEQ / frames telemetry, then the window
///   caption cluster.
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

  /// Width [logical px] at or above which all three zones share one row.
  ///
  /// Below it the connection block drops to its own line (the identity /
  /// telemetry / caption row stays first), so the bar still fits from the
  /// 1024px window minimum up.
  static const double wideBreakpoint = 1800;

  @override
  State<TopBar> createState() => _TopBarState();
}

class _TopBarState extends State<TopBar> with WindowListener {
  final TextEditingController _addrCtrl = TextEditingController(
    text: '127.0.0.1:9000',
  );
  bool _busy = false;
  bool _maximized = false;

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
    final overlay = Overlay.maybeOf(context);
    setState(() => _busy = true);
    final ok = await widget.state.connect(_addrCtrl.text.trim());
    if (!mounted) return;
    setState(() => _busy = false);
    if (overlay == null) return;
    showGuiToastIn(
      overlay,
      ok
          ? '已连接 ${widget.state.deviceName}'
          : '连接失败：${widget.state.lastError}',
      tone: ok ? ToastTone.success : ToastTone.error,
    );
  }

  Future<void> _doDisconnect() async {
    final overlay = Overlay.maybeOf(context);
    setState(() => _busy = true);
    await widget.state.disconnect();
    if (!mounted) return;
    setState(() => _busy = false);
    if (overlay == null) return;
    showGuiToastIn(overlay, '已断开连接', tone: ToastTone.info);
  }

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    return HairlineFrame(
      child: Container(
        color: palette.bgPanel,
        padding: const EdgeInsets.fromLTRB(16, 10, 16, 10),
        child: _bar(context),
      ),
    );
  }

  // ── top bar ─────────────────────────────────────────────────────────────

  Widget _bar(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        final connection = _connectionBlock(context);
        final identity = _dragRegion(_deviceInfo(context));
        final telemetry = _telemetry(context);
        final caption = WindowCaptionBar(
          maximized: _maximized,
          onMinimize: _minimize,
          onToggleMaximize: _toggleMaximize,
          onClose: _close,
        );

        if (constraints.maxWidth >= TopBar.wideBreakpoint) {
          return Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              connection,
              const SizedBox(width: 20),
              Expanded(child: identity),
              const SizedBox(width: 16),
              telemetry,
              const SizedBox(width: 12),
              caption,
            ],
          );
        }
        // Narrow: identity / telemetry / caption first, the connection block
        // wraps onto its own line so nothing is ever clipped.
        return Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Expanded(child: identity),
                const SizedBox(width: 16),
                telemetry,
                const SizedBox(width: 12),
                caption,
              ],
            ),
            const SizedBox(height: 10),
            Align(alignment: Alignment.centerLeft, child: connection),
          ],
        );
      },
    );
  }

  /// The blank middle of the bar acts as the custom title bar: drag to move the
  /// window, double-click to toggle maximize (design-system §4.2).
  Widget _dragRegion(Widget child) {
    return GestureDetector(
      key: const Key('top_bar_drag_region'),
      behavior: HitTestBehavior.opaque,
      onPanStart: (_) => _startDrag(),
      onDoubleTap: _toggleMaximize,
      child: child,
    );
  }

  // ── left: connection block ───────────────────────────────────────────────

  Widget _connectionBlock(BuildContext context) {
    final s = widget.state;
    return Wrap(
      key: const Key('top_bar_connection_block'),
      spacing: 12,
      runSpacing: 8,
      crossAxisAlignment: WrapCrossAlignment.center,
      children: <Widget>[
        _linkSegment(context),
        SizedBox(
          width: 160,
          child: GuiTextField(
            controller: _addrCtrl,
            hintText: '127.0.0.1:9000',
          ),
        ),
        GuiButton(
          label: '连接',
          variant: GuiButtonVariant.primary,
          icon: Icons.link,
          onPressed: _busy ? null : _doConnect,
        ),
        GuiButton(
          label: '断开',
          icon: Icons.link_off,
          onPressed: (_busy || !s.connected) ? null : _doDisconnect,
        ),
      ],
    );
  }

  /// Compact transport segment. TCP is the live link; USB / UART are disabled
  /// placeholders tagged `M2` rather than silently offering dead controls.
  Widget _linkSegment(BuildContext context) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: const <Widget>[
        GuiButton(
          key: Key('top_bar_link_tcp'),
          label: 'TCP',
          variant: GuiButtonVariant.primary,
          onPressed: _noop,
        ),
        SizedBox(width: 6),
        GuiButton(
          key: Key('top_bar_link_usb'),
          label: 'USB M2',
          onPressed: null,
        ),
        SizedBox(width: 6),
        GuiButton(
          key: Key('top_bar_link_uart'),
          label: 'UART M2',
          onPressed: null,
        ),
      ],
    );
  }

  // ── middle: device identity ─────────────────────────────────────────────

  Widget _deviceInfo(BuildContext context) {
    final s = widget.state;
    final connected = s.connected && s.deviceName.isNotEmpty;
    // Scale down rather than overflow when the bar is narrower than the art
    // plus the info grid (the shell window minimum is 1024, but the app also
    // degrades gracefully on smaller test surfaces).
    return FittedBox(
      key: const Key('top_bar_device_info'),
      fit: BoxFit.scaleDown,
      alignment: Alignment.centerLeft,
      child: Row(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          DeviceAscii(connected: connected),
          const SizedBox(width: 16),
          _infoGrid(context, connected),
        ],
      ),
    );
  }

  /// Two-column key:value grid under the art. Labels stay [GuiPalette.fgDim],
  /// values [GuiPalette.fgStrong]. `FW` is honestly `—`: the protocol does not
  /// carry a firmware version.
  Widget _infoGrid(BuildContext context, bool connected) {
    final s = widget.state;
    final proto = connected ? 'v${s.protoMajor}.${s.protoMinor}' : '—';
    final caps = connected
        ? '0x${s.capabilities.toRadixString(16).padLeft(2, '0')}'
        : '—';
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        _modelEntry(context, connected ? s.deviceName : '—'),
        const SizedBox(height: 6),
        Wrap(
          spacing: 20,
          runSpacing: 4,
          children: <Widget>[
            _infoEntry(
              context,
              'MCU',
              'HPM53M1',
              valueKey: const Key('top_bar_mcu_value'),
            ),
            _infoEntry(
              context,
              'PROTO',
              proto,
              valueKey: const Key('top_bar_proto_value'),
            ),
            _infoEntry(
              context,
              'FW',
              '—',
              valueKey: const Key('top_bar_fw_value'),
            ),
            _infoEntry(
              context,
              'CAP',
              caps,
              valueKey: const Key('top_bar_cap_value'),
            ),
          ],
        ),
      ],
    );
  }

  /// The model line. The value is the one free-text field (a `PROTO_INFO`
  /// name); the enclosing [FittedBox] keeps an over-long name from forcing the
  /// bar to overflow.
  Widget _modelEntry(BuildContext context, String value) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return Row(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.baseline,
      textBaseline: TextBaseline.alphabetic,
      children: <Widget>[
        Text('MODEL', style: scale.label.copyWith(color: palette.fgDim)),
        const SizedBox(width: 6),
        Text(
          value,
          key: const Key('top_bar_model_value'),
          style: scale.sectionTitle.copyWith(color: palette.fgStrong),
        ),
      ],
    );
  }

  Widget _infoEntry(
    BuildContext context,
    String label,
    String value, {
    required Key valueKey,
  }) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return Row(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.baseline,
      textBaseline: TextBaseline.alphabetic,
      children: <Widget>[
        Text(label, style: scale.label.copyWith(color: palette.fgDim)),
        const SizedBox(width: 6),
        Text(
          value,
          key: valueKey,
          style: scale.label.copyWith(color: palette.fgStrong),
        ),
      ],
    );
  }

  // ── right: telemetry ─────────────────────────────────────────────────────

  Widget _telemetry(BuildContext context) {
    final s = widget.state;
    final connected = s.connected;
    // Compact 2x2 readout grid: four rows would make the resident bar needlessly
    // tall next to the block art.
    return Column(
      key: const Key('top_bar_telemetry'),
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.end,
      children: <Widget>[
        Row(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            _telemetryRow(
              context,
              'POLL',
              connected ? '500 ms' : null,
              const Key('top_bar_poll_value'),
            ),
            const SizedBox(width: 18),
            _telemetryRow(
              context,
              'UPTIME',
              connected && s.sessionUptime != null
                  ? formatUptime(s.sessionUptime!)
                  : null,
              const Key('top_bar_uptime_value'),
            ),
          ],
        ),
        const SizedBox(height: 4),
        Row(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            _telemetryRow(context, 'SEQ', null, const Key('top_bar_seq_value')),
            const SizedBox(width: 18),
            _telemetryRow(
              context,
              '帧',
              connected ? '${s.attempts}' : null,
              const Key('top_bar_frames_value'),
            ),
          ],
        ),
      ],
    );
  }

  Widget _telemetryRow(
    BuildContext context,
    String label,
    String? value,
    Key key,
  ) {
    final scale = GuiTypeScale.of(context);
    final palette = GuiPalette.of(context);
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: <Widget>[
        Text(label, style: guiLabelStyle(palette, scale)),
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
}

/// Static no-op used by the currently selected transport segment.
void _noop() {}
