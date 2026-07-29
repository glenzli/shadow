use std::collections::BTreeSet;

use serde::{Deserialize, Serialize};

use crate::{
    AI_JOB_REQUEST_CONTRACT_VERSION, AdmissionBlocker, AdmissionDecision, AdmissionRequest,
    AiCapability, AiJobRequest, BackendKind, HardwareProfile, ModelAvailability, ModelManifest,
    ResourceEstimate, ResourcePolicy, RunPlan, admit,
};

use super::{
    AdmittedModelIdentity, ExecutionPlanIdentity, ExecutionRouteIdentity, FallbackDisclosure,
    ProviderExecutionClass, ProviderIdentity, RuntimeContractError,
    identity::{validate_provider, validate_route, validate_text},
};

/// Immutable request-to-route binding produced after admission.
#[derive(Debug)]
pub struct AdmittedExecution {
    pub(super) execution_id: String,
    pub(super) request: AiJobRequest,
    pub(super) route: ExecutionRouteIdentity,
    pub(super) plan: ExecutionPlanIdentity,
    pub(super) route_estimate: ResourceEstimate,
    pub(super) fallback: FallbackDisclosure,
}

impl AdmittedExecution {
    pub fn execution_id(&self) -> &str {
        &self.execution_id
    }

    pub const fn request(&self) -> &AiJobRequest {
        &self.request
    }

    pub const fn route(&self) -> &ExecutionRouteIdentity {
        &self.route
    }

    pub const fn plan(&self) -> &RunPlan {
        &self.plan.plan
    }

    pub const fn plan_identity(&self) -> &ExecutionPlanIdentity {
        &self.plan
    }

    pub const fn route_estimate(&self) -> ResourceEstimate {
        self.route_estimate
    }

    pub const fn fallback(&self) -> &FallbackDisclosure {
        &self.fallback
    }

    pub(super) fn validate(&self) -> Result<(), RuntimeContractError> {
        validate_request(&self.request)?;
        validate_text(&self.execution_id, "execution_id")?;
        validate_route(&self.route)?;
        self.plan.validate()?;
        validate_backend_route(&self.route, &self.plan.plan)?;
        if self.route_estimate.cpu_threads == 0 {
            return Err(RuntimeContractError::InvalidRouteEstimate);
        }
        if self.plan.plan.cpu_threads < self.route_estimate.cpu_threads
            || self.plan.plan.reserved_system_ram_bytes < self.route_estimate.peak_system_ram_bytes
            || self.plan.plan.reserved_device_memory_bytes
                < self.route_estimate.peak_device_memory_bytes
        {
            return Err(RuntimeContractError::RoutePlanUnderreserves);
        }
        validate_fallback(&self.route, &self.fallback)?;
        Ok(())
    }
}

#[derive(Debug)]
pub enum LocalExecutionAdmission {
    Admitted { execution: Box<AdmittedExecution> },
    Deferred { blockers: Vec<AdmissionBlocker> },
}

/// Provider-selected inputs to deterministic local admission.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct LocalExecutionBinding {
    pub execution_id: String,
    pub request: AiJobRequest,
    pub provider: ProviderIdentity,
    /// Provider/route estimate used for hard safety admission. The request's
    /// own estimate remains only a caller scheduling hint.
    pub route_estimate: ResourceEstimate,
    pub fallback: FallbackDisclosure,
}

/// Binds a provider-neutral request to an exact local artifact set and current
/// deterministic resource plan.
///
/// # Errors
///
/// Returns an error when the request, manifest, provider identity, or fallback
/// disclosure is structurally invalid. Resource/policy rejection is returned
/// as `Deferred`, not as a fabricated execution.
pub fn admit_local_execution(
    binding: LocalExecutionBinding,
    manifest: &ModelManifest,
    hardware: &HardwareProfile,
    policy: ResourcePolicy,
    availability: ModelAvailability,
) -> Result<LocalExecutionAdmission, RuntimeContractError> {
    let LocalExecutionBinding {
        execution_id,
        request,
        provider,
        route_estimate,
        fallback,
    } = binding;
    validate_request(&request)?;
    manifest.validate()?;
    validate_provider(&provider)?;
    if provider.execution_class != ProviderExecutionClass::LocalModel {
        return Err(RuntimeContractError::WrongProviderClass {
            expected: ProviderExecutionClass::LocalModel,
            actual: provider.execution_class,
        });
    }
    if !manifest.capabilities.contains(&request.task.capability()) {
        return Err(RuntimeContractError::CapabilityUnsupported(
            request.task.capability(),
        ));
    }

    let admission = admit(
        manifest,
        hardware,
        policy,
        &AdmissionRequest {
            priority: request.priority,
            privacy: request.privacy,
            estimate: route_estimate,
            availability,
        },
    );
    let AdmissionDecision::Run { plan } = admission else {
        let AdmissionDecision::Defer { blockers } = admission else {
            unreachable!("admission decision has exactly two variants");
        };
        return Ok(LocalExecutionAdmission::Deferred { blockers });
    };

    let execution = AdmittedExecution {
        execution_id,
        request,
        route: ExecutionRouteIdentity {
            contract_version: super::identity::EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
            provider,
            model: AdmittedModelIdentity::LocalArtifactSet {
                model_id: manifest.model_id.clone(),
                exact_revision: manifest.exact_revision.clone(),
                artifact_set_blake3: manifest.artifact_set.inventory_blake3.clone(),
                preprocessing_version: manifest.preprocessing_version.clone(),
            },
        },
        plan: ExecutionPlanIdentity::from_plan(plan)?,
        route_estimate,
        fallback,
    };
    execution.validate()?;
    Ok(LocalExecutionAdmission::Admitted {
        execution: Box::new(execution),
    })
}

