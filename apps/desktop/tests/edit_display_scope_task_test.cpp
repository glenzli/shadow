#include "edit_display_scope_task.hpp"

#include <QBuffer>
#include <QColor>

#include <cstdlib>
#include <iostream>

namespace {

[[nodiscard]] bool require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

} // namespace

int main() {
    QImage original(QSize(32, 32), QImage::Format_RGB888);
    original.fill(QColor(255, 0, 0));
    QByteArray encoded;
    QBuffer buffer(&encoded);
    if (!buffer.open(QIODevice::WriteOnly) || !original.save(&buffer, "JPEG", 100)) {
        return EXIT_FAILURE;
    }
    const PreviewScopeHueQualifier red{
        .center_degrees = 30.0,
        .width_degrees = 12.0,
        .softness = 0.0,
    };
    const EditDisplayScopeTaskResult first = run_edit_display_scope_task({
        .kind = EditPreviewKind::Current,
        .photo_generation = 4,
        .preview_generation = 8,
        .request_revision = 2,
        .reference_epoch = 3,
        .encoded_preview = encoded,
        .qualifier = red,
    });
    if (!require(first.scope.available, "JPEG scope is analyzed")
        || !require(first.reference.has_value(), "Point Color captures a frozen reference")
        || !require(first.scope.reference_selection, "first analysis uses captured samples")
        || !require(first.scope.matched_pixels == 1'024, "red samples are selected")
        || !require(first.decoded_preview.size() == original.size(), "decoded image is reusable")
        || !require(first.photo_generation == 4 && first.preview_generation == 8
                        && first.request_revision == 2 && first.reference_epoch == 3,
                    "result retains all acceptance generations")) {
        return EXIT_FAILURE;
    }

    QImage shifted = first.decoded_preview;
    shifted.fill(QColor(0, 0, 255));
    const EditDisplayScopeTaskResult frozen = run_edit_display_scope_task({
        .kind = EditPreviewKind::Current,
        .decoded_preview = shifted,
        .qualifier = red,
        .reference = first.reference,
    });
    const EditDisplayScopeTaskResult live = run_edit_display_scope_task({
        .kind = EditPreviewKind::Current,
        .decoded_preview = shifted,
        .qualifier = red,
    });
    const EditDisplayScopeTaskResult invalid = run_edit_display_scope_task({
        .kind = EditPreviewKind::NeutralBefore,
        .encoded_preview = QByteArrayLiteral("not a JPEG"),
    });
    if (!require(frozen.scope.reference_selection && frozen.scope.matched_pixels == 1'024,
                 "frozen scope follows its original spatial sample")
        || !require(live.scope.matched_pixels == 0,
                    "a new selector does not inherit the old reference")
        || !require(!invalid.scope.available && invalid.decoded_preview.isNull(),
                    "malformed preview cannot publish a scope")) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
