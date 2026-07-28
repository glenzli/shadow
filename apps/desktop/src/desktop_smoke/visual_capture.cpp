#include "visual_capture.hpp"

#include <QApplication>
#include <QDebug>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>

#include <cstdlib>

namespace DesktopSmoke {

void installVisualCapture(
    QApplication& application,
    QQmlApplicationEngine& engine
) {
    const QString capture_path =
        qEnvironmentVariable("SHADOW_DESKTOP_CAPTURE_PATH");
    if (capture_path.isEmpty()) {
        return;
    }

    bool delay_is_valid = false;
    const int configured_delay =
        qEnvironmentVariableIntValue(
            "SHADOW_DESKTOP_CAPTURE_DELAY_MS",
            &delay_is_valid
        );
    const int delay_ms =
        delay_is_valid && configured_delay >= 0 ? configured_delay : 4'000;

    QTimer::singleShot(delay_ms, &application, [
        &application,
        &engine,
        capture_path
    ]() {
        if (engine.rootObjects().isEmpty()) {
            qCritical() << "Visual capture has no root window";
            application.exit(EXIT_FAILURE);
            return;
        }

        auto* const window =
            qobject_cast<QQuickWindow*>(engine.rootObjects().front());
        if (window == nullptr) {
            qCritical() << "Visual capture root is not a quick window";
            application.exit(EXIT_FAILURE);
            return;
        }

        const QImage image = window->grabWindow();
        if (image.isNull() || !image.save(capture_path)) {
            qCritical() << "Visual capture could not save" << capture_path;
            application.exit(EXIT_FAILURE);
            return;
        }

        qInfo().noquote() << "Visual capture saved" << capture_path
                          << image.size();
        application.quit();
    });
}

} // namespace DesktopSmoke
