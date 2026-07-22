#include "lut_preview_provider.hpp"

#include <QCoreApplication>
#include <QColor>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

QByteArray cube(const bool invert) {
    QByteArray result("TITLE \"Preview test\"\nLUT_3D_SIZE 2\n");
    for (int blue = 0; blue < 2; ++blue) {
        for (int green = 0; green < 2; ++green) {
            for (int red = 0; red < 2; ++red) {
                const auto value = [invert](const int channel) {
                    return invert ? 1 - channel : channel;
                };
                result += QByteArray::number(value(red)) + ' '
                    + QByteArray::number(value(green)) + ' '
                    + QByteArray::number(value(blue)) + '\n';
            }
        }
    }
    return result;
}

void write_bytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
        "could not create test LUT");
    require(file.write(bytes) == bytes.size(), "could not write test LUT");
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QTemporaryDir root;
    require(root.isValid(), "could not create temporary test directory");

    const QString store = QDir(root.path()).filePath(QStringLiteral("store"));
    const QString cache = QDir(root.path()).filePath(QStringLiteral("cache"));
    require(QDir().mkpath(store), "could not create managed store");

    QImage reference(60, 40, QImage::Format_RGB32);
    reference.fill(QColor(48, 96, 192));
    const QString reference_path = QDir(root.path()).filePath(
        QStringLiteral("reference.png")
    );
    require(reference.save(reference_path), "could not save reference image");

    const QByteArray invert_cube = cube(true);
    const QString id = QString::fromLatin1(
        QCryptographicHash::hash(
            invert_cube,
            QCryptographicHash::Sha256
        ).toHex()
    );
    const QString managed_path = QDir(store).filePath(id + QStringLiteral(".cube"));
    write_bytes(managed_path, invert_cube);

    LutPreviewProvider provider(store, cache, reference_path);
    QSize rendered_size;
    const QImage rendered = provider.requestImage(id, &rendered_size, QSize(90, 60));
    require(!rendered.isNull(), "valid LUT preview was not rendered");
    require(rendered_size == QSize(90, 60), "preview did not honor requested size");
    require(QDir(cache).entryList({QStringLiteral("*.png")}, QDir::Files).size() == 1,
        "content-addressed preview was not cached");

    write_bytes(managed_path, cube(false));
    require(provider.requestImage(id, nullptr, {}).isNull(),
        "tampered managed LUT reused a stale cached preview");
    require(!provider.requestImage(QStringLiteral("original"), nullptr, {}).isNull(),
        "bundled reference preview was not available");
    return EXIT_SUCCESS;
}
