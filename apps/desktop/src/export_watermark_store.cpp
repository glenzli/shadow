#include "export_watermark_store.hpp"

#include <QJsonDocument>
#include <QSettings>
#include <QStringList>
#include <QUrl>
#include <QUuid>

#include <algorithm>

namespace {

constexpr auto export_watermarks_settings_key = "export/watermarks_json";

[[nodiscard]] QString normalized_path(const QVariantMap& definition) {
    QString path = definition.value(QStringLiteral("watermarkPath")).toString();
    if (path.startsWith(QStringLiteral("file:"))) {
        path = QUrl(path).toLocalFile();
    }
    return path.trimmed();
}

[[nodiscard]] QString normalized_anchor(const QVariantMap& definition) {
    const QString anchor = definition
        .value(QStringLiteral("watermarkAnchor"), QStringLiteral("bottom-right"))
        .toString();
    static const QStringList anchors{
        QStringLiteral("top-left"),
        QStringLiteral("top-center"),
        QStringLiteral("top-right"),
        QStringLiteral("middle-left"),
        QStringLiteral("middle-center"),
        QStringLiteral("middle-right"),
        QStringLiteral("bottom-left"),
        QStringLiteral("bottom-center"),
        QStringLiteral("bottom-right"),
    };
    return anchors.contains(anchor) ? anchor : QStringLiteral("bottom-right");
}

} // namespace

ExportWatermarkStore::ExportWatermarkStore(const QString& isolated_settings_file)
    : settings_(isolated_settings_file.isEmpty()
          ? std::make_unique<QSettings>()
          : std::make_unique<QSettings>(
                isolated_settings_file,
                QSettings::IniFormat
            )) {
    const auto parsed = QJsonDocument::fromJson(
        settings_->value(QString::fromLatin1(export_watermarks_settings_key))
            .toByteArray()
    );
    bool normalized_any = false;
    if (parsed.isArray()) {
        for (const QVariant& value : parsed.toVariant().toList()) {
            const QVariantMap stored = value.toMap();
            const QVariantMap normalized = normalize(
                stored.value(QStringLiteral("id")).toString(),
                stored.value(QStringLiteral("name")).toString(),
                stored
            );
            if (!normalized.value(QStringLiteral("id")).toString().isEmpty()
                && !normalized.value(QStringLiteral("name")).toString().isEmpty()
                && !normalized.value(QStringLiteral("watermarkPath")).toString().isEmpty()) {
                watermarks_.push_back(normalized);
            }
            normalized_any = normalized_any || normalized != stored;
        }
    }
    if (normalized_any) {
        persist();
    }
}

ExportWatermarkStore::~ExportWatermarkStore() = default;

const QVariantList& ExportWatermarkStore::watermarks() const noexcept {
    return watermarks_;
}

QString ExportWatermarkStore::save(
    const QString& name,
    const QVariantMap& definition
) {
    const QString normalized_name = name.trimmed();
    if (normalized_name.isEmpty() || normalized_path(definition).isEmpty()) {
        return {};
    }
    for (const QVariant& value : watermarks_) {
        const QVariantMap watermark = value.toMap();
        if (watermark.value(QStringLiteral("name")).toString().compare(
                normalized_name,
                Qt::CaseInsensitive
            ) == 0) {
            return update(
                watermark.value(QStringLiteral("id")).toString(),
                normalized_name,
                definition
            );
        }
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    watermarks_.push_back(normalize(id, normalized_name, definition));
    persist();
    return id;
}

QString ExportWatermarkStore::update(
    const QString& watermark_id,
    const QString& name,
    const QVariantMap& definition
) {
    const QString normalized_name = name.trimmed();
    if (watermark_id.isEmpty() || normalized_name.isEmpty()
        || normalized_path(definition).isEmpty()) {
        return {};
    }
    const bool name_conflicts = std::any_of(
        watermarks_.cbegin(),
        watermarks_.cend(),
        [&watermark_id, &normalized_name](const QVariant& value) {
            const QVariantMap watermark = value.toMap();
            return watermark.value(QStringLiteral("id")).toString() != watermark_id
                && watermark.value(QStringLiteral("name")).toString().compare(
                       normalized_name,
                       Qt::CaseInsensitive
                   ) == 0;
        }
    );
    if (name_conflicts) {
        return {};
    }
    for (QVariant& value : watermarks_) {
        if (value.toMap().value(QStringLiteral("id")).toString()
            != watermark_id) {
            continue;
        }
        value = normalize(watermark_id, normalized_name, definition);
        persist();
        return watermark_id;
    }
    return {};
}

bool ExportWatermarkStore::remove(const QString& watermark_id) {
    const auto before = watermarks_.size();
    watermarks_.erase(
        std::remove_if(
            watermarks_.begin(),
            watermarks_.end(),
            [&watermark_id](const QVariant& value) {
                return value.toMap().value(QStringLiteral("id")).toString()
                    == watermark_id;
            }
        ),
        watermarks_.end()
    );
    if (watermarks_.size() == before) {
        return false;
    }
    persist();
    return true;
}

void ExportWatermarkStore::persist() {
    settings_->setValue(
        QString::fromLatin1(export_watermarks_settings_key),
        QJsonDocument::fromVariant(watermarks_).toJson(QJsonDocument::Compact)
    );
    settings_->sync();
}

QVariantMap ExportWatermarkStore::normalize(
    const QString& id,
    const QString& name,
    const QVariantMap& definition
) {
    return {
        {QStringLiteral("id"), id.trimmed()},
        {QStringLiteral("name"), name.trimmed()},
        {QStringLiteral("watermarkPath"), normalized_path(definition)},
        {
            QStringLiteral("watermarkOpacity"),
            std::clamp(
                definition.value(QStringLiteral("watermarkOpacity"), 0.72).toDouble(),
                0.0,
                1.0
            )
        },
        {
            QStringLiteral("watermarkScale"),
            std::clamp(
                definition.value(QStringLiteral("watermarkScale"), 0.18).toDouble(),
                0.03,
                0.5
            )
        },
        {
            QStringLiteral("watermarkInset"),
            std::clamp(
                definition.value(QStringLiteral("watermarkInset"), 0.02).toDouble(),
                0.0,
                0.25
            )
        },
        {QStringLiteral("watermarkAnchor"), normalized_anchor(definition)},
    };
}
