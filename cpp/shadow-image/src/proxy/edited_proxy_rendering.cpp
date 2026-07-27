#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/edited_proxy_rendering.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include "proxy_render_request_validation.hpp"

#include <span>

namespace shadow::image {

EncodedProxy render_edited_reference_proxy_jpeg(
    const DecodeSession& session,
    const std::span<const AdjustmentNode> nodes,
    const ProxyRequest request,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    return render_edited_reference_proxy_jpeg(
        session,
        nodes,
        request,
        preview_raw_development_plan(),
        optics_provider,
        optics_settings
    );
}

EncodedProxy render_edited_reference_proxy_jpeg(
    const DecodeSession& session,
    const std::span<const AdjustmentNode> nodes,
    const ProxyRequest request,
    const RawDevelopmentPlan& raw_development_plan,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    proxy_detail::validate_proxy_request(request);
    validate_adjustment_nodes(nodes);
    const WarmEditPreviewSession preview = prepare_warm_edit_preview(
        session,
        request.max_edge,
        raw_development_plan,
        optics_provider,
        optics_settings
    );
    return preview.render_jpeg(nodes, request.jpeg_quality);
}

EncodedProxy render_edited_reference_proxy_jpeg_layers(
    const DecodeSession& session,
    const std::span<const AdjustmentLayer> layers,
    const ProxyRequest request,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    return render_edited_reference_proxy_jpeg_layers(
        session,
        layers,
        request,
        preview_raw_development_plan(),
        optics_provider,
        optics_settings
    );
}

EncodedProxy render_edited_reference_proxy_jpeg_layers(
    const DecodeSession& session,
    const std::span<const AdjustmentLayer> layers,
    const ProxyRequest request,
    const RawDevelopmentPlan& raw_development_plan,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    proxy_detail::validate_proxy_request(request);
    const WarmEditPreviewSession preview = prepare_warm_edit_preview(
        session,
        request.max_edge,
        raw_development_plan,
        optics_provider,
        optics_settings
    );
    return preview.render_jpeg_layers(layers, request.jpeg_quality);
}

} // namespace shadow::image
