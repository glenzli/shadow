#pragma once

class QQmlApplicationEngine;
class PipelineRunController;
class EditController;

// Inert except for the explicit raw-white-balance pipeline acceptance action.
void installRawWhiteBalanceSmokeHarness(
    QQmlApplicationEngine& engine, PipelineRunController& pipeline, EditController& editor
);
