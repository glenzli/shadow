#pragma once
class QQmlApplicationEngine;
class PipelineRunController;
class EditController;
void installPaintInteractionSmokeHarness(
    QQmlApplicationEngine&,
    PipelineRunController&,
    EditController&
);
