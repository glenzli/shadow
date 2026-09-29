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
    const QColor pixel = rendered.pixelColor(45, 30);
    // An inverted linear-light LUT is not 255 minus each encoded channel.
    // These independently calculated sRGB values catch a missing transfer decode.
    require(std::abs(pixel.red() - 252) <= 1 && std::abs(pixel.green() - 241) <= 1
            && std::abs(pixel.blue() - 183) <= 1,
        "LUT preview did not evaluate in linear sRGB");
    require(QDir(cache).entryList({QStringLiteral("*.png")}, QDir::Files).size() == 1,
        "content-addressed preview was not cached");

    write_bytes(managed_path, cube(false));
    require(provider.requestImage(id, nullptr, {}).isNull(),
        "tampered managed LUT reused a stale cached preview");
    require(!provider.requestImage(QStringLiteral("original"), nullptr, {}).isNull(),
        "bundled reference preview was not available");

    const QByteArray identity = cube(false);
    const QString identity_id = QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
    write_bytes(QDir(store).filePath(identity_id + QStringLiteral(".cube")), identity);
    const QColor identity_pixel = provider.requestImage(identity_id, nullptr, {}).pixelColor(0, 0);
    require(std::abs(identity_pixel.red() - 48) <= 1
            && std::abs(identity_pixel.green() - 96) <= 1
            && std::abs(identity_pixel.blue() - 192) <= 1,
        "identity LUT changed the display reference");
    return EXIT_SUCCESS;
}
