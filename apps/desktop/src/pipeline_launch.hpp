#pragma once

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <optional>

struct PipelinePhotoRequest final {
    QString input_path;
    QString output_path;
};

/// Immutable caller contract, or an interactive --isolate launch. No normal
/// Library state is consulted while resolving these paths.
struct PipelineLaunchRequest final {
    QString request_id;
    QVector<PipelinePhotoRequest> photos;
    QString result_path;
    QVariantMap export_options;
    bool interactive = false;
    bool legacy_single = false;
};

[[nodiscard]] std::optional<PipelineLaunchRequest>
parsePipelineLaunch(const QStringList& arguments, QString* error);

[[nodiscard]] bool writePipelineResult(
    const PipelineLaunchRequest& request,
    const QString& outcome,
    const QStringList& errors,
    const QSet<QString>& completed = {}
);
