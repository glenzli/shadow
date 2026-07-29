use blake3::Hasher;
use serde::{Deserialize, Deserializer, Serialize, de::Error as _};

#[cfg(test)]
use crate::AiJobRequest;
use crate::{AiTaskKind, ObservationTarget};

use super::{
    AdmittedExecution, ExecutionPlanIdentity, ExecutionRouteIdentity, RuntimeContractError,
    identity::{validate_blake3, validate_route, validate_text},
};

pub const MODEL_PROVENANCE_CONTRACT_VERSION: u32 = 1;

/// Durable evidence bound by the runtime to one admitted request, route, and
/// complete scheduler plan.
///
/// This value is descriptive evidence, not promotion authority. The move-only
/// [`super::LeaseBoundOutput`] is the authority that proves it came from a
/// consumed execution lease.
#[derive(Debug, Clone, Eq, PartialEq, Serialize)]
pub struct ModelProvenance {
    contract_version: u32,
    request_id: String,
    generation: u64,
    task: AiTaskKind,
    target: ObservationTarget,
    request_blake3: String,
    input_source_blake3: String,
    execution_route: ExecutionRouteIdentity,
    execution_plan: ExecutionPlanIdentity,
    cache_key_blake3: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ModelProvenanceWire {
    contract_version: u32,
    request_id: String,
    generation: u64,
    task: AiTaskKind,
    target: ObservationTarget,
    request_blake3: String,
    input_source_blake3: String,
    execution_route: ExecutionRouteIdentity,
    execution_plan: ExecutionPlanIdentity,
    cache_key_blake3: String,
}

impl<'de> Deserialize<'de> for ModelProvenance {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let wire = ModelProvenanceWire::deserialize(deserializer)?;
        let provenance = Self {
            contract_version: wire.contract_version,
            request_id: wire.request_id,
            generation: wire.generation,
            task: wire.task,
            target: wire.target,
            request_blake3: wire.request_blake3,
            input_source_blake3: wire.input_source_blake3,
            execution_route: wire.execution_route,
            execution_plan: wire.execution_plan,
            cache_key_blake3: wire.cache_key_blake3,
        };
        provenance.validate().map_err(D::Error::custom)?;
        Ok(provenance)
    }
}

impl ModelProvenance {
    pub const fn contract_version(&self) -> u32 {
        self.contract_version
    }

    pub fn request_id(&self) -> &str {
        &self.request_id
    }

    pub const fn generation(&self) -> u64 {
        self.generation
    }

    pub const fn task(&self) -> AiTaskKind {
        self.task
    }

    pub const fn target(&self) -> &ObservationTarget {
        &self.target
    }

    pub fn request_blake3(&self) -> &str {
        &self.request_blake3
    }

    pub fn input_source_blake3(&self) -> &str {
        &self.input_source_blake3
    }

    pub const fn execution_route(&self) -> &ExecutionRouteIdentity {
        &self.execution_route
    }

    pub const fn execution_plan(&self) -> &ExecutionPlanIdentity {
        &self.execution_plan
    }

    pub fn cache_key_blake3(&self) -> &str {
        &self.cache_key_blake3
    }

    /// Validates the exact v1 evidence shape and its route/plan cache binding.
    ///
    /// # Errors
    ///
    /// Returns an error for unsupported versions, missing request identity,
    /// malformed digests, invalid route/plan identity, or a stale cache key.
    pub fn validate(&self) -> Result<(), RuntimeContractError> {
        if self.contract_version != MODEL_PROVENANCE_CONTRACT_VERSION {
            return Err(RuntimeContractError::UnsupportedProvenanceContract(
                self.contract_version,
            ));
        }
        validate_text(&self.request_id, "provenance.request_id")?;
        validate_blake3(&self.request_blake3)?;
        validate_blake3(&self.input_source_blake3)?;
        validate_route(&self.execution_route)?;
        self.execution_plan.validate()?;
        validate_blake3(&self.cache_key_blake3)?;
        let expected = cache_key(
            &self.request_blake3,
            &self.execution_route,
            &self.execution_plan,
        )?;
        if self.cache_key_blake3 != expected {
            return Err(RuntimeContractError::ProvenanceCacheKeyMismatch);
        }
        Ok(())
    }

    pub(super) fn from_admitted(
        execution: &AdmittedExecution,
    ) -> Result<Self, RuntimeContractError> {
        let request_blake3 =
            canonical_json_blake3("shadow.ai.admitted-request.v1", &execution.request)?;
        let input_source_blake3 =
            canonical_json_blake3("shadow.ai.admitted-inputs.v1", &execution.request.inputs)?;
        let cache_key_blake3 = cache_key(&request_blake3, &execution.route, &execution.plan)?;
        let provenance = Self {
            contract_version: MODEL_PROVENANCE_CONTRACT_VERSION,
            request_id: execution.request.request_id.clone(),
            generation: execution.request.generation,
            task: execution.request.task,
            target: execution.request.target.clone(),
            request_blake3,
            input_source_blake3,
            execution_route: execution.route.clone(),
            execution_plan: execution.plan.clone(),
            cache_key_blake3,
        };
        provenance.validate()?;
        Ok(provenance)
    }

    #[cfg(test)]
    pub(crate) fn from_test_request(
        request: &AiJobRequest,
        route: ExecutionRouteIdentity,
        plan: ExecutionPlanIdentity,
    ) -> Result<Self, RuntimeContractError> {
        let execution = AdmittedExecution {
            execution_id: "test-provenance-execution".into(),
            request: request.clone(),
            route,
            plan,
            route_estimate: request.estimate,
            fallback: super::FallbackDisclosure::Primary,
        };
        Self::from_admitted(&execution)
    }
}

fn cache_key(
    request_blake3: &str,
    route: &ExecutionRouteIdentity,
    plan: &ExecutionPlanIdentity,
) -> Result<String, RuntimeContractError> {
    #[derive(Serialize)]
    struct CacheIdentity<'a> {
        request_blake3: &'a str,
        route: &'a ExecutionRouteIdentity,
        plan: &'a ExecutionPlanIdentity,
    }

    canonical_json_blake3(
        "shadow.ai.execution-cache-key.v1",
        &CacheIdentity {
            request_blake3,
            route,
            plan,
        },
    )
}

fn canonical_json_blake3<T: Serialize + ?Sized>(
    context: &'static str,
    value: &T,
) -> Result<String, RuntimeContractError> {
    let bytes = serde_json::to_vec(value)
        .map_err(|_| RuntimeContractError::CanonicalIdentityEncodingFailed)?;
    let mut hasher = Hasher::new_derive_key(context);
    hasher.update(&(bytes.len() as u64).to_be_bytes());
    hasher.update(&bytes);
    Ok(hasher.finalize().to_hex().to_string())
}
