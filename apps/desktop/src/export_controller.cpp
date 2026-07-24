#include "export_controller.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QUuid>
#include <QtConcurrentRun>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

constexpr auto export_presets_settings_key = "export/presets_json";

[[nodiscard]] int checked_export_count(const qsizetype count) {
    if (count < 0 || count > std::numeric_limits<int>::max()) {
        throw std::length_error("export selection exceeds the desktop count limit");
    }
    return static_cast<int>(count);
}

struct ExportPhoto final {
    QString photo_id;
    QString source_path;
    QString title;
};

[[nodiscard]] QString safe_stem(const QString& requested, const QString& source_path) {
    QString stem = requested.trimmed();
    if (stem.isEmpty()) {
        stem = QFileInfo(source_path).completeBaseName();
    }
    stem.replace(
        QRegularExpression(QStringLiteral(R"([/\\:\*\?"<>\|])")),
        QStringLiteral("_")
    );
    stem = stem.trimmed();
    return stem.isEmpty() ? QStringLiteral("Shadow export") : stem;
}

[[nodiscard]] QString unique_destination(
    const QDir& folder,
    const QString& stem,
    const QString& suffix,
    const QString& extension
) {
    const QString base = stem + suffix;
    QString candidate = folder.filePath(base + QStringLiteral(".") + extension);
    for (int copy = 2; QFileInfo::exists(candidate); ++copy) {
        candidate = folder.filePath(
            QStringLiteral("%1-%2.%3").arg(base).arg(copy).arg(extension)
        );
    }
    return candidate;
}

[[nodiscard]] BackendExportOptions backend_options(const QVariantMap& values) {
    BackendExportOptions options;
    options.format = values.value(QStringLiteral("format"), QStringLiteral("jpeg"))
                         .toString()
                         .toLower();
    options.max_edge = static_cast<std::uint32_t>(
        std::clamp(values.value(QStringLiteral("maxEdge"), 0).toInt(), 0, 16'384)
    );
    options.jpeg_quality = static_cast<std::uint8_t>(
        std::clamp(values.value(QStringLiteral("quality"), 90).toInt(), 1, 100)
    );
    options.watermark_path =
        values.value(QStringLiteral("watermarkPath")).toString();
    if (options.watermark_path.startsWith(QStringLiteral("file:"))) {
        options.watermark_path = QUrl(options.watermark_path).toLocalFile();
    }
    options.watermark_opacity = std::clamp(
        values.value(QStringLiteral("watermarkOpacity"), 0.72).toDouble(),
        0.0,
        1.0
    );
    options.watermark_scale = std::clamp(
        values.value(QStringLiteral("watermarkScale"), 0.18).toDouble(),
        0.01,
        1.0
    );
    options.watermark_inset = std::clamp(
        values.value(QStringLiteral("watermarkInset"), 0.02).toDouble(),
        0.0,
        0.25
    );
    options.watermark_anchor =
        values.value(
            QStringLiteral("watermarkAnchor"),
            QStringLiteral("bottom-right")
        ).toString();
    return options;
}

[[nodiscard]] ExportTaskResult run_export(
    const std::shared_ptr<DesktopBackend>& backend,
    const QVector<ExportPhoto>& photos,
    const QString& destination_folder,
    const QVariantMap& values
) {
    ExportTaskResult result;
    result.requested = checked_export_count(photos.size());
    const QDir folder(destination_folder);
    const BackendExportOptions options = backend_options(values);
    const QString extension =
        options.format == QStringLiteral("png")
            ? QStringLiteral("png") : QStringLiteral("jpg");
    const QString suffix = values.value(QStringLiteral("filenameSuffix")).toString();
    for (const auto& photo : photos) {
        try {
            const QString destination = unique_destination(
                folder,
                safe_stem(photo.title, photo.source_path),
                suffix,
                extension
            );
            const auto receipt = backend->exportPhoto(
                photo.photo_id,
                photo.source_path,
                destination,
                options
            );
            result.destination_paths.push_back(receipt.destination_path);
            ++result.completed;
        } catch (const std::exception& error) {
            result.errors.push_back(
                QStringLiteral("%1 · %2")
                    .arg(photo.title, QString::fromUtf8(error.what()))
            );
            ++result.failed;
        }
    }
    return result;
}

} // namespace

ExportController::ExportController(
    std::shared_ptr<DesktopBackend> backend,
    const QString& isolated_settings_file,
    QObject* parent
)
    : QObject(parent),
      backend_(std::move(backend)),
      settings_(isolated_settings_file.isEmpty()
              ? std::make_unique<QSettings>()
              : std::make_unique<QSettings>(
                    isolated_settings_file,
                    QSettings::IniFormat
                )) {
    const QByteArray stored =
        settings_->value(QString::fromLatin1(export_presets_settings_key)).toByteArray();
    const auto parsed = QJsonDocument::fromJson(stored);
    presets_ = parsed.isArray() ? parsed.toVariant().toList() : defaultPresets();
    connect(
        &watcher_,
        &QFutureWatcher<ExportTaskResult>::finished,
        this,
        &ExportController::finishExport
    );
}

ExportController::~ExportController() {
    watcher_.waitForFinished();
}

bool ExportController::busy() const noexcept {
    return watcher_.isRunning();
}

QString ExportController::statusText() const {
    return status_text_;
}

int ExportController::completedCount() const noexcept {
    return completed_count_;
}

int ExportController::totalCount() const noexcept {
    return total_count_;
}

QVariantList ExportController::presets() const {
    return presets_;
}

