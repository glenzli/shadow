#pragma once

class QApplication;
class EditController;
class QQmlApplicationEngine;
class ReviewController;
class UiPreferences;

namespace DesktopSmoke {

void startDirtyCloseLifecycle(
    QApplication& application,
    QQmlApplicationEngine& engine,
    EditController& editor
);
void startCloseLifecycle(
    QApplication& application,
    QQmlApplicationEngine& engine
);
void startI18nLifecycle(
    QApplication& application,
    QQmlApplicationEngine& engine,
    ReviewController& controller,
    UiPreferences& preferences
);

} // namespace DesktopSmoke
