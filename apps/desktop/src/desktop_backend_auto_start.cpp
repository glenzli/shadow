#include "backend/desktop_backend_private.hpp"
#include "backend/edit_settings_projection.hpp"
#include "backend/rust_qt_projection.hpp"
#include <stdexcept>
#include <vector>
namespace {
std::vector<shadow::desktop::FfiAutoStartMask>
bindings(const QVector<BackendAutoStartMask>& masks) {
    std::vector<shadow::desktop::FfiAutoStartMask> result;
    for (const auto& mask : masks)
        result.push_back({mask.proposal_token, mask.generation, mask.node_id.toStdString()});
    return result;
}
} // namespace
BackendEditedPreview DesktopBackend::renderAutoStartPreview(
    const QString& photo,
    const QString& source,
    const QString& base,
    const BackendGradeStack& stack,
    std::uint64_t token,
    const QVector<BackendAutoStartMask>& masks
) const {
    shadow::desktop::FfiEditPreviewRequest request;
    request.base_commit_id = base.toStdString();
    request.settings = desktop_backend_projection::ffi_grade_stack(stack);
    request.render_token = token;
    request.max_edge = 1024;
    request.jpeg_quality = 95;
    request.policy = shadow::desktop::FfiEditPreviewPolicy::Settled;
    request.use_working_recipe = true;
    auto candidates = bindings(masks);
    auto result = impl_->session->render_auto_start_preview(
        photo.toStdString(),
        source.toStdString(),
        request,
        rust::Slice<const shadow::desktop::FfiAutoStartMask>(candidates.data(), candidates.size())
    );
    if (result.terminal == shadow::desktop::FfiEditPreviewTerminal::Cancelled)
        return {.terminal = EditPreviewTerminal::Cancelled};
    if (result.terminal != shadow::desktop::FfiEditPreviewTerminal::Completed
        || result.row_stride_bytes != 0 || result.width == 0 || result.height == 0)
        throw std::runtime_error("Invalid automatic starting-point preview");
    return {
        .bytes = desktop_backend_projection::qbytes(result.bytes),
        .width = result.width,
        .height = result.height
    };
}
BackendPhotoEditState DesktopBackend::applyAutoStart(
    const QString& photo,
    const QString& source,
    const QString& base,
    const QString& expected,
    const QString& expected_variant,
    const BackendGradeStack& stack,
    const QVector<BackendAutoStartMask>& masks
) const {
    const auto settings = desktop_backend_projection::ffi_grade_stack(stack);
    auto candidates = bindings(masks);
    return desktop_backend_projection::edit_state(impl_->session->apply_auto_start(
        photo.toStdString(),
        source.toStdString(),
        base.toStdString(),
        expected.toStdString(),
        expected_variant.toStdString(),
        settings,
        rust::Slice<const shadow::desktop::FfiAutoStartMask>(candidates.data(), candidates.size())
    ));
}
