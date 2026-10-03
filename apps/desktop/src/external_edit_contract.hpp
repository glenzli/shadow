#pragma once

#include "backend/export_backend.hpp"
#include <QJsonObject>
#include <QStringList>
#include <atomic>
#include <optional>

/// Caller identities remain separate from Shadow's private Catalog and source.
struct ExternalEditBinding final {
    QString request_id, input_path, input_revision, input_sha256, output_path;
    QString session_id, private_source_path;
};
namespace ExternalEditContract {
[[nodiscard]] std::optional<ExternalEditBinding> parse(
    const QJsonObject& object,
    const QString& request_path,
    const QString& result_path,
    QString* error
);
// Called only on admission/export workers; never hashes material on the GUI thread.
void snapshotSource(const ExternalEditBinding& binding, const std::atomic_bool& cancelled);
void verifyOriginal(const ExternalEditBinding& binding);
[[nodiscard]] QJsonObject result(
    const ExternalEditBinding& binding,
    const QString& outcome,
    const QStringList& errors,
    const std::optional<BackendExportReceipt>& receipt
);
[[nodiscard]] QJsonObject discovery(const ExternalEditBinding& binding, const QString& result_path);
} // namespace ExternalEditContract
