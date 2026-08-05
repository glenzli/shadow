#include "library_source_quick_probe.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Library source quick-probe contract failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] bool touch(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("probe") == 5;
}

} // namespace

int main() {
    QTemporaryDir root;
    if (!require(root.isValid(), "temporary root is available")) {
        return EXIT_FAILURE;
    }
    QDir directory(root.path());
    if (!require(directory.mkpath(QStringLiteral("nested")), "nested folder is created")
        || !require(touch(root.filePath(QStringLiteral("one.NEF"))), "RAW fixture is created")
        || !require(
            touch(root.filePath(QStringLiteral("nested/two.jpeg"))),
            "raster fixture is created"
        )
        || !require(
            touch(root.filePath(QStringLiteral("nested/sidecar.xmp"))),
            "sidecar fixture is created"
        )) {
        return EXIT_FAILURE;
    }

    const LibrarySourceQuickProbe probe = probeLibrarySource(root.path());
    if (!require(probe.root_available, "existing root is available")
        || !require(probe.inventory_complete, "small inventory is complete")
        || !require(probe.supported_files == 2, "only supported photos are counted")
        || !require(
            !probeLibrarySource(root.filePath(QStringLiteral("missing"))).root_available,
            "missing root is unavailable"
        )
        || !require(isLibraryPhotoPath(QStringLiteral("photo.CR3")), "RAW suffix is supported")
        || !require(!isLibraryPhotoPath(QStringLiteral("photo.xmp")), "sidecar is excluded")) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
