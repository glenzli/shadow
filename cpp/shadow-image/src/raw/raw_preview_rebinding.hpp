#pragma once

#include <shadow/image/camera_profile_catalog.hpp>
#include <shadow/image/decoder_session.hpp>
#include <shadow/image/raw_foundation.hpp>
#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/raw_white_balance.hpp>

#include <cstdint>
#include <memory>
#include <optional>

namespace shadow::image::raw_pipeline_detail {

class PreparedRawFrameSource;
struct PreparedRawPreviewRebinding;

// Immutable sensor/camera-space owner shared by every white-balance variant of one bounded
// preview. The ordinary RAW route retains one already-denoised RawFrame; the AI route retains
// only its bounded camera-RGB reconstruction. bind() creates a fresh scene-linear result and
// exact provenance without reopening the provider or repeating an irreversible denoise stage.
class RawPreviewRebindingSource final {
  public:
    RawPreviewRebindingSource(const RawPreviewRebindingSource&) = delete;
    RawPreviewRebindingSource& operator=(const RawPreviewRebindingSource&) = delete;
    ~RawPreviewRebindingSource();

    [[nodiscard]] DevelopedSourceReference bind(const RawDevelopmentPlan& requested_plan) const;
    [[nodiscard]] DevelopedSourceReference bind_foundation_amount(
        const RawDevelopmentPlan& requested_plan,
        std::uint8_t amount_percent
    ) const;
    [[nodiscard]] bool supports_foundation_amount_rebinding() const noexcept;
    // Available only when the immutable preview retained its original
    // denoised CFA frame.  Foundation/RGB compatibility paths deliberately do
    // not expose a picker because their pixels are no longer sensor samples.
    [[nodiscard]] bool supports_raw_white_balance_picker() const noexcept;
    [[nodiscard]] std::optional<RawWhiteBalancePresentation>
    pick_raw_white_balance(double normalized_x, double normalized_y) const noexcept;
    [[nodiscard]] const AssetMetadata& metadata() const noexcept;
    [[nodiscard]] RawPreviewRebindingTelemetry telemetry() const noexcept;

  private:
    struct Impl;
    explicit RawPreviewRebindingSource(std::unique_ptr<Impl> impl);
    [[nodiscard]] DevelopedSourceReference bind_impl(
        const RawDevelopmentPlan& requested_plan,
        std::optional<std::uint8_t> foundation_amount_percent
    ) const;

    std::unique_ptr<Impl> impl_;

    friend struct PreparedRawPreviewRebinding;
    friend PreparedRawPreviewRebinding
    prepare_raw_preview_rebinding(PreparedRawFrameSource prepared);
    friend PreparedRawPreviewRebinding prepare_raw_foundation_preview_rebinding(
        PreparedRawFrameSource prepared,
        const RawFoundationCameraRgbView& foundation,
        const RawDevelopmentPlan& requested_plan
    );
};

struct PreparedRawPreviewRebinding final {
    std::shared_ptr<const RawPreviewRebindingSource> source;
    DevelopedSourceReference developed;
};

[[nodiscard]] PreparedRawPreviewRebinding
prepare_raw_preview_rebinding(PreparedRawFrameSource prepared);

[[nodiscard]] PreparedRawPreviewRebinding prepare_raw_foundation_preview_rebinding(
    PreparedRawFrameSource prepared,
    const RawFoundationCameraRgbView& foundation,
    const RawDevelopmentPlan& requested_plan
);

// Returns no source when the current policy deliberately selects a decoded/provider-processed
// path. In automatic mode an unsupported owned RawFrame also returns no source so the canonical
// provider compatibility path remains responsible for its explicit fallback receipt.
[[nodiscard]] std::optional<PreparedRawPreviewRebinding> try_prepare_raw_preview_rebinding(
    const DecodeSession& session,
    const RawDevelopmentPlan& requested_plan,
    std::uint32_t max_edge,
    const RawPipelinePolicy& policy,
    const CameraProfileCatalog& camera_profiles
);

[[nodiscard]] PreparedRawPreviewRebinding prepare_raw_foundation_preview_rebinding(
    const DecodeSession& session,
    const RawDevelopmentPlan& requested_plan,
    const RawFoundationCameraRgbView& foundation,
    std::uint32_t max_edge,
    const RawPipelinePolicy& policy,
    const CameraProfileCatalog& camera_profiles
);

[[nodiscard]] PreparedRawPreviewRebinding prepare_raw_foundation_preview_rebinding(
    const DecodeSession& metadata_session,
    RawFrame staged_frame,
    const RawDevelopmentPlan& requested_plan,
    const RawFoundationCameraRgbView& foundation,
    std::uint32_t max_edge,
    const RawPipelinePolicy& policy,
    const CameraProfileCatalog& camera_profiles
);

} // namespace shadow::image::raw_pipeline_detail
