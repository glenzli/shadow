#include "edit_preview_provider.hpp"

#include "platform/edit_preview_texture_factory.hpp"

#include <QBuffer>
#include <QColorSpace>
#include <QImageReader>
#include <QQuickTextureFactory>
#include <QQuickWindow>
#include <QSGTexture>
#include <QUrlQuery>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace {

void release_byte_array(void* const owner) noexcept {
    delete static_cast<QByteArray*>(owner);
}

void release_frame_owner(void* const owner) noexcept {
    delete static_cast<std::shared_ptr<const BackendEditPreviewFrame>*>(owner);
}

[[nodiscard]] QImage rgb8_image(EditPreviewStore::Snapshot snapshot) {
    const bool valid_dimensions = snapshot.dimensions.isValid();
    const quint64 minimum_stride =
        valid_dimensions ? static_cast<quint64>(snapshot.dimensions.width()) * 3U : 0U;
    const quint64 expected_bytes = snapshot.row_stride_bytes > 0 && valid_dimensions
                                       ? static_cast<quint64>(snapshot.row_stride_bytes)
                                             * static_cast<quint64>(snapshot.dimensions.height())
                                       : 0U;
    std::span<const std::uint8_t> frame_pixels;
    if (snapshot.frame != nullptr) {
        try {
            frame_pixels = snapshot.frame->materializeRgb8();
        } catch (...) {
            return {};
        }
    }
    const quint64 actual_bytes = snapshot.frame ? static_cast<quint64>(frame_pixels.size())
                                                : static_cast<quint64>(snapshot.bytes.size());
    const bool valid_layout = valid_dimensions
                              && static_cast<quint64>(snapshot.row_stride_bytes) == minimum_stride
                              && snapshot.row_stride_bytes > 0 && expected_bytes == actual_bytes;
    if (!valid_layout || (snapshot.frame == nullptr && snapshot.bytes.isEmpty())
        || (snapshot.frame != nullptr
            && (snapshot.frame->dimensions() != snapshot.dimensions
                || snapshot.frame->rowStrideBytes()
                       != static_cast<std::size_t>(snapshot.row_stride_bytes)))) {
        return {};
    }

    const uchar* pixels = nullptr;
    QImageCleanupFunction cleanup = nullptr;
    void* cleanup_info = nullptr;
    if (snapshot.frame != nullptr) {
        pixels = frame_pixels.data();
        cleanup = release_frame_owner;
        cleanup_info =
            new std::shared_ptr<const BackendEditPreviewFrame>(std::move(snapshot.frame));
    } else {
        auto* const pixel_owner = new QByteArray(std::move(snapshot.bytes));
        pixels = reinterpret_cast<const uchar*>(pixel_owner->constData());
        cleanup = release_byte_array;
        cleanup_info = pixel_owner;
    }
    QImage image(
        pixels,
        snapshot.dimensions.width(),
        snapshot.dimensions.height(),
        snapshot.row_stride_bytes,
        QImage::Format_RGB888,
        cleanup,
        cleanup_info
    );
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}

[[nodiscard]] QImage alpha8_image(EditPreviewStore::MaskCoverageSnapshot snapshot) {
    const bool valid_dimensions = snapshot.dimensions.isValid() && !snapshot.dimensions.isEmpty();
    std::span<const std::uint8_t> frame_samples;
    if (snapshot.frame != nullptr) {
        const auto coverage = snapshot.frame->maskCoverage();
        if (!coverage.has_value() || coverage->dimensions != snapshot.dimensions
            || coverage->row_stride_bytes != static_cast<std::size_t>(snapshot.row_stride_bytes)) {
            return {};
        }
        frame_samples = coverage->samples;
    }
    const quint64 expected_bytes = valid_dimensions && snapshot.row_stride_bytes > 0
                                       ? static_cast<quint64>(snapshot.row_stride_bytes)
                                             * static_cast<quint64>(snapshot.dimensions.height())
                                       : 0U;
    const quint64 actual_bytes = snapshot.frame ? static_cast<quint64>(frame_samples.size())
                                                : static_cast<quint64>(snapshot.samples.size());
    if (!valid_dimensions || (snapshot.frame == nullptr && snapshot.samples.isEmpty())
        || snapshot.row_stride_bytes != snapshot.dimensions.width()
        || expected_bytes != actual_bytes) {
        return {};
    }
    const uchar* samples = nullptr;
    QImageCleanupFunction cleanup = nullptr;
    void* cleanup_info = nullptr;
    if (snapshot.frame != nullptr) {
        samples = frame_samples.data();
        cleanup = release_frame_owner;
        cleanup_info =
            new std::shared_ptr<const BackendEditPreviewFrame>(std::move(snapshot.frame));
    } else {
        auto* const sample_owner = new QByteArray(std::move(snapshot.samples));
        samples = reinterpret_cast<const uchar*>(sample_owner->constData());
        cleanup = release_byte_array;
        cleanup_info = sample_owner;
    }
    return QImage(
        samples,
        snapshot.dimensions.width(),
        snapshot.dimensions.height(),
        snapshot.row_stride_bytes,
        QImage::Format_Alpha8,
        cleanup,
        cleanup_info
    );
}

