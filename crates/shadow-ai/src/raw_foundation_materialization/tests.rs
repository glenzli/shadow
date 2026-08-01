use std::str::FromStr;

use shadow_domain::PhotoId;

use super::*;
use crate::{
    AI_JOB_REQUEST_CONTRACT_VERSION, ArtifactReference, ObservationTarget, PrivacyClass,
    ResourceEstimate, TaskPriority,
};

fn request() -> AiJobRequest {
    AiJobRequest {
        contract_version: AI_JOB_REQUEST_CONTRACT_VERSION,
        request_id: "foundation-materialization-request".into(),
        generation: 1,
        task: AiTaskKind::MaterializeRawFoundation,
        target: ObservationTarget::Photo {
            photo_id: PhotoId::from_str("018f3ec1-6219-7df2-a52d-f744c4f88533").expect("photo id"),
        },
        priority: TaskPriority::CurrentInput,
        privacy: PrivacyClass::Personal,
        inputs: vec![ArtifactReference {
            role: InputRole::RawFile,
            content_hash: "a".repeat(64),
            byte_len: 17,
            media_type: "image/x-raw".into(),
            privacy: PrivacyClass::Personal,
        }],
        parameters: AiTaskParameters::RawFoundation,
        estimate: ResourceEstimate {
            peak_system_ram_bytes: 2 * 1024 * 1024 * 1024,
            peak_device_memory_bytes: 0,
            cpu_threads: 4,
            scratch_disk_bytes: 1024 * 1024 * 1024,
            upload_bytes: 0,
            estimated_duration_ms: Some(120_000),
        },
    }
}

#[test]
fn only_the_exact_single_raw_foundation_request_is_accepted() {
    let valid = request();
    assert_eq!(
        exact_raw_foundation_input(&valid)
            .expect("exact input")
            .content_hash,
        "a".repeat(64)
    );

    let mut wrong_task = request();
    wrong_task.task = AiTaskKind::Denoise;
    assert!(exact_raw_foundation_input(&wrong_task).is_none());

    let mut duplicate_input = request();
    duplicate_input
        .inputs
        .push(duplicate_input.inputs[0].clone());
    assert!(exact_raw_foundation_input(&duplicate_input).is_none());

    let mut wrong_role = request();
    wrong_role.inputs[0].role = InputRole::DisplayProxy;
    assert!(exact_raw_foundation_input(&wrong_role).is_none());
}

#[test]
fn publication_status_preserves_cache_hit_provenance() {
    assert_eq!(
        publication_disposition(FoundationArtifactPublicationStatus::Published),
        RawFoundationMaterializationDisposition::Published
    );
    assert_eq!(
        publication_disposition(FoundationArtifactPublicationStatus::ReusedExisting),
        RawFoundationMaterializationDisposition::ReusedConcurrent
    );
}

const _: () = assert!(RAW_FOUNDATION_PUBLISHING_PROGRESS < crate::RUNTIME_PROGRESS_COMPLETE);
