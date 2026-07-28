#include "libraw_reference_development.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/libraw_development_settings.hpp>
#include <shadow/image/raw_development_plan.hpp>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

using shadow::image::DecodeError;
using shadow::image::DecodeErrorCode;
using shadow::image::Dimensions;
using shadow::image::LibRawReferenceDeveloper;
using shadow::image::ProviderInfo;
using shadow::image::RawDevelopmentIntent;
using shadow::image::RawDevelopmentPlanNegotiationStatus;
using shadow::image::RawDevelopmentQuality;
using shadow::image::default_libraw_development_settings;
using shadow::image::default_raw_development_plan;
using shadow::image::libraw_development_settings_signature;
using shadow::image::preview_raw_development_plan;
using shadow::image::validate_libraw_development_settings;

namespace {

[[nodiscard]] ProviderInfo provider_info() {
    ProviderInfo info;
    info.id = "libraw";
    info.version = "reference-contract";
    return info;
}

void settings_contract() {
    const auto settings = default_libraw_development_settings();
    validate_libraw_development_settings(settings);
    assert(
        libraw_development_settings_signature(settings)
        == "shadow-libraw-develop-v1;wb=camera;matrix=camera;"
           "auto-bright=off;exposure=off;bright=1;max-adjust=0;"
           "bps=16;qual=3"
    );

    auto invalid = settings;
    invalid.output_bits_per_channel = 8U;
    bool rejected = false;
    try {
        validate_libraw_development_settings(invalid);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

void capability_and_negotiation_contract() {
    LibRawReferenceDeveloper developer(
        std::filesystem::path("/not-opened/reference.nef"),
        default_libraw_development_settings(),
        provider_info(),
        Dimensions{8'256U, 5'504U},
        true,
        false,
        0U
    );
    const auto& capabilities = developer.capabilities();
    assert(capabilities.available);
    assert(!capabilities.raw_frame);
    assert(capabilities.supports(default_raw_development_plan()));
    assert(capabilities.supports(preview_raw_development_plan()));

    auto export_plan = default_raw_development_plan();
    export_plan.intent = RawDevelopmentIntent::export_image;
    export_plan.quality = RawDevelopmentQuality::high;
    const auto negotiation = developer.negotiate(export_plan);
    assert(negotiation.accepted());
    assert(
        negotiation.status
        == RawDevelopmentPlanNegotiationStatus::adjusted
    );
    assert(
        negotiation.effective.quality
        == RawDevelopmentQuality::balanced
    );
}

void render_admission_contract() {
    LibRawReferenceDeveloper developer(
        std::filesystem::path("/not-opened/reference.nef"),
        default_libraw_development_settings(),
        provider_info(),
        Dimensions{8'256U, 5'504U},
        true,
        true,
        0U
    );

    bool rejected_zero_edge = false;
    try {
        static_cast<void>(developer.render_preview(
            0U,
            preview_raw_development_plan()
        ));
    } catch (const DecodeError& error) {
        rejected_zero_edge =
            error.code() == DecodeErrorCode::invalid_request;
    }
    assert(rejected_zero_edge);

    bool rejected_wrong_intent = false;
    try {
        static_cast<void>(developer.render(
            preview_raw_development_plan()
        ));
    } catch (const DecodeError& error) {
        rejected_wrong_intent =
            error.code() == DecodeErrorCode::invalid_request;
    }
    assert(rejected_wrong_intent);
}

} // namespace

int main() {
    settings_contract();
    capability_and_negotiation_contract();
    render_admission_contract();
}
