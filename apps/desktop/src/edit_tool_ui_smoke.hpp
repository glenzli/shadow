#pragma once
#include <QString>
#include <memory>
class QQmlApplicationEngine;
class PipelineRunController;
class EditController;
class DesktopBackend;
// Opt-in owner/UI regression, separate from the formal --agent-stdio path.
void installEditToolUiSmoke(
    QQmlApplicationEngine&,
    PipelineRunController&,
    EditController&,
    const std::shared_ptr<DesktopBackend>&,
    const QString& source
);
