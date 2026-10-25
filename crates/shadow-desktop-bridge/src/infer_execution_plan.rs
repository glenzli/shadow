//! Admission of an already executed local Infer vision result into promotion.
//! Model installation and live resource admission stay in Infer; this boundary
//! preserves its exact Build identities and records the actual execution kind.

use shadow_ai::{
    AdmittedModelIdentity, BackendKind, ExecutionRouteIdentity, NumericPrecision,
    ProviderExecutionClass, ProviderIdentity, RunPlan, VisionProvenance,
};

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub(crate) enum AdmissionError {
    FallbackDisclosed,
    MalformedProvenance,
}

pub(crate) fn admit(
    provenance: &VisionProvenance,
    api_contract_revision: &str,
) -> Result<(ExecutionRouteIdentity, RunPlan), AdmissionError> {
    let actual = provenance.actual_execution_provider.to_ascii_lowercase();
    let requested_cuda = provenance
        .requested_execution_provider
        .eq_ignore_ascii_case("cuda");
    if provenance.execution_provider_fallback_reason.is_some()
        || (requested_cuda && actual != "cuda")
    {
        return Err(AdmissionError::FallbackDisclosed);
    }
    let backend_kind = match actual.as_str() {
        "cuda" if requested_cuda => BackendKind::Cuda,
        value if value.contains("coreml") => BackendKind::CoreMl,
        value if value.contains("cpu") => BackendKind::Cpu,
        _ => return Err(AdmissionError::MalformedProvenance),
    };
    let precision = match provenance.precision.to_ascii_lowercase().as_str() {
        "float16" | "fp16" | "f16" => NumericPrecision::Float16,
        "float32" | "fp32" | "f32" => NumericPrecision::Float32,
        _ => return Err(AdmissionError::MalformedProvenance),
    };
    Ok((
        ExecutionRouteIdentity {
            contract_version: shadow_ai::EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
            provider: ProviderIdentity {
                provider_id: provenance.provider.clone(),
                adapter_revision: provenance.postprocessing_identity.clone(),
                execution_class: ProviderExecutionClass::LocalService,
            },
            model: AdmittedModelIdentity::RemoteService {
                service_revision: provenance.runtime.clone(),
                model_id: provenance.model_build.clone(),
                model_revision: provenance.artifact_sha256.clone(),
                api_contract_revision: api_contract_revision.into(),
            },
        },
        RunPlan {
            backend_id: format!("infer-runtime/{}", provenance.deployment),
            backend_kind,
            precision,
            cpu_threads: 1,
            reserved_system_ram_bytes: 0,
            reserved_device_memory_bytes: 0,
        },
    ))
}

#[cfg(test)]
mod tests;
