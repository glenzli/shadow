#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <functional>

struct BackendLutExportOmission final {
    QString node_label;
    QString reason;
};

struct BackendLutExportResult final {
    QByteArray document;
    double maximum_absolute_error = 0.0;
    double root_mean_square_error = 0.0;
    std::uint32_t probe_count = 0;
    QString error;
};

/// Immutable value snapshot and its independent cancellable worker. Neither
/// callback retains an editor, Catalog, or source-image lifetime.
struct BackendLutExportSnapshot final {
    QStringList included_nodes;
    QVector<BackendLutExportOmission> omissions;
    bool can_bake = false;
    std::function<BackendLutExportResult(std::uint16_t)> bake;
    std::function<void()> cancel;
};
