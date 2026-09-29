#pragma once
class QQmlApplicationEngine;
class PipelineRunController;
class EditController;
// Inert unless SHADOW_PIPELINE_SMOKE_ACTION is explicitly set by acceptance tests.
void installPipelineSmokeHarness(
    QQmlApplicationEngine& engine,
    PipelineRunController& pipeline,
    EditController& editor
);
