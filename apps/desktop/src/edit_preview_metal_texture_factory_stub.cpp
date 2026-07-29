#include "edit_preview_metal_texture_factory.hpp"

#include <QColorSpace>
#include <QImage>
#include <QQuickWindow>
#include <QSGTexture>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <utility>

namespace {

struct AtomicEditPreviewTextureTelemetry final {
    std::atomic<std::uint64_t> native_factory_count{0U};
    std::atomic<std::uint64_t> native_request_count{0U};
    std::atomic<std::uint64_t> native_request_bytes{0U};
    std::atomic<std::uint64_t> native_import_count{0U};
    std::atomic<std::uint64_t> native_import_bytes{0U};
    std::atomic<std::uint64_t> host_materialization_calls{0U};
    std::atomic<std::uint64_t> host_materialization_bytes{0U};
    std::atomic<std::uint64_t> cpu_upload_bytes{0U};
    std::atomic<std::uint64_t> last_resource_id{0U};
    std::atomic<std::uint64_t> last_epoch{0U};
    std::array<std::atomic<std::uint64_t>, EDIT_PREVIEW_TEXTURE_FALLBACK_REASON_COUNT>
        fallback_by_reason{};
};

[[nodiscard]] AtomicEditPreviewTextureTelemetry& telemetry() noexcept {
    static AtomicEditPreviewTextureTelemetry value;
    return value;
}

[[nodiscard]] constexpr std::size_t
fallback_index(const EditPreviewTextureFallbackReason reason) noexcept {
    return static_cast<std::size_t>(reason);
}

void record_fallback(const EditPreviewTextureFallbackReason reason) noexcept {
    const auto index = fallback_index(reason);
    if (index < EDIT_PREVIEW_TEXTURE_FALLBACK_REASON_COUNT) {
        telemetry().fallback_by_reason[index].fetch_add(1U, std::memory_order_relaxed);
    }
}

[[nodiscard]] std::uint64_t checked_rgb8_bytes(const BackendEditPreviewFrame& frame) noexcept {
    const QSize dimensions = frame.dimensions();
    if (!dimensions.isValid() || dimensions.isEmpty()) {
        return 0U;
    }
    const auto stride = static_cast<std::uint64_t>(frame.rowStrideBytes());
    const auto height = static_cast<std::uint64_t>(dimensions.height());
    if (stride == 0U || height > std::numeric_limits<std::uint64_t>::max() / stride) {
        return 0U;
    }
    return stride * height;
}

class EditPreviewTextureFactory final : public QQuickTextureFactory {
  public:
    EditPreviewTextureFactory(
        std::shared_ptr<const BackendEditPreviewFrame> frame,
        const EditPreviewPresentationBinding presentation_binding,
        std::shared_ptr<EditPreviewPresentationContext> presentation_context
    ) :
        frame_(std::move(frame)), presentation_context_(std::move(presentation_context)),
        dimensions_(frame_ ? frame_->dimensions() : QSize{}) {
        if (frame_ != nullptr
            && frame_->storageKind() == BackendEditPreviewStorage::AppleMetalRgba8Srgb) {
            telemetry().native_factory_count.fetch_add(1U, std::memory_order_relaxed);
            telemetry().last_epoch.store(presentation_binding.epoch, std::memory_order_relaxed);
            const auto native = frame_->appleMetalTexture();
            if (native.has_value()) {
                telemetry().last_resource_id.store(native->resource_id, std::memory_order_relaxed);
            }
        }
    }

    [[nodiscard]] QSGTexture* createTexture(QQuickWindow* const window) const override {
        if (window == nullptr) {
            record_fallback(EditPreviewTextureFallbackReason::NullWindow);
            return nullptr;
        }
        if (!window->isSceneGraphInitialized()) {
            record_fallback(EditPreviewTextureFallbackReason::SceneGraphUninitialized);
            return nullptr;
        }
        if (frame_ == nullptr) {
            record_fallback(EditPreviewTextureFallbackReason::InvalidNativeDescriptor);
            return nullptr;
        }
        const auto reason = frame_->storageKind() == BackendEditPreviewStorage::AppleMetalRgba8Srgb
                                ? EditPreviewTextureFallbackReason::WrongApi
                                : EditPreviewTextureFallbackReason::HostStorage;
        if (frame_->storageKind() == BackendEditPreviewStorage::AppleMetalRgba8Srgb) {
            telemetry().native_request_count.fetch_add(1U, std::memory_order_relaxed);
        }
        record_fallback(reason);
        const QImage image = materializedImage();
        if (image.isNull()) {
            return nullptr;
        }
        try {
            QSGTexture* const texture = window->createTextureFromImage(image);
            if (texture == nullptr) {
                record_fallback(EditPreviewTextureFallbackReason::CpuUploadFailed);
                return nullptr;
            }
            telemetry().cpu_upload_bytes.fetch_add(
                static_cast<std::uint64_t>(image.sizeInBytes()),
                std::memory_order_relaxed
            );
            return texture;
        } catch (...) {
            record_fallback(EditPreviewTextureFallbackReason::CpuUploadFailed);
            return nullptr;
        }
    }

    [[nodiscard]] QSize textureSize() const override {
        return dimensions_;
    }

    [[nodiscard]] int textureByteCount() const override {
        const auto bytes = frame_ != nullptr ? checked_rgb8_bytes(*frame_) : 0U;
        return static_cast<int>(std::min<std::uint64_t>(
            bytes,
            static_cast<std::uint64_t>(std::numeric_limits<int>::max())
        ));
    }

