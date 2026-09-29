import 'package:flutter/material.dart';

import '../state/app_state.dart';

/// Parameter panel placeholder; the editable table lands in Task 5.
class ParamPage extends StatelessWidget {
  const ParamPage({super.key, required this.state});

  final AppState state;

  @override
  Widget build(BuildContext context) =>
      const Scaffold(body: Center(child: Text('参数面板（T5）')));
}
