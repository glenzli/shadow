#pragma once

#include <array>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace shadow::image {

struct Dimensions final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] std::uint64_t pixel_count() const noexcept;
    auto operator<=>(const Dimensions&) const = default;
};

struct Margins final {
    std::uint32_t left = 0;
    std::uint32_t top = 0;
    std::uint32_t right = 0;
    std::uint32_t bottom = 0;

    auto operator<=>(const Margins&) const = default;
};

enum class PreviewFormat : std::uint8_t {
    unknown,
    jpeg,
    bitmap,
    jpeg_xl,
    h265,
};

enum class ByteOrder : std::uint8_t {
    not_applicable,
    native,
    little_endian,
    big_endian,
};

// PixelBuffer names primaries, transfer, and processing reference separately. In particular,
// "sRGB primaries" must never be read as "sRGB-encoded samples".
enum class RgbPrimaries : std::uint8_t {
    unknown,
    srgb_rec709_d65,
};

enum class RgbTransferFunction : std::uint8_t {
    unknown,
    linear,
};

enum class RgbBufferReference : std::uint8_t {
    unknown,
    // RGB produced after a RAW provider's black subtraction, white balance, demosaic,
    // camera-to-output color conversion, and integer-range scaling. This is linear-light
    // processed RGB, not an untouched sensor-linear mosaic or a lossless radiance buffer.
    processed_raw,
    // RGB decoded from an ordinary rendered image (for example JPEG or SDR HEIF) after its
    // embedded profile, or an explicit sRGB fallback, has been transformed to linear
    // sRGB/Rec.709-D65. The source started display-referred, so it must not receive the RAW
    // scene-to-display curve at the output boundary. It nevertheless shares the same linear
    // node graph, history, LUT, cache and export pipeline as processed RAW RGB.
    decoded_raster,
};

struct PendingCorrections final {
    std::array<std::uint32_t, 3> dng_opcode_list_bytes{};

    [[nodiscard]] bool has_pending() const noexcept;
    auto operator<=>(const PendingCorrections&) const = default;
};

struct ProviderInfo final {
    std::string id;
    std::string version;
    bool dng_sdk = false;
    bool rawspeed = false;
    bool jpeg = false;
};

// All of LibRaw's user-visible processing switches are concentrated here rather than being
// spread across preview and detail render code. This is the configuration boundary between
// Shadow's provider-neutral decoder contract and LibRaw's private processing API. It deliberately
// describes the reference/development raster only; sensor-domain RAW white balance, DNG opcodes,
// optical profiles and camera-specific colour transforms will become separate pipeline stages.
//
// `demosaic_quality` maps directly to LibRaw's documented `user_qual` selector. It remains an
// implementation detail for now because the available algorithms vary with the linked LibRaw
// build. Shadow's UI will expose intent presets only after every provider can honour them.
struct LibRawDevelopmentSettings final {
    std::uint32_t schema_version = 1;
    bool use_camera_white_balance = true;
    bool use_camera_matrix = true;
    bool use_auto_brightness = false;
    bool use_exposure_correction = false;
    float brightness = 1.0F;
    float maximum_adjustment_threshold = 0.0F;
    std::uint16_t output_bits_per_channel = 16;
    std::int32_t demosaic_quality = 3;

    auto operator<=>(const LibRawDevelopmentSettings&) const = default;
};

inline constexpr std::uint32_t libraw_development_settings_schema_version = 1U;

[[nodiscard]] LibRawDevelopmentSettings default_libraw_development_settings() noexcept;
[[nodiscard]] std::string libraw_development_settings_signature(
    const LibRawDevelopmentSettings& settings
);

// A RAW-development plan is deliberately expressed in photographic intent rather than in a
// particular decoder's switches.  For example, a future provider may map `noise_robust` to an
// LMMSE-like Bayer path while LibRaw may only be able to decline it; neither case leaks a
// vendor-SDK enum or a `user_qual` number through Shadow's public contract.
//
// The plan is source preparation, not an edit Recipe.  Its identity therefore belongs in the
// generated source/cache key and in RawDevelopmentReceipt, alongside the provider identity that
// interpreted it.  A later RawFrame implementation can add sensor-domain stages without
// changing the meaning of the current plan fields.
inline constexpr std::uint32_t raw_development_plan_schema_version = 1U;

enum class RawDevelopmentIntent : std::uint8_t {
    preview,
    detail,
    export_image,
};

enum class RawDevelopmentQuality : std::uint8_t {
    fast,
    balanced,
    high,
};

// A DNG opcode policy is intentionally a promise level, not a claim that every provider can
// manipulate every opcode list. `provider_default` preserves a decoder's documented normal
// behaviour. `require_applied` and `defer_to_shadow` must be rejected unless a provider
// explicitly advertises them; silently treating either as a default would make calibration
// provenance untrustworthy.
enum class DngOpcodePolicy : std::uint8_t {
    provider_default,
    require_applied,
    defer_to_shadow,
};

// These are plans for future sensor/linear-domain stages.  They are already part of the plan
// identity so adding a real RawFrame path cannot accidentally reuse a source raster developed
// under a different noise or highlight policy.  Current LibRaw support intentionally accepts
// only `provider_default` for both axes.
enum class RawNoiseReductionIntent : std::uint8_t {
    provider_default,
    disabled,
    conservative,
    noise_robust,
};

