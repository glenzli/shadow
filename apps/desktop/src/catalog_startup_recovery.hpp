#pragma once

#include <QMessageBox>
#include <QString>
#include <exception>

[[nodiscard]] bool is_development_catalog_reset_error(const std::exception& error);
[[nodiscard]] QMessageBox::StandardButton
offer_development_catalog_reset(const std::exception& error, const QString& catalog_path);
// Preserve the complete prior catalog and its source locations before starting a test catalog.
[[nodiscard]] bool reset_local_development_catalog(
    const QString& catalog_path,
    const QString& cache_root,
    QString* error_message
);
