#pragma once
class QQmlApplicationEngine;
class PipelineRunController;
class EditController;
void installDocumentSmokeHarness(
    QQmlApplicationEngine& engine,
    PipelineRunController& pipeline,
    EditController& editor
);