enum class RawHighlightRecoveryIntent : std::uint8_t {
    provider_default,
    disabled,
    conservative,
    aggressive,
};

struct RawDevelopmentPlan final {
    std::uint32_t schema_version = raw_development_plan_schema_version;
    RawDevelopmentIntent intent = RawDevelopmentIntent::detail;
    RawDevelopmentQuality quality = RawDevelopmentQuality::balanced;
    DngOpcodePolicy dng_opcode_policy = DngOpcodePolicy::provider_default;
    RawNoiseReductionIntent noise_reduction = RawNoiseReductionIntent::provider_default;
    RawHighlightRecoveryIntent highlight_recovery =
        RawHighlightRecoveryIntent::provider_default;

    auto operator<=>(const RawDevelopmentPlan&) const = default;
};

[[nodiscard]] constexpr RawDevelopmentPlan default_raw_development_plan() noexcept {
    return RawDevelopmentPlan{
        .schema_version = raw_development_plan_schema_version,
        .intent = RawDevelopmentIntent::detail,
        .quality = RawDevelopmentQuality::balanced,
        .dng_opcode_policy = DngOpcodePolicy::provider_default,
        .noise_reduction = RawNoiseReductionIntent::provider_default,
        .highlight_recovery = RawHighlightRecoveryIntent::provider_default,
    };
}

[[nodiscard]] constexpr RawDevelopmentPlan preview_raw_development_plan() noexcept {
    auto plan = default_raw_development_plan();
    plan.intent = RawDevelopmentIntent::preview;
    return plan;
}

// This is a canonical, reversible and provider-neutral cache component.  It describes only the
// requested/effective plan; callers must pair it with ProviderInfo::id/version before using it
// as a representation identity.
enum class RawDevelopmentPlanAspect : std::uint32_t {
    none = 0U,
    schema = 1U << 0U,
    intent = 1U << 1U,
    quality = 1U << 2U,
    dng_opcode_policy = 1U << 3U,
    noise_reduction = 1U << 4U,
    highlight_recovery = 1U << 5U,
};

[[nodiscard]] constexpr RawDevelopmentPlanAspect operator|(
    const RawDevelopmentPlanAspect left,
    const RawDevelopmentPlanAspect right
) noexcept {
    return static_cast<RawDevelopmentPlanAspect>(
        static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right)
    );
}

constexpr RawDevelopmentPlanAspect& operator|=(
    RawDevelopmentPlanAspect& left,
    const RawDevelopmentPlanAspect right
) noexcept {
    left = left | right;
    return left;
}

[[nodiscard]] constexpr bool raw_development_plan_aspect_contains(
    const RawDevelopmentPlanAspect value,
    const RawDevelopmentPlanAspect flag
) noexcept {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0U;
}

// Bit masks make the capability contract forward-compatible: a provider built against a newer
// header can advertise only the enum values it understands while an older host fails closed on
// unknown schema/values. Use the helpers rather than constructing shifts from enum ordinals.
[[nodiscard]] constexpr std::uint32_t raw_development_intent_mask(
    const RawDevelopmentIntent intent
) noexcept {
    switch (intent) {
    case RawDevelopmentIntent::preview:
        return 1U << 0U;
    case RawDevelopmentIntent::detail:
        return 1U << 1U;
    case RawDevelopmentIntent::export_image:
        return 1U << 2U;
    }
    return 0U;
}

[[nodiscard]] constexpr std::uint32_t raw_development_quality_mask(
    const RawDevelopmentQuality quality
) noexcept {
    switch (quality) {
    case RawDevelopmentQuality::fast:
        return 1U << 0U;
    case RawDevelopmentQuality::balanced:
        return 1U << 1U;
    case RawDevelopmentQuality::high:
        return 1U << 2U;
    }
    return 0U;
}

[[nodiscard]] constexpr std::uint32_t dng_opcode_policy_mask(
    const DngOpcodePolicy policy
) noexcept {
    switch (policy) {
    case DngOpcodePolicy::provider_default:
        return 1U << 0U;
    case DngOpcodePolicy::require_applied:
        return 1U << 1U;
    case DngOpcodePolicy::defer_to_shadow:
        return 1U << 2U;
    }
    return 0U;
}

[[nodiscard]] constexpr std::uint32_t raw_noise_reduction_intent_mask(
    const RawNoiseReductionIntent intent
) noexcept {
    switch (intent) {
    case RawNoiseReductionIntent::provider_default:
        return 1U << 0U;
    case RawNoiseReductionIntent::disabled:
        return 1U << 1U;
    case RawNoiseReductionIntent::conservative:
        return 1U << 2U;
    case RawNoiseReductionIntent::noise_robust:
        return 1U << 3U;
    }
    return 0U;
}

[[nodiscard]] constexpr std::uint32_t raw_highlight_recovery_intent_mask(
    const RawHighlightRecoveryIntent intent
) noexcept {
    switch (intent) {
    case RawHighlightRecoveryIntent::provider_default:
        return 1U << 0U;
    case RawHighlightRecoveryIntent::disabled:
        return 1U << 1U;
    case RawHighlightRecoveryIntent::conservative:
        return 1U << 2U;
    case RawHighlightRecoveryIntent::aggressive:
        return 1U << 3U;
    }
    return 0U;
}

