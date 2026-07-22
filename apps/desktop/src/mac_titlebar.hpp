#pragma once

class QWindow;

// Aligns the native macOS window controls with Shadow's expanded title bar.
// The implementation is macOS-only and uses public AppKit APIs.
void installMacTitleBarAlignment(QWindow* window, int title_bar_height);
