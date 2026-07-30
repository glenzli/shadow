#include "resident_raw_source.hpp"

#include "metal_resident_raw_source.hpp"

#include <shadow/image/dcp_color_development.hpp>
#include <shadow/image/decoder_error.hpp>

#include "raw_denoise_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace shadow::image::raw_pipeline_detail {

namespace {

[[nodiscard]] std::uint64_t checked_retained_bytes(const RawFrame& frame) {
    if (frame.samples.capacity()
        > std::numeric_limits<std::uint64_t>::max() / sizeof(std::uint16_t)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "resident RAW source retained byte count overflows"
        );
    }
    return static_cast<std::uint64_t>(frame.samples.capacity()) * sizeof(std::uint16_t);
}

} // namespace

ResidentRawSource::ResidentRawSource(
    RawFrame frame,
    PreparedRawFrameDevelopment development,
    detail::PreparedSceneLinearRegionOptics optics,
    RawDevelopmentReceipt raw_development_receipt,
    RawPipelineReceipt raw_pipeline_receipt,
    const std::uint64_t retained_bytes
) :
    frame_(std::move(frame)), development_(std::move(development)), optics_(std::move(optics)),
    raw_development_receipt_(std::move(raw_development_receipt)),
    raw_pipeline_receipt_(std::move(raw_pipeline_receipt)), retained_bytes_(retained_bytes) {}

ResidentRawSource::ResidentRawSource(
    std::unique_ptr<MetalResidentRawSource> metal_source,
    PreparedRawFrameDevelopment development,
    detail::PreparedSceneLinearRegionOptics optics,
    RawDevelopmentReceipt raw_development_receipt,
    RawPipelineReceipt raw_pipeline_receipt
) :
    development_(std::move(development)), optics_(std::move(optics)),
    raw_development_receipt_(std::move(raw_development_receipt)),
    raw_pipeline_receipt_(std::move(raw_pipeline_receipt)), metal_source_(std::move(metal_source)) {
}

ResidentRawSource::ResidentRawSource(ResidentRawSource&& other) noexcept :
    frame_(std::move(other.frame_)), development_(std::move(other.development_)),
    optics_(std::move(other.optics_)),
    raw_development_receipt_(std::move(other.raw_development_receipt_)),
    raw_pipeline_receipt_(std::move(other.raw_pipeline_receipt_)),
    retained_bytes_(other.retained_bytes_), metal_source_(std::move(other.metal_source_)),
    device_path_invalidated_(other.device_path_invalidated_.load(std::memory_order_acquire)) {}

ResidentRawSource::~ResidentRawSource() = default;

Dimensions ResidentRawSource::dimensions() const noexcept {
    if (metal_source_ != nullptr) {
        return metal_source_->dimensions();
    }
    return oriented_raw_dimensions(
        development_.reconstruction_dimensions(),
        development_.descriptor().orientation
    );
}

std::uint64_t ResidentRawSource::retained_bytes() const noexcept {
    return metal_source_ == nullptr ? retained_bytes_ : metal_source_->retained_bytes();
}

const RawDevelopmentReceipt& ResidentRawSource::raw_development_receipt() const noexcept {
    return raw_development_receipt_;
}

const RawPipelineReceipt& ResidentRawSource::raw_pipeline_receipt() const noexcept {
    return raw_pipeline_receipt_;
}

const OpticsProfileReceipt& ResidentRawSource::optics_receipt() const noexcept {
    return optics_.receipt();
}

bool ResidentRawSource::metal_resident() const noexcept {
    return metal_source_ != nullptr;
}

bool ResidentRawSource::device_path_valid() const noexcept {
    return metal_source_ != nullptr && metal_source_->valid()
           && !device_path_invalidated_.load(std::memory_order_acquire);
}

const MetalResidentRawSource& ResidentRawSource::metal_source() const {
    if (metal_source_ == nullptr) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "resident RAW source does not own a Metal representation"
        );
    }
    if (device_path_invalidated_.load(std::memory_order_acquire) || !metal_source_->valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "resident RAW source Metal representation is terminally invalidated"
        );
    }
    return *metal_source_;
}

const detail::PreparedSceneLinearRegionOptics& ResidentRawSource::region_optics() const noexcept {
    return optics_;
}

void ResidentRawSource::invalidate_device_path() const noexcept {
    device_path_invalidated_.store(true, std::memory_order_release);
    if (metal_source_ != nullptr) {
        metal_source_->invalidate();
    }
}

PreparedRawFrameRegion
ResidentRawSource::prepare_region(const GeometryPixelRect requested_core) const {
    if (metal_source_ != nullptr) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Metal resident RAW source cannot execute through the CPU region API"
        );
    }
    return prepare_raw_frame_region(
        frame_,
        development_.development_plan().quality,
        development_.raw_denoise(),
        requested_core
    );
}

