#include "backend/desktop_backend_private.hpp"
#include "backend/edit_settings_projection.hpp"
#include "backend/rust_qt_projection.hpp"

#include <stdexcept>

namespace {

using desktop_backend_projection::edit_state;
using desktop_backend_projection::ffi_grade_stack;
using desktop_backend_projection::qbytes;
using desktop_backend_projection::qstring;

[[nodiscard]] BackendImageCompletionTerminal
completion_terminal(const shadow::desktop::FfiImageCompletionTerminal terminal) {
    switch (terminal) {
    case shadow::desktop::FfiImageCompletionTerminal::Staged:
        return BackendImageCompletionTerminal::Staged;
    case shadow::desktop::FfiImageCompletionTerminal::Unavailable:
        return BackendImageCompletionTerminal::Unavailable;
    case shadow::desktop::FfiImageCompletionTerminal::Cancelled:
        return BackendImageCompletionTerminal::Cancelled;
    case shadow::desktop::FfiImageCompletionTerminal::Failed:
        return BackendImageCompletionTerminal::Failed;
    default:
        throw std::runtime_error("image-completion provider returned an unknown terminal state");
    }
}

} // namespace

std::uint64_t DesktopBackend::beginImageCompletionJob() const {
    return impl_->session->begin_image_completion_job();
}

void DesktopBackend::cancelImageCompletionJob(
    const std::uint64_t image_completion_job_token
) const {
    impl_->session->cancel_image_completion_job(image_completion_job_token);
}

BackendImageCompletionResult DesktopBackend::executeImageCompletionJob(
    const QString& photo_id,
    const QString& source_path,
    const BackendImageCompletionRequest& request
) const {
    shadow::desktop::FfiImageCompletionRequest ffi_request;
    ffi_request.job_token = request.job_token;
    ffi_request.generation = request.generation;
    ffi_request.base_commit_id = request.base_commit_id.toStdString();
    ffi_request.settings = ffi_grade_stack(request.grade_stack);
    ffi_request.selection_expansion = request.selection_expansion;
    ffi_request.refresh_region_index = request.refresh_region_index;
    ffi_request.force_regenerate = request.force_regenerate;
    ffi_request.points.reserve(static_cast<std::size_t>(request.points.size()));
    for (const auto& point : request.points) {
        ffi_request.points.push_back({
            .x = point.x,
            .y = point.y,
            .radius = point.radius,
            .erase = point.erase,
            .stroke_id = point.stroke_id,
        });
    }
    const auto result = impl_->session->execute_image_completion_job(
        photo_id.toStdString(),
        source_path.toStdString(),
        ffi_request
    );
    return {
        .terminal = completion_terminal(result.terminal),
        .job_token = result.job_token,
        .generation = result.generation,
        .proposal_token = result.proposal_token,
        .detail = qstring(result.detail),
        .preview_width = result.preview_width,
        .preview_height = result.preview_height,
        .preview_rgba8 = qbytes(result.preview_rgba8),
    };
}

BackendPhotoEditState DesktopBackend::applyImageCompletionProposal(
    const QString& photo_id,
    const QString& source_path,
    const BackendImageCompletionApplyRequest& request
) const {
    shadow::desktop::FfiImageCompletionApplyRequest ffi_request;
    ffi_request.proposal_token = request.proposal_token;
    ffi_request.generation = request.generation;
    ffi_request.base_commit_id = request.base_commit_id.toStdString();
    ffi_request.expected_working_commit_id = request.expected_working_commit_id.toStdString();
    ffi_request.expected_variant_id = request.expected_variant_id.toStdString();
    ffi_request.settings = ffi_grade_stack(request.grade_stack);
    ffi_request.replace_region_index = request.replace_region_index;
    return edit_state(impl_->session->apply_image_completion_proposal(
        photo_id.toStdString(),
        source_path.toStdString(),
        ffi_request
    ));
}

void DesktopBackend::discardImageCompletionProposal(const std::uint64_t proposal_token) const {
    impl_->session->discard_image_completion_proposal(proposal_token);
}
