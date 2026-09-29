#include "backend/desktop_backend_private.hpp"
#include "backend/edit_settings_projection.hpp"
#include "backend/rust_qt_projection.hpp"

#include <memory>

namespace {
struct ExportOwner final {
    rust::Box<shadow::desktop::LutExportPlan> plan;
    rust::Box<shadow::desktop::LutExportCancellation> cancellation;
};
} // namespace

BackendLutExportSnapshot DesktopBackend::prepareLutExport(
    const BackendGradeStack& grade_stack, const QString& selected_node_id
) const {
    using desktop_backend_projection::ffi_grade_stack;
    using desktop_backend_projection::qbytes;
    using desktop_backend_projection::qstring;
    auto owner = std::make_shared<ExportOwner>(ExportOwner{
        shadow::desktop::prepare_lut_export(ffi_grade_stack(grade_stack), selected_node_id.toStdString()),
        shadow::desktop::new_lut_export_cancellation(),
    });
    const auto preview = shadow::desktop::lut_export_preview(*owner->plan);
    BackendLutExportSnapshot snapshot;
    snapshot.can_bake = preview.can_bake;
    for (const auto& label : preview.included_nodes) {
        snapshot.included_nodes.append(qstring(label));
    }
    for (const auto& omission : preview.omissions) {
        snapshot.omissions.append({qstring(omission.node_label), qstring(omission.reason)});
    }
    snapshot.bake = [owner](std::uint16_t size) {
        const auto result = shadow::desktop::bake_lut_export(*owner->plan, size, *owner->cancellation);
        return BackendLutExportResult{
            .document = qbytes(result.document),
            .maximum_absolute_error = result.maximum_absolute_error,
            .root_mean_square_error = result.root_mean_square_error,
            .probe_count = result.probe_count,
        };
    };
    snapshot.cancel = [owner] { shadow::desktop::cancel_lut_export(*owner->cancellation); };
    return snapshot;
}
