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
    case shadow::desktop::FfiSubjectMaskTerminal::AnalysisReady:
        return BackendSubjectMaskTerminal::AnalysisReady;
    case shadow::desktop::FfiSubjectMaskTerminal::PeopleReady:
        return BackendSubjectMaskTerminal::PeopleReady;
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
    case BackendSubjectMaskKind::PeopleDiscovery:
        return shadow::desktop::FfiSubjectMaskKind::PeopleDiscovery;
    case BackendSubjectMaskKind::PeopleRegions:
        return shadow::desktop::FfiSubjectMaskKind::PeopleRegions;
    case BackendSubjectMaskKind::SubjectAnalysis:
        return shadow::desktop::FfiSubjectMaskKind::SubjectAnalysis;
    case BackendSubjectMaskKind::SubjectEmphasis:
        return shadow::desktop::FfiSubjectMaskKind::SubjectEmphasis;
    case BackendSubjectMaskKind::SemanticQuery:
        return shadow::desktop::FfiSubjectMaskKind::SemanticQuery;
    }
    throw std::runtime_error("unknown subject-mask selection kind");
}

} // namespace

std::uint64_t DesktopBackend::beginSubjectMaskInputSession() const {
    return impl_->session->begin_subject_mask_input_session();
}

void DesktopBackend::finishSubjectMaskInputSession(
    const std::uint64_t subject_mask_input_session_token
) const {
    impl_->session->finish_subject_mask_input_session(subject_mask_input_session_token);
}

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
    ffi_request.input_session_token = request.input_session_token;
    ffi_request.job_token = request.job_token;
    ffi_request.generation = request.generation;
    ffi_request.base_commit_id = request.base_commit_id.toStdString();
    ffi_request.settings = ffi_grade_stack(request.grade_stack);
    ffi_request.target_grade_node_index = request.target_grade_node_index;
    ffi_request.target_grade_node_id = request.target_grade_node_id.toStdString();
    ffi_request.kind = ffi_subject_mask_kind(request.kind);
    ffi_request.person_index = request.person_index;
    ffi_request.face_region_mask = request.face_region_mask;
    ffi_request.semantic_query = request.semantic_query.toStdString();
    ffi_request.semantic_maximum_regions = request.semantic_maximum_regions;
    ffi_request.semantic_score_threshold_percent = request.semantic_score_threshold_percent;
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
    BackendSubjectMaskResult projected{
        .terminal = subject_mask_terminal(result.terminal),
        .job_token = result.job_token,
        .generation = result.generation,
        .proposal_token = result.proposal_token,
        .detail = qstring(result.detail),
        .preview_width = result.preview_width,
        .preview_height = result.preview_height,
        .preview_samples = qbytes(result.preview_samples),
    };
    projected.description = qstring(result.description);
    projected.analysis_preview_jpeg = qbytes(result.analysis_preview_jpeg);
    projected.analysis_model = qstring(result.analysis_model);
    projected.emphasis_exposure = result.emphasis_exposure;
    projected.emphasis_saturation = result.emphasis_saturation;
    projected.emphasis_background = result.emphasis_background;
    projected.emphasis_reason = result.emphasis_reason;
    for (const auto& query : result.subject_queries) {
        projected.subject_queries.push_back(qstring(query));
    }
    projected.people.reserve(static_cast<qsizetype>(result.people.size()));
    for (const auto& person : result.people) {
        projected.people.push_back({
            .index = person.index,
            .confidence = person.confidence,
            .thumbnail_jpeg = qbytes(person.thumbnail_jpeg),
            .regions_analyzed = person.regions_analyzed,
            .available_region_mask = person.available_region_mask,
        });
    }
    return projected;
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
    ffi_request.expected_variant_id = request.expected_variant_id.toStdString();
    ffi_request.settings = ffi_grade_stack(request.grade_stack);
    ffi_request.target_grade_node_index = request.target_grade_node_index;
    ffi_request.target_grade_node_id = request.target_grade_node_id.toStdString();
    ffi_request.target_mask_operation = request.target_mask_operation;
    ffi_request.invert = request.invert;
    ffi_request.semantic_query = request.semantic_query.toStdString();
    ffi_request.semantic_maximum_regions = request.semantic_maximum_regions;
    ffi_request.semantic_score_threshold_percent = request.semantic_score_threshold_percent;
    return edit_state(impl_->session->apply_subject_mask_proposal(
        photo_id.toStdString(),
        source_path.toStdString(),
        ffi_request
    ));
}

void DesktopBackend::discardSubjectMaskProposal(const std::uint64_t proposal_token) const {
    impl_->session->discard_subject_mask_proposal(proposal_token);
}
