import 'package:flutter/material.dart';

/// Builds the single Material 3 dark theme used across the Gui app.
ThemeData buildGuiTheme() {
  final scheme = ColorScheme.fromSeed(
    seedColor: const Color(0xFF4DB6AC),
    brightness: Brightness.dark,
  );
  return ThemeData(
    useMaterial3: true,
    colorScheme: scheme,
    scaffoldBackgroundColor: scheme.surface,
  );
}
