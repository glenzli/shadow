#include "backend/desktop_backend_private.hpp"
#include "backend/edit_settings_projection.hpp"
#include "backend/rust_qt_projection.hpp"

#include <stdexcept>
#include <utility>

namespace {

using desktop_backend_projection::edit_state;
using desktop_backend_projection::ffi_grade_stack;
using desktop_backend_projection::qbytes;
using desktop_backend_projection::qstring;

[[nodiscard]] BackendSubjectMaskTerminal
subject_mask_terminal(const shadow::desktop::FfiSubjectMaskTerminal terminal) {
    switch (terminal) {
    case shadow::desktop::FfiSubjectMaskTerminal::Staged:
        return BackendSubjectMaskTerminal::Staged;
    case shadow::desktop::FfiSubjectMaskTerminal::Unavailable:
        return BackendSubjectMaskTerminal::Unavailable;
    case shadow::desktop::FfiSubjectMaskTerminal::Cancelled:
        return BackendSubjectMaskTerminal::Cancelled;
    case shadow::desktop::FfiSubjectMaskTerminal::Failed:
        return BackendSubjectMaskTerminal::Failed;
    default:
        throw std::runtime_error("subject-mask provider returned an unknown terminal state");
    }
}

[[nodiscard]] shadow::desktop::FfiSubjectMaskKind
ffi_subject_mask_kind(const BackendSubjectMaskKind kind) {
    switch (kind) {
    case BackendSubjectMaskKind::PromptedSubject:
        return shadow::desktop::FfiSubjectMaskKind::PromptedSubject;
    case BackendSubjectMaskKind::FaceRegion:
        return shadow::desktop::FfiSubjectMaskKind::FaceRegion;
    }
    throw std::runtime_error("unknown subject-mask selection kind");
}

[[nodiscard]] shadow::desktop::FfiFaceRegion ffi_face_region(const BackendFaceRegion region) {
    switch (region) {
    case BackendFaceRegion::Face:
        return shadow::desktop::FfiFaceRegion::Face;
    case BackendFaceRegion::Skin:
        return shadow::desktop::FfiFaceRegion::Skin;
    case BackendFaceRegion::Eyes:
        return shadow::desktop::FfiFaceRegion::Eyes;
    case BackendFaceRegion::Eyebrows:
        return shadow::desktop::FfiFaceRegion::Eyebrows;
    case BackendFaceRegion::LipsAndMouth:
        return shadow::desktop::FfiFaceRegion::LipsAndMouth;
    case BackendFaceRegion::Nose:
        return shadow::desktop::FfiFaceRegion::Nose;
    case BackendFaceRegion::Ears:
        return shadow::desktop::FfiFaceRegion::Ears;
    case BackendFaceRegion::Hair:
        return shadow::desktop::FfiFaceRegion::Hair;
    case BackendFaceRegion::Neck:
        return shadow::desktop::FfiFaceRegion::Neck;
    case BackendFaceRegion::Clothing:
        return shadow::desktop::FfiFaceRegion::Clothing;
    case BackendFaceRegion::Accessories:
        return shadow::desktop::FfiFaceRegion::Accessories;
    }
    throw std::runtime_error("unknown face-region selector");
}

} // namespace

std::uint64_t DesktopBackend::beginSubjectMaskJob() const {
    return impl_->session->begin_subject_mask_job();
}

void DesktopBackend::cancelSubjectMaskJob(const std::uint64_t subject_mask_job_token) const {
    impl_->session->cancel_subject_mask_job(subject_mask_job_token);
}

BackendSubjectMaskResult DesktopBackend::executeSubjectMaskJob(
    const QString& photo_id,
    const QString& source_path,
    const BackendSubjectMaskRequest& request
) const {
    shadow::desktop::FfiSubjectMaskRequest ffi_request;
    ffi_request.job_token = request.job_token;
    ffi_request.generation = request.generation;
    ffi_request.base_commit_id = request.base_commit_id.toStdString();
    ffi_request.settings = ffi_grade_stack(request.grade_stack);
    ffi_request.target_grade_node_index = request.target_grade_node_index;
    ffi_request.target_grade_node_id = request.target_grade_node_id.toStdString();
    ffi_request.kind = ffi_subject_mask_kind(request.kind);
    ffi_request.face_region = ffi_face_region(request.face_region);
    ffi_request.points.reserve(static_cast<std::size_t>(request.points.size()));
    for (const auto& point : request.points) {
        ffi_request.points.push_back({
            .x = point.x,
            .y = point.y,
            .foreground = point.foreground,
        });
    }
    const auto result = impl_->session->execute_subject_mask_job(
        photo_id.toStdString(),
        source_path.toStdString(),
        ffi_request
    );
    return {
        .terminal = subject_mask_terminal(result.terminal),
        .job_token = result.job_token,
        .generation = result.generation,
        .proposal_token = result.proposal_token,
        .detail = qstring(result.detail),
        .preview_width = result.preview_width,
        .preview_height = result.preview_height,
        .preview_samples = qbytes(result.preview_samples),
    };
}

BackendPhotoEditState DesktopBackend::applySubjectMaskProposal(
    const QString& photo_id,
    const QString& source_path,
    const BackendSubjectMaskApplyRequest& request
) const {
    shadow::desktop::FfiSubjectMaskApplyRequest ffi_request;
    ffi_request.proposal_token = request.proposal_token;
    ffi_request.generation = request.generation;
    ffi_request.base_commit_id = request.base_commit_id.toStdString();
    ffi_request.expected_working_commit_id = request.expected_working_commit_id.toStdString();
    ffi_request.settings = ffi_grade_stack(request.grade_stack);
    ffi_request.target_grade_node_index = request.target_grade_node_index;
    ffi_request.target_grade_node_id = request.target_grade_node_id.toStdString();
    ffi_request.invert = request.invert;
    return edit_state(impl_->session->apply_subject_mask_proposal(
        photo_id.toStdString(),
        source_path.toStdString(),
        ffi_request
    ));
}

void DesktopBackend::discardSubjectMaskProposal(const std::uint64_t proposal_token) const {
    impl_->session->discard_subject_mask_proposal(proposal_token);
}
