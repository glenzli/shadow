#pragma once

#include <QString>

#include <cstdint>

namespace shadow::desktop {
struct DesktopSession;
}

struct BackendScanReport final {
    QString folder_path;
    std::uint64_t files_seen = 0;
    std::uint64_t supported_files = 0;
    std::uint64_t inserted = 0;
    std::uint64_t unchanged = 0;
    std::uint64_t needs_revalidation = 0;
    std::uint64_t decode_queued = 0;
    std::uint64_t decode_completed = 0;
    std::uint64_t decode_hard_failures = 0;
    std::uint64_t preview_failures = 0;
    std::uint64_t decode_cancelled = 0;
    std::uint64_t issue_count = 0;
    bool cancelled = false;
};

enum class BackendScanPhase : std::uint8_t {
    Idle,
    Discovering,
    PreparingPreviews,
    Cancelling,
    Completed,
    Cancelled,
    Failed,
};

struct BackendScanProgress final {
    std::uint64_t scan_id = 0;
    std::uint64_t update_sequence = 0;
    std::uint64_t files_seen = 0;
    std::uint64_t supported_files = 0;
    std::uint64_t inserted = 0;
    std::uint64_t unchanged = 0;
    std::uint64_t needs_revalidation = 0;
    std::uint64_t decode_queued = 0;
    std::uint64_t preview_artifacts_ready = 0;
    std::uint64_t decode_completed = 0;
    std::uint64_t decode_hard_failures = 0;
    std::uint64_t preview_failures = 0;
    std::uint64_t decode_cancelled = 0;
    std::uint64_t skipped = 0;
    std::uint64_t issue_count = 0;
    BackendScanPhase phase = BackendScanPhase::Idle;
    bool valid = false;
};

/// Owns the Qt/CXX boundary for one folder-import lifecycle: admission,
/// controlled discovery and preview preparation, progress projection,
/// cooperative cancellation, and terminal reporting.
class FolderScanBackend final {
public:
    explicit FolderScanBackend(
        shadow::desktop::DesktopSession& session
    ) noexcept;

    FolderScanBackend(const FolderScanBackend&) = delete;
    FolderScanBackend& operator=(const FolderScanBackend&) = delete;

    void beginFolderScan(std::uint64_t scan_id) const;
    [[nodiscard]] BackendScanReport scanFolder(
        const QString& folder_path,
        std::uint64_t scan_id
    ) const;
    [[nodiscard]] BackendScanProgress scanProgress(
        std::uint64_t scan_id
    ) const;
    [[nodiscard]] bool cancelFolderScan(std::uint64_t scan_id) const;

private:
    shadow::desktop::DesktopSession* session_;
};
