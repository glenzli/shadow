#include "backend/desktop_backend_private.hpp"
#include "backend/raw_foundation_projection.hpp"

BackendRawFoundationRuntimeStatus DesktopBackend::probeRawFoundationRuntime() const {
    return project_raw_foundation_runtime_status(impl_->session->probe_raw_foundation_runtime());
}

std::uint64_t DesktopBackend::beginRawFoundationJob(
    const QString& request_id,
    const std::uint64_t generation
) const {
    return impl_->session->begin_raw_foundation_job(request_id.toStdString(), generation);
}

void DesktopBackend::cancelRawFoundationJob(const std::uint64_t raw_foundation_job_token) const {
    impl_->session->cancel_raw_foundation_job(raw_foundation_job_token);
}

BackendRawFoundationJobStatus
DesktopBackend::rawFoundationJobStatus(const std::uint64_t raw_foundation_job_token) const {
    return project_raw_foundation_job_status(
        impl_->session->raw_foundation_job_status(raw_foundation_job_token)
    );
}

BackendRawFoundationJobStatus DesktopBackend::executeRawFoundationJob(
    const std::uint64_t raw_foundation_job_token,
    const QString& photo_id,
    const QString& source_path
) const {
    return project_raw_foundation_job_status(impl_->session->execute_raw_foundation_job(
        raw_foundation_job_token,
        photo_id.toStdString(),
        source_path.toStdString()
    ));
}

void DesktopBackend::retireRawFoundationJob(const std::uint64_t raw_foundation_job_token) const {
    impl_->session->retire_raw_foundation_job(raw_foundation_job_token);
}