class RetainedEditPreviewTextureFactory final : public QQuickTextureFactory {
  public:
    explicit RetainedEditPreviewTextureFactory(
        QImage image,
        std::shared_ptr<const BackendEditPreviewFrame> frame
    ) : image_(std::move(image)), frame_(std::move(frame)) {}

    [[nodiscard]] QSGTexture* createTexture(QQuickWindow* const window) const override {
        return window != nullptr ? window->createTextureFromImage(image_) : nullptr;
    }

    [[nodiscard]] QSize textureSize() const override {
        return image_.size();
    }

    [[nodiscard]] int textureByteCount() const override {
        return static_cast<int>(
            std::min<qsizetype>(image_.sizeInBytes(), std::numeric_limits<int>::max())
        );
    }

    [[nodiscard]] QImage image() const override {
        // QQuickTextureFactory requires this escape hatch to own its storage
        // independently of both the factory and an external pixel buffer.
        return image_.copy();
    }

  private:
    QImage image_;
    std::shared_ptr<const BackendEditPreviewFrame> frame_;
};

struct OverviewRequest final {
    EditPreviewSlot slot = EditPreviewSlot::Current;
    quint64 generation = 0U;
};

[[nodiscard]] std::optional<OverviewRequest> parse_overview_request(const QString& id) {
    const qsizetype query_start = id.indexOf(QLatin1Char('?'));
    const QString slot_name = query_start >= 0 ? id.left(query_start) : id;
    EditPreviewSlot slot;
    if (slot_name == QStringLiteral("current")) {
        slot = EditPreviewSlot::Current;
    } else if (slot_name == QStringLiteral("before")) {
        slot = EditPreviewSlot::Before;
    } else {
        return std::nullopt;
    }
    const QUrlQuery query(query_start >= 0 ? id.mid(query_start + 1) : QString{});
    bool valid_generation = false;
    const quint64 generation =
        query.queryItemValue(QStringLiteral("generation")).toULongLong(&valid_generation);
    if (!valid_generation) {
        return std::nullopt;
    }
    return OverviewRequest{
        .slot = slot,
        .generation = generation,
    };
}

} // namespace

EditPreviewProvider::EditPreviewProvider(
    std::shared_ptr<EditPreviewStore> store,
    std::shared_ptr<EditPreviewPresentationContext> presentation_context
) :
    QQuickImageProvider(
        QQuickImageProvider::Texture,
        QQmlImageProviderBase::ForceAsynchronousImageLoading
    ),
    store_(std::move(store)), presentation_context_(std::move(presentation_context)) {}

QImage
EditPreviewProvider::requestImage(const QString& id, QSize* size, const QSize& requested_size) {
    return resolveImage(id, size, requested_size, nullptr);
}

QQuickTextureFactory*
EditPreviewProvider::requestTexture(const QString& id, QSize* size, const QSize& requested_size) {
    // Interactive overview frames cross the loading thread as immutable
    // descriptors. Native/QSG inspection and any compatibility readback are
    // deferred to createTexture() on Qt Quick's render thread.
    const auto overview = parse_overview_request(id);
    if (overview.has_value()) {
        const auto snapshot = store_->snapshot(overview->slot, overview->generation);
        if (snapshot.frame != nullptr && snapshot.row_stride_bytes > 0) {
            if (size != nullptr) {
                *size = snapshot.dimensions;
            }
            return makeEditPreviewTextureFactory(
                snapshot.frame,
                snapshot.presentation_binding,
                presentation_context_
            );
        }
    }

    std::shared_ptr<const BackendEditPreviewFrame> retained_frame;
    QImage image = resolveImage(id, size, requested_size, &retained_frame);
    if (image.isNull()) {
        return nullptr;
    }
    return new RetainedEditPreviewTextureFactory(std::move(image), std::move(retained_frame));
}

