use std::collections::BTreeSet;

use crate::{
    AdmittedExecution, AdmittedModelIdentity, AiCapability, CancellationToken,
    EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION, ExecutionPlanIdentity, ExecutionRouteIdentity,
    FeaturePrintDistanceBatch, ProviderExecutionClass, ProviderIdentity, ProviderTerminal,
    ProviderUnavailable, RuntimeProgressReporter, RuntimeProvider, RuntimeUsage,
};

pub const APPLE_VISION_FEATURE_PRINT_REQUEST_REVISION: u32 = 1;
pub const APPLE_VISION_FEATURE_PRINT_ADAPTER_REVISION: &str =
    "vision-feature-print-adapter-contract-v1";

/// Apple Vision `FeaturePrint` route boundary.
///
/// This Rust target does not currently link Objective-C Vision. The provider
/// therefore emits no distances and always returns an honest unavailable
/// terminal. The route identity describes the adapter protocol, framework
/// request revision, and OS build—not whether this build happens to link the
/// native implementation.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct AppleVisionFeaturePrintProvider {
    route: ExecutionRouteIdentity,
    execution_plan: ExecutionPlanIdentity,
}

impl AppleVisionFeaturePrintProvider {
    pub fn new(
        operating_system_build: impl Into<String>,
        execution_plan: ExecutionPlanIdentity,
    ) -> Self {
        Self {
            route: ExecutionRouteIdentity {
                contract_version: EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
                provider: ProviderIdentity {
                    provider_id: "apple.vision.feature-print".into(),
                    adapter_revision: APPLE_VISION_FEATURE_PRINT_ADAPTER_REVISION.into(),
                    execution_class: ProviderExecutionClass::SystemFramework,
                },
                model: AdmittedModelIdentity::SystemFramework {
                    framework: "Vision".into(),
                    request_name:
                        "VNGenerateImageFeaturePrintRequest+VNFeaturePrintObservation.computeDistance"
                            .into(),
                    request_revision: APPLE_VISION_FEATURE_PRINT_REQUEST_REVISION,
                    operating_system_build: operating_system_build.into(),
                },
            },
            execution_plan,
        }
    }

    pub fn capabilities() -> BTreeSet<AiCapability> {
        BTreeSet::from([AiCapability::BurstGrouping])
    }

    /// Pins the complete Vision request route used by both admission and
    /// execution.
    pub const fn route(&self) -> &ExecutionRouteIdentity {
        &self.route
    }

    pub const fn execution_plan(&self) -> &ExecutionPlanIdentity {
        &self.execution_plan
    }
}

impl RuntimeProvider for AppleVisionFeaturePrintProvider {
    type Output = FeaturePrintDistanceBatch;

    fn route(&self) -> &ExecutionRouteIdentity {
        &self.route
    }

    fn execution_plan(&self) -> &ExecutionPlanIdentity {
        &self.execution_plan
    }

    fn supports(&self, capability: AiCapability) -> bool {
        capability == AiCapability::BurstGrouping
    }

    fn execute(
        &self,
        _execution: &AdmittedExecution,
        _cancellation: &CancellationToken,
        _progress: &RuntimeProgressReporter<'_>,
    ) -> ProviderTerminal<Self::Output> {
        let reason = if cfg!(target_os = "macos") {
            ProviderUnavailable::AdapterNotLinked
        } else {
            ProviderUnavailable::PlatformUnsupported
        };
        ProviderTerminal::Unavailable {
            reason,
            usage: RuntimeUsage::default(),
        }
    }
}

#[cfg(test)]
mod tests;
