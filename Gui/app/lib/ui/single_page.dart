import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_glow.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/hairline_frame.dart';
import 'bottom_bar.dart';
import 'control_zone.dart';
import 'encoder_zone.dart';
import 'limit_zone.dart';
import 'param_zone.dart';
import 'scope_zone.dart';
import 'status_zone.dart';
import 'top_bar.dart';

/// One L1 navigation entry: an icon, its Chinese name and a small Latin
/// caption.
class NavItem {
  const NavItem({
    required this.key,
    required this.zh,
    required this.latin,
    required this.icon,
  });

  /// Finder key of the sidebar slot (`nav_<category>`).
  final String key;

  /// Chinese display name.
  final String zh;

  /// Latin sub-caption.
  final String latin;

  /// Leading icon.
  final IconData icon;
}

/// The L1 categories, in sidebar order.
const List<NavItem> kNavItems = <NavItem>[
  NavItem(
    key: 'nav_monitor',
    zh: '监视',
    latin: 'MONITOR',
    icon: Icons.dashboard_outlined,
  ),
  NavItem(key: 'nav_scope', zh: '示波', latin: 'SCOPE', icon: Icons.show_chart),
  NavItem(
    key: 'nav_control',
    zh: '控制',
    latin: 'CONTROL',
    icon: Icons.power_settings_new,
  ),
  NavItem(key: 'nav_params', zh: '参数', latin: 'PARAMS', icon: Icons.tune),
];

/// The application shell: a left L1 sidebar beside the flat content area,
/// framed by the unchanged connection top bar and verdict bottom bar.
///
/// Layout: `TopBar → Row[ Sidebar | Expanded( content ) ] → BottomBar`. Each L1
/// page lays its zones out flat (stacked vertically) in its own scroll column;
/// there are no L2 tabs. Pages stay alive across switches via an
/// [Offstage] + [TickerMode] stack (an [IndexedStack] equivalent), so per-page
/// scroll position, the parameter search field and the encoder schematic keep
/// their state.
class SinglePage extends StatefulWidget {
  const SinglePage({super.key, required this.state});

  final AppState state;

  @override
  State<SinglePage> createState() => _SinglePageState();
}

class _SinglePageState extends State<SinglePage> {
  /// Active L1 category index (index into [kNavItems]).
  int _navIndex = 0;

  /// Vertical gap between the stacked zones of one page (`sp-5`, 24px).
  static const double _sectionGap = 24;

  /// Sidebar width [logical px].
  static const double _sidebarWidth = 150;

  void _selectNav(int index) => setState(() => _navIndex = index);

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Column(
        children: <Widget>[
          TopBar(state: widget.state),
          Expanded(
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: <Widget>[
                _Sidebar(activeIndex: _navIndex, onSelect: _selectNav),
                Expanded(child: _content()),
              ],
            ),
          ),
          BottomBar(state: widget.state),
        ],
      ),
    );
  }

  /// The page stack for the current L1 category; only the selected page is
  /// onstage (the others stay built for keep-alive).
  ///
  /// Each page lays its zones out flat inside one scroll viewport, separated
  /// by [_sectionGap]:
  /// - 监视: `StatusZone → EncoderZone`
  /// - 示波: `ScopeZone (TrendChart panel) → LimitZone`
  /// - 控制: `ControlZone`
  /// - 参数: `ParamZone`
  Widget _content() {
    return _PageStack(
      index: _navIndex,
      children: <Widget>[
        _scroll(
          _stack(<Widget>[
            StatusZone(state: widget.state),
            EncoderZone(state: widget.state),
          ]),
        ),
        _scroll(
          _stack(<Widget>[
            ScopeZone(state: widget.state),
            LimitZone(state: widget.state),
          ]),
        ),
        _scroll(ControlZone(state: widget.state)),
        _scroll(ParamZone(state: widget.state)),
      ],
    );
  }

  /// Stacks [zones] vertically, separating them with [_sectionGap].
  Widget _stack(List<Widget> zones) => Column(
    crossAxisAlignment: CrossAxisAlignment.stretch,
    children: <Widget>[
      for (var i = 0; i < zones.length; i++) ...<Widget>[
        if (i > 0) const SizedBox(height: _sectionGap),
        zones[i],
      ],
    ],
  );

  /// Wraps a page body in its own scroll viewport.
  Widget _scroll(Widget child) => SingleChildScrollView(
    padding: const EdgeInsets.fromLTRB(20, 16, 20, 24),
    child: child,
  );
}