namespace detail {

[[nodiscard]] constexpr std::string_view raw_development_intent_identity_name(
    const RawDevelopmentIntent intent
) noexcept {
    switch (intent) {
    case RawDevelopmentIntent::preview:
        return "preview";
    case RawDevelopmentIntent::detail:
        return "detail";
    case RawDevelopmentIntent::export_image:
        return "export";
    }
    return {};
}

[[nodiscard]] constexpr std::string_view raw_development_quality_identity_name(
    const RawDevelopmentQuality quality
) noexcept {
    switch (quality) {
    case RawDevelopmentQuality::fast:
        return "fast";
    case RawDevelopmentQuality::balanced:
        return "balanced";
    case RawDevelopmentQuality::high:
        return "high";
    }
    return {};
}

[[nodiscard]] constexpr std::string_view dng_opcode_policy_identity_name(
    const DngOpcodePolicy policy
) noexcept {
    switch (policy) {
    case DngOpcodePolicy::provider_default:
        return "provider-default";
    case DngOpcodePolicy::require_applied:
        return "require-applied";
    case DngOpcodePolicy::defer_to_shadow:
        return "defer-to-shadow";
    }
    return {};
}

[[nodiscard]] constexpr std::string_view raw_noise_reduction_identity_name(
    const RawNoiseReductionIntent intent
) noexcept {
    switch (intent) {
    case RawNoiseReductionIntent::provider_default:
        return "provider-default";
    case RawNoiseReductionIntent::disabled:
        return "disabled";
    case RawNoiseReductionIntent::conservative:
        return "conservative";
    case RawNoiseReductionIntent::noise_robust:
        return "noise-robust";
    }
    return {};
}

[[nodiscard]] constexpr std::string_view raw_highlight_recovery_identity_name(
    const RawHighlightRecoveryIntent intent
) noexcept {
    switch (intent) {
    case RawHighlightRecoveryIntent::provider_default:
        return "provider-default";
    case RawHighlightRecoveryIntent::disabled:
        return "disabled";
    case RawHighlightRecoveryIntent::conservative:
        return "conservative";
    case RawHighlightRecoveryIntent::aggressive:
        return "aggressive";
    }
    return {};
}

} // namespace detail

// This is a canonical, reversible and provider-neutral cache component. It describes only the
// requested/effective plan; callers must pair it with ProviderInfo::id/version before using it
// as a representation identity. It is header-defined because a private plugin must be able to
// construct a receipt without dynamically linking the host's static image library.
[[nodiscard]] inline std::string raw_development_plan_identity(const RawDevelopmentPlan& plan) {
    if (plan.schema_version != raw_development_plan_schema_version) {
        throw std::invalid_argument("unsupported RAW development plan schema version");
    }
    const auto intent = detail::raw_development_intent_identity_name(plan.intent);
    const auto quality = detail::raw_development_quality_identity_name(plan.quality);
    const auto opcode_policy = detail::dng_opcode_policy_identity_name(plan.dng_opcode_policy);
    const auto noise_reduction = detail::raw_noise_reduction_identity_name(plan.noise_reduction);
    const auto highlight_recovery = detail::raw_highlight_recovery_identity_name(
        plan.highlight_recovery
    );
    if (
        intent.empty() || quality.empty() || opcode_policy.empty() || noise_reduction.empty()
        || highlight_recovery.empty()
    ) {
        throw std::invalid_argument("RAW development plan contains an unknown enum value");
    }
    return "shadow-raw-plan-v" + std::to_string(plan.schema_version)
        + ";intent=" + std::string(intent)
        + ";quality=" + std::string(quality)
        + ";opcodes=" + std::string(opcode_policy)
        + ";nr=" + std::string(noise_reduction)
        + ";highlights=" + std::string(highlight_recovery);
}

inline constexpr std::uint32_t raw_development_capabilities_schema_version = 1U;

struct RawDevelopmentCapabilities final {
    std::uint32_t schema_version = raw_development_capabilities_schema_version;
    // False means this source/provider is already rendered RGB or has not opted into plan-aware
    // RAW development. Calling the new DecodeSession overloads remains safe for that provider,
    // but the requested RAW plan is not represented in its pixels or receipt.
    bool available = false;
    // True only when this provider can supply an independently owned RawFrame with the current
    // schema. It remains separate from plan negotiation: a provider may expose source samples
    // while declining a requested RAW-domain policy it cannot execute honestly.
    bool raw_frame = false;
    // True only if the provider can honestly distinguish application outcomes for every declared
    // DNG opcode list in RawDevelopmentReceipt. A `provider_default` status alone is useful
    // provenance, but does not qualify: LibRaw v1 therefore leaves this false.
    bool dng_opcode_execution_receipt = false;
    std::uint32_t supported_intents = 0U;
    std::uint32_t supported_qualities = 0U;
    std::uint32_t supported_dng_opcode_policies = 0U;
    std::uint32_t supported_noise_reduction_intents = 0U;
    std::uint32_t supported_highlight_recovery_intents = 0U;

