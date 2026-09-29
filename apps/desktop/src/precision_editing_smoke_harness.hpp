#pragma once
class QQmlApplicationEngine;
class PipelineRunController;
class EditController;
void installPrecisionEditingSmokeHarness(
    QQmlApplicationEngine&,
    PipelineRunController&,
    EditController&
);
