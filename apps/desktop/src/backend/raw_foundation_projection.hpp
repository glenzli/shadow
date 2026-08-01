#pragma once

#include "edit_types.hpp"

namespace shadow::desktop {
enum class FfiRawFoundationJobPhase : std::uint8_t;
enum class FfiRawFoundationNoiseLevel : std::uint8_t;
struct FfiRawFoundationNoiseAssessment;
struct FfiRawFoundationRuntimeStatus;
struct FfiRawFoundationJobStatus;
} // namespace shadow::desktop

[[nodiscard]] BackendRawFoundationJobPhase
project_raw_foundation_job_phase(shadow::desktop::FfiRawFoundationJobPhase source);

[[nodiscard]] BackendRawFoundationNoiseLevel
project_raw_foundation_noise_level(shadow::desktop::FfiRawFoundationNoiseLevel source);

[[nodiscard]] BackendRawFoundationNoiseAssessment project_raw_foundation_noise_assessment(
    const shadow::desktop::FfiRawFoundationNoiseAssessment& source
);

[[nodiscard]] BackendRawFoundationRuntimeStatus
project_raw_foundation_runtime_status(const shadow::desktop::FfiRawFoundationRuntimeStatus& source);

[[nodiscard]] BackendRawFoundationJobStatus
project_raw_foundation_job_status(const shadow::desktop::FfiRawFoundationJobStatus& source);