/// Binds a system-framework route to a resource plan already admitted by the
/// application scheduler.
///
/// System APIs such as Apple Vision have no downloadable `ModelManifest`; the
/// caller must supply a framework request revision, OS build, and plan from the
/// same hardware snapshot that owns the lease.
///
/// # Errors
///
/// Returns an error when the route is not a system framework, the request is
/// unsupported, or any identity is incomplete.
pub fn bind_system_execution(
    execution_id: String,
    request: AiJobRequest,
    route: ExecutionRouteIdentity,
    supported_capabilities: &BTreeSet<AiCapability>,
    plan: RunPlan,
    route_estimate: ResourceEstimate,
    fallback: FallbackDisclosure,
) -> Result<AdmittedExecution, RuntimeContractError> {
    validate_request(&request)?;
    validate_route(&route)?;
    if route.provider.execution_class != ProviderExecutionClass::SystemFramework {
        return Err(RuntimeContractError::WrongProviderClass {
            expected: ProviderExecutionClass::SystemFramework,
            actual: route.provider.execution_class,
        });
    }
    if !matches!(route.model, AdmittedModelIdentity::SystemFramework { .. }) {
        return Err(RuntimeContractError::ProviderModelClassMismatch);
    }
    if !supported_capabilities.contains(&request.task.capability()) {
        return Err(RuntimeContractError::CapabilityUnsupported(
            request.task.capability(),
        ));
    }
    let execution = AdmittedExecution {
        execution_id,
        request,
        route,
        plan: ExecutionPlanIdentity::from_plan(plan)?,
        route_estimate,
        fallback,
    };
    execution.validate()?;
    Ok(execution)
}

fn validate_request(request: &AiJobRequest) -> Result<(), RuntimeContractError> {
    if request.contract_version != AI_JOB_REQUEST_CONTRACT_VERSION {
        return Err(RuntimeContractError::UnsupportedRequestContract(
            request.contract_version,
        ));
    }
    validate_text(&request.request_id, "request_id")?;
    request.validate_task_parameters()?;
    if request
        .inputs
        .iter()
        .any(|input| input.privacy > request.privacy)
    {
        return Err(RuntimeContractError::RequestUnderstatesInputPrivacy);
    }
    Ok(())
}

fn validate_backend_route(
    route: &ExecutionRouteIdentity,
    plan: &RunPlan,
) -> Result<(), RuntimeContractError> {
    let compatible = match route.provider.execution_class {
        ProviderExecutionClass::LocalModel | ProviderExecutionClass::SystemFramework => {
            plan.backend_kind != BackendKind::RemoteApi
        }
        ProviderExecutionClass::RemoteService => plan.backend_kind == BackendKind::RemoteApi,
    };
    if compatible {
        Ok(())
    } else {
        Err(RuntimeContractError::BackendExecutionClassMismatch {
            execution_class: route.provider.execution_class,
            backend_kind: plan.backend_kind,
        })
    }
}

pub(super) fn validate_fallback(
    selected: &ExecutionRouteIdentity,
    fallback: &FallbackDisclosure,
) -> Result<(), RuntimeContractError> {
    let FallbackDisclosure::DeclaredFallback {
        primary,
        reason_code,
    } = fallback
    else {
        return Ok(());
    };
    validate_route(primary)?;
    validate_text(reason_code, "fallback.reason_code")?;
    if **primary == *selected {
        return Err(RuntimeContractError::FallbackRepeatsSelectedRoute);
    }
    if selected.provider.execution_class == ProviderExecutionClass::RemoteService {
        return Err(RuntimeContractError::FallbackSelectsRemoteRoute);
    }
    Ok(())
}