    [[nodiscard]] bool supports(const RawDevelopmentPlan& plan) const noexcept {
        return plan.schema_version == raw_development_plan_schema_version
            && schema_version == raw_development_capabilities_schema_version && available
            && (supported_intents & raw_development_intent_mask(plan.intent)) != 0U
            && (supported_qualities & raw_development_quality_mask(plan.quality)) != 0U
            && (supported_dng_opcode_policies & dng_opcode_policy_mask(plan.dng_opcode_policy))
                != 0U
            && (supported_noise_reduction_intents
                    & raw_noise_reduction_intent_mask(plan.noise_reduction))
                != 0U
            && (supported_highlight_recovery_intents
                    & raw_highlight_recovery_intent_mask(plan.highlight_recovery))
                != 0U;
    }
    auto operator<=>(const RawDevelopmentCapabilities&) const = default;
};

enum class RawDevelopmentPlanNegotiationStatus : std::uint8_t {
    accepted,
    adjusted,
    rejected,
};

struct RawDevelopmentPlanNegotiation final {
    RawDevelopmentPlan requested;
    RawDevelopmentPlan effective;
    RawDevelopmentPlanNegotiationStatus status = RawDevelopmentPlanNegotiationStatus::rejected;
    RawDevelopmentPlanAspect unresolved = RawDevelopmentPlanAspect::none;

    [[nodiscard]] bool accepted() const noexcept {
        return status != RawDevelopmentPlanNegotiationStatus::rejected;
    }

    [[nodiscard]] bool exact() const noexcept {
        return status == RawDevelopmentPlanNegotiationStatus::accepted;
    }
};

// The generic negotiation never silently changes a plan: it returns `accepted` only for an
// exact capability match and `rejected` otherwise. A specialized provider may override
// DecodeSession::negotiate_raw_development_plan to return an explicit `adjusted` effective plan;
// such a provider must record both identities in its receipt.
[[nodiscard]] inline RawDevelopmentPlanNegotiation negotiate_raw_development_plan(
    const RawDevelopmentPlan& plan,
    const RawDevelopmentCapabilities& capabilities
) noexcept {
    RawDevelopmentPlanNegotiation negotiation;
    negotiation.requested = plan;
    negotiation.effective = plan;
    negotiation.status = RawDevelopmentPlanNegotiationStatus::rejected;

    if (plan.schema_version != raw_development_plan_schema_version
        || capabilities.schema_version != raw_development_capabilities_schema_version) {
        negotiation.unresolved |= RawDevelopmentPlanAspect::schema;
    }

    const auto supports = [&capabilities](
                              const std::uint32_t advertised,
                              const std::uint32_t requested
                          ) noexcept {
        return capabilities.available && requested != 0U && (advertised & requested) != 0U;
    };
    if (!supports(capabilities.supported_intents, raw_development_intent_mask(plan.intent))) {
        negotiation.unresolved |= RawDevelopmentPlanAspect::intent;
    }
    if (!supports(capabilities.supported_qualities, raw_development_quality_mask(plan.quality))) {
        negotiation.unresolved |= RawDevelopmentPlanAspect::quality;
    }
    if (!supports(
            capabilities.supported_dng_opcode_policies,
            dng_opcode_policy_mask(plan.dng_opcode_policy)
        )) {
        negotiation.unresolved |= RawDevelopmentPlanAspect::dng_opcode_policy;
    }
    if (!supports(
            capabilities.supported_noise_reduction_intents,
            raw_noise_reduction_intent_mask(plan.noise_reduction)
        )) {
        negotiation.unresolved |= RawDevelopmentPlanAspect::noise_reduction;
    }
    if (!supports(
            capabilities.supported_highlight_recovery_intents,
            raw_highlight_recovery_intent_mask(plan.highlight_recovery)
        )) {
        negotiation.unresolved |= RawDevelopmentPlanAspect::highlight_recovery;
    }
    if (negotiation.unresolved == RawDevelopmentPlanAspect::none) {
        negotiation.status = RawDevelopmentPlanNegotiationStatus::accepted;
    }
    return negotiation;
}

struct DecodeCapabilities final {
    bool metadata = false;
    bool embedded_previews = false;
    // An independently owned, sensor-coordinate RawFrame is available. This is deliberately
    // distinct from `reference_rgb`: a provider must never claim the latter's already-developed
    // pixels are suitable input for Bayer-domain work.
    bool raw_frame = false;
    bool reference_rgb = false;
    PendingCorrections pending_corrections;
    RawDevelopmentCapabilities raw_development;
};

