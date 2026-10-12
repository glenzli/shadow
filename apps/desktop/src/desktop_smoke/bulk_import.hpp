#pragma once

class QApplication;
class QQmlApplicationEngine;
class ReviewController;

namespace DesktopSmoke {
void startBulkImportLifecycle(
    QApplication& application,
    QQmlApplicationEngine& engine,
    ReviewController& controller
);
}
