#pragma once

#include <QString>

class QApplication;
class EditController;
class EditNeighborPreheater;
class EditPreviewProvider;
class QQmlApplicationEngine;
class ReviewController;
class ThumbnailProvider;
class UiPreferences;

// Owns environment-driven desktop automation and smoke-test lifecycle wiring.
// Production startup remains in main.cpp; this harness is inert unless one of
// the documented SHADOW_DESKTOP_* automation flags is present.
void installDesktopSmokeHarness(
    QApplication& application,
    QQmlApplicationEngine& engine,
    ReviewController& controller,
    EditController& editor,
    EditNeighborPreheater& edit_neighbor_preheater,
    ThumbnailProvider* thumbnail_provider,
    EditPreviewProvider* edit_preview_provider,
    UiPreferences& preferences,
    const QString& initial_folder
);
