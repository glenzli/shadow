#pragma once

#include <QElapsedTimer>
#include <QString>

class QQmlApplicationEngine;
class QQuickItem;
class QQuickWindow;
class EditAutoStartController;

enum class AutoStartUiStep { Pending, Passed, Failed };

// Opt-in acceptance only: pointer events target the actual packaged controls,
// and captures contain this application's window, never the user's desktop.
class AutoStartUiSmoke final {
  public:
    explicit AutoStartUiSmoke(QQmlApplicationEngine& engine);
    bool enabled() const {
        return enabled_;
    }
    AutoStartUiStep reviewAndHide(const EditAutoStartController&, QString& error);
    AutoStartUiStep openForApply(QString& error);
    AutoStartUiStep captureSettled(const QString& name, QString& error);
    bool click(const QString& name, QString& error);

  private:
    QQuickItem* item(const QString& name) const;
    bool pointer(const QString& name, bool down, QString& error);
    bool capture(const QString& name, QString& error);
    QQuickWindow* window_ = nullptr;
    bool enabled_ = false;
    int review_stage_ = 0;
    bool opening_ = false;
    QString capture_name_;
    QElapsedTimer settling_;
};
