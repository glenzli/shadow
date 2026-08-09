#pragma once

#include "backend/edit_preview_frame.hpp"
#include "edit_preview_contract.hpp"

#include <QQuickTextureFactory>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

class EditPreviewPresentationContext;

enum class EditPreviewTextureFallbackReason : std::uint8_t {
    HostStorage = 0U,
    BindingUnavailable,
    NullWindow,
    SceneGraphUninitialized,
    WrongWindow,
    EpochMismatch,
    WrongApi,
    DeviceUnavailable,
    WrongDevice,
    InvalidNativeDescriptor,
    NativeImportFailed,
    MaterializationFailed,
    CpuUploadFailed,
    Count,
};

inline constexpr std::size_t EDIT_PREVIEW_TEXTURE_FALLBACK_REASON_COUNT =
    static_cast<std::size_t>(EditPreviewTextureFallbackReason::Count);

struct EditPreviewTextureTelemetrySnapshot final {
    std::uint64_t native_factory_count = 0U;
    std::uint64_t native_request_count = 0U;
    std::uint64_t native_request_bytes = 0U;
    std::uint64_t native_import_count = 0U;
    std::uint64_t native_import_bytes = 0U;
    std::uint64_t active_native_frame_guards = 0U;
    std::uint64_t peak_native_frame_guards = 0U;
    std::uint64_t host_materialization_calls = 0U;
    std::uint64_t host_materialization_bytes = 0U;
    std::uint64_t cpu_upload_bytes = 0U;
    std::uint64_t last_resource_id = 0U;
    std::uint64_t last_epoch = 0U;
    std::array<std::uint64_t, EDIT_PREVIEW_TEXTURE_FALLBACK_REASON_COUNT> fallback_by_reason{};

    [[nodiscard]] std::uint64_t
    fallbackCount(EditPreviewTextureFallbackReason reason) const noexcept;

    [[nodiscard]] std::uint64_t totalFallbackCount() const noexcept;
};

[[nodiscard]] EditPreviewTextureTelemetrySnapshot editPreviewTextureTelemetry() noexcept;

void resetEditPreviewTextureTelemetry() noexcept;

// Loading-thread entry. Construction reads only immutable descriptors; all
// QSG/native validation and texture creation happens later in createTexture()
// on Qt Quick's render thread.
[[nodiscard]] QQuickTextureFactory* makeEditPreviewTextureFactory(
    std::shared_ptr<const BackendEditPreviewFrame> frame,
    EditPreviewPresentationBinding presentation_binding,
    std::shared_ptr<EditPreviewPresentationContext> presentation_context
);
