import 'package:flutter/material.dart';

import '../state/app_state.dart';

/// Persistent bottom strip showing link state and transport counters.
class StatusBar extends StatelessWidget {
  const StatusBar({super.key, required this.state});

  final AppState state;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    return Container(
      height: 28,
      color: scheme.surfaceContainerHighest,
      padding: const EdgeInsets.symmetric(horizontal: 12),
      child: Row(
        children: [
          Icon(
            state.connected ? Icons.link : Icons.link_off,
            size: 14,
            color: state.connected ? Colors.greenAccent : scheme.error,
          ),
          const SizedBox(width: 8),
          Text(
            state.connected ? '已连接 ${state.deviceName}' : '未连接',
            style: const TextStyle(fontSize: 12),
          ),
          const Spacer(),
          Text(
            'reconnect ${state.reconnects}   crc ${state.crcErrors}   fail ${state.requestFailures}',
            style: TextStyle(fontSize: 12, color: scheme.outline),
          ),
        ],
      ),
    );
  }
}
