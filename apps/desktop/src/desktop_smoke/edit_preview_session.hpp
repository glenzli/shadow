#pragma once

class EditController;
class EditPreviewProvider;
class QCoreApplication;
class QQmlApplicationEngine;
class ReviewController;

namespace DesktopSmoke {

enum class EditPreviewTransportExpectation {
    None,
    MetalNative,
    SoftwareFallback,
};

struct EditPreviewSessionOptions final {
    bool request_before = false;
    bool request_full_detail = false;
    bool rapid_parameter_updates = false;
    EditPreviewTransportExpectation transport_expectation = EditPreviewTransportExpectation::None;
};

/// Runs the complete first-photo Precision acceptance lifecycle.
///
/// The runner owns Review-row readiness, opening the first photo, current
/// preview and QML-frame acceptance, optional Before acceptance, optional
/// full-detail publication/readback, its deadline, diagnostics, and the unique
/// process terminal.
void startEditPreviewSession(
    QCoreApplication& application,
    QQmlApplicationEngine& engine,
    ReviewController& review,
    EditController& editor,
    EditPreviewProvider* edit_preview_provider,
    EditPreviewSessionOptions options
);

} // namespace DesktopSmoke