/// Keeps every child built while showing only the one at [index] — the
/// keep-alive equivalent of an [IndexedStack], with explicit offstage
/// semantics so `skipOffstage` finders agree with what is visible.
class _PageStack extends StatelessWidget {
  const _PageStack({required this.index, required this.children});

  final int index;
  final List<Widget> children;

  @override
  Widget build(BuildContext context) {
    return Stack(
      fit: StackFit.expand,
      children: <Widget>[
        for (var i = 0; i < children.length; i++)
          Offstage(
            offstage: i != index,
            child: TickerMode(enabled: i == index, child: children[i]),
          ),
      ],
    );
  }
}

/// The L1 navigation rail: a vertical stack of square blocks, the active one
/// framed in a bright 1.5px line over a raised fill.
class _Sidebar extends StatelessWidget {
  const _Sidebar({required this.activeIndex, required this.onSelect});

  final int activeIndex;
  final ValueChanged<int> onSelect;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    return Container(
      width: _SinglePageState._sidebarWidth,
      decoration: BoxDecoration(
        color: palette.bgPanel,
        border: Border(
          right: BorderSide(color: palette.lineDim, width: palette.lineWidth),
        ),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          for (var i = 0; i < kNavItems.length; i++)
            _SidebarItem(
              key: Key(kNavItems[i].key),
              item: kNavItems[i],
              active: i == activeIndex,
              onTap: () => onSelect(i),
            ),
        ],
      ),
    );
  }
}

/// One sidebar block. Inactive entries recede to [GuiPalette.fgDim]; the active
/// entry takes a 1.5px [GuiPalette.lineHi] frame, a [GuiPalette.bgHover] fill
/// and [GuiPalette.fgStrong] text. Hover uses the button vocabulary: a bright
/// line with one faint same-hue glow.
class _SidebarItem extends StatefulWidget {
  const _SidebarItem({
    super.key,
    required this.item,
    required this.active,
    required this.onTap,
  });

  final NavItem item;
  final bool active;
  final VoidCallback onTap;

  @override
  State<_SidebarItem> createState() => _SidebarItemState();
}

class _SidebarItemState extends State<_SidebarItem> {
  bool _hovered = false;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final active = widget.active;
    final highlighted = active || _hovered;
    final textColor = active ? palette.fgStrong : palette.fgDim;
    return MouseRegion(
      cursor: SystemMouseCursors.click,
      onEnter: (_) => setState(() => _hovered = true),
      onExit: (_) => setState(() => _hovered = false),
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: widget.onTap,
        child: Container(
          margin: const EdgeInsets.fromLTRB(10, 8, 10, 0),
          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 9),
          decoration: BoxDecoration(
            color: active
                ? palette.bgHover
                : (_hovered ? palette.bgRaise : palette.bgPanel),
            border: Border.all(
              color: highlighted ? palette.lineHi : Colors.transparent,
              width: HairlineFrame.brightWidth,
            ),
            boxShadow: _hovered && !active
                ? <BoxShadow>[
                    BoxShadow(
                      color: palette.glowSoft,
                      blurRadius: GuiGlowStyles.softBlur,
                    ),
                  ]
                : const <BoxShadow>[],
          ),
          child: Row(
            children: <Widget>[
              Icon(widget.item.icon, size: 16, color: textColor),
              const SizedBox(width: 10),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: <Widget>[
                    Text(
                      widget.item.zh,
                      style: scale.body.copyWith(fontSize: 12, color: textColor),
                    ),
                    const SizedBox(height: 2),
                    Text(
                      widget.item.latin,
                      style: scale.label.copyWith(
                        color: active ? palette.fgStrong : palette.fgMute,
                      ),
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

