#pragma once

class QApplication;
class QQmlApplicationEngine;

namespace DesktopSmoke {

// Saves the rendered root window when SHADOW_DESKTOP_CAPTURE_PATH is set.
// This keeps visual acceptance deterministic on hosts where system-level
// screenshots are unavailable or privacy-filtered.
void installVisualCapture(
    QApplication& application,
    QQmlApplicationEngine& engine
);

} // namespace DesktopSmoke