struct AssetMetadata final {
    std::string make;
    std::string model;
    std::string normalized_make;
    std::string normalized_model;
    std::string dng_version;
    std::uint32_t raw_count = 0;
    Dimensions raw_dimensions;
    Dimensions image_dimensions;
    Margins margins;
    std::int32_t orientation = 0;
    std::string cfa_pattern;
    std::uint32_t sensor_colors = 0;
    std::uint32_t sensor_bits = 0;
    std::uint32_t black_level = 0;
    std::uint32_t white_level = 0;
    std::array<double, 4> as_shot_neutral{};
    double baseline_exposure = 0.0;
    double iso_speed = 0.0;
    double exposure_time_seconds = 0.0;
    double aperture_f_number = 0.0;
    double focal_length_mm = 0.0;
    // Approximate focus distance in metres when a provider can establish it.  Zero means
    // unknown, never infinity or a guessed substitute. Lens vignetting calibration is
    // distance-dependent, so optical correction must leave that component disabled without it.
    double focus_distance_meters = 0.0;
    std::int64_t captured_at_unix_seconds = 0;
    std::string lens_make;
    std::string lens_model;
    double focal_length_35mm = 0.0;
};

struct PreviewDescriptor final {
    std::size_t id = 0;
    PreviewFormat format = PreviewFormat::unknown;
    Dimensions dimensions;
    std::uint16_t bits_per_channel = 0;
    std::uint16_t channels = 0;
    std::uint64_t encoded_bytes = 0;
    bool decodable = false;
};

struct PreviewPayload final {
    PreviewDescriptor descriptor;
    ByteOrder byte_order = ByteOrder::not_applicable;
    std::vector<std::uint8_t> bytes;
};

// RawFrame is the provider-neutral boundary for *unprocessed* sensor samples. It owns a tight
// native-endian u16 plane in raw sensor coordinates: no crop, orientation, black subtraction,
// white balance, DNG opcode, demosaic, RGB conversion, highlight recovery or denoise has been
// applied. A provider may expose a non-Bayer RawFrame, but Bayer-only stages must require the
// explicit `bayer_2x2` layout rather than attempting to infer one from a display string.
//
// Shadow is still in its fast, pre-release iteration phase: this number names the one current
// RawFrame layout, not a backwards-compatibility promise. When the layout changes, all local
// providers are rebuilt together and obsolete artifacts are discarded.
inline constexpr std::uint32_t raw_frame_schema_version = 1U;

enum class RawFrameSampleEncoding : std::uint8_t {
    uint16_native,
};

enum class RawFrameCfaLayout : std::uint8_t {
    // A frame can preserve samples from a sensor whose mosaic Shadow does not yet understand.
    // Its metadata string remains useful for inspection, but demosaic must decline it.
    unknown,
    bayer_2x2,
    monochrome,
};

enum class RawCfaColor : std::uint8_t {
    unknown,
    red,
    green,
    blue,
};

// A provider resolves this for the individual source file from validated metadata or a locally
// installed calibration database. Shadow stores only a numeric model in a known unit, never an
// opaque vendor profile format. For `poisson_gaussian_per_cfa`, variance in raw-DN squared is
// `shot_noise_variance_per_dn * max(sample - black_level, 0) + read_noise_stddev_dn^2`.
inline constexpr std::uint32_t raw_sensor_noise_calibration_schema_version = 1U;

enum class RawSensorNoiseModel : std::uint8_t {
    unavailable,
    poisson_gaussian_per_cfa,
};

enum class RawSensorNoiseCalibrationSource : std::uint8_t {
    unavailable,
    embedded_metadata,
    // A configured provider matched a locally installed calibration profile for this exact
    // camera/ISO. The underlying profile stays private and is never put in a Shadow catalog,
    // recipe or public plugin.
    provider_calibration_profile,
};

struct RawSensorNoiseCalibration final {
    std::uint32_t schema_version = raw_sensor_noise_calibration_schema_version;
    RawSensorNoiseModel model = RawSensorNoiseModel::unavailable;
    RawSensorNoiseCalibrationSource source = RawSensorNoiseCalibrationSource::unavailable;
    // The ISO at which the model was resolved. It is zero only when no model is available.
    double iso_sensitivity = 0.0;
    // Order is R, G1, G2, B, matching `bayer_2x2` and black/white-level arrays.
    std::array<double, 4U> read_noise_stddev_dn{};
    std::array<double, 4U> shot_noise_variance_per_dn{};

    [[nodiscard]] bool valid() const noexcept {
        if (schema_version != raw_sensor_noise_calibration_schema_version) {
            return false;
        }
        if (model == RawSensorNoiseModel::unavailable) {
            return source == RawSensorNoiseCalibrationSource::unavailable
                && iso_sensitivity == 0.0;
        }
        if (
            model != RawSensorNoiseModel::poisson_gaussian_per_cfa
            || source == RawSensorNoiseCalibrationSource::unavailable
            || !std::isfinite(iso_sensitivity) || iso_sensitivity <= 0.0
        ) {
            return false;
        }
        for (std::size_t index = 0U; index < read_noise_stddev_dn.size(); ++index) {
            if (
                !std::isfinite(read_noise_stddev_dn[index])
                || !std::isfinite(shot_noise_variance_per_dn[index])
                || read_noise_stddev_dn[index] < 0.0
                || shot_noise_variance_per_dn[index] <= 0.0
            ) {
                return false;
            }
        }
        return true;
    }
};

