#pragma once
class QQmlApplicationEngine;
class PipelineRunController;
class EditController;
// Explicit acceptance-only RGB curve lifecycle in the real packaged editor.
void installRgbCurveSmokeHarness(QQmlApplicationEngine&, PipelineRunController&, EditController&);
