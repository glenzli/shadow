#include "lut_preview_provider.hpp"

#include <shadow/image/lut_baking.hpp>
#include <shadow/image/lut.hpp>

#include <QByteArrayView>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <string_view>
#include <utility>

namespace {

constexpr int preview_width = 360;
constexpr int preview_height = 240;
constexpr qint64 maximum_cube_bytes = 16LL * 1'024LL * 1'024LL;
constexpr auto preview_renderer_version = "lut-preview-linear-srgb-v2";

[[nodiscard]] bool is_content_id(const QString& id) {
    if (id.size() != 64) {
        return false;
    }
    return std::ranges::all_of(id, [](const QChar character) {
        return character.isDigit()
            || (character >= QLatin1Char('a') && character <= QLatin1Char('f'));
    });
}

[[nodiscard]] QString rgba8888_hash(const QImage& image) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const qsizetype row_bytes = static_cast<qsizetype>(image.width()) * 4;
    for (int row = 0; row < image.height(); ++row) {
        hash.addData(QByteArrayView(
            reinterpret_cast<const char*>(image.constScanLine(row)),
            row_bytes
        ));
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QImage render_preview(
    const QImage& reference,
    const shadow::image::CubeLut3D& lut
) {
    QImage result = reference.copy();
    std::vector<float> samples;
    samples.reserve(static_cast<std::size_t>(result.width() * result.height()) * 3U);
    for (int row = 0; row < result.height(); ++row) {
        const auto* pixels = result.constScanLine(row);
        for (int column = 0; column < result.width(); ++column) {
            for (int channel = 0; channel < 3; ++channel) {
                samples.push_back(static_cast<float>(pixels[column * 4 + channel]) / 255.0F);
            }
        }
    }
    const auto rendered = shadow::image::render_cube_lut_reference(
        lut,
        {static_cast<std::uint32_t>(result.width()), static_cast<std::uint32_t>(result.height())},
        samples
    );
    for (int row = 0; row < result.height(); ++row) {
        auto* pixels = result.scanLine(row);
        const auto* rgb = rendered.bytes.data() + static_cast<std::size_t>(row) * rendered.row_stride_bytes;
        for (int column = 0; column < result.width(); ++column) {
            std::copy_n(rgb + column * 3, 3, pixels + column * 4);
        }
    }
    return result;
}

} // namespace

LutPreviewProvider::LutPreviewProvider(
    QString managed_store_root,
    QString preview_cache_root,
    QString reference_path
)
    : QQuickImageProvider(
          QQuickImageProvider::Image,
          QQmlImageProviderBase::ForceAsynchronousImageLoading
      ),
      managed_store_root_(QDir::cleanPath(std::move(managed_store_root))),
      preview_cache_root_(QDir::cleanPath(std::move(preview_cache_root))) {
    QImage source(std::move(reference_path));
    if (!source.isNull()) {
        if (!source.colorSpace().isValid()) {
            source.setColorSpace(QColorSpace::SRgb);
        }
        source.convertToColorSpace(QColorSpace::SRgb);
        reference_ = source.scaled(
            preview_width,
            preview_height,
            Qt::IgnoreAspectRatio,
            Qt::SmoothTransformation
        ).convertToFormat(QImage::Format_RGBA8888);
        reference_hash_ = rgba8888_hash(reference_);
    }
    QDir().mkpath(preview_cache_root_);
}

QImage LutPreviewProvider::requestImage(
    const QString& id,
    QSize* const size,
    const QSize& requested_size
) {
    if (size != nullptr) {
        *size = {};
    }
    if (reference_.isNull()) {
        return {};
    }

    const qsizetype query_start = id.indexOf(QLatin1Char('?'));
    const QString resource_id = query_start >= 0 ? id.left(query_start) : id;
    if (resource_id == QStringLiteral("original")) {
        const QImage result = scaledForRequest(reference_, requested_size);
        if (size != nullptr) {
            *size = result.size();
        }
        return result;
    }
    if (!is_content_id(resource_id)) {
        return {};
    }

    QFile lut_file(QDir(managed_store_root_).filePath(
        resource_id + QStringLiteral(".cube")
    ));
    if (!lut_file.open(QIODevice::ReadOnly)
        || lut_file.size() <= 0 || lut_file.size() > maximum_cube_bytes) {
        return {};
    }
    const QByteArray lut_bytes = lut_file.readAll();
    const QString actual_id = QString::fromLatin1(
        QCryptographicHash::hash(lut_bytes, QCryptographicHash::Sha256).toHex()
    );
    if (actual_id != resource_id) {
        return {};
    }

    QMutexLocker lock(&cache_mutex_);
    const QString cache_path = cachePath(resource_id);
    QImage rendered(cache_path);
    if (rendered.size() == reference_.size()) {
        rendered = rendered.convertToFormat(QImage::Format_RGBA8888);
    } else {
        try {
            const auto lut = shadow::image::parse_cube_lut(std::string_view(
                lut_bytes.constData(),
                static_cast<std::size_t>(lut_bytes.size())
            ));
            rendered = render_preview(reference_, lut);
        } catch (const std::exception&) {
            return {};
        }

        QSaveFile output(cache_path);
        if (!output.open(QIODevice::WriteOnly)
            || !rendered.save(&output, "PNG") || !output.commit()) {
            return {};
        }
    }

    const QImage result = scaledForRequest(rendered, requested_size);
    if (size != nullptr) {
        *size = result.size();
    }
    return result;
}

QImage LutPreviewProvider::scaledForRequest(
    const QImage& image,
    const QSize& requested_size
) const {
    if (!requested_size.isValid() || requested_size == image.size()) {
        return image;
    }
    return image.scaled(
        requested_size,
        Qt::KeepAspectRatio,
        Qt::SmoothTransformation
    );
}

QString LutPreviewProvider::cachePath(const QString& content_id) const {
    return QDir(preview_cache_root_).filePath(
        content_id + QLatin1Char('-') + reference_hash_.left(16)
            + QLatin1Char('-') + QString::fromLatin1(preview_renderer_version)
            + QStringLiteral(".png")
    );
}
