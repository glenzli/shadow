#pragma once
class QQmlApplicationEngine;
class PipelineRunController;
class EditController;
class AiPreferences;
void installAutoStartSmokeHarness(
    QQmlApplicationEngine&,
    PipelineRunController&,
    EditController&,
    AiPreferences&
);
