//! Real apply-boundary tests using tiny, lease-bound synthetic provider outputs.

use std::{collections::BTreeSet, path::Path};

use shadow_ai::{
    AI_JOB_REQUEST_CONTRACT_VERSION, AdmittedExecution, AdmittedModelIdentity, AiCapability,
    AiGeneratedPayload, AiJobRequest, AiTaskKind, AiTaskParameters, ArtifactHashAlgorithm,
    ArtifactReference, BackendKind, CancellationToken, EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
    ExecutionLease, ExecutionPlanIdentity, ExecutionRouteIdentity, FallbackDisclosure,
    GeneratedArtifactReference, ImageCompletionParameters, ImageCompletionPatchArtifact, InputRole,
    MaskPrompt, MaskSemantic, NumericPrecision, ObservationTarget, PrivacyClass,
    ProviderExecutionClass, ProviderIdentity, ProviderTerminal, RasterExtent, ResourceEstimate,
    RunPlan, RuntimeProgressReporter, RuntimeProvider, RuntimeUsage, SoftMaskArtifact,
    SoftMaskEncoding, SubjectMaskParameters, TaskPriority, UnitInterval, bind_system_execution,
};
use shadow_catalog::{CatalogError, RegisterAsset};
use shadow_core::execute_and_stage_derived_raster;
use shadow_domain::{AssetLocation, MaskCoordinateSpace, PhotoId, Platform, RepresentationKind};

use crate::{
    DesktopSession, ffi,
    image_completion_service::{
        ImageCompletionCompletion, ImageCompletionPlacement, ImageCompletionServiceError,
    },
    open_desktop_session,
    subject_mask_service::{SubjectMaskCompletion, SubjectMaskServiceError},
    tests::fixtures::{edit_session::test_edit_session, grade_stack::ffi_parameters},
};

#[derive(Clone, Copy)]
enum Kind {
    Mask,
    Completion,
}

fn stage(session: &DesktopSession, root: &Path, photo_id: PhotoId, kind: Kind) -> u64 {
    let extent = RasterExtent::new(2, 2).unwrap();
    let (task, capability, bytes, media, parameters) = match kind {
        Kind::Mask => (
            AiTaskKind::ProposeSubjectMask,
            AiCapability::SubjectMask,
            vec![0, 64, 192, 255],
            "application/x-shadow-soft-mask",
            AiTaskParameters::SubjectMask(SubjectMaskParameters {
                prompt: MaskPrompt::AutomaticSubject,
                coordinate_space: MaskCoordinateSpace::Original,
                coordinate_extent: extent,
                edge_refinement: UnitInterval::new(0.5).unwrap(),
                maximum_candidates: 1,
            }),
        ),
        Kind::Completion => (
            AiTaskKind::GenerateInpaintPatch,
            AiCapability::InpaintPatch,
            vec![255; 16],
            "application/x-shadow-rgba8",
            AiTaskParameters::ImageCompletion(ImageCompletionParameters {
                coordinate_extent: extent,
                source_recipe_blake3: "b".repeat(64),
                mask_revision: "d".repeat(64),
            }),
        ),
    };
    let scratch = root.join("synthetic-proposal.bin");
    std::fs::write(&scratch, &bytes).unwrap();
    let artifact = GeneratedArtifactReference::new(
        ArtifactHashAlgorithm::Blake3_256,
        blake3::hash(&bytes).to_hex().to_string(),
        bytes.len() as u64,
        media.into(),
        1,
    )
    .unwrap();
    let payload = match kind {
        Kind::Mask => AiGeneratedPayload::SoftMask(SoftMaskArtifact {
            artifact,
            raster_extent: extent,
            coordinate_extent: extent,
            coordinate_space: MaskCoordinateSpace::Original,
            encoding: SoftMaskEncoding::Gray8Unorm,
            semantic: MaskSemantic::UserPrompt,
        }),
        Kind::Completion => {
            AiGeneratedPayload::ImageCompletionPatch(ImageCompletionPatchArtifact {
                source_context: None,
                artifact,
                raster_extent: extent,
                coordinate_extent: extent,
                source_recipe_blake3: "b".repeat(64),
                provider: "fixture".into(),
                deployment: "fixture".into(),
                model_build: "fixture".into(),
                postprocessing_identity: "fixture".into(),
                api_contract_revision: "fixture".into(),
                actual_execution_provider: "cpu".into(),
            })
        }
    };
    let route = ExecutionRouteIdentity {
        contract_version: EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
        provider: ProviderIdentity {
            provider_id: "fixture.apply".into(),
            adapter_revision: "fixture-v1".into(),
            execution_class: ProviderExecutionClass::SystemFramework,
        },
        model: AdmittedModelIdentity::SystemFramework {
            framework: "Fixture".into(),
            request_name: "proposal".into(),
            request_revision: 1,
            operating_system_build: "fixture".into(),
        },
    };
    let estimate = ResourceEstimate {
        peak_system_ram_bytes: 64,
        peak_device_memory_bytes: 0,
        cpu_threads: 1,
        scratch_disk_bytes: 0,
        upload_bytes: 0,
        estimated_duration_ms: Some(1),
    };
    let execution = bind_system_execution(
        "execution-7".into(),
        AiJobRequest {
            contract_version: AI_JOB_REQUEST_CONTRACT_VERSION,
            request_id: "request-7".into(),
            generation: 7,
            task,
            target: ObservationTarget::Photo { photo_id },
            priority: TaskPriority::CurrentInput,
            privacy: PrivacyClass::Personal,
            inputs: vec![ArtifactReference {
                role: InputRole::CurrentRenderedCrop,
                content_hash: "c".repeat(64),
                byte_len: 12,
                media_type: "image/png".into(),
                privacy: PrivacyClass::Personal,
            }],
            parameters,
            estimate,
        },
        route.clone(),
        &BTreeSet::from([capability]),
        RunPlan {
            backend_id: "fixture".into(),
            backend_kind: BackendKind::Cpu,
            precision: NumericPrecision::Float32,
            cpu_threads: 1,
            reserved_system_ram_bytes: 64,
            reserved_device_memory_bytes: 0,
        },
        estimate,
        FallbackDisclosure::Primary,
    )
    .unwrap();
    let provider = FixtureProvider {
        route,
        plan: execution.plan_identity().clone(),
        payload,
        capability,
    };
    let store = match kind {
        Kind::Mask => session.subject_masks.store(),
        Kind::Completion => session.image_completions.store(),
    };
    let receipt = execute_and_stage_derived_raster(
        store,
        "promotion-7".into(),
        ExecutionLease::issue("lease-7".into(), execution).unwrap(),
        &provider,
        &CancellationToken::default(),
        &scratch,
    )
    .unwrap();
    match kind {
        Kind::Mask => {
            let job = session.subject_masks.begin_job().unwrap();
            let SubjectMaskCompletion::Staged { proposal_token, .. } =
                session.subject_masks.complete_job(job, receipt).unwrap()
            else {
                panic!("synthetic mask was not staged");
            };
            proposal_token
        }
        Kind::Completion => {
            let job = session.image_completions.begin_job().unwrap();
            let ImageCompletionCompletion::Staged { proposal_token, .. } = session
                .image_completions
                .complete_job(
                    job,
                    receipt,
                    ImageCompletionPlacement {
                        bounds_left: shadow_domain::UnitInterval::ZERO,
                        bounds_top: shadow_domain::UnitInterval::ZERO,
                        bounds_right: shadow_domain::UnitInterval::ONE,
                        bounds_bottom: shadow_domain::UnitInterval::ONE,
                    },
                )
                .unwrap()
            else {
                panic!("synthetic completion was not staged");
            };
            session.image_completions.finish_job(job).unwrap();
            proposal_token
        }
    }
}

