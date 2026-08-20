#pragma once

#include <shadow/image/decoder_types.hpp>
#include <shadow/image/raw_development_plan.hpp>

#include <array>
#include <cstdint>
#include <string>

namespace shadow::image {

// A renderer-produced record of the exact RAW-development request that yielded a processed
// reference raster. It intentionally lives beside the pixels, rather than in Recipe data: RAW
// provider behavior is source provenance, while a recipe is only the photographer's editable
// intent. A default/empty receipt means that a generic provider did not expose one; callers must
// not infer sensor-domain behavior from the absence of a receipt.
//
// `provider_version` is Shadow's complete cache-visible provider identity. `library_version`
// separately identifies the underlying renderer/library release when the provider has one; it
// may be empty for a provider that intentionally does not expose that implementation detail.
// `process_warnings` preserves a renderer's complete warning bit mask without collapsing future
// warning bits into a lossy boolean set.
//
// The provider-neutral development plan identity and per-list DNG opcode
// outcome belong to the dated local receipt. Incompatible development
// data is regenerated rather than migrated.
inline constexpr std::uint32_t raw_development_receipt_schema_version = 2'026'082'101U;

enum class DngOpcodeExecutionStatus : std::uint8_t {
    not_declared,
    // The provider followed its documented default handling but cannot prove that the list was
    // applied or skipped. This is the honest LibRaw v1 result.
    provider_default,
    applied,
    deferred_to_shadow,
    skipped_for_preview,
    unsupported,
};

struct RawDevelopmentReceipt final {
    // Zero means that the provider did not record RAW-development provenance for this buffer.
    std::uint32_t schema_version = 0U;
    std::string provider_id;
    std::string provider_version;
    std::string library_version;
    std::string development_settings_signature;
    // Both strings must be canonical `raw_development_plan_identity(...)` values. They are
    // separate because a specialized provider may negotiate an explicit adjusted plan; the
    // source cache must use the effective identity while UI/audit code retains the request.
    std::string requested_plan_identity;
    std::string effective_plan_identity;
    RawDevelopmentPlan requested_plan;
    RawDevelopmentPlan effective_plan;
    RawDevelopmentPlanNegotiationStatus plan_negotiation_status =
        RawDevelopmentPlanNegotiationStatus::rejected;
    std::uint32_t processed_linear_reference_contract_version = 0U;

    // These are LibRaw's declared pre-render image dimensions and orientation alongside the
    // actual output raster. They make a half-size or orientation-related difference auditable
    // without assuming that the metadata dimensions already describe the processed bitmap.
    Dimensions declared_image_dimensions;
    Dimensions rendered_dimensions;
    std::int32_t orientation = 0;
    bool half_size = false;

    bool use_camera_white_balance = false;
    bool use_camera_matrix = false;
    bool use_auto_brightness = false;
    bool use_exposure_correction = false;
    float brightness = 0.0F;
    float maximum_adjustment_threshold = 0.0F;
    std::uint16_t output_bits_per_channel = 0U;
    std::int32_t demosaic_quality = 0;
    std::int32_t output_color = 0;
    double gamma_inverse_power = 0.0;
    double gamma_linear_toe_slope = 0.0;

    // Opcode lengths describe the source declarations. The execution entries below name whether
    // this particular render applied, deferred, skipped, or could only retain provider-default
    // semantics for each list. A provider must never use `applied` merely because a list exists.
    PendingCorrections declared_dng_opcode_lists;
    std::array<DngOpcodeExecutionStatus, 3U> dng_opcode_execution{};
    std::uint32_t process_warnings = 0U;

    [[nodiscard]] bool recorded() const noexcept {
        return schema_version != 0U;
    }

    // A caller may use `recorded()` to distinguish generic RGB input from provider-rendered RAW,
    // but must use this guard before interpreting the fields of a known schema. It keeps a future
    // private provider from being silently parsed as the current contract.
    [[nodiscard]] bool uses_current_schema() const noexcept {
        return schema_version == raw_development_receipt_schema_version;
    }
};

} // namespace shadow::image
