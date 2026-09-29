#include "pipeline_launch.hpp"
#include "backend/export_settings_codec.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryFile>
#include <exception>
#include <filesystem>

namespace {
constexpr qsizetype max_photos = 256;

QString existingFile(const QString& value) {
    const QFileInfo file(value);
    return !value.isEmpty() && file.isFile() ? file.canonicalFilePath() : QString{};
}

QString newFile(const QString& value) {
    const QFileInfo file(value);
    const QString parent = file.dir().canonicalPath();
    if (value.isEmpty() || file.exists() || file.isSymLink() || parent.isEmpty()
        || file.fileName().isEmpty()) {
        return {};
    }
    return QDir(parent).filePath(file.fileName());
}

QString pathKey(const QString& path) {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // Fail closed even on a case-sensitive volume; portable requests must not
    // alias on the usual desktop filesystems.
    return path.toCaseFolded();
#else
    return path;
#endif
}
} // namespace

std::optional<PipelineLaunchRequest>
parsePipelineLaunch(const QStringList& arguments, QString* error) {
    if (error)
        error->clear();
    const bool direct = arguments.contains(QStringLiteral("--isolate"));
    const bool protocol = arguments.contains(QStringLiteral("--pipeline-edit"));
    if (!direct && !protocol)
        return std::nullopt;
    const auto fail = [error](const QString& message) -> std::optional<PipelineLaunchRequest> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (direct && protocol)
        return fail(QStringLiteral("choose --isolate or --pipeline-edit"));

    PipelineLaunchRequest request;
    request.interactive = direct;
    if (direct) {
        bool positional = false;
        QSet<QString> inputs;
        for (qsizetype i = 1; i < arguments.size(); ++i) {
            const QString& argument = arguments[i];
            if (!positional && argument == QStringLiteral("--isolate"))
                continue;
            if (!positional && argument == QStringLiteral("--")) {
                positional = true;
                continue;
            }
            if (!positional && argument.startsWith(QStringLiteral("--")))
                return fail(QStringLiteral("unknown isolated editor option: %1").arg(argument));
            const QString input = existingFile(argument);
            if (input.isEmpty())
                return fail(QStringLiteral("input must be an existing file: %1").arg(argument));
            if (!inputs.contains(pathKey(input))) {
                inputs.insert(pathKey(input));
                request.photos.push_back({input, {}});
            }
        }
        if (request.photos.size() > max_photos)
            return fail(QStringLiteral("an isolated session accepts at most 256 photos"));
        return request;
    }

    QMap<QString, QString> options;
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        const QString argument = arguments[i];
        if (argument == QStringLiteral("--pipeline-edit"))
            continue;
        const auto split = argument.indexOf(QLatin1Char('='));
        const QString key = split >= 0 ? argument.first(split) : argument;
        if ((key != QStringLiteral("--request") && key != QStringLiteral("--result"))
            || options.contains(key))
            return fail(QStringLiteral("unknown or duplicate pipeline option"));
        const QString value = split >= 0 ? argument.sliced(split + 1)
                                         : (++i < arguments.size() ? arguments[i] : QString{});
        options.insert(key, value);
    }
    const QString request_path = existingFile(options.value(QStringLiteral("--request")));
    request.result_path = newFile(options.value(QStringLiteral("--result")));
    if (request_path.isEmpty() || request.result_path.isEmpty())
        return fail(QStringLiteral(
            "--pipeline-edit requires an existing --request and a new --result file path"
        ));
    QFile file(request_path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
        return fail(QStringLiteral("pipeline request must be readable and at most 1 MiB"));
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
        return fail(QStringLiteral("pipeline request must be one JSON object"));
    const auto object = document.object();
    const QString schema = object.value(QStringLiteral("schema")).toString();
    request.legacy_single = schema == QStringLiteral("shadow-pipeline-edit-20260814.1");
    if (!request.legacy_single && schema != QStringLiteral("shadow-pipeline-edit-20260920.1"))
        return fail(QStringLiteral("unsupported pipeline request schema"));
    request.request_id = object.value(QStringLiteral("requestId")).toString();
    if (request.request_id.trimmed().isEmpty())
        return fail(QStringLiteral("pipeline requestId is required"));
    const QJsonArray photos = request.legacy_single
                                  ? QJsonArray{object}
                                  : object.value(QStringLiteral("photos")).toArray();
    if (photos.isEmpty() || photos.size() > max_photos)
        return fail(QStringLiteral("pipeline photos must contain 1 to 256 items"));
    QSet<QString> inputs;
    QSet<QString> outputs{pathKey(request.result_path), pathKey(request_path)};
    for (const auto& value : photos) {
        const auto photo = value.toObject();
        const QString input = existingFile(photo.value(QStringLiteral("input")).toString());
        const QString output = newFile(photo.value(QStringLiteral("output")).toString());
        if (input.isEmpty() || output.isEmpty())
            return fail(QStringLiteral("each input must exist and each output must be a new file"));
        if (inputs.contains(pathKey(input)) || outputs.contains(pathKey(output)))
            return fail(
                QStringLiteral("pipeline inputs, outputs and result must have distinct paths")
            );
        inputs.insert(pathKey(input));
        outputs.insert(pathKey(output));
        request.photos.push_back({input, output});
    }
    for (const auto& input : inputs) {
        if (outputs.contains(input))
            return fail(QStringLiteral("pipeline output aliases an input"));
    }
    if (object.contains(QStringLiteral("export"))
        && !object.value(QStringLiteral("export")).isObject())
        return fail(QStringLiteral("pipeline export must be an object"));
    request.export_options = object.value(QStringLiteral("export")).toObject().toVariantMap();
    try {
        const auto checked = ExportSettingsCodec::fromVariantMap(request.export_options);
        if (checked.format == QStringLiteral("dng"))
            return fail(
                QStringLiteral("isolated editing requires a rendered export: jpeg, png or tiff")
            );
    } catch (const std::exception& exception) {
        return fail(QString::fromUtf8(exception.what()));
    }
    return request;
}

