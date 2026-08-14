#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <optional>

/// Caller-owned immutable request for one isolated pipeline-editor process.
/// Paths are resolved before any task-private runtime state is created.
struct PipelineLaunchRequest final {
    QString request_id;
    QString input_path;
    QString output_path;
    QString result_path;
    QVariantMap export_options;
};

/// Returns no value without an error when normal desktop startup was requested.
/// A non-empty error means `--pipeline-edit` was present but invalid.
[[nodiscard]] std::optional<PipelineLaunchRequest>
parsePipelineLaunch(const QStringList& arguments, QString* error);
