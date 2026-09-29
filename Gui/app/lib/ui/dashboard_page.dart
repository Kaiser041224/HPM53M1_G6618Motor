import 'package:flutter/material.dart';

import '../state/app_state.dart';

/// Dashboard placeholder; live telemetry lands in Task 6.
class DashboardPage extends StatelessWidget {
  const DashboardPage({super.key, required this.state});

  final AppState state;

  @override
  Widget build(BuildContext context) =>
      const Scaffold(body: Center(child: Text('Dashboard（T6）')));
}
