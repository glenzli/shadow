#include <shadow/image/decoder_error.hpp>
#include <shadow/image/display_output.hpp>
#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/source_rendering.hpp>
#include <shadow/image/working_rgb.hpp>

#include "developed_source_raster.hpp"
#include "jpeg_proxy_encoding.hpp"
#include "proxy_render_request_validation.hpp"

#include <algorithm>
#include <cstdint>
#include <variant>

namespace shadow::image {

Dimensions proxy_dimensions(const Dimensions source, const std::uint32_t max_edge) {
    if (source.width == 0U || source.height == 0U || max_edge == 0U) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "proxy dimensions must be non-zero"
        );
    }
    const std::uint32_t source_edge = std::max(source.width, source.height);
    if (source_edge <= max_edge) {
        return source;
    }
    const auto scaled = [source_edge, max_edge](const std::uint32_t value) {
        const std::uint64_t numerator = static_cast<std::uint64_t>(value) * max_edge;
        return std::max(
            1U,
            static_cast<std::uint32_t>((numerator + source_edge / 2U) / source_edge)
        );
    };
    return Dimensions{scaled(source.width), scaled(source.height)};
}

EncodedProxy render_reference_proxy_jpeg(
    const DecodeSession& session,
    const ProxyRequest request
) {
    return render_reference_proxy_jpeg(
        session,
        request,
        preview_raw_development_plan()
    );
}

EncodedProxy render_reference_proxy_jpeg(
    const DecodeSession& session,
    const ProxyRequest request,
    const RawDevelopmentPlan& raw_development_plan
) {
    proxy_detail::validate_proxy_request(request);
    proxy_detail::validate_raw_development_plan_intent(
        raw_development_plan,
        RawDevelopmentIntent::preview,
        "reference proxy"
    );
    DevelopedSourceReference developed = develop_source_reference(
        session,
        raw_development_plan,
        request.max_edge,
        raw_pipeline_policy_from_environment()
    );
    const Dimensions target = proxy_dimensions(
        proxy_detail::developed_source_dimensions(developed.source),
        request.max_edge
    );
    FloatRgbImage working = proxy_detail::resize_developed_source_to_working(
        developed.source,
        target
    );
    const SourceRenderingReceipt source_rendering = std::visit(
        [&](const auto& value) {
            return resolve_source_rendering(
                value,
                session.metadata(),
                developed.pipeline_receipt
            );
        },
        developed.source
    );
    apply_source_rendering(working, source_rendering);
    auto rendered = render_linear_srgb_to_display_srgb8(
        working,
        DisplayOutputRequest{.target_dimensions = target}
    );

    EncodedProxy proxy;
    proxy.dimensions = target;
    proxy.bytes = proxy_detail::encode_proxy_jpeg(
        rendered.bytes,
        target,
        request.jpeg_quality
    );
    return proxy;
}

} // namespace shadow::image
