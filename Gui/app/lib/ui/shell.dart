import 'package:flutter/material.dart';

import '../state/app_state.dart';
import 'connect_page.dart';
import 'dashboard_page.dart';
import 'param_page.dart';
import 'status_bar.dart';

/// Application shell: navigation rail plus the persistent status bar.
class Shell extends StatefulWidget {
  const Shell({super.key, required this.state});

  final AppState state;

  @override
  State<Shell> createState() => _ShellState();
}

class _ShellState extends State<Shell> {
  int _index = 0;

  @override
  Widget build(BuildContext context) {
    final pages = [
      ConnectPage(state: widget.state),
      ParamPage(state: widget.state),
      DashboardPage(state: widget.state),
    ];
    return Scaffold(
      body: Row(
        children: [
          NavigationRail(
            selectedIndex: _index,
            onDestinationSelected: (i) => setState(() => _index = i),
            labelType: NavigationRailLabelType.all,
            destinations: const [
              NavigationRailDestination(
                icon: Icon(Icons.cable),
                label: Text('连接'),
              ),
              NavigationRailDestination(
                icon: Icon(Icons.tune),
                label: Text('参数'),
              ),
              NavigationRailDestination(
                icon: Icon(Icons.speed),
                label: Text('Dashboard'),
              ),
            ],
          ),
          const VerticalDivider(width: 1),
          Expanded(child: pages[_index]),
        ],
      ),
      bottomNavigationBar: StatusBar(state: widget.state),
    );
  }
}
