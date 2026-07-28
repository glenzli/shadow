#include "desktop_backend.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
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
        std::cerr << "source-health backend contract changed: "
                  << contract << '\n';
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

    const QString original =
        QDir(import_path).filePath(QStringLiteral("original.jpg"));
    const QString wrong_candidate =
        QDir(moved_path).filePath(QStringLiteral("wrong.jpg"));
    QImage original_image(16, 12, QImage::Format_RGB32);
    original_image.fill(qRgb(31, 83, 149));
    require(original_image.save(original, "JPEG", 95), "original JPEG");
    QImage wrong_image(16, 12, QImage::Format_RGB32);
    wrong_image.fill(qRgb(211, 47, 79));
    require(
        wrong_image.save(wrong_candidate, "JPEG", 95),
        "wrong candidate JPEG"
    );

    const DesktopBackend backend(
        root.filePath(QStringLiteral("catalog.sqlite")),
        root.filePath(QStringLiteral("cache"))
    );
    backend.beginFolderScan(501);
    const BackendScanReport first = backend.scanFolder(import_path, 501);
    require(!first.cancelled, "initial scan completed");
    require(first.inserted == 1, "initial scan registered one original");

    require(QFile::remove(original), "remove original before health scan");
    backend.beginFolderScan(502);
    const BackendScanReport second = backend.scanFolder(import_path, 502);
    require(!second.cancelled, "source-health scan completed");

    const QVector<BackendLibrarySourceHealth> health =
        backend.librarySourceHealth();
    require(health.size() == 1, "one configured source");
    require(health.constFirst().has_latest_completed_scan, "completed scan");
    require(
        health.constFirst().known_locations == 1,
        "known location count"
    );
    require(
        health.constFirst().seen_locations == 0,
        "seen location count"
    );
    require(
        health.constFirst().not_seen_locations == 1,
        "not-seen location count"
    );
    require(
        !health.constFirst().scan_session_id.isEmpty(),
        "scan-scoped evidence identity"
    );

    const BackendMissingSourceLocationPage page =
        backend.missingSourceLocationPage(
            health.constFirst().scan_session_id,
            {},
            24
        );
    require(page.has_scan, "missing page retains completed scan");
    require(page.items.size() == 1, "one missing location");
    require(!page.has_more, "single missing page is terminal");
    require(
        page.items.constFirst().source_display_path == original,
        "missing location preserves the historical path"
    );

    bool rejected = false;
    try {
        static_cast<void>(backend.relinkMissingSourceLocation(
            health.constFirst().scan_session_id,
            page.items.constFirst().location_id,
            wrong_candidate
        ));
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "wrong complete-file identity was rejected");

    const BackendMissingSourceLocationPage unchanged =
        backend.missingSourceLocationPage(
            health.constFirst().scan_session_id,
            {},
            24
        );
    require(
        unchanged.has_scan && unchanged.items.size() == 1,
        "failed relink left historical evidence intact"
    );
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    try {
        completed_scans_expose_missing_evidence_and_reject_a_wrong_relink();
    } catch (const std::exception& error) {
        std::cerr << "source-health production path failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