struct RawFrameDescriptor final {
    std::uint32_t schema_version = raw_frame_schema_version;
    // Cache-visible identity of the provider that extracted these sensor samples. Generic
    // fixtures may leave both fields empty, but a production provider must set both together.
    // This belongs to the frame rather than a later rendered receipt because two providers can
    // expose different unpacked samples or calibration for the same source bytes.
    std::string provider_id;
    std::string provider_version;
    // `storage_dimensions` covers the full sensor plane. `active_margins` and
    // `active_dimensions` identify the visible active rectangle inside it, before orientation.
    Dimensions storage_dimensions;
    Dimensions active_dimensions;
    Margins active_margins;
    std::int32_t orientation = 0;
    RawFrameSampleEncoding sample_encoding = RawFrameSampleEncoding::uint16_native;
    RawFrameCfaLayout cfa_layout = RawFrameCfaLayout::unknown;
    // Row-major colours at raw coordinates (0,0), (1,0), (0,1), (1,1). They are meaningful
    // only for `bayer_2x2`; green has two deliberately separate slots for per-site calibration.
    std::array<RawCfaColor, 4U> bayer_2x2{};
    std::string cfa_pattern;
    std::uint32_t bits_per_sample = 0;
    std::array<std::uint32_t, 4U> black_levels{};
    std::array<std::uint32_t, 4U> white_levels{};
    std::array<double, 4U> as_shot_neutral{};
    // Optional resolved source calibration for later RAW-domain denoise. A provider must leave
    // this unavailable rather than guessing by scanning an arbitrary profile binary.
    RawSensorNoiseCalibration sensor_noise;
    // Optional, row-major Camera RGB -> CIE XYZ matrix under a D50 white point.  The input
    // order is the camera-linear RGB frame produced after the two green sites have been
    // reconstructed into its single green channel: `XYZ[j] = sum_i camera_rgb[i] * M[i][j]`.
    // A provider must leave `has_camera_to_xyz_d50` false rather than guessing the white point
    // or relabelling a camera-to-sRGB matrix as XYZ D50.
    std::array<double, 9U> camera_to_xyz_d50{};
    bool has_camera_to_xyz_d50 = false;
    // Optional, row-major Camera RGB -> linear sRGB/Rec.709 under D65. The camera input order is
    // canonical R, G, B after the two green CFA sites have been reconstructed into one channel:
    // `linear_srgb[row] = sum(camera_rgb[column] * M[row * 3 + column])`.
    //
    // LibRaw exposes this transform directly as `rgb_cam`. Keeping it distinct from the D50 XYZ
    // matrix prevents a provider from silently changing the transform's output space or white
    // point. A provider leaves the flag false when it cannot establish a usable transform.
    std::array<double, 9U> camera_to_linear_srgb_d65{};
    bool has_camera_to_linear_srgb_d65 = false;
    PendingCorrections declared_pending_corrections;
};

struct RawFrame final {
    RawFrameDescriptor descriptor;
    std::vector<std::uint16_t> samples;

    [[nodiscard]] bool valid() const noexcept {
        const auto width = static_cast<std::uint64_t>(descriptor.storage_dimensions.width);
        const auto height = static_cast<std::uint64_t>(descriptor.storage_dimensions.height);
        if (
            descriptor.schema_version != raw_frame_schema_version || width == 0U || height == 0U
            || descriptor.active_dimensions.width == 0U || descriptor.active_dimensions.height == 0U
            || descriptor.sample_encoding != RawFrameSampleEncoding::uint16_native
            || descriptor.cfa_pattern.empty() || !descriptor.sensor_noise.valid()
            || descriptor.provider_id.empty() != descriptor.provider_version.empty()
        ) {
            return false;
        }
        const auto sample_count = width * height;
        if (
            sample_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
            || samples.size() != static_cast<std::size_t>(sample_count)
        ) {
            return false;
        }
        const auto right = static_cast<std::uint64_t>(descriptor.active_margins.left)
            + descriptor.active_dimensions.width + descriptor.active_margins.right;
        const auto bottom = static_cast<std::uint64_t>(descriptor.active_margins.top)
            + descriptor.active_dimensions.height + descriptor.active_margins.bottom;
        if (right != width || bottom != height || descriptor.bits_per_sample == 0U
            || descriptor.bits_per_sample > 16U) {
            return false;
        }
        for (std::size_t index = 0U; index < descriptor.black_levels.size(); ++index) {
            if (
                descriptor.black_levels[index] >= descriptor.white_levels[index]
                || !std::isfinite(descriptor.as_shot_neutral[index])
                || descriptor.as_shot_neutral[index] <= 0.0
            ) {
                return false;
            }
        }
        const auto valid_declared_matrix = [](const auto& matrix, const bool declared) noexcept {
            if (!declared) {
                return true;
            }
            bool has_non_zero_coefficient = false;
            for (const auto value : matrix) {
                if (!std::isfinite(value)) {
                    return false;
                }
                has_non_zero_coefficient = has_non_zero_coefficient || value != 0.0;
            }
            return has_non_zero_coefficient;
        };
        if (
            !valid_declared_matrix(
                descriptor.camera_to_xyz_d50,
                descriptor.has_camera_to_xyz_d50
            )
            || !valid_declared_matrix(
                descriptor.camera_to_linear_srgb_d65,
                descriptor.has_camera_to_linear_srgb_d65
            )
        ) {
            return false;
        }
        if (descriptor.cfa_layout != RawFrameCfaLayout::bayer_2x2) {
            return true;
        }
        std::size_t red = 0U;
        std::size_t green = 0U;
        std::size_t blue = 0U;
        for (const auto color : descriptor.bayer_2x2) {
            red += color == RawCfaColor::red ? 1U : 0U;
            green += color == RawCfaColor::green ? 1U : 0U;
            blue += color == RawCfaColor::blue ? 1U : 0U;
        }
        return red == 1U && green == 2U && blue == 1U;
    }

