#include "pipeline_launch.hpp"

#include "backend/export_settings_codec.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <exception>

namespace {

[[nodiscard]] QString optionValue(const QStringList& arguments, const QString& option) {
    const QString prefix = option + QStringLiteral("=");
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        if (arguments[index] == option) {
            return index + 1 < arguments.size() ? arguments[index + 1] : QString{};
        }
        if (arguments[index].startsWith(prefix)) {
            return arguments[index].sliced(prefix.size());
        }
    }
    return {};
}

[[nodiscard]] bool hasOption(const QStringList& arguments, const QString& option) {
    return arguments.contains(option)
           || std::any_of(arguments.cbegin(), arguments.cend(), [&option](const QString& value) {
                  return value.startsWith(option + QStringLiteral("="));
              });
}

[[nodiscard]] QString absoluteExistingFile(const QString& value) {
    const QFileInfo file(value);
    return file.isFile() ? file.absoluteFilePath() : QString{};
}

[[nodiscard]] QString absoluteNewFile(const QString& value) {
    const QFileInfo file(value);
    if (file.exists() || !file.dir().exists()) {
        return {};
    }
    return file.absoluteFilePath();
}

} // namespace

std::optional<PipelineLaunchRequest>
parsePipelineLaunch(const QStringList& arguments, QString* const error) {
    if (error != nullptr) {
        error->clear();
    }
    if (!hasOption(arguments, QStringLiteral("--pipeline-edit"))) {
        return std::nullopt;
    }
    const QString request_path =
        absoluteExistingFile(optionValue(arguments, QStringLiteral("--request")));
    const QString result_path = absoluteNewFile(optionValue(arguments, QStringLiteral("--result")));
    if (request_path.isEmpty() || result_path.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "--pipeline-edit requires an existing --request and a new --result file path"
            );
        }
        return std::nullopt;
    }
    QFile request_file(request_path);
    if (!request_file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("could not read pipeline request");
        }
        return std::nullopt;
    }
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(request_file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("pipeline request must be one JSON object");
        }
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("schema")).toString()
        != QStringLiteral("shadow-pipeline-edit-20260814.1")) {
        if (error != nullptr) {
            *error = QStringLiteral("unsupported pipeline request schema");
        }
        return std::nullopt;
    }
    const QString input_path =
        absoluteExistingFile(object.value(QStringLiteral("input")).toString());
    const QString output_path = absoluteNewFile(object.value(QStringLiteral("output")).toString());
    const QString request_id = object.value(QStringLiteral("requestId")).toString();
    if (request_id.isEmpty() || input_path.isEmpty() || output_path.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "pipeline requestId is required; input must exist and output must be a new file"
            );
        }
        return std::nullopt;
    }
    const QVariantMap export_options =
        object.value(QStringLiteral("export")).toObject().toVariantMap();
    try {
        const BackendExportOptions checked_options =
            ExportSettingsCodec::fromVariantMap(export_options);
        (void)checked_options;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = QString::fromUtf8(exception.what());
        }
        return std::nullopt;
    }
    return PipelineLaunchRequest{
        .request_id = request_id,
        .input_path = input_path,
        .output_path = output_path,
        .result_path = result_path,
        .export_options = export_options,
    };
}
