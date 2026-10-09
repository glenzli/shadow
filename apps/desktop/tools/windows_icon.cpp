#include <QBuffer>
#include <QCoreApplication>
#include <QDataStream>
#include <QDebug>
#include <QImage>
#include <QSaveFile>

#include <array>
#include <cstdlib>

// Generate the Windows container from the canonical artwork in the build tree.
// Both the executable resource and Qt's window icon retain the same artwork.
int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
    if (arguments.size() != 3) {
        qCritical() << "Expected source PNG and output ICO paths";
        return EXIT_FAILURE;
    }
    const QImage source(arguments[1]);
    if (source.isNull() || source.width() != source.height()) {
        qCritical() << "Application artwork must be a readable square image";
        return EXIT_FAILURE;
    }

    constexpr std::array<int, 7> sizes{16, 24, 32, 48, 64, 128, 256};
    std::array<QByteArray, sizes.size()> frames;
    for (std::size_t index = 0; index < sizes.size(); ++index) {
        QBuffer buffer(&frames[index]);
        const auto frame = source.scaled(
            sizes[index], sizes[index], Qt::IgnoreAspectRatio, Qt::SmoothTransformation
        ).convertToFormat(QImage::Format_ARGB32);
        if (!buffer.open(QIODevice::WriteOnly) || !frame.save(&buffer, "PNG")) {
            qCritical() << "Cannot encode application icon frame";
            return EXIT_FAILURE;
        }
    }

    QSaveFile output(arguments[2]);
    if (!output.open(QIODevice::WriteOnly)) {
        qCritical() << "Cannot write Windows application icon";
        return EXIT_FAILURE;
    }
    QDataStream stream(&output);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint16{0} << quint16{1} << static_cast<quint16>(sizes.size());
    quint32 offset = 6U + 16U * static_cast<quint32>(sizes.size());
    for (std::size_t index = 0; index < sizes.size(); ++index) {
        const auto dimension = static_cast<quint8>(sizes[index] == 256 ? 0 : sizes[index]);
        const auto byte_count = static_cast<quint32>(frames[index].size());
        stream << dimension << dimension << quint8{0} << quint8{0}
               << quint16{1} << quint16{32} << byte_count << offset;
        offset += byte_count;
    }
    for (const auto& frame : frames) {
        if (output.write(frame) != frame.size()) {
            qCritical() << "Cannot write Windows application icon frame";
            return EXIT_FAILURE;
        }
    }
    if (stream.status() != QDataStream::Ok || !output.commit()) {
        qCritical() << "Cannot publish Windows application icon";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
