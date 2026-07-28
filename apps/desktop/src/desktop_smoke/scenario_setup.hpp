#pragma once

class QApplication;
class EditController;
class QQmlApplicationEngine;
class ReviewController;
class ThumbnailProvider;

namespace DesktopSmoke {

void installFirstPhotoOpener(
    QApplication& application,
    QQmlApplicationEngine& engine,
    ReviewController& controller,
    EditController& editor
);
void installFirstComparisonRequest(
    QApplication& application,
    ReviewController& controller,
    ThumbnailProvider* thumbnail_provider
);
void installFirstDecisionRequest(
    QApplication& application,
    ReviewController& controller
);

} // namespace DesktopSmoke
