#include "auto_start_ui_smoke.hpp"
#include "edit_auto_start_controller.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QKeyEvent>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QUrl>
#ifdef SHADOW_DESKTOP_UI_SMOKE
#include <QtTest/QTest>
#endif

AutoStartUiSmoke::AutoStartUiSmoke(QQmlApplicationEngine& engine) :
    enabled_(qEnvironmentVariableIsSet("SHADOW_AUTO_START_SMOKE_UI")) {
    if (!engine.rootObjects().isEmpty())
        window_ = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
}

QQuickItem* AutoStartUiSmoke::item(const QString& name) const {
    return window_ ? window_->findChild<QQuickItem*>(name) : nullptr;
}

bool AutoStartUiSmoke::pointer(const QString& name, bool down, QString& error) {
    auto* target = item(name);
    if (!target || !target->isVisible() || !target->isEnabled() || target->width() <= 0
        || target->height() <= 0) {
        error = QStringLiteral("control unavailable: %1").arg(name);
        return false;
    }
#ifdef SHADOW_DESKTOP_UI_SMOKE
    const QPoint point = target->mapToScene({target->width() / 2, target->height() / 2}).toPoint();
    // Enter through the window system so delivery retains ordinary hit testing,
    // device button state and timestamps. Sending directly to the control would
    // conceal overlays or title-bar regions that prevent a real click.
    if (down)
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, point);
    else
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, point);
    if (target->property("down").toBool() != down) {
        error = QStringLiteral("window pointer event did not reach control: %1").arg(name);
        return false;
    }
#else
    Q_UNUSED(down);
    error = QStringLiteral("actual-control acceptance requires BUILD_TESTING");
    return false;
#endif
    return true;
}

bool AutoStartUiSmoke::click(const QString& name, QString& error) {
    return pointer(name, true, error) && pointer(name, false, error);
}

bool AutoStartUiSmoke::capture(const QString& name, QString& error) {
    const QString directory = qEnvironmentVariable("SHADOW_AUTO_START_EVIDENCE_DIR");
    if (!window_ || directory.isEmpty() || !QDir(directory).exists()
        || !window_->grabWindow().save(QDir(directory).filePath(name))) {
        error = QStringLiteral("could not capture application window: %1").arg(name);
        return false;
    }
    return true;
}

AutoStartUiStep
AutoStartUiSmoke::reviewAndHide(const EditAutoStartController& feature, QString& error) {
    if (!window_) {
        error = QStringLiteral("missing application window");
        return AutoStartUiStep::Failed;
    }
    if (review_stage_ == 0) {
        window_->requestActivate();
        if (!click(QStringLiteral("autoStartEntry"), error))
            return AutoStartUiStep::Failed;
        settling_.start();
        review_stage_ = 1;
        return AutoStartUiStep::Pending;
    }
    if (settling_.elapsed() < 250)
        return AutoStartUiStep::Pending;
    auto* dialog = window_->findChild<QObject*>(QStringLiteral("autoStartDialog"));
    auto* compare = item(QStringLiteral("autoStartCompare"));
    auto* preview = item(QStringLiteral("autoStartPreview"));
    if (!dialog || !compare || !preview) {
        error = QStringLiteral("Auto panel controls are missing");
        return AutoStartUiStep::Failed;
    }
    const auto shown = preview->property("source").toUrl().toString();
    switch (review_stage_) {
    case 1:
        if (!dialog->property("visible").toBool() || shown != feature.previewSource()
            || !capture(QStringLiteral("01-auto-ready-window.png"), error)
            || !pointer(QStringLiteral("autoStartCompare"), true, error)) {
            if (error.isEmpty())
                error = QStringLiteral("Auto panel did not present its candidate");
            return AutoStartUiStep::Failed;
        }
        break;
    case 2:
        if (!compare->property("down").toBool() || shown != feature.originalSource()
            || !capture(QStringLiteral("02-auto-before-window.png"), error)
            || !pointer(QStringLiteral("autoStartCompare"), false, error)) {
            if (error.isEmpty())
                error = QStringLiteral("held comparison did not show Before");
            return AutoStartUiStep::Failed;
        }
        break;
    case 3: {
        if (compare->property("down").toBool() || shown != feature.previewSource()
            || !capture(QStringLiteral("03-auto-candidate-window.png"), error)) {
            if (error.isEmpty())
                error = QStringLiteral("released comparison did not restore candidate");
            return AutoStartUiStep::Failed;
        }
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(window_, &press);
        QCoreApplication::sendEvent(window_, &release);
        break;
    }
    default:
        if (dialog->property("visible").toBool() || !feature.active()) {
            error = QStringLiteral("Escape must hide the panel while retaining the suggestion");
            return AutoStartUiStep::Failed;
        }
        return AutoStartUiStep::Passed;
    }
    ++review_stage_;
    settling_.restart();
    return AutoStartUiStep::Pending;
}

AutoStartUiStep AutoStartUiSmoke::openForApply(QString& error) {
    if (!opening_) {
        if (!click(QStringLiteral("autoStartEntry"), error))
            return AutoStartUiStep::Failed;
        opening_ = true;
        settling_.restart();
    }
    return settling_.elapsed() < 250 ? AutoStartUiStep::Pending : AutoStartUiStep::Passed;
}

AutoStartUiStep AutoStartUiSmoke::captureSettled(const QString& name, QString& error) {
    if (capture_name_ != name) {
        capture_name_ = name;
        settling_.start();
        return AutoStartUiStep::Pending;
    }
    if (settling_.elapsed() < 250)
        return AutoStartUiStep::Pending;
    return capture(name, error) ? AutoStartUiStep::Passed : AutoStartUiStep::Failed;
}