fn apply(
    session: &DesktopSession,
    photo: &str,
    source: &str,
    state: &ffi::FfiPhotoEditState,
    token: u64,
    kind: Kind,
) -> anyhow::Result<ffi::FfiPhotoEditState> {
    match kind {
        Kind::Completion => session.apply_image_completion_proposal(
            photo,
            source,
            &ffi::FfiImageCompletionApplyRequest {
                proposal_token: token,
                generation: 7,
                base_commit_id: state.working_commit_id.clone(),
                expected_working_commit_id: state.working_commit_id.clone(),
                expected_variant_id: state.active_variant_id.clone(),
                settings: state.settings.clone(),
                replace_region_index: -1,
            },
        ),
        Kind::Mask => session.apply_subject_mask_proposal(
            photo,
            source,
            &ffi::FfiSubjectMaskApplyRequest {
                proposal_token: token,
                generation: 7,
                base_commit_id: state.working_commit_id.clone(),
                expected_working_commit_id: state.working_commit_id.clone(),
                expected_variant_id: state.active_variant_id.clone(),
                settings: state.settings.clone(),
                target_grade_node_index: 0,
                target_grade_node_id: state.settings.grade_nodes[0].grade_node_id.clone(),
                target_mask_operation: 0,
                invert: false,
                semantic_query: String::new(),
                semantic_maximum_regions: 1,
                semantic_score_threshold_percent: 50,
            },
        ),
    }
}

