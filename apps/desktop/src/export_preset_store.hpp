#pragma once

#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

class QSettings;

class ExportPresetStore final {
public:
    explicit ExportPresetStore(const QString& isolated_settings_file = {});
    ~ExportPresetStore();

    ExportPresetStore(const ExportPresetStore&) = delete;
    ExportPresetStore& operator=(const ExportPresetStore&) = delete;

    [[nodiscard]] const QVariantList& presets() const noexcept;
    [[nodiscard]] QString save(
        const QString& name,
        const QVariantMap& options
    );
    [[nodiscard]] bool remove(const QString& preset_id);
    [[nodiscard]] bool retranslateBuiltins();

private:
    void persist();
    [[nodiscard]] static QVariantList defaultPresets();

    std::unique_ptr<QSettings> settings_;
    QVariantList presets_;
};