    [[nodiscard]] bool is_bayer_2x2() const noexcept {
        return valid() && descriptor.cfa_layout == RawFrameCfaLayout::bayer_2x2;
    }
};

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
// Version 2 adds the provider-neutral development plan identity and the per-list DNG opcode
// outcome. It intentionally does not retroactively interpret a v1 receipt: a rendering system
// must regenerate source pixels if it needs plan-aware cache/provenance data.
inline constexpr std::uint32_t raw_development_receipt_schema_version = 2U;

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
    // Both strings must be canonical `raw_development_plan_identity(...)` values.  They are
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

struct PixelBuffer final {
    Dimensions dimensions;
    std::uint16_t bits_per_channel = 0;
    std::uint16_t channels = 0;
    std::size_t row_stride_bytes = 0;
    RgbPrimaries primaries = RgbPrimaries::unknown;
    RgbTransferFunction transfer_function = RgbTransferFunction::unknown;
    RgbBufferReference reference = RgbBufferReference::unknown;
    std::vector<std::uint16_t> samples;
    RawDevelopmentReceipt raw_development_receipt;
};

// Version 1 fixes linear gamma, camera WB/matrix conversion, unit brightness, no exposure or
// histogram auto-brightening, and no frame-content adaptive maximum rescaling. Any change to
// those decode semantics must increment this cache-visible contract version.
inline constexpr std::uint32_t processed_linear_reference_rgb_contract_version = 1U;
inline constexpr float processed_linear_reference_maximum_adjustment_threshold = 0.0F;
// Version 6 accepts both standardized scene-referred RAW RGB and standardized display-referred
// raster RGB. RAW first receives Shadow's neutral scene-to-display curve; JPEG/SDR HEIF keeps
// its existing display rendering and receives only gamut mapping plus the sRGB OETF. JPEG proxy
// encoding uses 4:4:4 sampling so this output contract does not discard chroma detail after
// rendering. It is a deterministic SDR display rendering, not a camera-JPEG emulation.
inline constexpr std::uint32_t display_srgb8_output_transform_version = 6U;
// The v4 gamut mapper is bounded work per out-of-gamut pixel. 0.5 is a conservative ceiling
// above the display-sRGB Oklab gamut; sixteen bisections resolve chroma well below one 8-bit code
// step.
inline constexpr double display_srgb8_maximum_oklab_chroma = 0.5;
inline constexpr std::uint32_t display_srgb8_gamut_search_iterations = 16U;

struct ProxyRequest final {
    std::uint32_t max_edge = 2'048;
    std::uint8_t jpeg_quality = 95;
};

struct EncodedProxy final {
    Dimensions dimensions;
    PreviewFormat format = PreviewFormat::jpeg;
    std::uint16_t bits_per_channel = 8;
    std::uint16_t channels = 3;
    std::vector<std::uint8_t> bytes;
};

enum class DecodeErrorCode : std::uint8_t {
    unsupported,
    io,
    corrupt_data,
    no_preview,
    unsupported_layout,
    invalid_request,
    resource_limit,
    cancelled,
    internal,
};

class DecodeError final : public std::runtime_error {
public:
    DecodeError(DecodeErrorCode code, int provider_code, std::string message);

    [[nodiscard]] DecodeErrorCode code() const noexcept;
    [[nodiscard]] int provider_code() const noexcept;

private:
    DecodeErrorCode code_;
    int provider_code_;
};

class DecodeSession {
public:
    DecodeSession() = default;
    DecodeSession(const DecodeSession&) = delete;
    DecodeSession& operator=(const DecodeSession&) = delete;
    DecodeSession(DecodeSession&&) = delete;
    DecodeSession& operator=(DecodeSession&&) = delete;
    virtual ~DecodeSession() = default;

    [[nodiscard]] virtual const AssetMetadata& metadata() const noexcept = 0;
    [[nodiscard]] virtual const DecodeCapabilities& capabilities() const noexcept = 0;
    [[nodiscard]] virtual std::span<const PreviewDescriptor> previews() const noexcept = 0;
    [[nodiscard]] virtual PreviewPayload decode_preview(std::size_t id) = 0;
    [[nodiscard]] virtual RawFrame decode_raw_frame() = 0;
    // RawFrame extraction is logically a source render. Existing provider ABIs expose a
    // non-const entry because unpacking may populate private decoder caches; the host-facing
    // const overload preserves the immutable DecodeSession API while delegating to that cache.
    // Providers should keep all externally observable metadata/capabilities unchanged.
    [[nodiscard]] virtual RawFrame decode_raw_frame() const {
        return const_cast<DecodeSession*>(this)->decode_raw_frame();
    }
    [[nodiscard]] virtual PixelBuffer render_reference_rgb() const = 0;

