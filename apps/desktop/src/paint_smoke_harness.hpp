#pragma once
class EditController;
class PipelineRunController;
// Inert unless the explicit packaged paint acceptance scenario is selected.
void installPaintSmokeHarness(PipelineRunController& pipeline, EditController& editor);
