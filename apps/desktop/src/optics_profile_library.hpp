#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

/// A local, data-only library of reusable manual optical corrections.
///
/// Entries live in the application's profile directory as independent JSON
/// files. They are deliberately not decoder plugins: applying a profile copies
/// its resolved values into the photo Recipe, so a later profile edit cannot
/// alter an already edited photograph.
class OpticsProfileLibrary final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString rootDirectory READ rootDirectory CONSTANT)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY libraryChanged)
    Q_PROPERTY(int count READ count NOTIFY libraryChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY libraryChanged)

public:
    explicit OpticsProfileLibrary(QString root_directory, QObject* parent = nullptr);

    OpticsProfileLibrary(const OpticsProfileLibrary&) = delete;
    OpticsProfileLibrary& operator=(const OpticsProfileLibrary&) = delete;
    OpticsProfileLibrary(OpticsProfileLibrary&&) = delete;
    OpticsProfileLibrary& operator=(OpticsProfileLibrary&&) = delete;

    [[nodiscard]] QString rootDirectory() const;
    [[nodiscard]] QVariantList entries() const;
    [[nodiscard]] int count() const noexcept;
    [[nodiscard]] QString lastError() const;

    Q_INVOKABLE bool saveProfile(
        const QString& title,
        const QString& lens_label,
        const QVariantMap& corrections
    );
    Q_INVOKABLE bool removeProfile(const QString& profile_id);
    Q_INVOKABLE void rescan();

signals:
    void libraryChanged();

private:
    QString root_directory_;
    QVariantList entries_;
    QString last_error_;
};
