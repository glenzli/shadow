#include "pipeline_launch.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdlib>

namespace {

[[nodiscard]] bool writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

} // namespace

int main() {
    QTemporaryDir directory;
    if (!directory.isValid()) {
        return EXIT_FAILURE;
    }
    const QString input = directory.filePath(QStringLiteral("input.jpg"));
    const QString request = directory.filePath(QStringLiteral("request.json"));
    const QString result = directory.filePath(QStringLiteral("result.json"));
    const QString output = directory.filePath(QStringLiteral("output.jpg"));
    if (!writeFile(input, QByteArrayLiteral("not decoded by this parser"))) {
        return EXIT_FAILURE;
    }
    const QJsonObject request_object{
        {QStringLiteral("schema"), QStringLiteral("shadow-pipeline-edit-20260814.1")},
        {QStringLiteral("requestId"), QStringLiteral("pipeline-launch-contract")},
        {QStringLiteral("input"), input},
        {QStringLiteral("output"), output},
        {QStringLiteral("export"), QJsonObject{{QStringLiteral("format"), QStringLiteral("jpeg")}}},
    };
    if (!writeFile(request, QJsonDocument(request_object).toJson(QJsonDocument::Compact))) {
        return EXIT_FAILURE;
    }
    QString error;
    const auto parsed = parsePipelineLaunch(
        {QStringLiteral("Shadow"),
         QStringLiteral("--pipeline-edit"),
         QStringLiteral("--request"),
         request,
         QStringLiteral("--result"),
         result},
        &error
    );
    if (!parsed.has_value() || !error.isEmpty() || parsed->input_path != input
        || parsed->output_path != output || parsed->result_path != result) {
        return EXIT_FAILURE;
    }
    const auto normal = parsePipelineLaunch({QStringLiteral("Shadow")}, &error);
    if (normal.has_value() || !error.isEmpty()) {
        return EXIT_FAILURE;
    }
    const auto invalid = parsePipelineLaunch(
        {QStringLiteral("Shadow"),
         QStringLiteral("--pipeline-edit"),
         QStringLiteral("--request"),
         request},
        &error
    );
    return !invalid.has_value() && !error.isEmpty() ? EXIT_SUCCESS : EXIT_FAILURE;
}
