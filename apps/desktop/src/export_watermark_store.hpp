#pragma once

#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

class QSettings;

// Owns the local library of reusable watermark definitions. Output Recipes
// still freeze the selected definition's exact rendering values.
class ExportWatermarkStore final {
public:
    explicit ExportWatermarkStore(const QString& isolated_settings_file = {});
    ~ExportWatermarkStore();

    ExportWatermarkStore(const ExportWatermarkStore&) = delete;
    ExportWatermarkStore& operator=(const ExportWatermarkStore&) = delete;

    [[nodiscard]] const QVariantList& watermarks() const noexcept;
    [[nodiscard]] QString save(
        const QString& name,
        const QVariantMap& definition
    );
    [[nodiscard]] QString update(
        const QString& watermark_id,
        const QString& name,
        const QVariantMap& definition
    );
    [[nodiscard]] bool remove(const QString& watermark_id);

private:
    void persist();
    [[nodiscard]] static QVariantMap normalize(
        const QString& id,
        const QString& name,
        const QVariantMap& definition
    );

    std::unique_ptr<QSettings> settings_;
    QVariantList watermarks_;
};