fn same_head_variant_switch(kind: Kind) {
    let (root, session, photo, source) = test_edit_session();
    let original = session
        .autosave_basic_edit_working_at(
            &photo,
            &source,
            "",
            "",
            &ffi_parameters(0.25, 1.0, [0.0; 2], 1.0),
            100,
        )
        .unwrap();
    let alternate = session
        .create_photo_variant(&photo, &source, "Alternate")
        .unwrap();
    session
        .activate_photo_variant(&photo, &source, &original.active_variant_id)
        .unwrap();
    let token = stage(&session, &root, photo.parse().unwrap(), kind);
    let other = open_desktop_session(
        root.join("catalog.sqlite").to_str().unwrap(),
        root.join("cache").to_str().unwrap(),
    )
    .unwrap();
    other
        .activate_photo_variant(&photo, &source, &alternate.active_variant_id)
        .unwrap();
    let result = apply(&session, &photo, &source, &original, token, kind);
    let after = session.photo_edit_state(&photo, &source).unwrap();
    assert!(matches!(
        result.unwrap_err().downcast_ref::<CatalogError>(),
        Some(CatalogError::PhotoVariantExpectationMismatch { .. })
    ));
    assert_eq!(after.working_commit_id, original.working_commit_id);
    assert_eq!(after.active_variant_id, alternate.active_variant_id);
    // Preflight rejection keeps the candidate available for the captured target.
    other
        .activate_photo_variant(&photo, &source, &original.active_variant_id)
        .unwrap();
    let accepted = apply(&session, &photo, &source, &original, token, kind).unwrap();
    assert_ne!(accepted.working_commit_id, original.working_commit_id);
    assert_eq!(accepted.active_variant_id, original.active_variant_id);
    drop(other);
    drop(session);
    std::fs::remove_dir_all(root).unwrap();
}

fn another_photo(kind: Kind) {
    let (root, session, photo, original_source) = test_edit_session();
    let original = session.photo_edit_state(&photo, &original_source).unwrap();
    let token = stage(&session, &root, photo.parse().unwrap(), kind);
    let source = root.join("other.dng").to_string_lossy().into_owned();
    let other = session
        .catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                source.as_bytes().to_vec(),
                source.clone(),
            ),
            byte_len: 8192,
            modified_at_ms: Some(234),
            now_ms: 200,
        })
        .unwrap();
    let other_photo = other.photo_id.to_string();
    let state = session.photo_edit_state(&other_photo, &source).unwrap();
    let error = apply(&session, &other_photo, &source, &state, token, kind).unwrap_err();
    match kind {
        Kind::Completion => assert!(matches!(
            error.downcast_ref::<ImageCompletionServiceError>(),
            Some(ImageCompletionServiceError::ProposalPhotoMismatch)
        )),
        Kind::Mask => assert!(matches!(
            error.downcast_ref::<SubjectMaskServiceError>(),
            Some(SubjectMaskServiceError::ProposalPhotoMismatch)
        )),
    }
    assert!(
        session
            .catalog
            .recipe_commits(other.photo_id)
            .unwrap()
            .is_empty()
    );
    let accepted = apply(&session, &photo, &original_source, &original, token, kind).unwrap();
    assert!(!accepted.working_commit_id.is_empty());
    assert_eq!(accepted.active_variant_id, original.active_variant_id);
    drop(session);
    std::fs::remove_dir_all(root).unwrap();
}

fn empty_variant(kind: Kind) {
    let (root, session, photo, source) = test_edit_session();
    let original = session.photo_edit_state(&photo, &source).unwrap();
    let mut missing_variant = session.photo_edit_state(&photo, &source).unwrap();
    missing_variant.active_variant_id.clear();
    let token = stage(&session, &root, photo.parse().unwrap(), kind);
    let error = apply(&session, &photo, &source, &missing_variant, token, kind).unwrap_err();
    assert!(
        error.to_string().starts_with("parse photo Variant id"),
        "{error:#}"
    );
    assert!(
        session
            .catalog
            .recipe_commits(photo.parse().unwrap())
            .unwrap()
            .is_empty()
    );
    let accepted = apply(&session, &photo, &source, &original, token, kind).unwrap();
    assert!(!accepted.working_commit_id.is_empty());
    drop(session);
    std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn completion_requires_captured_variant() {
    empty_variant(Kind::Completion);
}
#[test]
fn mask_requires_captured_variant() {
    empty_variant(Kind::Mask);
}

#[test]
fn completion_rejects_same_head_variant_switch() {
    same_head_variant_switch(Kind::Completion);
}
#[test]
fn mask_rejects_same_head_variant_switch() {
    same_head_variant_switch(Kind::Mask);
}
#[test]
fn completion_rejects_another_photo() {
    another_photo(Kind::Completion);
}
#[test]
fn mask_rejects_another_photo() {
    another_photo(Kind::Mask);
}

#[derive(Debug)]
struct FixtureProvider {
    route: ExecutionRouteIdentity,
    plan: ExecutionPlanIdentity,
    payload: AiGeneratedPayload,
    capability: AiCapability,
}
impl RuntimeProvider for FixtureProvider {
    type Output = AiGeneratedPayload;
    fn route(&self) -> &ExecutionRouteIdentity {
        &self.route
    }
    fn execution_plan(&self) -> &ExecutionPlanIdentity {
        &self.plan
    }
    fn supports(&self, capability: AiCapability) -> bool {
        capability == self.capability
    }
    fn execute(
        &self,
        _: &AdmittedExecution,
        _: &CancellationToken,
        _: &RuntimeProgressReporter<'_>,
    ) -> ProviderTerminal<Self::Output> {
        ProviderTerminal::Succeeded {
            output: self.payload.clone(),
            usage: RuntimeUsage::default(),
        }
    }
}
