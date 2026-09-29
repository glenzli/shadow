use blake3::Hasher;
use serde::{Deserialize, Deserializer, Serialize, de::Error as _};

use crate::{BackendKind, NumericPrecision, RunPlan};

use super::RuntimeContractError;

pub const EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION: u32 = 1;
pub const EXECUTION_PLAN_IDENTITY_CONTRACT_VERSION: u32 = 1;

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ProviderExecutionClass {
    LocalModel,
    /// A loopback-only local service owns model installation and scheduling;
    /// the consumer receives a typed, attested result rather than model bytes.
    LocalService,
    SystemFramework,
    RemoteService,
}

/// Exact adapter identity selected after a route is admitted.
#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ProviderIdentity {
    pub provider_id: String,
    pub adapter_revision: String,
    pub execution_class: ProviderExecutionClass,
}

/// Exact model or framework algorithm bound to a provider route.
///
/// System frameworks do not expose redistributable model bytes. They instead
/// pin the framework request revision and OS build that produced the result.
#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "kind", deny_unknown_fields)]
pub enum AdmittedModelIdentity {
    LocalArtifactSet {
        model_id: String,
        exact_revision: String,
        artifact_set_blake3: String,
        preprocessing_version: String,
    },
    SystemFramework {
        framework: String,
        request_name: String,
        request_revision: u32,
        operating_system_build: String,
    },
    RemoteService {
        service_revision: String,
        model_id: String,
        model_revision: String,
        api_contract_revision: String,
    },
}

#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize)]
pub struct ExecutionRouteIdentity {
    pub contract_version: u32,
    pub provider: ProviderIdentity,
    pub model: AdmittedModelIdentity,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ExecutionRouteIdentityWire {
    contract_version: u32,
    provider: ProviderIdentity,
    model: AdmittedModelIdentity,
}

impl<'de> Deserialize<'de> for ExecutionRouteIdentity {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let wire = ExecutionRouteIdentityWire::deserialize(deserializer)?;
        let route = Self {
            contract_version: wire.contract_version,
            provider: wire.provider,
            model: wire.model,
        };
        route.validate().map_err(D::Error::custom)?;
        Ok(route)
    }
}

impl ExecutionRouteIdentity {
    /// Validates complete provider and route-specific execution identity.
    ///
    /// # Errors
    ///
    /// Returns an error when the provider/model class disagrees or any exact
    /// artifact, framework, service, adapter, or OS identity is incomplete.
    pub fn validate(&self) -> Result<(), RuntimeContractError> {
        validate_route(self)
    }
}

/// Stable identity of every execution parameter admitted by the scheduler.
///
/// The complete plan remains embedded for auditability. Its digest protects
/// cache/provenance identities from silently omitting a newly added plan field.
#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize)]
pub struct ExecutionPlanIdentity {
    pub contract_version: u32,
    pub plan: RunPlan,
    pub plan_blake3: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ExecutionPlanIdentityWire {
    contract_version: u32,
    plan: RunPlan,
    plan_blake3: String,
}

impl<'de> Deserialize<'de> for ExecutionPlanIdentity {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let wire = ExecutionPlanIdentityWire::deserialize(deserializer)?;
        let identity = Self {
            contract_version: wire.contract_version,
            plan: wire.plan,
            plan_blake3: wire.plan_blake3,
        };
        identity.validate().map_err(D::Error::custom)?;
        Ok(identity)
    }
}

impl ExecutionPlanIdentity {
    /// Binds one complete scheduler plan to its stable v1 identity.
    ///
    /// # Errors
    ///
    /// Returns an error when the plan is incomplete.
    pub fn from_plan(plan: RunPlan) -> Result<Self, RuntimeContractError> {
        validate_plan(&plan)?;
        let plan_blake3 = compute_plan_blake3(&plan);
        Ok(Self {
            contract_version: EXECUTION_PLAN_IDENTITY_CONTRACT_VERSION,
            plan,
            plan_blake3,
        })
    }

    /// Validates the exact version, full plan, and canonical digest.
    ///
    /// # Errors
    ///
    /// Returns an error when serialized plan identity is incomplete, stale, or
    /// internally inconsistent.
    pub fn validate(&self) -> Result<(), RuntimeContractError> {
        if self.contract_version != EXECUTION_PLAN_IDENTITY_CONTRACT_VERSION {
            return Err(RuntimeContractError::UnsupportedPlanIdentityContract(
                self.contract_version,
            ));
        }
        validate_plan(&self.plan)?;
        validate_blake3(&self.plan_blake3)?;
        let expected = compute_plan_blake3(&self.plan);
        if self.plan_blake3 != expected {
            return Err(RuntimeContractError::ExecutionPlanIdentityMismatch);
        }
        Ok(())
    }
}

/// Discloses whether the selected route is the primary route or a fallback.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "route", deny_unknown_fields)]
pub enum FallbackDisclosure {
    Primary,
    DeclaredFallback {
        primary: Box<ExecutionRouteIdentity>,
        reason_code: String,
    },
}

