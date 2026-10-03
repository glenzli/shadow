#pragma once

#include "backend/export_settings_codec.hpp"

#include <QString>
#include <QVector>

#include <cstdint>
#include <optional>

namespace shadow::desktop {
struct DesktopSession;
}

struct BackendExportReceipt final {
    QString destination_path;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t byte_length = 0;
    QString output_format;
    QString receipt_json;
};

/// A validated request before the bridge freezes its immutable job snapshot.
struct BackendDurableExportTarget final {
    QString photo_id;
    QString source_path;
    QString output_path;
    QString recipe_commit_id{};
    QString representation_id{};
};

/// One immutable work item claimed from the catalog-backed export queue. The
/// desktop shell may execute it, but cannot change its Recipe/source/output
/// snapshot. Raster formats use the frozen Recipe; RAW DNG preserves the
/// source CFA and deliberately ignores that Recipe.
struct BackendDurableExportItem final {
    QString item_id;
    QString job_id;
    QString photo_id;
    QString source_path;
    QString output_path;
    QString settings_json;
};

struct BackendDurableExportJob final {
    QString job_id;
    std::uint32_t item_count = 0;
};

struct BackendDurableExportRecovery final {
    std::uint32_t interrupted_items = 0;
    std::uint32_t requeued_items = 0;
    std::uint32_t queued_items = 0;
};

struct BackendDurableExportProgress final {
    std::uint32_t queued = 0;
    std::uint32_t active = 0;
    std::uint32_t completed = 0;
    std::uint32_t failed = 0;
    std::uint32_t cancelled = 0;
    std::uint32_t paused_conflict = 0;
    std::uint32_t total = 0;
};

/// Owns the Qt half of the durable export transaction on the application's
/// single long-lived Rust desktop session: queue recovery and claims, exact
/// render execution, raster watermark/encoding, conflict handling, atomic
/// publication, terminal completion, cancellation, and progress projection.
/// Rust owns RAW DNG staging/encoding/publication so CFA bytes never cross CXX.
class ExportBackend final {
  public:
    explicit ExportBackend(shadow::desktop::DesktopSession& session) noexcept;

    ExportBackend(const ExportBackend&) = delete;
    ExportBackend& operator=(const ExportBackend&) = delete;

    [[nodiscard]] BackendDurableExportJob enqueueDurableExportJob(
        const QVector<BackendDurableExportTarget>& targets,
        const QString& settings_json
    ) const;
    [[nodiscard]] BackendDurableExportRecovery recoverDurableExportQueue() const;
    [[nodiscard]] std::optional<BackendDurableExportItem> claimNextDurableExportItem() const;
    [[nodiscard]] BackendExportReceipt
    executeDurableExportItem(const BackendDurableExportItem& item) const;
    void cancelDurableExportJob(const QString& job_id) const;
    [[nodiscard]] BackendDurableExportProgress durableExportProgress(const QString& job_id) const;

  private:
    shadow::desktop::DesktopSession* session_;
};
