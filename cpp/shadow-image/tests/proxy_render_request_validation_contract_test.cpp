#include "proxy_render_request_validation.hpp"

#include <shadow/image/decoder_error.hpp>

#include <iostream>
#include <string_view>

namespace image = shadow::image;
namespace proxy_detail = shadow::image::proxy_detail;

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

template <typename Function>
void expect_invalid_request(Function&& function, const std::string_view message) {
    try {
        function();
        expect(false, message);
    } catch (const image::DecodeError& error) {
        expect(error.code() == image::DecodeErrorCode::invalid_request, message);
    }
}

void proxy_request_contract_is_owned_at_one_boundary() {
    proxy_detail::validate_proxy_request(
        image::ProxyRequest{.max_edge = 16'384U, .jpeg_quality = 100U}
    );
    expect_invalid_request(
        [] {
            proxy_detail::validate_proxy_request(
                image::ProxyRequest{.max_edge = 0U, .jpeg_quality = 95U}
            );
        },
        "proxy request rejects an empty maximum edge"
    );
    expect_invalid_request(
        [] {
            proxy_detail::validate_proxy_request(
                image::ProxyRequest{.max_edge = 16'385U, .jpeg_quality = 95U}
            );
        },
        "proxy request rejects an oversized maximum edge"
    );
    expect_invalid_request(
        [] {
            proxy_detail::validate_proxy_request(
                image::ProxyRequest{.max_edge = 2'048U, .jpeg_quality = 0U}
            );
        },
        "proxy request delegates JPEG quality validation to the codec owner"
    );
}

void raw_plan_contract_preserves_schema_and_intent_failures() {
    image::RawDevelopmentPlan preview;
    preview.intent = image::RawDevelopmentIntent::preview;
    proxy_detail::validate_raw_development_plan_intent(
        preview,
        image::RawDevelopmentIntent::preview,
        "contract test"
    );

    image::RawDevelopmentPlan stale = preview;
    stale.schema_version = image::raw_development_plan_schema_version + 1U;
    expect_invalid_request(
        [&] {
            proxy_detail::validate_raw_development_plan_intent(
                stale,
                image::RawDevelopmentIntent::preview,
                "contract test"
            );
        },
        "RAW plan validation rejects a stale schema before route negotiation"
    );

    image::RawDevelopmentPlan detail = preview;
    detail.intent = image::RawDevelopmentIntent::detail;
    expect_invalid_request(
        [&] {
            proxy_detail::validate_raw_development_plan_intent(
                detail,
                image::RawDevelopmentIntent::preview,
                "contract test"
            );
        },
        "RAW plan validation rejects an incompatible render intent"
    );
}

} // namespace

int main() {
    proxy_request_contract_is_owned_at_one_boundary();
    raw_plan_contract_preserves_schema_and_intent_failures();
    return failures == 0 ? 0 : 1;
}
