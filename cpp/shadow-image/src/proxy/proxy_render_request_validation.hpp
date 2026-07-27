#pragma once

#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/raw_development_plan.hpp>

#include <string_view>

namespace shadow::image::proxy_detail {

void validate_proxy_request(ProxyRequest request);

void validate_raw_development_plan_intent(
    const RawDevelopmentPlan& plan,
    RawDevelopmentIntent required_intent,
    std::string_view operation
);

} // namespace shadow::image::proxy_detail
