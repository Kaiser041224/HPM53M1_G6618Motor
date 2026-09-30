import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_theme.dart';
import '../widgets/hairline_frame.dart';
import '../widgets/section_title.dart';
import 'scope_zone.dart';
import 'status_bar.dart';
import 'status_zone.dart';
import 'top_bar.dart';

/// Single-page flat layout (design-system §4.1): no navigation rail.
///
/// Top bar -> scrolling body (status -> scope -> parameters) -> status bar.
/// The parameters zone is still a task-scoped placeholder; Task 5 replaces it
/// with the real `param_zone` widget.
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
                  const _PlaceholderZone(
                    title: '参数',
                    en: 'parameters',
                    note: '双栏分组表 · 搜索 · COMMIT 落盘 · 联锁提示 —— Task 5 实装',
                  ),
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

/// Temporary section body for a zone not yet implemented.
class _PlaceholderZone extends StatelessWidget {
  const _PlaceholderZone({
    required this.title,
    required this.en,
    required this.note,
  });

  final String title;
  final String en;
  final String note;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        SectionTitle(title: title, en: en),
        const SizedBox(height: 12),
        HairlineFrame(
          variant: HairlineVariant.sub,
          child: Padding(
            padding: const EdgeInsets.all(16),
            child: Text(
              note,
              style: scale.body.copyWith(color: palette.fgMute),
            ),
          ),
        ),
      ],
    );
  }
}
