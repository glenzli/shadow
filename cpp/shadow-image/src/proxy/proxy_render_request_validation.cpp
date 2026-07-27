#include "proxy_render_request_validation.hpp"

#include "jpeg_proxy_encoding.hpp"

#include <shadow/image/decoder_error.hpp>

#include <cstdint>
#include <string>

namespace shadow::image::proxy_detail {

void validate_proxy_request(const ProxyRequest request) {
    constexpr std::uint32_t maximum_proxy_edge = 16'384;
    if (request.max_edge == 0U || request.max_edge > maximum_proxy_edge) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "proxy max edge must be in 1..=16384"
        );
    }
    validate_jpeg_quality(request.jpeg_quality);
}

void validate_raw_development_plan_intent(
    const RawDevelopmentPlan& plan,
    const RawDevelopmentIntent required_intent,
    const std::string_view operation
) {
    if (plan.schema_version != raw_development_plan_schema_version) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            std::string(operation)
                + " requires the current RawDevelopmentPlan schema"
        );
    }
    if (plan.intent != required_intent) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            std::string(operation)
                + " received a RawDevelopmentPlan with an incompatible intent"
        );
    }

    // Whether a plan is executable is route-dependent. A LibRaw provider can only advertise
    // its processed-RGB fallback, while Shadow's owned RawFrame route can implement additional
    // plans (for example robust CFA denoise) before demosaic. Defer capability negotiation to
    // develop_source_reference(), after the source route has been selected, so the fallback
    // cannot pre-empt a capable host-owned RAW developer.
}

} // namespace shadow::image::proxy_detail