    // RAW providers advertise their exact source-development contract here. Rendered-raster
    // providers intentionally leave it unavailable: JPEG/HEIF still share the edit graph, but a
    // RAW plan has no hidden effect on their already-developed pixels.
    [[nodiscard]] virtual const RawDevelopmentCapabilities& raw_development_capabilities() const
        noexcept {
        return capabilities().raw_development;
    }

    [[nodiscard]] virtual RawDevelopmentPlanNegotiation negotiate_raw_development_plan(
        const RawDevelopmentPlan& plan
    ) const noexcept {
        return shadow::image::negotiate_raw_development_plan(
            plan,
            raw_development_capabilities()
        );
    }

    // These overloads keep existing third-party and test sessions source-compatible. A provider
    // that does not opt into RawDevelopmentCapabilities simply preserves its legacy render path;
    // callers can observe the unavailable negotiation result and no RAW receipt is fabricated.
    // Plan-aware RAW providers must override them and record the requested/effective plan in the
    // returned RawDevelopmentReceipt.
    [[nodiscard]] virtual PixelBuffer render_reference_rgb(
        const RawDevelopmentPlan& plan
    ) const {
        (void)plan;
        return render_reference_rgb();
    }

    // Interactive preview is allowed to ask a provider for a bounded-quality reference. The
    // default keeps third-party/provider test implementations exact; LibRaw overrides it with
    // its documented half-size RAW path only when the native frame is far larger than the
    // requested preview. Full-detail rendering always calls render_reference_rgb().
    [[nodiscard]] virtual PixelBuffer render_reference_rgb_for_preview(
        std::uint32_t max_edge
    ) const {
        (void)max_edge;
        return render_reference_rgb();
    }

    [[nodiscard]] virtual PixelBuffer render_reference_rgb_for_preview(
        const std::uint32_t max_edge,
        const RawDevelopmentPlan& plan
    ) const {
        (void)plan;
        return render_reference_rgb_for_preview(max_edge);
    }
};

class DecoderProvider {
public:
    DecoderProvider() = default;
    DecoderProvider(const DecoderProvider&) = delete;
    DecoderProvider& operator=(const DecoderProvider&) = delete;
    DecoderProvider(DecoderProvider&&) = delete;
    DecoderProvider& operator=(DecoderProvider&&) = delete;
    virtual ~DecoderProvider() = default;

    [[nodiscard]] virtual const ProviderInfo& info() const noexcept = 0;
    [[nodiscard]] virtual std::unique_ptr<DecodeSession> open(
        const std::filesystem::path& path
    ) const = 0;
};

[[nodiscard]] std::unique_ptr<DecoderProvider> make_libraw_decoder_provider(
    LibRawDevelopmentSettings settings = default_libraw_development_settings()
);

// Decodes ordinary display-referred image files into Shadow's common 16-bit linear sRGB
// reference contract. JPEG is built in through libjpeg-turbo; HEIF/HEIC availability is an
// optional backend capability and is reported as an explicit unsupported error when omitted
// from a local build. This provider never pretends to expose sensor RawFrames or RAW development
// provenance.
[[nodiscard]] std::unique_ptr<DecoderProvider> make_raster_decoder_provider();

// File extensions that the raster provider in this binary can actually decode. This is a
// capability query rather than a broad format claim: the catalog must not schedule HEIF simply
// because it recognizes the suffix when this build deliberately omitted libheif.
[[nodiscard]] std::vector<std::string> raster_supported_file_extensions();

// Routes a path to the appropriate public provider. It is intentionally the desktop application's
// normal entry point: a catalog item should not need to know whether it was shot as RAW or
// delivered as JPEG/HEIF in order to reach the common non-destructive edit graph. If
// `SHADOW_PRIVATE_DECODER_PLUGIN_PATH` names an explicit local module, the router tries that
// module first for non-raster files and falls back to LibRaw only when the module declares the
// source unsupported. Without this temporary override, the router discovers v1 link files in
// the per-user plugin root (`~/Library/Application Support/Shadow/plugins/decoders` on macOS;
// platform equivalents elsewhere). A link names a locally compiled private module; neither its
// SDK nor its implementation enters the Shadow repository or catalog.
[[nodiscard]] std::unique_ptr<DecoderProvider> make_photo_decoder_provider();

// Explicit-test and embedding form of the normal router. An empty path is exactly equivalent to
// the environment/discovered form above. The path is never scanned, copied, persisted, or
// distributed by Shadow; it is only passed to the local private-plugin loader for this provider
// instance.
[[nodiscard]] std::unique_ptr<DecoderProvider> make_photo_decoder_provider(
    const std::filesystem::path& private_decoder_plugin_path
);

[[nodiscard]] std::optional<std::size_t> select_best_preview(
    std::span<const PreviewDescriptor> previews
) noexcept;

[[nodiscard]] std::string_view to_string(PreviewFormat format) noexcept;

[[nodiscard]] Dimensions proxy_dimensions(Dimensions source, std::uint32_t max_edge);

[[nodiscard]] EncodedProxy render_reference_proxy_jpeg(
    const DecodeSession& session,
    ProxyRequest request = {}
);

} // namespace shadow::image
