use std::{collections::BTreeSet, str::FromStr};

use shadow_domain::{PhotoId, Platform};

use crate::{
    AI_JOB_REQUEST_CONTRACT_VERSION, AdmittedExecution, AiCapability, AiJobRequest, AiTaskKind,
    ArtifactReference, BackendKind, BackendRequirement, Dimension, DistributionTerms, ElementType,
    ExecutionBackend, FallbackDisclosure, HardwareProfile, InputRole, LicensePermission,
    LicenseTerms, LocalExecutionAdmission, LocalExecutionBinding, LocalModelAvailability,
    ModelAccess, ModelArtifact, ModelArtifactRole, ModelArtifactSet, ModelAvailability,
    ModelFormat, ModelManifest, NumericPrecision, NumericRange, ObservationTarget, OnBatteryPolicy,
    PrivacyClass, ProviderExecutionClass, ProviderIdentity, Quantization, ResourceEstimate,
    ResourcePolicy, TaskPriority, TensorContract, TensorLayout, TensorSemantics,
    admit_local_execution,
};

fn photo() -> PhotoId {
    PhotoId::from_str("018f3ec1-6219-7df2-a52d-f744c4f88533").expect("photo id")
}

pub(super) fn request() -> AiJobRequest {
    AiJobRequest {
        contract_version: AI_JOB_REQUEST_CONTRACT_VERSION,
        request_id: "request-1".into(),
        generation: 7,
        task: AiTaskKind::ExtractSimilarityEmbedding,
        target: ObservationTarget::Photo { photo_id: photo() },
        priority: TaskPriority::CurrentCollectionAnalysis,
        privacy: PrivacyClass::Personal,
        inputs: vec![ArtifactReference {
            role: InputRole::DisplayProxy,
            content_hash: "a".repeat(64),
            byte_len: 4096,
            media_type: "image/jpeg".into(),
            privacy: PrivacyClass::Personal,
        }],
        parameters: crate::AiTaskParameters::None,
        estimate: ResourceEstimate {
            peak_system_ram_bytes: 64,
            peak_device_memory_bytes: 0,
            cpu_threads: 1,
            scratch_disk_bytes: 0,
            upload_bytes: 0,
            estimated_duration_ms: Some(10),
        },
    }
}

pub(super) fn artifact_set() -> ModelArtifactSet {
    let mut set = ModelArtifactSet {
        set_id: "example.similarity-r1".into(),
        inventory_blake3: String::new(),
        artifacts: vec![ModelArtifact {
            relative_path: "model.onnx".into(),
            role: ModelArtifactRole::ModelDefinition,
            byte_len: 42,
            sha256: "c".repeat(64),
        }],
    };
    set.inventory_blake3 = set
        .computed_inventory_blake3()
        .expect("valid artifact fixture");
    set
}

pub(super) fn manifest() -> ModelManifest {
    ModelManifest {
        schema_version: 1,
        model_id: "example.similarity".into(),
        exact_revision: "r1".into(),
        artifact_set: artifact_set(),
        capabilities: BTreeSet::from([AiCapability::SimilarityEmbedding]),
        format: ModelFormat::Onnx,
        opset: Some(18),
        quantization: Quantization::None,
        preprocessing_version: "display-srgb-224-v1".into(),
        inputs: vec![TensorContract {
            name: "image".into(),
            semantics: TensorSemantics::Image,
            element_type: ElementType::Float32,
            layout: TensorLayout::Nchw,
            shape: vec![
                Dimension::Fixed { value: 1 },
                Dimension::Fixed { value: 3 },
                Dimension::Fixed { value: 224 },
                Dimension::Fixed { value: 224 },
            ],
            color_space: Some("display_srgb".into()),
            numeric_range: Some(NumericRange {
                minimum: 0.0,
                maximum: 1.0,
            }),
        }],
        outputs: vec![TensorContract {
            name: "embedding".into(),
            semantics: TensorSemantics::ImageEmbedding,
            element_type: ElementType::Float32,
            layout: TensorLayout::Nc,
            shape: vec![
                Dimension::Fixed { value: 1 },
                Dimension::Fixed { value: 384 },
            ],
            color_space: None,
            numeric_range: None,
        }],
        execution_targets: vec![BackendRequirement {
            kind: BackendKind::Cpu,
            minimum_runtime_version: None,
            precisions: vec![NumericPrecision::Float32],
        }],
        minimum_ram_bytes: 64,
        recommended_ram_bytes: 128,
        minimum_device_memory_bytes: 0,
        recommended_device_memory_bytes: 0,
        licensing: LicenseTerms {
            code_license: "Apache-2.0".into(),
            weight_license: "Apache-2.0".into(),
            training_data_notes: Some("audit fixture".into()),
            redistribution: LicensePermission::Allowed,
            commercial_use: LicensePermission::Allowed,
            access: ModelAccess::Open,
            attribution_files: vec![],
        },
        distribution: DistributionTerms {
            bundled_by_default: false,
            automatic_download_allowed: false,
            side_load_allowed: true,
            upstream_url: "https://example.invalid/model".into(),
        },
    }
}

pub(super) fn hardware() -> HardwareProfile {
    HardwareProfile {
        platform: Platform::MacOs,
        total_system_ram_bytes: 16_384,
        available_system_ram_bytes: 8192,
        logical_cpu_threads: 8,
        on_battery: false,
        backends: vec![ExecutionBackend {
            id: "cpu".into(),
            kind: BackendKind::Cpu,
            available: true,
            supported_precisions: vec![NumericPrecision::Float32],
            total_device_memory_bytes: None,
            available_device_memory_bytes: None,
            maximum_concurrent_sessions: 2,
            active_sessions: 0,
        }],
    }
}

pub(super) fn policy() -> ResourcePolicy {
    ResourcePolicy {
        maximum_ai_system_ram_bytes: 4096,
        reserved_system_ram_bytes: 1024,
        maximum_ai_cpu_threads: 4,
        maximum_device_memory_percent: 75,
        on_battery: OnBatteryPolicy::PauseBackground,
    }
}

pub(super) fn availability() -> ModelAvailability {
    ModelAvailability {
        local: LocalModelAvailability::Installed {
            artifact_set_blake3: artifact_set().inventory_blake3,
            license_accepted: true,
        },
    }
}

pub(super) fn route_estimate() -> ResourceEstimate {
    ResourceEstimate {
        peak_system_ram_bytes: 96,
        peak_device_memory_bytes: 0,
        cpu_threads: 2,
        scratch_disk_bytes: 0,
        upload_bytes: 0,
        estimated_duration_ms: Some(8),
    }
}

pub(super) fn admitted_execution() -> AdmittedExecution {
    let provider = ProviderIdentity {
        provider_id: "shadow.onnx".into(),
        adapter_revision: "v1".into(),
        execution_class: ProviderExecutionClass::LocalModel,
    };
    let LocalExecutionAdmission::Admitted { execution } = admit_local_execution(
        LocalExecutionBinding {
            execution_id: "execution-1".into(),
            request: request(),
            provider,
            route_estimate: route_estimate(),
            fallback: FallbackDisclosure::Primary,
        },
        &manifest(),
        &hardware(),
        policy(),
        availability(),
    )
    .expect("valid admission") else {
        panic!("expected admission");
    };
    *execution
}
