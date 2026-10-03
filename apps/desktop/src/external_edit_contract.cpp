#include "external_edit_contract.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <stdexcept>

namespace {
constexpr qint64 maximum_bytes = 64 * 1024 * 1024;
bool fields(const QJsonObject& value, const QSet<QString>& keys) {
    if (value.size() != keys.size())
        return false;
    for (auto it = value.begin(); it != value.end(); ++it)
        if (!keys.contains(it.key()))
            return false;
    return true;
}
bool text(const QJsonValue& value, const qsizetype maximum = 256) {
    return value.isString() && !value.toString().trimmed().isEmpty()
           && value.toString().size() <= maximum && !value.toString().contains(QChar::Null);
}
QString key(const QString& path) {
    const QFileInfo info(path);
    const auto resolved = info.exists()
                              ? info.canonicalFilePath()
                              : QDir(info.dir().canonicalPath()).filePath(info.fileName());
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    return resolved.toCaseFolded();
#else
    return resolved;
#endif
}
QJsonObject input(const ExternalEditBinding& binding) {
    return {
        {"path", binding.input_path},
        {"revision", binding.input_revision},
        {"sha256", binding.input_sha256}
    };
}
QString
readSource(const ExternalEditBinding& binding, QFile* copy, const std::atomic_bool* cancel) {
    QFile source(binding.input_path);
    if (!source.open(QIODevice::ReadOnly) || source.size() <= 0 || source.size() > maximum_bytes)
        throw std::runtime_error("input_unavailable: expected a readable PNG of at most 64 MiB");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 count = 0;
    while (!source.atEnd()) {
        if (cancel && cancel->load(std::memory_order_relaxed))
            return {};
        const auto bytes = source.read(256 * 1024);
        count += bytes.size();
        if (bytes.isEmpty() || count > maximum_bytes || source.error() != QFileDevice::NoError)
            throw std::runtime_error("input_unavailable: source read failed or exceeded its bound");
        hash.addData(bytes);
        if (copy && copy->write(bytes) != bytes.size())
            throw std::runtime_error("source_snapshot_failed: could not preserve input bytes");
    }
    const auto digest = QString::fromLatin1(hash.result().toHex());
    if (digest != binding.input_sha256)
        throw std::runtime_error("input_digest_mismatch: caller materialization changed");
    return digest;
}
} // namespace

std::optional<ExternalEditBinding> ExternalEditContract::parse(
    const QJsonObject& object,
    const QString& request_path,
    const QString& result_path,
    QString* error
) {
    const auto fail = [error](const char* message) -> std::optional<ExternalEditBinding> {
        if (error)
            *error = QString::fromLatin1(message);
        return std::nullopt;
    };
    const auto in = object.value("input").toObject();
    const auto out = object.value("output").toObject();
    if (!fields(object, {"schema", "requestId", "input", "output"})
        || !fields(in, {"path", "revision", "sha256"}) || !fields(out, {"path"})
        || !text(object.value("requestId")) || !text(in.value("revision"))
        || !text(in.value("path"), 4096) || !text(out.value("path"), 4096)
        || !in.value("sha256").isString()
        || !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$"))
                .match(in.value("sha256").toString())
                .hasMatch())
        return fail(
            "external edit requires exact requestId, input path/revision/sha256 and output path"
        );
    const QFileInfo source(in.value("path").toString()), destination(out.value("path").toString());
    if (!source.isAbsolute() || !source.isFile() || source.suffix().toLower() != "png"
        || !destination.isAbsolute() || destination.exists() || destination.isSymLink()
        || !destination.dir().exists() || destination.suffix().toLower() != "png")
        return fail(
            "external edit requires an existing absolute PNG input and a new absolute PNG output"
        );
    const QSet<QString> paths{
        key(source.filePath()),
        key(destination.filePath()),
        key(request_path),
        key(result_path)
    };
    if (paths.size() != 4)
        return fail("external edit paths must be distinct");
    return ExternalEditBinding{
        object.value("requestId").toString(),
        source.filePath(),
        in.value("revision").toString(),
        in.value("sha256").toString(),
        destination.filePath(),
        QUuid::createUuid().toString(QUuid::WithoutBraces),
        {}
    };
}
void ExternalEditContract::snapshotSource(
    const ExternalEditBinding& binding,
    const std::atomic_bool& cancelled
) {
    QFile copy(binding.private_source_path);
    if (binding.private_source_path.isEmpty()
        || !copy.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        throw std::runtime_error("source_snapshot_failed: private source cannot be created");
    const auto digest = readSource(binding, &copy, &cancelled);
    if (digest.isEmpty())
        return;
    if (!copy.flush() || !copy.setPermissions(QFileDevice::ReadOwner))
        throw std::runtime_error("source_snapshot_failed: private source cannot be finalized");
}
void ExternalEditContract::verifyOriginal(const ExternalEditBinding& binding) {
    (void)readSource(binding, nullptr, nullptr);
}
QJsonObject ExternalEditContract::result(
    const ExternalEditBinding& binding,
    const QString& outcome,
    const QStringList& errors,
    const std::optional<BackendExportReceipt>& receipt
) {
    QJsonArray failures;
    for (const auto& error : errors)
        failures.append(error);
    QJsonObject result{
        {"schema", "shadow.external-edit-result/1"},
        {"requestId", binding.request_id},
        {"input", input(binding)},
        {"outcome", outcome},
        {"errors", failures}
    };
    if (outcome == "completed" && receipt) {
        const auto render = QJsonDocument::fromJson(receipt->receipt_json.toUtf8()).object();
        result.insert(
            "artifact",
            QJsonObject{
                {"path", receipt->destination_path},
                {"sha256", render.value("sha256")},
                {"byteLength", QString::number(receipt->byte_length)},
                {"width", static_cast<int>(receipt->width)},
                {"height", static_cast<int>(receipt->height)},
                {"format", "png"},
                {"colorSpace", "sRGB"},
                {"bitDepth", 8}
            }
        );
        result.insert(
            "render",
            QJsonObject{
                {"sessionId", binding.session_id},
                {"photoId", render.value("photoId")},
                {"representationId", render.value("representationId")},
                {"recipeCommitId", render.value("recipeCommitId")},
                {"recipeSnapshotDigest", render.value("recipeSnapshotDigest")}
            }
        );
    }
    return result;
}
QJsonObject
ExternalEditContract::discovery(const ExternalEditBinding& binding, const QString& result_path) {
    return {
        {"schema", "shadow.external-edit/1"},
        {"requestId", binding.request_id},
        {"input", input(binding)},
        {"outputPath", binding.output_path},
        {"resultPath", result_path},
        {"complete", "save_export_return_candidate"},
        {"shutdown", "cancel_session"},
        {"exportCancellation", "unsupported"},
        {"inputClose", "detach_keep_human_window"}
    };
}