void ExportController::startExport(
    const QVariantList& targets,
    const QUrl& destination_folder,
    const QVariantMap& options
) {
    if (busy()) {
        return;
    }
    const QString folder_path = destination_folder.toLocalFile();
    if (folder_path.isEmpty() || !QDir(folder_path).exists()) {
        status_text_ = tr("Choose an export folder");
        emit statusTextChanged();
        return;
    }
    QVector<ExportPhoto> photos;
    photos.reserve(targets.size());
    QSet<QString> seen;
    for (const auto& value : targets) {
        const QVariantMap target = value.toMap();
        const QString photo_id = target.value(QStringLiteral("photoId")).toString();
        const QString source_path =
            target.value(QStringLiteral("sourcePath")).toString();
        if (photo_id.isEmpty() || source_path.isEmpty() || seen.contains(photo_id)) {
            continue;
        }
        seen.insert(photo_id);
        photos.push_back({
            .photo_id = photo_id,
            .source_path = source_path,
            .title = target.value(QStringLiteral("title")).toString(),
        });
    }
    if (photos.isEmpty()) {
        status_text_ = tr("Select at least one photo to export");
        emit statusTextChanged();
        return;
    }
    completed_count_ = 0;
    total_count_ = checked_export_count(photos.size());
    status_text_ = tr("Preparing %1 photos…").arg(total_count_);
    emit progressChanged();
    emit statusTextChanged();
    emit busyChanged();
    watcher_.setFuture(QtConcurrent::run(
        run_export,
        backend_,
        photos,
        folder_path,
        options
    ));
}

QString ExportController::savePreset(
    const QString& name,
    const QVariantMap& options
) {
    const QString normalized_name = name.trimmed();
    if (normalized_name.isEmpty()) {
        return {};
    }
    QString id;
    for (const auto& value : presets_) {
        const QVariantMap preset = value.toMap();
        if (preset.value(QStringLiteral("name")).toString().compare(
                normalized_name,
                Qt::CaseInsensitive
            ) == 0) {
            id = preset.value(QStringLiteral("id")).toString();
            break;
        }
    }
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    const QVariantMap normalized =
        normalizedPreset(id, normalized_name, options);
    bool replaced = false;
    for (qsizetype index = 0; index < presets_.size(); ++index) {
        if (presets_[index].toMap().value(QStringLiteral("id")).toString() == id) {
            presets_[index] = normalized;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        presets_.push_back(normalized);
    }
    persistPresets();
    emit presetsChanged();
    return id;
}

void ExportController::removePreset(const QString& preset_id) {
    const auto before = presets_.size();
    presets_.erase(
        std::remove_if(
            presets_.begin(),
            presets_.end(),
            [&preset_id](const QVariant& value) {
                return value.toMap().value(QStringLiteral("id")).toString()
                    == preset_id;
            }
        ),
        presets_.end()
    );
    if (presets_.size() != before) {
        persistPresets();
        emit presetsChanged();
    }
}

void ExportController::finishExport() {
    const ExportTaskResult result = watcher_.result();
    completed_count_ = result.completed;
    total_count_ = result.requested;
    status_text_ = result.failed == 0
        ? tr("Exported %1 photos").arg(result.completed)
        : tr("Exported %1 photos · %2 failed")
              .arg(result.completed)
              .arg(result.failed);
    emit progressChanged();
    emit statusTextChanged();
    emit busyChanged();
    emit exportFinished(
        result.completed,
        result.failed,
        result.destination_paths
    );
}

void ExportController::persistPresets() {
    settings_->setValue(
        QString::fromLatin1(export_presets_settings_key),
        QJsonDocument::fromVariant(presets_).toJson(QJsonDocument::Compact)
    );
    settings_->sync();
}

QVariantList ExportController::defaultPresets() {
    return {
        normalizedPreset(
            QStringLiteral("builtin-full-jpeg"),
            tr("Full-size JPEG"),
            {
                {QStringLiteral("format"), QStringLiteral("jpeg")},
                {QStringLiteral("maxEdge"), 0},
                {QStringLiteral("quality"), 92},
            }
        ),
        normalizedPreset(
            QStringLiteral("builtin-web-jpeg"),
            tr("Web JPEG"),
            {
                {QStringLiteral("format"), QStringLiteral("jpeg")},
                {QStringLiteral("maxEdge"), 2560},
                {QStringLiteral("quality"), 86},
            }
        ),
        normalizedPreset(
            QStringLiteral("builtin-png"),
            tr("Full-size PNG"),
            {
                {QStringLiteral("format"), QStringLiteral("png")},
                {QStringLiteral("maxEdge"), 0},
                {QStringLiteral("quality"), 100},
            }
        ),
    };
}

QVariantMap ExportController::normalizedPreset(
    const QString& id,
    const QString& name,
    const QVariantMap& options
) {
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {
            QStringLiteral("format"),
            options.value(QStringLiteral("format"), QStringLiteral("jpeg"))
        },
        {
            QStringLiteral("maxEdge"),
            options.value(QStringLiteral("maxEdge"), 0)
        },
        {
            QStringLiteral("quality"),
            options.value(QStringLiteral("quality"), 90)
        },
        {
            QStringLiteral("filenameSuffix"),
            options.value(QStringLiteral("filenameSuffix"))
        },
        {
            QStringLiteral("watermarkPath"),
            options.value(QStringLiteral("watermarkPath"))
        },
        {
            QStringLiteral("watermarkOpacity"),
            options.value(QStringLiteral("watermarkOpacity"), 0.72)
        },
        {
            QStringLiteral("watermarkScale"),
            options.value(QStringLiteral("watermarkScale"), 0.18)
        },
        {
            QStringLiteral("watermarkInset"),
            options.value(QStringLiteral("watermarkInset"), 0.02)
        },
        {
            QStringLiteral("watermarkAnchor"),
            options.value(
                QStringLiteral("watermarkAnchor"),
                QStringLiteral("bottom-right")
            )
        },
    };
}
