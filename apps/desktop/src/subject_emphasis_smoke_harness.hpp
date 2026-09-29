#pragma once
class QQmlApplicationEngine;
class PipelineRunController;
class EditController;
// Explicit opt-in only: consumes one local fixture through the packaged editor.
void installSubjectEmphasisSmokeHarness(
    QQmlApplicationEngine&,
    PipelineRunController&,
    EditController&
);
