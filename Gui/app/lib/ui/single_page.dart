import 'package:flutter/material.dart';

import '../state/app_state.dart';
import '../theme/gui_glow.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import '../widgets/crt_backdrop.dart';
import '../widgets/hairline_frame.dart';
import 'bottom_bar.dart';
import 'control_zone.dart';
import 'encoder_zone.dart';
import 'limit_zone.dart';
import 'motor_drive_zone.dart';
import 'param_zone.dart';
import 'pid_panel.dart';
import 'tuning_blocks.dart';
import 'scope_zone.dart';
import 'start_adjust_zone.dart';
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
///
/// 控制已与监视合并（调参时必须同时看读数）；「电机 · 驱动」紧随其后 ——
/// 它是接着监视·控制的最常用落点（看读数 → 改驱动/电机参数），
/// 示波与全量参数表排在后面。
const List<NavItem> kNavItems = <NavItem>[
  NavItem(
    key: 'nav_monitor',
    zh: '监视 · 控制',
    latin: 'MONITOR',
    icon: Icons.dashboard_outlined,
  ),
  NavItem(
    key: 'nav_motor',
    zh: '电机 · 驱动',
    latin: 'MOTOR',
    icon: Icons.settings_input_component,
  ),
  NavItem(key: 'nav_scope', zh: '示波', latin: 'SCOPE', icon: Icons.show_chart),
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

  /// 监视·控制页并排分栏所需的最小内容宽 [logical px]。
  ///
  /// 低于此值改为上下堆叠。取值依据：编码器面板实测需要约 410px，
  /// 控制栏约 400px，加 24px 栏间距 → 约 834px，留余量取 880。
  static const double _twoColumnMinWidth = 880;

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
                Expanded(
                  // CRT 纹理挂在内容区底层：扫描线 + ~12Hz 动态噪点在页面内容
                  // 之下（Stack 首个孩子先绘制 = 更靠后）。
                  child: Stack(
                    key: const Key('content_stack'),
                    fit: StackFit.expand,
                    children: <Widget>[const CrtBackdrop(), _content()],
                  ),
                ),
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
  /// **顺序必须与 [kNavItems] 严格一致** —— 索引即页面。
  /// - 监视·控制: **左右分栏** —— 左栏监视（`StatusZone → EncoderZone`），
  ///   右栏控制（`ControlZone` 故障 → `StartAdjustZone` 启动/调整 →
  ///   `PidPanel` 三环 PID 参数 → 保护门限）。
  ///   合并的理由：调参时必须同时看着读数，分两页来回切是反效率的；
  ///   启动 / 调整与速度环 / 角度环 PID 入口亦从「电机 · 驱动」页迁来同屏。
  /// - 电机·驱动: `MotorDriveZone`（左标定程序 / 右参数对照）
  /// - 示波: `ScopeZone (TrendChart panel) → LimitZone`
  /// - 参数: `ParamZone`（全量表，可编辑）
  Widget _content() {
    return _PageStack(
      index: _navIndex,
      children: <Widget>[
        _scroll(_monitorControlPage()),
        _scroll(MotorDriveZone(state: widget.state)),
        _scroll(
          _stack(<Widget>[
            ScopeZone(state: widget.state),
            LimitZone(state: widget.state),
          ]),
        ),
        _scroll(ParamZone(state: widget.state)),
      ],
    );
  }

  /// 监视·控制合并页：左监视、右控制，整页共享一条滚动。
  ///
  /// 左栏自上而下：`StatusZone → EncoderZone`。
  ///
  /// 右栏自上而下（严格顺序）：
  /// `ControlZone`（故障构成 + 清除故障）→
  /// `StartAdjustZone`（启停切换 + 力矩 / 速度 / 角度模式 + 设定输入）→
  /// `PidPanel`（三环 PID：电流环 / 速度环 / 角度环，单面板）→
  /// `ProtectionLimitsPanel`（保护门限，最底部）。
  /// FREQ / MOD 滑条与设备回读面板已删除。
  ///
  /// 两栏各自 `CrossAxisAlignment.start`，故栏内高度互不影响；
  /// 左右比例 11:10 —— 监视侧有 8 个状态瓦片与编码器示意，需要更宽。
  ///
  /// **窄屏兜底**：两栏并排需要约 [_twoColumnMinWidth] 的内容宽（窗口约
  /// 1030px，与 `window_manager` 的 `minimumSize` 1024 相当）。低于此宽度
  /// 改为上下堆叠 —— 窗口可自由缩放，不能假定总是够宽。
  Widget _monitorControlPage() {
    final monitor = _stack(<Widget>[
      StatusZone(state: widget.state),
      EncoderZone(state: widget.state),
    ]);
    final control = _stack(<Widget>[
      ControlZone(state: widget.state),
      StartAdjustZone(state: widget.state),
      PidPanel(state: widget.state),
      // 保护门限回归右栏最底部（PID 参数之下）：与「影响运行 / 安全」的参数组同列。
      ProtectionLimitsPanel(state: widget.state),
    ]);

    return LayoutBuilder(
      builder: (context, constraints) {
        if (constraints.maxWidth < _twoColumnMinWidth) {
          return _stack(<Widget>[monitor, control]);
        }
        return Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Expanded(flex: 11, child: monitor),
            const SizedBox(width: _sectionGap),
            Expanded(flex: 10, child: control),
          ],
        );
      },
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

  /// 键面高光色 —— 隆起态的顶边 / 内凹态的底边。
  Color _bevelHighlight(GuiPalette palette) =>
      palette.fgStrong.withValues(alpha: 0.10);

  /// 键面暗边色 —— 隆起态的底边 / 内凹态的顶边，兼作内阴影。
  Color _bevelShadow(GuiPalette palette) =>
      palette.bgVoid.withValues(alpha: 0.55);

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final active = widget.active;
    // 老式 CD 机按键：未选微隆起（顶亮底暗），选中 / 按下内凹（顶暗底亮）。
    final bevelTop = active ? _bevelShadow(palette) : _bevelHighlight(palette);
    final bevelBottom = active
        ? _bevelHighlight(palette)
        : _bevelShadow(palette);
    final faceGradient = active
        ? LinearGradient(
            begin: Alignment.topCenter,
            end: Alignment.bottomCenter,
            colors: <Color>[palette.bgPanel, palette.bgHover],
          )
        : _hovered
        ? LinearGradient(
            begin: Alignment.topCenter,
            end: Alignment.bottomCenter,
            colors: <Color>[palette.bgHover, palette.bgRaise],
          )
        : LinearGradient(
            begin: Alignment.topCenter,
            end: Alignment.bottomCenter,
            colors: <Color>[palette.bgRaise, palette.bgPanel],
          );
    final textColor = active ? palette.fgStrong : palette.fgDim;
    return MouseRegion(
      cursor: SystemMouseCursors.click,
      onEnter: (_) => setState(() => _hovered = true),
      onExit: (_) => setState(() => _hovered = false),
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: widget.onTap,
        child: Container(
          key: Key('nav_face_${widget.item.key}'),
          margin: const EdgeInsets.fromLTRB(10, 8, 10, 0),
          decoration: BoxDecoration(
            gradient: faceGradient,
            // 勾线：选中 = accent 1.5px；未选中 = lineDim 1px。
            border: Border.all(
              color: active ? palette.accent : palette.lineDim,
              width: active ? HairlineFrame.brightWidth : palette.lineWidth,
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
          child: Container(
            key: Key('nav_bevel_${widget.item.key}'),
            decoration: BoxDecoration(
              border: Border(
                top: BorderSide(color: bevelTop),
                bottom: BorderSide(color: bevelBottom),
              ),
            ),
            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 9),
            child: Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: <Widget>[
                Icon(widget.item.icon, size: 16, color: textColor),
                const SizedBox(width: 10),
                Flexible(
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[
                      Text(
                        widget.item.zh,
                        style: scale.body.copyWith(
                          fontSize: 12,
                          color: textColor,
                        ),
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
      ),
    );
  }
}
