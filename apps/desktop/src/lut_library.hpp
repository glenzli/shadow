#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <memory>

class QSettings;
class QUrl;

/// Persistent application-wide catalog of user supplied `.cube` resources.
///
/// The catalog stores only source directories. Every scan derives immutable
/// content hashes and validates files through the image kernel before they are
/// offered to Grade Nodes.
class LutLibrary final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList directories READ directories NOTIFY libraryChanged)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY libraryChanged)
    Q_PROPERTY(QVariantList availableEntries READ availableEntries NOTIFY libraryChanged)
    Q_PROPERTY(int validCount READ validCount NOTIFY libraryChanged)
    Q_PROPERTY(int errorCount READ errorCount NOTIFY libraryChanged)

public:
    explicit LutLibrary(
        const QString& isolated_settings_file = {},
        const QString& managed_store_root = {},
        QObject* parent = nullptr
    );
    ~LutLibrary() override;

    LutLibrary(const LutLibrary&) = delete;
    LutLibrary& operator=(const LutLibrary&) = delete;
    LutLibrary(LutLibrary&&) = delete;
    LutLibrary& operator=(LutLibrary&&) = delete;

    [[nodiscard]] QStringList directories() const;
    [[nodiscard]] QVariantList entries() const;
    [[nodiscard]] QVariantList availableEntries() const;
    [[nodiscard]] int validCount() const noexcept;
    [[nodiscard]] int errorCount() const noexcept;

    Q_INVOKABLE bool addDirectory(const QUrl& directory_url);
    Q_INVOKABLE void removeDirectory(const QString& directory);
    Q_INVOKABLE void rescan();

signals:
    void libraryChanged();

private:
    void persistDirectories();

    std::unique_ptr<QSettings> settings_;
    QStringList directories_;
    QString managed_store_root_;
    QVariantList entries_;
    int valid_count_ = 0;
    int error_count_ = 0;
};
