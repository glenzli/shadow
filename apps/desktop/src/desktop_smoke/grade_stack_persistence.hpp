#pragma once

class EditController;
class QCoreApplication;

namespace DesktopSmoke {

/// Runs the complete Grade Stack persistence acceptance lifecycle.
///
/// The runner owns its readiness, mutations, durable save, close/reopen
/// transition, identity and value verification, deadline, diagnostics, and
/// process terminal. The caller only selects this scenario from the documented
/// environment flags.
void startGradeStackPersistence(
    QCoreApplication& application,
    EditController& editor
);

} // namespace DesktopSmoke
