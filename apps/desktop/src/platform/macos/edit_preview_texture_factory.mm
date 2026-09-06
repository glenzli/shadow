#import <Metal/Metal.h>

#include "platform/edit_preview_texture_factory.hpp"

#include "edit_preview_presentation_context.hpp"

#include <QColorSpace>
#include <QImage>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QtQuick/qsgtexture_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <utility>

namespace {

struct AtomicEditPreviewTextureTelemetry final {
    std::atomic<std::uint64_t> native_factory_count{0U};
    std::atomic<std::uint64_t> native_request_count{0U};
    std::atomic<std::uint64_t> native_request_bytes{0U};
    std::atomic<std::uint64_t> native_import_count{0U};
    std::atomic<std::uint64_t> native_import_bytes{0U};
    std::atomic<std::uint64_t> active_native_frame_guards{0U};
    std::atomic<std::uint64_t> peak_native_frame_guards{0U};
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

[[nodiscard]] std::uint64_t checked_native_bytes(
    const BackendAppleMetalPreviewTexture& texture,
    const QSize dimensions
) noexcept {
    if (!dimensions.isValid() || dimensions.isEmpty()) {
        return 0U;
    }
    const auto height = static_cast<std::uint64_t>(dimensions.height());
    const auto stride = static_cast<std::uint64_t>(texture.row_stride_bytes);
    if (stride == 0U || height > std::numeric_limits<std::uint64_t>::max() / stride) {
        return 0U;
    }
    return stride * height;
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

class FrameLifetimeGuard final : public QObject {
  public:
    FrameLifetimeGuard(
        std::shared_ptr<const BackendEditPreviewFrame> frame,
        QQuickWindow* const window,
        QObject* const parent
    ) : QObject(parent), frame_(std::move(frame)) {
        // Qt's native wrapper does not own the texture, and its Metal command buffers
        // may use unretained resource references. Keep each submitted frame alive until
        // GPU completion, including replacement, scene-graph invalidation and shutdown.
        // This adds no pixel transfer, render wait, or semantic cache invalidation.
        QObject::connect(
            window,
            &QQuickWindow::beforeRendering,
            this,
            [this, window]() {
                auto* const renderer = window->rendererInterface();
                if (renderer == nullptr)
                    return;
                id<MTLCommandBuffer> const commands = reinterpret_cast<id<MTLCommandBuffer>>(
                    renderer->getResource(window, QSGRendererInterface::CommandListResource)
                );
                if (commands == nil)
                    return;
                const auto retained_frame = frame_;
                [commands addCompletedHandler:^(id<MTLCommandBuffer>) {
                  static_cast<void>(retained_frame);
                }];
            },
            Qt::DirectConnection
        );
        const std::uint64_t active =
            telemetry().active_native_frame_guards.fetch_add(1U, std::memory_order_relaxed) + 1U;
        std::uint64_t peak = telemetry().peak_native_frame_guards.load(std::memory_order_relaxed);
        while (peak < active
               && !telemetry().peak_native_frame_guards.compare_exchange_weak(
                   peak,
                   active,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed
               )) {}
    }

    ~FrameLifetimeGuard() override {
        telemetry().active_native_frame_guards.fetch_sub(1U, std::memory_order_relaxed);
    }

  private:
    std::shared_ptr<const BackendEditPreviewFrame> frame_;
};

class EditPreviewTextureFactory final : public QQuickTextureFactory {
  public:
    EditPreviewTextureFactory(
        std::shared_ptr<const BackendEditPreviewFrame> frame,
        const EditPreviewPresentationBinding presentation_binding,
        std::shared_ptr<EditPreviewPresentationContext> presentation_context
    ) :
        frame_(std::move(frame)), presentation_binding_(presentation_binding),
        presentation_context_(std::move(presentation_context)),
        dimensions_(frame_ ? frame_->dimensions() : QSize{}),
        native_texture_(frame_ ? frame_->appleMetalTexture() : std::nullopt),
        native_bytes_(native_texture_ ? checked_native_bytes(*native_texture_, dimensions_) : 0U) {
        if (native_texture_.has_value()) {
            telemetry().native_factory_count.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] QSGTexture* createTexture(QQuickWindow* const window) const override {
        if (window == nullptr) {
            record_fallback(EditPreviewTextureFallbackReason::NullWindow);
            return nullptr;
        }
        // Qt cannot create either a native wrapper or an upload texture while
        // this scene graph is down. Do not turn invalidation into a pointless
        // full-frame readback.
        if (!window->isSceneGraphInitialized()) {
            record_fallback(EditPreviewTextureFallbackReason::SceneGraphUninitialized);
            return nullptr;
        }
        if (frame_ == nullptr) {
            record_fallback(EditPreviewTextureFallbackReason::InvalidNativeDescriptor);
            return nullptr;
        }
        if (frame_->storageKind() != BackendEditPreviewStorage::AppleMetalRgba8Srgb) {
            return createHostTexture(window, EditPreviewTextureFallbackReason::HostStorage);
        }

        telemetry().native_request_count.fetch_add(1U, std::memory_order_relaxed);
        telemetry().native_request_bytes.fetch_add(native_bytes_, std::memory_order_relaxed);
        if (native_texture_.has_value()) {
            telemetry().last_resource_id.store(
                native_texture_->resource_id,
                std::memory_order_relaxed
            );
        }
        telemetry().last_epoch.store(presentation_binding_.epoch, std::memory_order_relaxed);

        if (!presentation_binding_.initialized || presentation_binding_.epoch == 0U
            || presentation_context_ == nullptr) {
            return createHostTexture(window, EditPreviewTextureFallbackReason::BindingUnavailable);
        }
        const auto actual_window_identity = reinterpret_cast<std::uintptr_t>(window);
        const EditPreviewPresentationBinding live = presentation_context_->snapshot();
        if (presentation_binding_.window_identity != actual_window_identity
            || live.window_identity != actual_window_identity) {
            return createHostTexture(window, EditPreviewTextureFallbackReason::WrongWindow);
        }
        if (live.epoch != presentation_binding_.epoch || !live.initialized) {
            return createHostTexture(window, EditPreviewTextureFallbackReason::EpochMismatch);
        }
        QSGRendererInterface* const renderer = window->rendererInterface();
        if (renderer == nullptr || presentation_binding_.api != EditPreviewPresentationApi::Metal
            || live.api != EditPreviewPresentationApi::Metal
            || renderer->graphicsApi() != QSGRendererInterface::Metal) {
            return createHostTexture(window, EditPreviewTextureFallbackReason::WrongApi);
        }
        void* const live_device_resource =
            renderer->getResource(window, QSGRendererInterface::DeviceResource);
        if (live_device_resource == nullptr) {
            return createHostTexture(window, EditPreviewTextureFallbackReason::DeviceUnavailable);
        }
        if (!native_texture_.has_value()) {
            return createHostTexture(
                window,
                EditPreviewTextureFallbackReason::InvalidNativeDescriptor
            );
        }
        const auto live_device_handle = reinterpret_cast<std::uintptr_t>(live_device_resource);
        if (live.device_handle != live_device_handle
            || presentation_binding_.device_handle != live_device_handle
            || native_texture_->device_handle != live_device_handle) {
            return createHostTexture(window, EditPreviewTextureFallbackReason::WrongDevice);
        }

        id<MTLDevice> const live_device = reinterpret_cast<id<MTLDevice>>(live_device_resource);
        id<MTLTexture> const native_texture =
            reinterpret_cast<id<MTLTexture>>(native_texture_->texture_handle);
        if (!validNativeTexture(native_texture, live_device)) {
            return createHostTexture(
                window,
                EditPreviewTextureFallbackReason::InvalidNativeDescriptor
            );
        }

        QSGTexture* const imported =
            QNativeInterface::QSGMetalTexture::fromNative(native_texture, window, dimensions_, {});
        if (imported == nullptr) {
            return createHostTexture(window, EditPreviewTextureFallbackReason::NativeImportFailed);
        }
        // The Qt wrapper is non-owning. Its child guard keeps the C2 owner,
        // buffer, and MTLTexture alive even if this factory is destroyed as
        // soon as createTexture() returns.
        static_cast<void>(new FrameLifetimeGuard(frame_, window, imported));
        telemetry().native_import_count.fetch_add(1U, std::memory_order_relaxed);
        telemetry().native_import_bytes.fetch_add(native_bytes_, std::memory_order_relaxed);
        return imported;
    }

    [[nodiscard]] QSize textureSize() const override {
        return dimensions_;
    }

    [[nodiscard]] int textureByteCount() const override {
        const std::uint64_t bytes =
            native_texture_.has_value() ? native_bytes_ : checked_rgb8_bytes(*frame_);
        return static_cast<int>(std::min<std::uint64_t>(
            bytes,
            static_cast<std::uint64_t>(std::numeric_limits<int>::max())
        ));
    }

    [[nodiscard]] QImage image() const override {
        return materializedImage();
    }

  private:
    [[nodiscard]] bool
    validNativeTexture(id<MTLTexture> const texture, id<MTLDevice> const device) const noexcept {
        if (texture == nil || device == nil
            || native_texture_->pixel_format != BACKEND_APPLE_METAL_RGBA8_SRGB_PIXEL_FORMAT
            || native_bytes_ == 0U) {
            return false;
        }
        // The metadata describes display-sRGB-encoded bytes. Their physical
        // Metal view stays unorm so Qt Quick does not decode the transfer once
        // more while sampling the opaque native texture.
        return [texture device] == device && texture.textureType == MTLTextureType2D
               && texture.pixelFormat == MTLPixelFormatRGBA8Unorm
               && texture.width == static_cast<NSUInteger>(dimensions_.width())
               && texture.height == static_cast<NSUInteger>(dimensions_.height())
               && texture.depth == 1U && texture.mipmapLevelCount == 1U && texture.sampleCount == 1U
               && texture.arrayLength == 1U
               && (texture.usage & MTLTextureUsageShaderRead) == MTLTextureUsageShaderRead;
    }

    [[nodiscard]] QImage materializedImage() const noexcept {
        telemetry().host_materialization_calls.fetch_add(1U, std::memory_order_relaxed);
        try {
            const auto pixels = frame_->materializeRgb8();
            const std::uint64_t expected_bytes = checked_rgb8_bytes(*frame_);
            const auto tight_row_bytes = static_cast<std::size_t>(dimensions_.width()) * 3U;
            if (expected_bytes == 0U || frame_->rowStrideBytes() != tight_row_bytes
                || pixels.size() != expected_bytes) {
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

    [[nodiscard]] QSGTexture* createHostTexture(
        QQuickWindow* const window,
        const EditPreviewTextureFallbackReason reason
    ) const noexcept {
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

    std::shared_ptr<const BackendEditPreviewFrame> frame_;
    EditPreviewPresentationBinding presentation_binding_;
    std::shared_ptr<EditPreviewPresentationContext> presentation_context_;
    QSize dimensions_;
    std::optional<BackendAppleMetalPreviewTexture> native_texture_;
    std::uint64_t native_bytes_ = 0U;
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
    result.active_native_frame_guards =
        telemetry().active_native_frame_guards.load(std::memory_order_relaxed);
    result.peak_native_frame_guards =
        telemetry().peak_native_frame_guards.load(std::memory_order_relaxed);
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
    const std::uint64_t active_native_frame_guards =
        telemetry().active_native_frame_guards.load(std::memory_order_relaxed);
    telemetry().peak_native_frame_guards.store(
        active_native_frame_guards,
        std::memory_order_relaxed
    );
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
