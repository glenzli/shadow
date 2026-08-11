#include "folder_scan_backend.hpp"

#include "backend/native_path_input.hpp"
#include "shadow-desktop-bridge/src/lib.rs.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {

[[nodiscard]] QString qstring(const rust::String& value) {
    const auto length = std::min<std::size_t>(
        value.size(),
        static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())
    );
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(length));
}
[[nodiscard]] BackendScanPhase scan_phase(
    const shadow::desktop::FfiScanPhase phase
) {
    switch (phase) {
    case shadow::desktop::FfiScanPhase::Idle:
        return BackendScanPhase::Idle;
    case shadow::desktop::FfiScanPhase::Discovering:
        return BackendScanPhase::Discovering;
    case shadow::desktop::FfiScanPhase::PreparingPreviews:
        return BackendScanPhase::PreparingPreviews;
    case shadow::desktop::FfiScanPhase::Cancelling:
        return BackendScanPhase::Cancelling;
    case shadow::desktop::FfiScanPhase::Completed:
        return BackendScanPhase::Completed;
    case shadow::desktop::FfiScanPhase::Cancelled:
        return BackendScanPhase::Cancelled;
    case shadow::desktop::FfiScanPhase::Failed:
        return BackendScanPhase::Failed;
    }
    throw std::invalid_argument("unknown folder scan phase");
}

} // namespace

FolderScanBackend::FolderScanBackend(
    shadow::desktop::DesktopSession& session
) noexcept
    : session_(&session) {}

void FolderScanBackend::beginFolderScan(const std::uint64_t scan_id) const {
    session_->begin_folder_scan(scan_id);
}

BackendScanReport FolderScanBackend::scanFolder(
    const QString& folder_path,
    const std::uint64_t scan_id
) const {
    const auto source = session_->scan_folder(native_path_input::path(folder_path), scan_id);
    return {
        .folder_path = qstring(source.folder_path),
        .files_seen = source.files_seen,
        .supported_files = source.supported_files,
        .inserted = source.inserted,
        .unchanged = source.unchanged,
        .needs_revalidation = source.needs_revalidation,
        .decode_queued = source.decode_inspections_queued,
        .decode_completed = source.decode_inspections_completed,
        .decode_hard_failures = source.decode_hard_failures,
        .preview_failures = source.preview_failures,
        .decode_cancelled = source.decode_inspections_cancelled,
        .issue_count = source.issue_count,
        .cancelled = source.cancelled,
    };
}

BackendScanProgress FolderScanBackend::scanProgress(
    const std::uint64_t scan_id
) const {
    const auto source = session_->scan_progress(scan_id);
    return {
        .scan_id = source.scan_id,
        .update_sequence = source.update_sequence,
        .files_seen = source.files_seen,
        .supported_files = source.supported_files,
        .inserted = source.inserted,
        .unchanged = source.unchanged,
        .needs_revalidation = source.needs_revalidation,
        .decode_queued = source.decode_inspections_queued,
        .preview_artifacts_ready = source.preview_artifacts_published,
        .decode_completed = source.decode_inspections_completed,
        .decode_hard_failures = source.decode_hard_failures,
        .preview_failures = source.preview_failures,
        .decode_cancelled = source.decode_inspections_cancelled,
        .skipped = source.skipped,
        .issue_count = source.issue_count,
        .phase = scan_phase(source.phase),
        .valid = source.valid,
    };
}

bool FolderScanBackend::cancelFolderScan(
    const std::uint64_t scan_id
) const {
    return session_->cancel_folder_scan(scan_id);
}
