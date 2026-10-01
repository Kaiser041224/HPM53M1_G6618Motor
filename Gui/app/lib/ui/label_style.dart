/// v5 label styling — one look for every name/label on a page.
///
/// Per design-system §2.6.3 a **parameter name / tile label / bar name /
/// data-column label is always cyan** ([GuiPalette.accent]) with the faint
/// same-hue "glow-cy" halo, while **only values** carry a dimension accent.
/// Units and sub-notes stay [GuiPalette.fgDim] / [GuiPalette.fgMute].
///
/// Keeping the rule in one helper stops the several page zones from drifting
/// apart, exactly as `tintForUnit` is the single point for value accents.
///
/// Copyright (c) 2026 Alliance HardwareGroup
/// SPDX-License-Identifier: BSD-3-Clause
library;

import 'package:flutter/material.dart';

import '../theme/gui_glow.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';

/// Cyan label style with the weak cyan halo, resolved from [palette]/[scale].
TextStyle guiLabelStyle(GuiPalette palette, GuiTypeScale scale) =>
    GuiGlowStyles.weak(scale.label.copyWith(color: palette.accent));
