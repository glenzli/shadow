#pragma once

class QQmlApplicationEngine;
class ReviewController;

namespace DesktopSmoke {
// Opt-in acceptance against the installed Runtime and an isolated portrait library.
bool installAiLibrarySmoke(QQmlApplicationEngine& engine, ReviewController& review);
} // namespace DesktopSmoke
