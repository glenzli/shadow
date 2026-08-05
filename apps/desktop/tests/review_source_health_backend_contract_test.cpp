#include "desktop_backend.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& contract) {
    if (!condition) {
        std::cerr << "source-health backend contract changed: " << contract << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void completed_scans_expose_missing_evidence_and_reject_a_wrong_relink() {
    QTemporaryDir root;
    require(root.isValid(), "temporary data root");
    const QString import_path = root.filePath(QStringLiteral("source"));
    const QString moved_path = root.filePath(QStringLiteral("moved"));
    require(QDir{}.mkpath(import_path), "source folder");
    require(QDir{}.mkpath(moved_path), "moved folder");

    const QString original = QDir(import_path).filePath(QStringLiteral("original.jpg"));
    const QString sibling = QDir(import_path).filePath(QStringLiteral("sibling.jpg"));
    const QString wrong_candidate = QDir(moved_path).filePath(QStringLiteral("wrong.jpg"));
    const QString recovered_original = QDir(moved_path).filePath(QStringLiteral("original.jpg"));
    const QString recovered_sibling = QDir(moved_path).filePath(QStringLiteral("sibling.jpg"));
    QImage original_image(16, 12, QImage::Format_RGB32);
    original_image.fill(qRgb(31, 83, 149));
    require(original_image.save(original, "JPEG", 95), "original JPEG");
    QImage sibling_image(16, 12, QImage::Format_RGB32);
    sibling_image.fill(qRgb(43, 119, 87));
    require(sibling_image.save(sibling, "JPEG", 95), "sibling JPEG");
    QImage wrong_image(16, 12, QImage::Format_RGB32);
    wrong_image.fill(qRgb(211, 47, 79));
    require(wrong_image.save(wrong_candidate, "JPEG", 95), "wrong candidate JPEG");

    const DesktopBackend backend(
        root.filePath(QStringLiteral("catalog.sqlite")),
        root.filePath(QStringLiteral("cache"))
    );
    backend.beginFolderScan(501);
    const BackendScanReport first = backend.scanFolder(import_path, 501);
    require(!first.cancelled, "initial scan completed");
    require(first.inserted == 2, "initial scan registered both originals");
    require(
        QFile::copy(original, recovered_original),
        "copy original into the user-selected replacement folder"
    );
    require(
        QFile::copy(sibling, recovered_sibling),
        "copy sibling into the user-selected replacement folder"
    );

    require(QFile::remove(original), "remove original before health scan");
    require(QFile::remove(sibling), "remove sibling before health scan");
    const BackendLibraryPhotoPage unavailable_page = backend.libraryPhotoPage(
        BackendLibraryPhotoFilter{},
        BackendLibraryPhotoOrder::FileNameAscending,
        BackendLibraryPhotoCursor{},
        8
    );
    require(unavailable_page.items.size() == 2, "both missing originals remain browseable");
    QString selected_location_id;
    QString selected_photo_id;
    for (const BackendReviewItem& item : unavailable_page.items) {
        if (item.source_path == original) {
            selected_location_id = item.location_id;
            selected_photo_id = item.photo_id;
            require(!item.source_available, "selected original is unavailable");
        }
    }
    require(
        !selected_location_id.isEmpty() && !selected_photo_id.isEmpty(),
        "the production browse projection exposes the selected missing source"
    );
    backend.beginFolderScan(502);
    const BackendScanReport second = backend.scanFolder(import_path, 502);
    require(!second.cancelled, "source-health scan completed");

    const QVector<BackendLibrarySourceHealth> health = backend.librarySourceHealth();
    require(health.size() == 1, "one configured source");
    require(health.constFirst().has_latest_completed_scan, "completed scan");
    require(health.constFirst().known_locations == 2, "known location count");
    require(health.constFirst().seen_locations == 0, "seen location count");
    require(health.constFirst().not_seen_locations == 2, "not-seen location count");
    require(!health.constFirst().scan_session_id.isEmpty(), "scan-scoped evidence identity");

    const BackendMissingSourceLocationPage page =
        backend.missingSourceLocationPage(health.constFirst().scan_session_id, {}, 24);
    require(page.has_scan, "missing page retains completed scan");
    require(page.items.size() == 2, "both missing locations are projected");
    require(!page.has_more, "two-item missing page is terminal");
    QString scan_selected_location_id;
    for (const BackendMissingSourceLocation& item : page.items) {
        if (item.source_display_path == original) {
            scan_selected_location_id = item.location_id;
        }
    }
    require(
        scan_selected_location_id == selected_location_id,
        "missing-location evidence preserves the selected historical path"
    );

    bool rejected = false;
    try {
        static_cast<void>(backend.relinkMissingSourceLocation(
            health.constFirst().scan_session_id,
            scan_selected_location_id,
            wrong_candidate
        ));
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "wrong complete-file identity was rejected");

    const BackendMissingSourceLocationPage unchanged =
        backend.missingSourceLocationPage(health.constFirst().scan_session_id, {}, 24);
    require(
        unchanged.has_scan && unchanged.items.size() == 2,
        "failed relink left historical evidence intact"
    );
    require(QFile::remove(wrong_candidate), "remove rejected candidate before folder scan");

    const BackendVerifiedSourceRelinkReceipt receipt =
        backend.relinkLibrarySourceLocation(scan_selected_location_id, moved_path);
    require(receipt.photo_id == selected_photo_id, "folder recovery preserved photo identity");
    require(
        receipt.display_path == QFileInfo(recovered_original).canonicalFilePath(),
        "folder recovery found the same-name original in the selected folder"
    );
    require(
        receipt.library_root_path == QFileInfo(moved_path).canonicalFilePath(),
        "selected folder became the recovery Library root"
    );

    bool replacement_source_adopted = false;
    const QVector<BackendLibrarySourceHealth> recovered_health = backend.librarySourceHealth();
    for (const BackendLibrarySourceHealth& source : recovered_health) {
        if (source.source_display_path == receipt.library_root_path) {
            replacement_source_adopted = true;
            break;
        }
    }
    require(
        replacement_source_adopted,
        "successful recovery added the selected folder to the Library"
    );

    backend.beginFolderScan(503);
    const BackendScanReport replacement_scan = backend.scanFolder(receipt.library_root_path, 503);
    require(!replacement_scan.cancelled, "replacement folder scan completed");
    require(
        replacement_scan.inserted == 0 && replacement_scan.unchanged == 2,
        "folder recovery attached both old photos before ordinary scanning (inserted="
            + std::to_string(replacement_scan.inserted)
            + ", unchanged=" + std::to_string(replacement_scan.unchanged)
            + ", needs_revalidation=" + std::to_string(replacement_scan.needs_revalidation) + ")"
    );

    const BackendLibraryPhotoPage recovered_page = backend.libraryPhotoPage(
        BackendLibraryPhotoFilter{},
        BackendLibraryPhotoOrder::FileNameAscending,
        BackendLibraryPhotoCursor{},
        8
    );
    require(recovered_page.items.size() == 2, "replacement scan kept exactly two logical photos");
    bool original_is_available = false;
    bool sibling_is_available = false;
    for (const BackendReviewItem& item : recovered_page.items) {
        if (item.photo_id == receipt.photo_id) {
            original_is_available =
                item.source_available && item.source_path == receipt.display_path;
        }
        if (item.source_path == QFileInfo(recovered_sibling).canonicalFilePath()) {
            sibling_is_available = item.source_available;
        }
    }
    require(
        original_is_available,
        "recovered original became available through the ordinary browse projection"
    );
    require(
        sibling_is_available,
        "same-directory sibling was recovered instead of imported as a new photo"
    );
}

void source_level_recovery_retires_an_unavailable_folder_without_duplicates() {
    QTemporaryDir root;
    require(root.isValid(), "source-recovery temporary root");
    const QString old_path = root.filePath(QStringLiteral("old-source"));
    const QString replacement_path = root.filePath(QStringLiteral("replacement-source"));
    require(QDir{}.mkpath(old_path), "old source folder");
    require(QDir{}.mkpath(replacement_path), "replacement source folder");
    const QString first = QDir(old_path).filePath(QStringLiteral("first.jpg"));
    const QString second = QDir(old_path).filePath(QStringLiteral("second.jpg"));
    const QString replacement_first = QDir(replacement_path).filePath(QStringLiteral("first.jpg"));
    const QString replacement_second =
        QDir(replacement_path).filePath(QStringLiteral("second.jpg"));
    QImage first_image(12, 9, QImage::Format_RGB32);
    first_image.fill(qRgb(19, 61, 113));
    QImage second_image(12, 9, QImage::Format_RGB32);
    second_image.fill(qRgb(151, 83, 37));
    require(first_image.save(first, "JPEG", 95), "first source JPEG");
    require(second_image.save(second, "JPEG", 95), "second source JPEG");
    require(QFile::copy(first, replacement_first), "copy first source replacement");
    require(QFile::copy(second, replacement_second), "copy second source replacement");

    const DesktopBackend backend(
        root.filePath(QStringLiteral("source-recovery.sqlite")),
        root.filePath(QStringLiteral("source-recovery-cache"))
    );
    backend.beginFolderScan(601);
    const BackendScanReport initial = backend.scanFolder(old_path, 601);
    require(initial.inserted == 2, "source-level initial scan");
    const QVector<BackendLibrarySourceHealth> initial_health = backend.librarySourceHealth();
    require(initial_health.size() == 1, "source-level initial source");
    const QString old_source_id = initial_health.constFirst().source_id;
    require(QDir(old_path).removeRecursively(), "disconnect entire old source");

    const BackendLibrarySourceRecoveryReceipt receipt =
        backend.recoverLibrarySource(old_source_id, replacement_path);
    require(receipt.recovered_photo_count == 2, "source-level recovered count");
    require(receipt.unresolved_photo_count == 0, "source-level unresolved count");
    require(receipt.retired_unavailable_source, "unavailable old source retired");
    require(
        receipt.library_root_path == QFileInfo(replacement_path).canonicalFilePath(),
        "source-level replacement root"
    );

    backend.beginFolderScan(602);
    const BackendScanReport replacement = backend.scanFolder(receipt.library_root_path, 602);
    require(
        replacement.inserted == 0 && replacement.unchanged == 2,
        "source-level replacement scan did not duplicate photos"
    );
    const QVector<BackendLibrarySourceHealth> final_health = backend.librarySourceHealth();
    require(final_health.size() == 1, "obsolete unavailable source removed from management");
    require(
        final_health.constFirst().source_display_path == receipt.library_root_path,
        "replacement source remains managed"
    );
}

void reconciliation_removes_only_completed_scan_absences() {
    QTemporaryDir root;
    require(root.isValid(), "reconciliation temporary root");
    const QString source_path = root.filePath(QStringLiteral("source"));
    require(QDir{}.mkpath(source_path), "reconciliation source folder");
    const QString available = QDir(source_path).filePath(QStringLiteral("available.jpg"));
    const QString missing = QDir(source_path).filePath(QStringLiteral("missing.jpg"));
    QImage available_image(10, 8, QImage::Format_RGB32);
    available_image.fill(qRgb(29, 139, 91));
    QImage missing_image(10, 8, QImage::Format_RGB32);
    missing_image.fill(qRgb(181, 47, 53));
    require(available_image.save(available, "JPEG", 95), "available reconciliation JPEG");
    require(missing_image.save(missing, "JPEG", 95), "missing reconciliation JPEG");

    const DesktopBackend backend(
        root.filePath(QStringLiteral("reconciliation.sqlite")),
        root.filePath(QStringLiteral("reconciliation-cache"))
    );
    backend.beginFolderScan(701);
    require(backend.scanFolder(source_path, 701).inserted == 2, "reconciliation initial scan");
    require(QFile::remove(missing), "remove reconciliation source");
    backend.beginFolderScan(702);
    const BackendScanReport checked = backend.scanFolder(source_path, 702);
    require(checked.unchanged == 1, "reconciliation check scan");
    const QVector<BackendLibrarySourceHealth> health = backend.librarySourceHealth();
    require(health.size() == 1, "reconciliation source health");
    require(health.constFirst().not_seen_locations == 1, "one completed-scan absence");

    const BackendSourceReconciliationReceipt receipt =
        backend.reconcileMissingSourcePhotos(health.constFirst().scan_session_id);
    require(receipt.reviewed == 1, "reconciliation reviewed count");
    require(receipt.archived == 1, "reconciliation archived count");
    require(receipt.retained_available == 0, "reconciliation retained count");
    require(
        backend.libraryPhotoCount(BackendLibraryPhotoFilter{}) == 1,
        "available photo remains after reconciliation"
    );
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    try {
        completed_scans_expose_missing_evidence_and_reject_a_wrong_relink();
        source_level_recovery_retires_an_unavailable_folder_without_duplicates();
        reconciliation_removes_only_completed_scan_absences();
    } catch (const std::exception& error) {
        std::cerr << "source-health production path failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
