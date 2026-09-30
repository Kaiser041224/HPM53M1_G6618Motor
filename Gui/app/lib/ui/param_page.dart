/// Compatibility re-export.
///
/// The parameter panel was redesigned into [ParamZone] (`param_zone.dart`),
/// which now owns the wire-type / IEEE-754 helpers. This shim keeps the old
/// import path stable for `encoding_test.dart`; the `ParamPage` widget itself
/// no longer exists.
///
/// Copyright (c) 2026 Alliance HardwareGroup
/// SPDX-License-Identifier: BSD-3-Clause
library;

export 'param_zone.dart' show typeName, f32FromRaw, f32ToRaw, formatValue;
