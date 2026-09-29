#include "edit_display_scope_task.hpp"

#include <utility>

namespace {

[[nodiscard]] bool same_qualifier(
    const PreviewScopeHueQualifier& left,
    const PreviewScopeHueQualifier& right
) noexcept {
    return left.center_degrees == right.center_degrees
        && left.width_degrees == right.width_degrees
        && left.softness == right.softness;
}

} // namespace

EditDisplayScopeTaskResult run_edit_display_scope_task(EditDisplayScopeTaskInput input) {
    EditDisplayScopeTaskResult result{
        .kind = input.kind,
        .photo_generation = input.photo_generation,
        .preview_generation = input.preview_generation,
        .request_revision = input.request_revision,
        .reference_epoch = input.reference_epoch,
        .decoded_preview = input.decoded_preview.isNull()
            ? QImage::fromData(input.encoded_preview, "JPEG")
            : std::move(input.decoded_preview),
    };
    if (result.decoded_preview.isNull()) {
        return result;
    }
    if (!input.qualifier.has_value()) {
        result.scope = analyze_display_scope(result.decoded_preview);
        return result;
    }
    if (input.reference.has_value() && input.reference->available
        && same_qualifier(input.reference->qualifier, *input.qualifier)) {
        result.scope = analyze_display_scope(result.decoded_preview, *input.reference);
        if (result.scope.available) {
            result.reference = std::move(input.reference);
            return result;
        }
    }
    result.reference = capture_display_scope_reference(result.decoded_preview, *input.qualifier);
    result.scope = result.reference->available
        ? analyze_display_scope(result.decoded_preview, *result.reference)
        : analyze_display_scope(result.decoded_preview, input.qualifier);
    if (!result.reference->available) {
        result.reference.reset();
    }
    return result;
}
