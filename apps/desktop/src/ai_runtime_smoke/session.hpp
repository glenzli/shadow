#pragma once

#include "../desktop_backend.hpp"
#include <QElapsedTimer>
#include <QObject>
#include <QPointF>
#include <QTimer>

class EditController;
class AiPreferences;
class PipelineRunController;
class QQmlApplicationEngine;

namespace AiRuntimeSmoke {
bool install(QQmlApplicationEngine&, PipelineRunController&, EditController&, AiPreferences&);
// Shared real-window readiness, history and export boundary for opt-in model acceptance.
class Session : public QObject {
  public:
    Session(QQmlApplicationEngine&, PipelineRunController&, EditController&, QString mode);

  protected:
    virtual void advance() = 0;
    virtual void observe() {}
    virtual void historyVerified();
    static void require(bool, const char*);
    void click(const char*, QPointF relative = {.5, .5});
    void slide(const char*, double position);
    void acceptEdit(const BackendGradeStack& before);
    void exportNow();
    [[nodiscard]] qint64 elapsed() const;
    void checkpoint(const char*);
    QQmlApplicationEngine& engine_;
    PipelineRunController& pipeline_;
    EditController& editor_;

  private:
    void tick();
    void advanceHistory();
    QString mode_;
    QTimer timer_;
    QElapsedTimer lifetime_;
    BackendGradeStack before_, after_;
    int history_stage_ = 0;
    bool exporting_ = false;
    qint64 last_wait_report_ = -5'000;
};

void installSubjectMask(
    QQmlApplicationEngine&,
    PipelineRunController&,
    EditController&,
    QString mode
);
void installCompletion(QQmlApplicationEngine&, PipelineRunController&, EditController&);
void installRawFoundation(QQmlApplicationEngine&, PipelineRunController&, EditController&);
} // namespace AiRuntimeSmoke
