#include "catalog_startup_recovery.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QUuid>
#include <QVector>

bool is_development_catalog_reset_error(const std::exception& error) {
    return QString::fromUtf8(error.what())
        .contains(QStringLiteral("development catalog reset required"));
}

QMessageBox::StandardButton
offer_development_catalog_reset(const std::exception& error, const QString& catalog_path) {
    const bool incompatible = is_development_catalog_reset_error(error);
    const QString detail = QObject::tr(
                               "Shadow could not open this catalog. Supported older catalogs are "
                               "upgraded automatically. "
                               "If another version created it, open a compatible Shadow build. "
                               "Your existing catalog has not been reset.\n\n"
                               "Application: %1\nCatalog: %2\nDetails: %3"
    )
                               .arg(
                                   QCoreApplication::applicationFilePath(),
                                   catalog_path,
                                   QString::fromUtf8(error.what())
                               );
    QMessageBox box(
        QMessageBox::Warning,
        QObject::tr("Could not open catalog"),
        detail,
        QMessageBox::Retry | QMessageBox::Cancel
    );
    if (incompatible) {
        box.addButton(QObject::tr("Create a new test catalog…"), QMessageBox::ResetRole);
    }
    box.setDefaultButton(QMessageBox::Cancel);
    box.setEscapeButton(QMessageBox::Cancel);
    box.exec();
    if (box.buttonRole(box.clickedButton()) != QMessageBox::ResetRole) {
        return box.standardButton(box.clickedButton());
    }
    return QMessageBox::question(
        nullptr,
        QObject::tr("Preserve catalog and start again?"),
        QObject::tr(
            "The existing catalog, its Library locations, edit history, people data, and cache "
            "will be kept in a recovery folder beside the catalog. A new empty test catalog "
            "will be created. Original photos are not changed."
        ),
        QMessageBox::Reset | QMessageBox::Cancel,
        QMessageBox::Cancel
    );
}

bool reset_local_development_catalog(
    const QString& catalog_path,
    const QString& cache_root,
    QString* error_message
) {
    const QDir root(QFileInfo(catalog_path).absolutePath());
    const QString recovery = root.filePath(
        QStringLiteral("catalog-recovery/%1-%2")
            .arg(
                QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzzZ")),
                QUuid::createUuid().toString(QUuid::Id128)
            )
    );
    if (!QDir().mkpath(recovery)) {
        *error_message = QObject::tr("Could not create the catalog recovery folder.");
        return false;
    }
    QStringList paths{
        catalog_path,
        catalog_path + QStringLiteral("-wal"),
        catalog_path + QStringLiteral("-shm"),
        root.filePath(QStringLiteral("people"))
    };
    // Remote preview mirrors survive a local Catalog reset. Keep their referenced
    // payloads available, while preserving local cache entries with the old Catalog.
    const QDir cache(cache_root);
    for (const auto& child :
         cache.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)) {
        if (child.fileName() != QStringLiteral("remote-library-previews"))
            paths.push_back(child.absoluteFilePath());
    }
    QVector<QPair<QString, QString>> moved;
    for (const auto& source : paths) {
        if (!QFileInfo::exists(source))
            continue;
        const bool cache_entry = QFileInfo(source).absolutePath() == cache.absolutePath();
        const QString destination = QDir(recovery).filePath(
            (cache_entry ? QStringLiteral("cache/") : QString()) + QFileInfo(source).fileName()
        );
        QDir().mkpath(QFileInfo(destination).absolutePath());
        if (!QDir().rename(source, destination)) {
            bool restored = true;
            for (auto i = moved.crbegin(); i != moved.crend(); ++i) {
                restored = QDir().rename(i->second, i->first) && restored;
            }
            *error_message =
                restored ? QObject::tr("Could not preserve %1. The existing catalog was retained.")
                               .arg(source)
                         : QObject::tr("Recovery could not finish. Preserved files remain in %1.")
                               .arg(recovery);
            return false;
        }
        moved.push_back({source, destination});
    }
    return true;
}