bool writePipelineResult(
    const PipelineLaunchRequest& request,
    const QString& outcome,
    const QStringList& errors,
    const QSet<QString>& completed
) {
    if (request.interactive)
        return true;
    QJsonArray error_values;
    for (const auto& error : errors)
        error_values.append(error);
    QJsonObject result{
        {QStringLiteral("schema"),
         request.legacy_single ? QStringLiteral("shadow-pipeline-result-20260814.1")
                               : QStringLiteral("shadow-pipeline-result-20260920.1")},
        {QStringLiteral("requestId"), request.request_id},
        {QStringLiteral("outcome"), outcome},
        {QStringLiteral("errors"), error_values},
    };
    if (request.legacy_single) {
        result.insert(QStringLiteral("input"), request.photos.front().input_path);
        result.insert(QStringLiteral("output"), request.photos.front().output_path);
    } else {
        QJsonArray photos;
        for (const auto& photo : request.photos)
            photos.append(
                QJsonObject{
                    {QStringLiteral("input"), photo.input_path},
                    {QStringLiteral("output"), photo.output_path},
                    {QStringLiteral("outcome"),
                     completed.contains(photo.output_path)    ? QStringLiteral("completed")
                     : outcome == QStringLiteral("completed") ? QStringLiteral("failed")
                                                              : outcome},
                }
            );
        result.insert(QStringLiteral("photos"), photos);
    }
    // Same-directory staging plus an exclusive hard link publishes one complete
    // result without replacing a file created since request admission.
    QTemporaryFile staging(
        QFileInfo(request.result_path).dir().filePath(QStringLiteral(".shadow-result-XXXXXX"))
    );
    if (!staging.open())
        return false;
    const QByteArray bytes = QJsonDocument(result).toJson(QJsonDocument::Compact);
    if (staging.write(bytes) != bytes.size() || !staging.flush())
        return false;
    staging.close();
    std::error_code error;
    std::filesystem::create_hard_link(
        QFileInfo(staging.fileName()).filesystemFilePath(),
        QFileInfo(request.result_path).filesystemFilePath(),
        error
    );
    return !error;
}