DevelopedResidentRawRegion
ResidentRawSource::develop_region(const GeometryPixelRect requested_core) const {
    PreparedRawFrameRegion dependency_plan = prepare_region(requested_core);
    SceneLinearRgbFrame scene_linear = develop_raw_frame_region_cpu(
        frame_,
        development_.linear_transform(),
        development_.development_plan().highlight_recovery,
        dependency_plan
    );
    const DcpColorTransform* camera_profile = development_.camera_profile();
    if (camera_profile != nullptr && camera_profile->has_post_matrix_stages()) {
        const DcpColorExecutionBackend backend =
            apply_dcp_color_rendering_stages(scene_linear, *camera_profile);
        if (backend != DcpColorExecutionBackend::cpu) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                "CPU resident RAW source unexpectedly selected a non-CPU DCP backend"
            );
        }
    }
    detail::apply_scene_linear_region_optics(
        scene_linear,
        dependency_plan.optics_output_preimage(),
        optics_
    );
    return DevelopedResidentRawRegion{
        .dependency_plan = dependency_plan,
        .scene_linear = std::move(scene_linear),
    };
}

bool cpu_resident_raw_source_supported(
    const PreparedRawFrameSource& prepared,
    const detail::PreparedSceneLinearRegionOptics& optics
) noexcept {
    // Automatic/Metal currently preserve their existing full-frame Metal transaction. Phase C
    // will replace that with a resident device resource; selecting CPU here would change both
    // numeric provenance and the published backend receipt on Metal-capable machines.
    return prepared.owns_region_optics(optics)
           && prepared.development().preview_max_edge() == std::nullopt
           && prepared.development().requested_backend() == RawDevelopmentBackendMode::cpu
           && optics.resident_eligible();
}

ResidentRawSource prepare_resident_raw_source(
    PreparedRawFrameSource prepared,
    detail::PreparedSceneLinearRegionOptics optics
) {
    if (!cpu_resident_raw_source_supported(prepared, optics)) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "prepared RAW source is not eligible for CPU region residency"
        );
    }

    const RawDevelopmentPlanNegotiationStatus plan_negotiation_status =
        prepared.plan_negotiation_status_;
    RawPipelineReceipt pipeline = std::move(prepared.pipeline_);
    detail::NeuralRawDenoiseResult neural_denoised =
        detail::execute_prepared_neural_raw_denoise(
            std::move(prepared.frame_),
            prepared.development_.neural_raw_denoise()
        );
    RawBayerDenoiseResult denoised = detail::execute_prepared_raw_bayer_denoise(
        std::move(neural_denoised.frame),
        prepared.development_.raw_denoise()
    );
    if (denoised.receipt.backend != RawBayerDenoiseBackend::cpu) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "CPU resident RAW source unexpectedly selected non-CPU CFA denoise"
        );
    }
    const RawDemosaicReceipt demosaic = raw_frame_region_demosaic_receipt(
        denoised.frame,
        prepared.development_.development_plan().quality
    );
    RawDevelopmentReceipt raw_receipt = finalize_raw_frame_development_receipt(
        prepared.development_,
        oriented_raw_dimensions(
            prepared.development_.reconstruction_dimensions(),
            prepared.development_.descriptor().orientation
        ),
        demosaic,
        RawDevelopmentBackend::cpu,
        neural_denoised.receipt,
        denoised.receipt,
        DcpColorExecutionBackend::cpu
    );
    raw_receipt.requested_plan = pipeline.requested_plan;
    raw_receipt.requested_plan_identity = raw_development_plan_identity(pipeline.requested_plan);
    raw_receipt.effective_plan = pipeline.effective_plan;
    raw_receipt.effective_plan_identity = raw_development_plan_identity(pipeline.effective_plan);
    raw_receipt.plan_negotiation_status = plan_negotiation_status;
    pipeline = finalize_raw_frame_pipeline_receipt(
        std::move(pipeline),
        RawDevelopmentBackend::cpu,
        prepared.development_.development_plan().highlight_recovery,
        detail::combined_raw_denoise_cache_identity(
            neural_denoised.receipt,
            denoised.receipt
        )
    );
    const std::uint64_t retained_bytes = checked_retained_bytes(denoised.frame);
    return ResidentRawSource(
        std::move(denoised.frame),
        std::move(prepared.development_),
        std::move(optics),
        std::move(raw_receipt),
        std::move(pipeline),
        retained_bytes
    );
}

} // namespace shadow::image::raw_pipeline_detail
