import 'package:flutter/material.dart';

import '../state/app_state.dart';
import 'param_zone.dart';
import 'scope_zone.dart';
import 'status_bar.dart';
import 'status_zone.dart';
import 'top_bar.dart';

/// Single-page flat layout (design-system §4.1): no navigation rail.
///
/// Top bar -> scrolling body (status -> scope -> parameters) -> status bar.
class SinglePage extends StatelessWidget {
  const SinglePage({super.key, required this.state});

  final AppState state;

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Column(
        children: <Widget>[
          TopBar(state: state),
          Expanded(
            child: SingleChildScrollView(
              padding: const EdgeInsets.fromLTRB(20, 16, 20, 24),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: <Widget>[
                  StatusZone(state: state),
                  const SizedBox(height: 24),
                  ScopeZone(state: state),
                  const SizedBox(height: 24),
                  ParamZone(state: state),
                ],
              ),
            ),
          ),
          StatusBar(state: state),
        ],
      ),
    );
  }
}