pub(super) fn validate_route(route: &ExecutionRouteIdentity) -> Result<(), RuntimeContractError> {
    if route.contract_version != EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION {
        return Err(RuntimeContractError::UnsupportedRouteIdentityContract(
            route.contract_version,
        ));
    }
    validate_provider(&route.provider)?;
    match &route.model {
        AdmittedModelIdentity::LocalArtifactSet {
            model_id,
            exact_revision,
            artifact_set_blake3,
            preprocessing_version,
        } => {
            validate_text(model_id, "model.model_id")?;
            validate_text(exact_revision, "model.exact_revision")?;
            validate_blake3(artifact_set_blake3)?;
            validate_text(preprocessing_version, "model.preprocessing_version")?;
            if route.provider.execution_class != ProviderExecutionClass::LocalModel {
                return Err(RuntimeContractError::ProviderModelClassMismatch);
            }
        }
        AdmittedModelIdentity::SystemFramework {
            framework,
            request_name,
            request_revision,
            operating_system_build,
        } => {
            validate_text(framework, "model.framework")?;
            validate_text(request_name, "model.request_name")?;
            if *request_revision == 0 {
                return Err(RuntimeContractError::InvalidSystemRequestRevision);
            }
            validate_text(operating_system_build, "model.operating_system_build")?;
            if route.provider.execution_class != ProviderExecutionClass::SystemFramework {
                return Err(RuntimeContractError::ProviderModelClassMismatch);
            }
        }
        AdmittedModelIdentity::RemoteService {
            service_revision,
            model_id,
            model_revision,
            api_contract_revision,
        } => {
            validate_text(service_revision, "model.service_revision")?;
            validate_text(model_id, "model.model_id")?;
            validate_text(model_revision, "model.model_revision")?;
            validate_text(api_contract_revision, "model.api_contract_revision")?;
            if !matches!(
                route.provider.execution_class,
                ProviderExecutionClass::LocalService | ProviderExecutionClass::RemoteService
            ) {
                return Err(RuntimeContractError::ProviderModelClassMismatch);
            }
        }
    }
    Ok(())
}

pub(super) fn validate_provider(provider: &ProviderIdentity) -> Result<(), RuntimeContractError> {
    validate_text(&provider.provider_id, "provider.provider_id")?;
    validate_text(&provider.adapter_revision, "provider.adapter_revision")
}

pub(super) fn validate_text(value: &str, field: &'static str) -> Result<(), RuntimeContractError> {
    if value.trim().is_empty() {
        Err(RuntimeContractError::MissingText(field))
    } else {
        Ok(())
    }
}

pub(super) fn validate_blake3(value: &str) -> Result<(), RuntimeContractError> {
    if value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        Ok(())
    } else {
        Err(RuntimeContractError::InvalidArtifactSetDigest)
    }
}

fn validate_plan(plan: &RunPlan) -> Result<(), RuntimeContractError> {
    validate_text(&plan.backend_id, "plan.backend_id")?;
    if plan.cpu_threads == 0 {
        return Err(RuntimeContractError::InvalidExecutionPlan);
    }
    Ok(())
}

fn compute_plan_blake3(plan: &RunPlan) -> String {
    let mut hasher = Hasher::new_derive_key("shadow.ai.execution-plan-identity.v1");
    update_identity_field(&mut hasher, 1, plan.backend_id.as_bytes());
    update_identity_field(&mut hasher, 2, backend_kind_tag(plan.backend_kind));
    update_identity_field(&mut hasher, 3, precision_tag(plan.precision));
    update_identity_field(&mut hasher, 4, &plan.cpu_threads.to_be_bytes());
    update_identity_field(
        &mut hasher,
        5,
        &plan.reserved_system_ram_bytes.to_be_bytes(),
    );
    update_identity_field(
        &mut hasher,
        6,
        &plan.reserved_device_memory_bytes.to_be_bytes(),
    );
    hasher.finalize().to_hex().to_string()
}

fn update_identity_field(hasher: &mut Hasher, tag: u8, value: &[u8]) {
    hasher.update(&[tag]);
    hasher.update(&(value.len() as u64).to_be_bytes());
    hasher.update(value);
}

const fn backend_kind_tag(kind: BackendKind) -> &'static [u8] {
    match kind {
        BackendKind::Cpu => b"cpu",
        BackendKind::CoreMl => b"core_ml",
        BackendKind::Metal => b"metal",
        BackendKind::Cuda => b"cuda",
        BackendKind::WindowsMl => b"windows_ml",
        BackendKind::DirectMl => b"direct_ml",
        BackendKind::OpenVino => b"open_vino",
        BackendKind::ExternalLocal => b"external_local",
        BackendKind::RemoteApi => b"remote_api",
    }
}

const fn precision_tag(precision: NumericPrecision) -> &'static [u8] {
    match precision {
        NumericPrecision::Float32 => b"float32",
        NumericPrecision::Float16 => b"float16",
        NumericPrecision::Bfloat16 => b"bfloat16",
        NumericPrecision::Int8 => b"int8",
        NumericPrecision::Int4 => b"int4",
    }
}
