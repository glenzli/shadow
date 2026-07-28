#pragma once

class QApplication;
class QQmlApplicationEngine;
class ReviewController;

namespace DesktopSmoke {

void startCancelScanLifecycle(
    QApplication& application,
    ReviewController& controller
);
void startStreamingScanLifecycle(
    QApplication& application,
    QQmlApplicationEngine& engine,
    ReviewController& controller
);
void startReopenLibraryLifecycle(
    QApplication& application,
    QQmlApplicationEngine& engine,
    ReviewController& controller
);

} // namespace DesktopSmoke
