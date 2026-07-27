#pragma once

#include <compare>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace shadow::image {

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

} // namespace shadow::image
