#include "edit_source_admission.hpp"

#include <QFile>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "edit source-admission contract failed: " << message << '\n';
    }
    return condition;
}

} // namespace

int main() {
    QTemporaryDir root;
    if (!require(root.isValid(), "temporary root is available")) {
        return EXIT_FAILURE;
    }
    const QString source_path = root.filePath(QStringLiteral("source.nef"));
    QFile source(source_path);
    if (!require(source.open(QIODevice::WriteOnly), "source fixture opens")
        || !require(source.write("raw") == 3, "source fixture writes")) {
        return EXIT_FAILURE;
    }
    source.close();

    if (!require(editSourceIsAvailable(source_path), "regular file is admitted")
        || !require(!editSourceIsAvailable(root.path()), "directory is rejected")
        || !require(
            !editSourceIsAvailable(root.filePath(QStringLiteral("missing.nef"))),
            "missing file is rejected"
        )
        || !require(!editSourceIsAvailable({}), "empty path is rejected")) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
