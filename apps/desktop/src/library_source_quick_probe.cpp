#include "library_source_quick_probe.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>

#include <limits>

namespace {

// Keep this discovery-only list aligned with shadow-core's folder scanner.
// Decoder capability is deliberately irrelevant here: the probe answers
// whether the folder inventory changed, not whether every file can be opened.
[[nodiscard]] const QSet<QString>& library_photo_suffixes() {
    static const QSet<QString> suffixes{
        QStringLiteral("nef"),
        QStringLiteral("nrw"),
        QStringLiteral("cr2"),
        QStringLiteral("cr3"),
        QStringLiteral("arw"),
        QStringLiteral("raf"),
        QStringLiteral("orf"),
        QStringLiteral("rw2"),
        QStringLiteral("pef"),
        QStringLiteral("srw"),
        QStringLiteral("dng"),
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("tif"),
        QStringLiteral("tiff"),
        QStringLiteral("png"),
        QStringLiteral("heic"),
        QStringLiteral("heif"),
    };
    return suffixes;
}

} // namespace

bool isLibraryPhotoPath(const QString& path) {
    return library_photo_suffixes().contains(QFileInfo(path).suffix().toLower());
}

LibrarySourceQuickProbe probeLibrarySource(const QString& root_path) {
    const QFileInfo root(root_path);
    if (!root.isDir()) {
        return {};
    }

    LibrarySourceQuickProbe result{
        .root_available = true,
        .inventory_complete = true,
    };
    QDirIterator iterator(
        root.absoluteFilePath(),
        QDir::Files | QDir::NoSymLinks,
        QDirIterator::Subdirectories
    );
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        if (!isLibraryPhotoPath(path)) {
            continue;
        }
        if (result.supported_files == std::numeric_limits<std::uint64_t>::max()) {
            result.inventory_complete = false;
            break;
        }
        ++result.supported_files;
    }
    return result;
}
