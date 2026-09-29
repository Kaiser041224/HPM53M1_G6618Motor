import 'package:flutter/material.dart';

import '../state/app_state.dart';

/// Connection management page. TCP is the only transport enabled for M1;
/// USB bulk and UART / CDC are disabled placeholders scheduled for M2.
class ConnectPage extends StatefulWidget {
  const ConnectPage({super.key, required this.state});

  final AppState state;

  @override
  State<ConnectPage> createState() => _ConnectPageState();
}

class _ConnectPageState extends State<ConnectPage> {
  final _addrCtrl = TextEditingController(text: '127.0.0.1:9000');
  bool _busy = false;

  @override
  void dispose() {
    _addrCtrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final s = widget.state;
    return Padding(
      padding: const EdgeInsets.all(24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text('连接管理', style: Theme.of(context).textTheme.titleLarge),
          const SizedBox(height: 16),
          SegmentedButton<String>(
            segments: const [
              ButtonSegment(value: 'tcp', label: Text('TCP / 无线适配器')),
              ButtonSegment(
                value: 'usb',
                label: Text('USB bulk（M2）'),
                enabled: false,
              ),
              ButtonSegment(
                value: 'serial',
                label: Text('UART / CDC（M2）'),
                enabled: false,
              ),
            ],
            selected: const {'tcp'},
            onSelectionChanged: (_) {},
          ),
          const SizedBox(height: 16),
          Row(
            children: [
              SizedBox(
                width: 280,
                child: TextField(
                  controller: _addrCtrl,
                  decoration: const InputDecoration(
                    labelText: '地址',
                    hintText: '127.0.0.1:9000',
                    border: OutlineInputBorder(),
                  ),
                ),
              ),
              const SizedBox(width: 12),
              FilledButton.icon(
                onPressed: _busy
                    ? null
                    : () async {
                        final messenger = ScaffoldMessenger.of(context);
                        setState(() => _busy = true);
                        final ok = await s.connect(_addrCtrl.text.trim());
                        if (!mounted) return;
                        setState(() => _busy = false);
                        messenger.showSnackBar(
                          SnackBar(
                            content: Text(
                              ok ? '已连接 ${s.deviceName}' : '连接失败：${s.lastError}',
                            ),
                          ),
                        );
                      },
                icon: const Icon(Icons.link),
                label: const Text('连接'),
              ),
              const SizedBox(width: 8),
              OutlinedButton.icon(
                onPressed: _busy || !s.connected ? null : () => s.disconnect(),
                icon: const Icon(Icons.link_off),
                label: const Text('断开'),
              ),
            ],
          ),
        ],
      ),
    );
  }
}
