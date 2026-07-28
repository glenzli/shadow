#include "export_preset_store.hpp"

#include "backend/export_settings_codec.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QSettings>
#include <QUuid>

#include <algorithm>

namespace {

constexpr auto export_presets_settings_key = "export/presets_json";

[[nodiscard]] QString translated_builtin_name(const QString& preset_id) {
    if (preset_id == QStringLiteral("builtin-full-jpeg")) {
        return QCoreApplication::translate(
            "ExportController",
            "Full-size JPEG"
        );
    }
    if (preset_id == QStringLiteral("builtin-web-jpeg")) {
        return QCoreApplication::translate(
            "ExportController",
            "Web JPEG"
        );
    }
    if (preset_id == QStringLiteral("builtin-png")) {
        return QCoreApplication::translate(
            "ExportController",
            "Full-size PNG"
        );
    }
    return {};
}

} // namespace

ExportPresetStore::ExportPresetStore(const QString& isolated_settings_file)
    : settings_(isolated_settings_file.isEmpty()
          ? std::make_unique<QSettings>()
          : std::make_unique<QSettings>(
                isolated_settings_file,
                QSettings::IniFormat
            )) {
    const QByteArray stored = settings_
        ->value(QString::fromLatin1(export_presets_settings_key))
        .toByteArray();
    const auto parsed = QJsonDocument::fromJson(stored);
    presets_ = parsed.isArray()
        ? parsed.toVariant().toList()
        : defaultPresets();
    (void)retranslateBuiltins();
}

ExportPresetStore::~ExportPresetStore() = default;

const QVariantList& ExportPresetStore::presets() const noexcept {
    return presets_;
}

QString ExportPresetStore::save(
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
    const QVariantMap normalized = ExportSettingsCodec::normalizedPreset(
        id,
        normalized_name,
        options
    );
    bool replaced = false;
    for (qsizetype index = 0; index < presets_.size(); ++index) {
        if (presets_[index]
                .toMap()
                .value(QStringLiteral("id"))
                .toString()
            == id) {
            presets_[index] = normalized;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        presets_.push_back(normalized);
    }
    persist();
    return id;
}

bool ExportPresetStore::remove(const QString& preset_id) {
    const auto before = presets_.size();
    presets_.erase(
        std::remove_if(
            presets_.begin(),
            presets_.end(),
            [&preset_id](const QVariant& value) {
                return value.toMap()
                        .value(QStringLiteral("id"))
                        .toString()
                    == preset_id;
            }
        ),
        presets_.end()
    );
    if (presets_.size() == before) {
        return false;
    }
    persist();
    return true;
}

bool ExportPresetStore::retranslateBuiltins() {
    bool changed = false;
    for (QVariant& value : presets_) {
        QVariantMap preset = value.toMap();
        const QString translated = translated_builtin_name(
            preset.value(QStringLiteral("id")).toString()
        );
        if (translated.isEmpty()
            || preset.value(QStringLiteral("name")).toString() == translated) {
            continue;
        }
        preset.insert(QStringLiteral("name"), translated);
        value = preset;
        changed = true;
    }
    if (changed) {
        persist();
    }
    return changed;
}

void ExportPresetStore::persist() {
    settings_->setValue(
        QString::fromLatin1(export_presets_settings_key),
        QJsonDocument::fromVariant(presets_).toJson(QJsonDocument::Compact)
    );
    settings_->sync();
}

QVariantList ExportPresetStore::defaultPresets() {
    return {
        ExportSettingsCodec::normalizedPreset(
            QStringLiteral("builtin-full-jpeg"),
            translated_builtin_name(QStringLiteral("builtin-full-jpeg")),
            {
                {QStringLiteral("format"), QStringLiteral("jpeg")},
                {QStringLiteral("maxEdge"), 0},
                {QStringLiteral("quality"), 92},
            }
        ),
        ExportSettingsCodec::normalizedPreset(
            QStringLiteral("builtin-web-jpeg"),
            translated_builtin_name(QStringLiteral("builtin-web-jpeg")),
            {
                {QStringLiteral("format"), QStringLiteral("jpeg")},
                {QStringLiteral("maxEdge"), 2560},
                {QStringLiteral("quality"), 86},
            }
        ),
        ExportSettingsCodec::normalizedPreset(
            QStringLiteral("builtin-png"),
            translated_builtin_name(QStringLiteral("builtin-png")),
            {
                {QStringLiteral("format"), QStringLiteral("png")},
                {QStringLiteral("maxEdge"), 0},
                {QStringLiteral("quality"), 100},
            }
        ),
    };
}
