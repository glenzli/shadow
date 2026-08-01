#include "raw_foundation_projection.hpp"

#include "rust_qt_projection.hpp"

#include <stdexcept>

BackendRawFoundationJobPhase
project_raw_foundation_job_phase(const shadow::desktop::FfiRawFoundationJobPhase source) {
    switch (source) {
    case shadow::desktop::FfiRawFoundationJobPhase::Queued:
        return BackendRawFoundationJobPhase::Queued;
    case shadow::desktop::FfiRawFoundationJobPhase::Planning:
        return BackendRawFoundationJobPhase::Planning;
    case shadow::desktop::FfiRawFoundationJobPhase::Running:
        return BackendRawFoundationJobPhase::Running;
    case shadow::desktop::FfiRawFoundationJobPhase::Ready:
        return BackendRawFoundationJobPhase::Ready;
    case shadow::desktop::FfiRawFoundationJobPhase::Unavailable:
        return BackendRawFoundationJobPhase::Unavailable;
    case shadow::desktop::FfiRawFoundationJobPhase::Cancelled:
        return BackendRawFoundationJobPhase::Cancelled;
    case shadow::desktop::FfiRawFoundationJobPhase::Failed:
        return BackendRawFoundationJobPhase::Failed;
    default:
        throw std::runtime_error("RAW foundation provider returned an unknown job phase");
    }
}

BackendRawFoundationNoiseLevel
project_raw_foundation_noise_level(const shadow::desktop::FfiRawFoundationNoiseLevel source) {
    switch (source) {
    case shadow::desktop::FfiRawFoundationNoiseLevel::Low:
        return BackendRawFoundationNoiseLevel::Low;
    case shadow::desktop::FfiRawFoundationNoiseLevel::Moderate:
        return BackendRawFoundationNoiseLevel::Moderate;
    case shadow::desktop::FfiRawFoundationNoiseLevel::High:
        return BackendRawFoundationNoiseLevel::High;
    default:
        throw std::runtime_error("RAW foundation returned an unknown noise level");
    }
}

BackendRawFoundationNoiseAssessment project_raw_foundation_noise_assessment(
    const shadow::desktop::FfiRawFoundationNoiseAssessment& source
) {
    using desktop_backend_projection::qstring;
    return {
        .level = project_raw_foundation_noise_level(source.level),
        .score_percent = source.score_percent,
        .confidence_percent = source.confidence_percent,
        .diagnostic = qstring(source.diagnostic),
    };
}

BackendRawFoundationRuntimeStatus project_raw_foundation_runtime_status(
    const shadow::desktop::FfiRawFoundationRuntimeStatus& source
) {
    using desktop_backend_projection::qstring;
    return {
        .available = source.available,
        .model_id = qstring(source.model_id),
        .runtime_version = qstring(source.runtime_version),
        .diagnostic = qstring(source.diagnostic),
    };
}

BackendRawFoundationJobStatus
project_raw_foundation_job_status(const shadow::desktop::FfiRawFoundationJobStatus& source) {
    using desktop_backend_projection::qstring;
    return {
        .job_token = source.job_token,
        .request_id = qstring(source.request_id),
        .generation = source.generation,
        .phase = project_raw_foundation_job_phase(source.phase),
        .phase_code = qstring(source.phase_code),
        .completed_basis_points = source.completed_basis_points,
        .cancellation_requested = source.cancellation_requested,
        .disposition = source.disposition,
        .cache_key_sha256 = qstring(source.cache_key_sha256),
        .artifact_identity_sha256 = qstring(source.artifact_identity_sha256),
        .width = source.width,
        .height = source.height,
        .diagnostic = qstring(source.diagnostic),
    };
}