    [[nodiscard]] QImage image() const override {
        return materializedImage();
    }

  private:
    [[nodiscard]] QImage materializedImage() const noexcept {
        if (frame_ == nullptr) {
            record_fallback(EditPreviewTextureFallbackReason::MaterializationFailed);
            return {};
        }
        telemetry().host_materialization_calls.fetch_add(1U, std::memory_order_relaxed);
        try {
            const auto pixels = frame_->materializeRgb8();
            const auto expected_bytes = checked_rgb8_bytes(*frame_);
            const auto tight_row_bytes = static_cast<std::size_t>(dimensions_.width()) * 3U;
            if (expected_bytes == 0U || frame_->rowStrideBytes() != tight_row_bytes
                || static_cast<std::uint64_t>(pixels.size()) != expected_bytes) {
                record_fallback(EditPreviewTextureFallbackReason::MaterializationFailed);
                return {};
            }
            QImage image(dimensions_, QImage::Format_RGB888);
            if (image.isNull()) {
                record_fallback(EditPreviewTextureFallbackReason::MaterializationFailed);
                return {};
            }
            for (int row = 0; row < dimensions_.height(); ++row) {
                std::memcpy(
                    image.scanLine(row),
                    pixels.data() + static_cast<std::size_t>(row) * frame_->rowStrideBytes(),
                    tight_row_bytes
                );
            }
            image.setColorSpace(QColorSpace::SRgb);
            telemetry().host_materialization_bytes.fetch_add(
                expected_bytes,
                std::memory_order_relaxed
            );
            return image;
        } catch (...) {
            record_fallback(EditPreviewTextureFallbackReason::MaterializationFailed);
            return {};
        }
    }

    std::shared_ptr<const BackendEditPreviewFrame> frame_;
    [[maybe_unused]] std::shared_ptr<EditPreviewPresentationContext> presentation_context_;
    QSize dimensions_;
};

} // namespace

std::uint64_t EditPreviewTextureTelemetrySnapshot::fallbackCount(
    const EditPreviewTextureFallbackReason reason
) const noexcept {
    const auto index = fallback_index(reason);
    return index < fallback_by_reason.size() ? fallback_by_reason[index] : 0U;
}

std::uint64_t EditPreviewTextureTelemetrySnapshot::totalFallbackCount() const noexcept {
    return std::accumulate(
        fallback_by_reason.cbegin(),
        fallback_by_reason.cend(),
        std::uint64_t{0U}
    );
}

EditPreviewTextureTelemetrySnapshot editPreviewTextureTelemetry() noexcept {
    EditPreviewTextureTelemetrySnapshot result;
    result.native_factory_count = telemetry().native_factory_count.load(std::memory_order_relaxed);
    result.native_request_count = telemetry().native_request_count.load(std::memory_order_relaxed);
    result.native_request_bytes = telemetry().native_request_bytes.load(std::memory_order_relaxed);
    result.native_import_count = telemetry().native_import_count.load(std::memory_order_relaxed);
    result.native_import_bytes = telemetry().native_import_bytes.load(std::memory_order_relaxed);
    result.host_materialization_calls =
        telemetry().host_materialization_calls.load(std::memory_order_relaxed);
    result.host_materialization_bytes =
        telemetry().host_materialization_bytes.load(std::memory_order_relaxed);
    result.cpu_upload_bytes = telemetry().cpu_upload_bytes.load(std::memory_order_relaxed);
    result.last_resource_id = telemetry().last_resource_id.load(std::memory_order_relaxed);
    result.last_epoch = telemetry().last_epoch.load(std::memory_order_relaxed);
    for (std::size_t index = 0U; index < EDIT_PREVIEW_TEXTURE_FALLBACK_REASON_COUNT; ++index) {
        result.fallback_by_reason[index] =
            telemetry().fallback_by_reason[index].load(std::memory_order_relaxed);
    }
    return result;
}

void resetEditPreviewTextureTelemetry() noexcept {
    telemetry().native_factory_count.store(0U, std::memory_order_relaxed);
    telemetry().native_request_count.store(0U, std::memory_order_relaxed);
    telemetry().native_request_bytes.store(0U, std::memory_order_relaxed);
    telemetry().native_import_count.store(0U, std::memory_order_relaxed);
    telemetry().native_import_bytes.store(0U, std::memory_order_relaxed);
    telemetry().host_materialization_calls.store(0U, std::memory_order_relaxed);
    telemetry().host_materialization_bytes.store(0U, std::memory_order_relaxed);
    telemetry().cpu_upload_bytes.store(0U, std::memory_order_relaxed);
    telemetry().last_resource_id.store(0U, std::memory_order_relaxed);
    telemetry().last_epoch.store(0U, std::memory_order_relaxed);
    for (auto& count : telemetry().fallback_by_reason) {
        count.store(0U, std::memory_order_relaxed);
    }
}

QQuickTextureFactory* makeEditPreviewTextureFactory(
    std::shared_ptr<const BackendEditPreviewFrame> frame,
    const EditPreviewPresentationBinding presentation_binding,
    std::shared_ptr<EditPreviewPresentationContext> presentation_context
) {
    if (frame == nullptr) {
        return nullptr;
    }
    try {
        return new EditPreviewTextureFactory(
            std::move(frame),
            presentation_binding,
            std::move(presentation_context)
        );
    } catch (...) {
        record_fallback(EditPreviewTextureFallbackReason::InvalidNativeDescriptor);
        return nullptr;
    }
}