QImage EditPreviewProvider::resolveImage(
    const QString& id,
    QSize* size,
    const QSize& requested_size,
    std::shared_ptr<const BackendEditPreviewFrame>* const retained_frame
) const {
    if (retained_frame != nullptr) {
        retained_frame->reset();
    }
    const qsizetype query_start = id.indexOf(QLatin1Char('?'));
    const QString slot_name = query_start >= 0 ? id.left(query_start) : id;
    const QUrlQuery query(query_start >= 0 ? id.mid(query_start + 1) : QString{});
    if (slot_name.startsWith(QStringLiteral("scope/"))) {
        const QStringList scope_parts = slot_name.split(QLatin1Char('/'));
        if (scope_parts.size() != 3) {
            if (size != nullptr) {
                *size = {};
            }
            return {};
        }
        if (scope_parts.at(1) == QStringLiteral("mask")) {
            if (scope_parts.at(2) != QStringLiteral("current")) {
                if (size != nullptr) {
                    *size = {};
                }
                return {};
            }
            bool valid_photo = false;
            bool valid_recipe = false;
            bool valid_target = false;
            bool valid_component = false;
            bool valid_selection = false;
            bool valid_preview = false;
            const MaskCoverageGeneration generation{
                .photo = query.queryItemValue(QStringLiteral("photo")).toULongLong(&valid_photo),
                .recipe_revision =
                    query.queryItemValue(QStringLiteral("recipe")).toULongLong(&valid_recipe),
                .target_layer_index =
                    query.queryItemValue(QStringLiteral("target")).toUInt(&valid_target),
                .target_component_index =
                    query.queryItemValue(QStringLiteral("component")).toInt(&valid_component),
                .selection_revision =
                    query.queryItemValue(QStringLiteral("selection")).toULongLong(&valid_selection),
                .paired_preview_generation =
                    query.queryItemValue(QStringLiteral("preview")).toULongLong(&valid_preview),
            };
            if (!valid_photo || !valid_recipe || !valid_target || !valid_component
                || !valid_selection || !valid_preview) {
                if (size != nullptr) {
                    *size = {};
                }
                return {};
            }
            auto snapshot = store_->maskCoverageSnapshot(generation);
            if (retained_frame != nullptr) {
                *retained_frame = snapshot.frame;
            }
            QImage image = alpha8_image(std::move(snapshot));
            if (size != nullptr) {
                *size = image.size();
            }
            return image;
        }
        if (scope_parts.at(1) != QStringLiteral("zebra")) {
            if (size != nullptr) {
                *size = {};
            }
            return {};
        }
        EditPreviewSlot scope_slot;
        if (scope_parts.at(2) == QStringLiteral("current")) {
            scope_slot = EditPreviewSlot::Current;
        } else if (scope_parts.at(2) == QStringLiteral("before")) {
            scope_slot = EditPreviewSlot::Before;
        } else {
            if (size != nullptr) {
                *size = {};
            }
            return {};
        }
        bool valid_generation = false;
        const quint64 generation =
            query.queryItemValue(QStringLiteral("generation")).toULongLong(&valid_generation);
        if (!valid_generation) {
            if (size != nullptr) {
                *size = {};
            }
            return {};
        }
        const auto snapshot = store_->snapshot(scope_slot, generation);
        const QImage scope_image = snapshot.display_zebra;
        if (scope_image.isNull()) {
            if (size != nullptr) {
                *size = {};
            }
            return {};
        }
        if (size != nullptr) {
            *size = scope_image.size();
        }
        return scope_image;
    }
    if (slot_name.startsWith(QStringLiteral("detail/"))) {
        const QString ticket = slot_name.mid(7);
        bool valid_photo = false;
        bool valid_recipe = false;
        bool valid_viewport = false;
        const EditDetailGeneration generation{
            .photo = query.queryItemValue(QStringLiteral("photo")).toULongLong(&valid_photo),
            .recipe_revision =
                query.queryItemValue(QStringLiteral("recipe")).toULongLong(&valid_recipe),
            .viewport_revision =
                query.queryItemValue(QStringLiteral("viewport")).toULongLong(&valid_viewport),
        };
        if (ticket.isEmpty() || !valid_photo || !valid_recipe || !valid_viewport) {
            if (size != nullptr) {
                *size = {};
            }
            return {};
        }
        QImage image = rgb8_image(store_->detailSnapshot(ticket, generation));
        if (image.isNull()) {
            if (size != nullptr) {
                *size = {};
            }
            return {};
        }
        if (size != nullptr) {
            *size = image.size();
        }
        return image;
    }
    EditPreviewSlot slot;
    if (slot_name == QStringLiteral("current")) {
        slot = EditPreviewSlot::Current;
    } else if (slot_name == QStringLiteral("before")) {
        slot = EditPreviewSlot::Before;
    } else {
        if (size != nullptr) {
            *size = {};
        }
        return {};
    }
    bool valid_generation = false;
    const quint64 generation =
        query.queryItemValue(QStringLiteral("generation")).toULongLong(&valid_generation);
    if (!valid_generation) {
        if (size != nullptr) {
            *size = {};
        }
        return {};
    }
    auto snapshot = store_->snapshot(slot, generation);
    if (snapshot.bytes.isEmpty() && snapshot.frame == nullptr) {
        if (size != nullptr) {
            *size = {};
        }
        return {};
    }
    if (snapshot.row_stride_bytes > 0) {
        if (retained_frame != nullptr) {
            *retained_frame = snapshot.frame;
        }
        QImage image = rgb8_image(std::move(snapshot));
        if (size != nullptr) {
            *size = image.size();
        }
        return image;
    }

    QBuffer buffer;
    buffer.setData(snapshot.bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const QSize original_size = reader.size().isValid() ? reader.size() : snapshot.dimensions;
    if (requested_size.isValid() && original_size.isValid()) {
        reader.setScaledSize(original_size.scaled(requested_size, Qt::KeepAspectRatio));
    }
    QImage image = reader.read();
    if (size != nullptr) {
        *size = image.size();
    }
    return image;
}
