#pragma once

class EditController;
class EditNeighborPreheater;
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
    bool before_composition = false;
    bool request_full_detail = false;
    bool rapid_parameter_updates = false;
    EditNeighborPreheater* edit_neighbor_preheater = nullptr;
    bool verify_adjacent_edit = false;
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
