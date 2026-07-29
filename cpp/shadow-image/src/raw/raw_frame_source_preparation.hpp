#pragma once

#include "raw_frame_development_plan.hpp"

#include <shadow/image/camera_profile_catalog.hpp>
#include <shadow/image/decoder_session.hpp>
#include <shadow/image/optics.hpp>
#include <shadow/image/raw_pipeline.hpp>

#include <memory>
#include <optional>
#include <string_view>

namespace shadow::image::detail {
class PreparedSceneLinearRegionOptics;
class PreparedRegionOpticsSourceIdentity;
} // namespace shadow::image::detail

namespace shadow::image::raw_pipeline_detail {

struct DevelopedRawFrame;
class ResidentRawSource;
struct ResidentRawSourceAttempt;
class PreparedRawFrameSource;

[[nodiscard]] ResidentRawSourceAttempt try_prepare_metal_resident_raw_source(
    PreparedRawFrameSource prepared,
    detail::PreparedSceneLinearRegionOptics optics
);

// The provider/session boundary is prepared once and then consumed by either the existing full
// materializer or a resident region aggregate. Pipeline provenance stays separate from pixel
// execution facts so both executors publish the same route, profile, and negotiation contract.
// Construction and consumption stay owner-controlled so a source-wide calibration/DCP plan can
// never be detached from the exact RawFrame that produced it.
class PreparedRawFrameSource final {
  public:
    PreparedRawFrameSource(const PreparedRawFrameSource&) = delete;
    PreparedRawFrameSource& operator=(const PreparedRawFrameSource&) = delete;
    PreparedRawFrameSource(PreparedRawFrameSource&&) noexcept = default;
    PreparedRawFrameSource& operator=(PreparedRawFrameSource&&) noexcept = delete;
    ~PreparedRawFrameSource() = default;

    [[nodiscard]] const PreparedRawFrameDevelopment& development() const noexcept;
    [[nodiscard]] detail::PreparedSceneLinearRegionOptics
    prepare_region_optics(const OpticsProvider* provider, const OpticsSettings& settings) const;
    [[nodiscard]] bool
    owns_region_optics(const detail::PreparedSceneLinearRegionOptics& optics) const noexcept;

  private:
    PreparedRawFrameSource(
        RawFrame frame,
        PreparedRawFrameDevelopment development,
        RawPipelineReceipt pipeline,
        RawDevelopmentPlanNegotiationStatus plan_negotiation_status,
        AssetMetadata metadata
    );

    RawFrame frame_;
    PreparedRawFrameDevelopment development_;
    RawPipelineReceipt pipeline_;
    RawDevelopmentPlanNegotiationStatus plan_negotiation_status_ =
        RawDevelopmentPlanNegotiationStatus::rejected;
    AssetMetadata metadata_;
    std::shared_ptr<const detail::PreparedRegionOpticsSourceIdentity> region_optics_identity_;

    friend PreparedRawFrameSource prepare_raw_frame_source(
        const DecodeSession& session,
        const RawDevelopmentPlan& requested_plan,
        std::optional<std::uint32_t> preview_max_edge,
        const CameraProfileCatalog& camera_profiles
    );
    friend DevelopedRawFrame develop_raw_frame(PreparedRawFrameSource& prepared);
    friend DevelopedSourceReference
    materialize_prepared_raw_frame_source(PreparedRawFrameSource prepared);
    friend ResidentRawSource prepare_resident_raw_source(
        PreparedRawFrameSource prepared,
        detail::PreparedSceneLinearRegionOptics optics
    );
    friend ResidentRawSourceAttempt try_prepare_metal_resident_raw_source(
        PreparedRawFrameSource prepared,
        detail::PreparedSceneLinearRegionOptics optics
    );
};

[[nodiscard]] PreparedRawFrameSource prepare_raw_frame_source(
    const DecodeSession& session,
    const RawDevelopmentPlan& requested_plan,
    std::optional<std::uint32_t> preview_max_edge,
    const CameraProfileCatalog& camera_profiles
);

[[nodiscard]] RawPipelineReceipt finalize_raw_frame_pipeline_receipt(
    RawPipelineReceipt prepared,
    RawDevelopmentBackend backend,
    RawHighlightRecoveryIntent highlight_recovery,
    std::string_view raw_denoise_cache_identity
);

[[nodiscard]] DevelopedSourceReference
materialize_prepared_raw_frame_source(PreparedRawFrameSource prepared);

} // namespace shadow::image::raw_pipeline_detail
