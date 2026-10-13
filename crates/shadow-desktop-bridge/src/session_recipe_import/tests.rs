use std::{collections::BTreeSet, path::PathBuf};

use super::{SemanticRecipeExecution, semantic_stage_receipt};
use crate::subject_mask_runtime::{SemanticMaskStageOutcome, SubjectMaskRuntimeError};
use crate::subject_mask_service::{SubjectMaskCompletion, SubjectMaskServiceError};
use crate::{
    DesktopSession, ffi, open_desktop_session,
    recipe_import_plan::SemanticLeafResolution,
    recipe_import_service::{RecipeImportExecutionContext, RecipeImportItemTerminal},
    recipe_v1::{
        GradeNodeDraft, GradeStackDraft, MAX_GRADE_NODES, decode_grade_stack_draft_recipe_v1,
        grade_stack_recipe_v1_snapshot,
    },
};
use shadow_ai::{
    AI_JOB_REQUEST_CONTRACT_VERSION, AdmittedExecution, AdmittedModelIdentity, AiCapability,
    AiGeneratedPayload, AiJobRequest, AiTaskKind, AiTaskParameters, ArtifactHashAlgorithm,
    ArtifactReference, BackendKind, CancellationToken, EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
    ExecutionLease, ExecutionPlanIdentity, ExecutionRouteIdentity, FallbackDisclosure,
    GeneratedArtifactReference, InputRole, MaskPrompt, MaskSemantic, NumericPrecision,
    ObservationTarget, PrivacyClass, ProviderExecutionClass, ProviderIdentity, ProviderTerminal,
    RasterExtent, ResourceEstimate, RunPlan, RuntimeProgressReporter, RuntimeProvider,
    RuntimeUsage, SoftMaskArtifact, SoftMaskEncoding, SubjectMaskParameters, TaskPriority,
    bind_system_execution,
};
use shadow_catalog::RegisterAsset;
use shadow_core::execute_and_stage_derived_raster;
use shadow_domain::{
    AssetLocation, ManagedRasterMask, MaskCoordinateSpace, MaskDefinition, PhotoCanvasNode,
    PhotoFoundationNode, Platform, RasterMaskEncoding, RawTemperatureTint, RawWhiteBalance,
    RecipeInputSettings, RecipeOpticsSettings, RecipeSnapshot, RepresentationKind,
    SemanticMaskAggregation, SemanticMaskIntent, ShadowRecipeDocument, UnitInterval,
};

#[test]
fn semantic_not_found_is_typed_independently_of_display_text() {
    let terminal = semantic_stage_receipt(SemanticMaskStageOutcome::NotFound)
        .expect_err("not found is terminal");
    assert!(matches!(terminal, SemanticRecipeExecution::NotFound));
}

#[test]
fn semantic_unavailable_is_typed_before_detail_projection() {
    let terminal = semantic_stage_receipt(SemanticMaskStageOutcome::Unavailable(
        SubjectMaskRuntimeError::InputSize(0),
    ))
    .expect_err("unavailable is terminal");
    assert!(matches!(terminal, SemanticRecipeExecution::Unavailable(_)));
}

struct ImportFixture {
    session: Box<DesktopSession>,
    photo: String,
    source: String,
    _root: tempfile::TempDir,
}

impl ImportFixture {
    fn new() -> Self {
        let root = tempfile::tempdir().expect("temporary import fixture");
        let session = open_desktop_session(
            root.path().join("catalog.sqlite").to_str().unwrap(),
            root.path().join("cache").to_str().unwrap(),
        )
        .expect("open import session");
        let source = root
            .path()
            .join("destination.dng")
            .to_string_lossy()
            .into_owned();
        std::fs::write(&source, [0_u8; 16]).unwrap();
        std::fs::File::options()
            .write(true)
            .open(&source)
            .unwrap()
            .set_times(
                std::fs::FileTimes::new()
                    .set_modified(std::time::UNIX_EPOCH + std::time::Duration::from_millis(100)),
            )
            .unwrap();
        let registered = session
            .catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    source.as_bytes().to_vec(),
                    source.clone(),
                ),
                byte_len: 16,
                modified_at_ms: Some(100),
                now_ms: 100,
            })
            .expect("register destination");
        Self {
            session,
            photo: registered.photo_id.to_string(),
            source,
            _root: root,
        }
    }

    fn seed(&self, node_count: usize) -> ffi::FfiPhotoEditState {
        let mut destination = GradeStackDraft {
            foundation: PhotoFoundationNode::new(
                RecipeInputSettings::new(RecipeOpticsSettings::default()).with_raw_white_balance(
                    RawWhiteBalance::temperature_tint(RawTemperatureTint::new(6_200, -8).unwrap()),
                ),
            ),
            retouch_enabled: false,
            image_completion_enabled: false,
            canvas: PhotoCanvasNode::added().with_enabled(false),
            ..GradeStackDraft::default()
        };
        destination.grade_nodes[0].local_mask = Some(semantic_definition('c'));
        destination.grade_nodes[0].opacity = UnitInterval::new(0.3).unwrap();
        while destination.grade_nodes.len() < node_count {
            destination
                .grade_nodes
                .push(GradeNodeDraft::neutral("Destination"));
        }
        self.session
            .autosave_grade_stack_working_at(&self.photo, &self.source, "", "", &destination, 100)
            .expect("persist destination with existing opaque mask")
    }

    fn plan(&self, state: &ffi::FfiPhotoEditState, resolve: bool) -> ffi::FfiRecipeImportPlan {
        let mut source = GradeStackDraft::default();
        source.grade_nodes[0].local_mask = Some(semantic_definition('a'));
        source.grade_nodes[0].opacity = UnitInterval::new(0.42).unwrap();
        source.grade_nodes[0].enabled = false;
        let document =
            ShadowRecipeDocument::new(None, grade_stack_recipe_v1_snapshot(&source, None).unwrap())
                .unwrap()
                .to_pretty_json()
                .unwrap();
        let plan = self
            .session
            .prepare_semantic_recipe_import(
                &self.photo,
                &self.source,
                &state.working_commit_id,
                &state.working_commit_id,
                &state.settings,
                &document,
            )
            .expect("prepare target-specific semantic plan");
        if resolve {
            // Model execution is outside this persistence contract. Supply one
            // typed destination result through the real plan state machine.
            let item = &plan.items[0];
            let job = self
                .session
                .begin_semantic_recipe_import_item(plan.plan_token, &item.item_id)
                .unwrap();
            let context = self
                .session
                .recipe_imports
                .execution_context(plan.plan_token, &item.item_id, job)
                .unwrap();
            self.session.subject_masks.finish_job(job).unwrap();
            self.session
                .recipe_imports
                .complete_item(
                    &context,
                    RecipeImportItemTerminal::Staged,
                    String::new(),
                    Some(71),
                    None,
                )
                .unwrap();
            self.session
                .recipe_imports
                .resolve_item(
                    plan.plan_token,
                    &item.item_id,
                    71,
                    context.generation,
                    SemanticLeafResolution {
                        item_id: context.item.item_id,
                        grade_node_id: context.item.grade_node_id,
                        component_id: context.item.component_id,
                        definition: semantic_definition('b'),
                    },
                )
                .unwrap();
        }
        plan
    }

    fn append(
        &self,
        plan: u64,
        state: &ffi::FfiPhotoEditState,
    ) -> anyhow::Result<ffi::FfiPhotoEditState> {
        self.session.commit_semantic_recipe_append(
            plan,
            &self.photo,
            &self.source,
            &state.working_commit_id,
            &state.working_commit_id,
            &state.active_variant_id,
            &state.settings,
        )
    }

    fn snapshot(&self, commit: &str) -> RecipeSnapshot {
        self.session
            .catalog
            .recipe_commit(self.photo.parse().unwrap(), commit.parse().unwrap())
            .unwrap()
            .expect("persisted Recipe")
            .commit
            .snapshot()
            .clone()
    }

    fn current_head(&self) -> String {
        self.session
            .photo_edit_state(&self.photo, &self.source)
            .unwrap()
            .working_commit_id
    }

    fn running_import(&self) -> RecipeImportExecutionContext {
        let state = self.seed(1);
        let plan = self.plan(&state, false);
        let item = &plan.items[0].item_id;
        let job = self
            .session
            .begin_semantic_recipe_import_item(plan.plan_token, item)
            .unwrap();
        self.session
            .recipe_imports
            .execution_context(plan.plan_token, item, job)
            .unwrap()
    }

    fn stage_mask(
        &self,
        context: &RecipeImportExecutionContext,
        encoding: SoftMaskEncoding,
    ) -> (SubjectMaskCompletion, PathBuf) {
        // A tiny admitted fixture provider supplies typed staging authority.
        // It never contacts Infer Runtime, a model, or a native image decoder.
        let extent = RasterExtent::new(2, 2).unwrap();
        let bytes = match encoding {
            SoftMaskEncoding::Gray8Unorm => vec![0, 64, 192, 255],
            SoftMaskEncoding::Gray16Float => vec![0; 8],
        };
        let scratch = self._root.path().join("fixture-mask.gray");
        std::fs::write(&scratch, &bytes).unwrap();
        let digest = blake3::hash(&bytes).to_hex().to_string();
        let payload = AiGeneratedPayload::SoftMask(SoftMaskArtifact {
            artifact: GeneratedArtifactReference::new(
                ArtifactHashAlgorithm::Blake3_256,
                digest.clone(),
                bytes.len() as u64,
                "application/x-shadow-soft-mask".into(),
                1,
            )
            .unwrap(),
            raster_extent: extent,
            coordinate_extent: extent,
            coordinate_space: MaskCoordinateSpace::Original,
            encoding,
            semantic: MaskSemantic::UserPrompt,
        });
        let route = ExecutionRouteIdentity {
            contract_version: EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
            provider: ProviderIdentity {
                provider_id: "fixture.recipe-import".into(),
                adapter_revision: "fixture-v1".into(),
                execution_class: ProviderExecutionClass::SystemFramework,
            },
            model: AdmittedModelIdentity::SystemFramework {
                framework: "Fixture".into(),
                request_name: "mask".into(),
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
            "fixture-recipe-import-execution".into(),
            AiJobRequest {
                contract_version: AI_JOB_REQUEST_CONTRACT_VERSION,
                request_id: "fixture-recipe-import-mask".into(),
                generation: context.generation,
                task: AiTaskKind::ProposeSubjectMask,
                target: ObservationTarget::Photo {
                    photo_id: self.photo.parse().unwrap(),
                },
                priority: TaskPriority::CurrentInput,
                privacy: PrivacyClass::Personal,
                inputs: vec![ArtifactReference {
                    role: InputRole::CurrentRenderedCrop,
                    content_hash: "c".repeat(64),
                    byte_len: 12,
                    media_type: "image/png".into(),
                    privacy: PrivacyClass::Personal,
                }],
                parameters: AiTaskParameters::SubjectMask(SubjectMaskParameters {
                    prompt: MaskPrompt::AutomaticSubject,
                    coordinate_space: MaskCoordinateSpace::Original,
                    coordinate_extent: extent,
                    edge_refinement: shadow_ai::UnitInterval::new(0.5).unwrap(),
                    maximum_candidates: 1,
                }),
                estimate,
            },
            route.clone(),
            &BTreeSet::from([AiCapability::SubjectMask]),
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
        let provider = ImportMaskProvider {
            route,
            plan: execution.plan_identity().clone(),
            payload,
        };
        let store = self.session.subject_masks.store();
        let receipt = execute_and_stage_derived_raster(
            store,
            "fixture-recipe-import-promotion".into(),
            ExecutionLease::issue("fixture-recipe-import-lease".into(), execution).unwrap(),
            &provider,
            &CancellationToken::default(),
            &scratch,
        )
        .unwrap();
        let completion = self
            .session
            .subject_masks
            .complete_job(context.job_token, receipt)
            .unwrap();
        let staged_path = store
            .root()
            .join("proposals/b3")
            .join(&digest[..2])
            .join(&digest[2..]);
        (completion, staged_path)
    }
}

#[derive(Debug)]
struct ImportMaskProvider {
    route: ExecutionRouteIdentity,
    plan: ExecutionPlanIdentity,
    payload: AiGeneratedPayload,
}

impl RuntimeProvider for ImportMaskProvider {
    type Output = AiGeneratedPayload;

    fn route(&self) -> &ExecutionRouteIdentity {
        &self.route
    }
    fn execution_plan(&self) -> &ExecutionPlanIdentity {
        &self.plan
    }
    fn supports(&self, capability: AiCapability) -> bool {
        capability == AiCapability::SubjectMask
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

fn completion_token(completion: &SubjectMaskCompletion) -> u64 {
    let SubjectMaskCompletion::Staged { proposal_token, .. } = completion else {
        panic!("the fixture must stage a proposal");
    };
    *proposal_token
}

#[test]
fn damaged_semantic_proposal_preview_retires_authority_after_the_job_completes() {
    let fixture = ImportFixture::new();
    let context = fixture.running_import();
    let (completion, staged_path) = fixture.stage_mask(&context, SoftMaskEncoding::Gray8Unorm);
    let token = completion_token(&completion);
    std::fs::write(staged_path, [255_u8; 4]).unwrap();
    assert!(
        fixture
            .session
            .subject_masks
            .cancellation(context.job_token)
            .is_err()
    );
    assert!(
        fixture
            .session
            .project_semantic_recipe_completion(&context, "fixture-recipe-import-mask", completion,)
            .is_err()
    );
    assert!(matches!(
        fixture.session.subject_masks.proposal_preview(token),
        Err(SubjectMaskServiceError::UnknownProposal(actual)) if actual == token,
    ));
    fixture
        .session
        .close_semantic_recipe_import(context.plan_token)
        .unwrap();
}

#[test]
fn incompatible_semantic_proposal_encoding_retires_staging_authority() {
    let fixture = ImportFixture::new();
    let context = fixture.running_import();
    let (completion, _) = fixture.stage_mask(&context, SoftMaskEncoding::Gray16Float);
    let token = completion_token(&completion);
    let error = fixture
        .session
        .project_semantic_recipe_completion(&context, "fixture-recipe-import-mask", completion)
        .expect_err("unsupported import preview encoding");
    assert!(error.to_string().contains("preview is incompatible"));
    assert!(matches!(
        fixture.session.subject_masks.proposal_preview(token),
        Err(SubjectMaskServiceError::UnknownProposal(actual)) if actual == token,
    ));
    fixture
        .session
        .close_semantic_recipe_import(context.plan_token)
        .unwrap();
}

#[test]
fn semantic_proposal_authority_survives_success_until_its_plan_is_closed() {
    let fixture = ImportFixture::new();
    let context = fixture.running_import();
    let (completion, _) = fixture.stage_mask(&context, SoftMaskEncoding::Gray8Unorm);
    let token = completion_token(&completion);
    let SemanticRecipeExecution::Staged {
        proposal_token,
        preview,
    } = fixture
        .session
        .project_semantic_recipe_completion(&context, "fixture-recipe-import-mask", completion)
        .unwrap()
    else {
        panic!("valid proposal is projected");
    };
    assert_eq!(proposal_token, token);
    assert_eq!(preview.samples.len(), 256 * 256);
    assert!(
        fixture
            .session
            .subject_masks
            .proposal_preview(token)
            .is_ok()
    );
    assert!(
        fixture
            .session
            .recipe_imports
            .complete_item(
                &context,
                RecipeImportItemTerminal::Staged,
                String::new(),
                Some(token),
                Some(preview),
            )
            .unwrap()
    );
    fixture
        .session
        .close_semantic_recipe_import(context.plan_token)
        .unwrap();
    assert!(matches!(
        fixture.session.subject_masks.proposal_preview(token),
        Err(SubjectMaskServiceError::UnknownProposal(actual)) if actual == token,
    ));
}

fn semantic_definition(seed: char) -> MaskDefinition {
    let hash = seed.to_string().repeat(64);
    let raster = ManagedRasterMask::new(
        format!("objects/v1/b3/{}/{}", &hash[..2], &hash[2..]),
        1,
        hash,
        16,
        4,
        4,
        4,
        4,
        RasterMaskEncoding::Gray8Unorm,
    )
    .unwrap();
    MaskDefinition::managed_raster_with_semantic_intent_and_refinement(
        raster,
        Some(
            SemanticMaskIntent::new("the subject", 4, 30, SemanticMaskAggregation::Union).unwrap(),
        ),
        -8,
        12,
        true,
    )
    .unwrap()
}

#[test]
fn semantic_append_persists_destination_masks_and_recovers_each_undo_redo_anchor() {
    let fixture = ImportFixture::new();
    let before = fixture.seed(1);
    let plan = fixture.plan(&before, true);
    let applied = fixture
        .append(plan.plan_token, &before)
        .expect("append concrete semantic raster");
    let acknowledged_variant = applied
        .variants
        .iter()
        .find(|variant| variant.is_active)
        .unwrap();
    assert_eq!(acknowledged_variant.variant_id, before.active_variant_id);
    assert_eq!(
        acknowledged_variant.head_commit_id,
        applied.working_commit_id
    );
    assert!(acknowledged_variant.has_head);
    assert!(applied.has_working_version);
    assert!(!applied.is_version_draft);
    let applied_snapshot = fixture.snapshot(&applied.working_commit_id);
    let added = &applied_snapshot.layers()[1];
    assert_eq!(
        applied_snapshot
            .resolve_mask(added.mask().unwrap())
            .unwrap()
            .definition(),
        &semantic_definition('b'),
    );
    let mut applied_draft = decode_grade_stack_draft_recipe_v1(&applied.settings).unwrap();
    let imported = applied_draft.grade_nodes.pop().unwrap();
    assert_eq!(imported.opacity, UnitInterval::new(0.42).unwrap());
    assert!(!imported.enabled);
    assert_eq!(
        applied_draft,
        decode_grade_stack_draft_recipe_v1(&before.settings).unwrap()
    );
    let reloaded = fixture
        .session
        .photo_edit_state(&fixture.photo, &fixture.source)
        .unwrap();
    assert_eq!(reloaded.working_commit_id, applied.working_commit_id);
    assert_eq!(
        decode_grade_stack_draft_recipe_v1(&reloaded.settings).unwrap(),
        decode_grade_stack_draft_recipe_v1(&applied.settings).unwrap(),
    );

    let undone = fixture
        .session
        .autosave_basic_edit_working_at(
            &fixture.photo,
            &fixture.source,
            &before.working_commit_id,
            &applied.working_commit_id,
            &before.settings,
            200,
        )
        .expect("undo uses the earlier mask content anchor");
    assert_eq!(
        fixture.snapshot(&undone.working_commit_id),
        fixture.snapshot(&before.working_commit_id)
    );
    let redone = fixture
        .session
        .autosave_basic_edit_working_at(
            &fixture.photo,
            &fixture.source,
            &applied.working_commit_id,
            &undone.working_commit_id,
            &applied.settings,
            300,
        )
        .expect("redo recovers the imported mask from its own content anchor");
    assert_eq!(
        fixture.snapshot(&redone.working_commit_id),
        applied_snapshot
    );
    fixture
        .session
        .close_semantic_recipe_import(plan.plan_token)
        .unwrap();
}

#[test]
fn semantic_append_rejects_incomplete_changed_and_stale_destinations_without_mutation() {
    let fixture = ImportFixture::new();
    let before = fixture.seed(1);
    let incomplete = fixture.plan(&before, false);
    assert!(fixture.append(incomplete.plan_token, &before).is_err());
    assert_eq!(fixture.current_head(), before.working_commit_id);
    fixture
        .session
        .close_semantic_recipe_import(incomplete.plan_token)
        .unwrap();
    let plan = fixture.plan(&before, true);
    let mut changed = fixture
        .session
        .photo_edit_state(&fixture.photo, &fixture.source)
        .unwrap();
    changed.settings.grade_nodes[0].basic.exposure_stops = 0.75;
    assert!(fixture.append(plan.plan_token, &changed).is_err());
    assert_eq!(fixture.current_head(), before.working_commit_id);

    let newer = fixture
        .session
        .autosave_basic_edit_working_at(
            &fixture.photo,
            &fixture.source,
            &before.working_commit_id,
            &before.working_commit_id,
            &changed.settings,
            200,
        )
        .unwrap();
    assert!(fixture.append(plan.plan_token, &before).is_err());
    assert_eq!(fixture.current_head(), newer.working_commit_id);
    fixture
        .session
        .close_semantic_recipe_import(plan.plan_token)
        .unwrap();
}

#[test]
fn semantic_append_rejects_same_head_variant_switch_and_node_overflow() {
    let fixture = ImportFixture::new();
    let before = fixture.seed(1);
    let plan = fixture.plan(&before, true);
    let alternate = fixture
        .session
        .create_photo_variant(&fixture.photo, &fixture.source, "Alternate")
        .unwrap();
    assert_eq!(alternate.working_commit_id, before.working_commit_id);
    assert!(fixture.append(plan.plan_token, &before).is_err());
    assert_eq!(fixture.current_head(), before.working_commit_id);
    fixture
        .session
        .activate_photo_variant(&fixture.photo, &fixture.source, &before.active_variant_id)
        .unwrap();
    fixture
        .append(plan.plan_token, &before)
        .expect("plan remains valid for original Variant");
    fixture
        .session
        .close_semantic_recipe_import(plan.plan_token)
        .unwrap();

    let full = ImportFixture::new();
    let before = full.seed(MAX_GRADE_NODES);
    let plan = full.plan(&before, true);
    assert!(full.append(plan.plan_token, &before).is_err());
    assert_eq!(full.current_head(), before.working_commit_id);
    full.session
        .close_semantic_recipe_import(plan.plan_token)
        .unwrap();
}

#[test]
fn semantic_append_rejects_source_replacement_at_the_same_destination_path() {
    let fixture = ImportFixture::new();
    let before = fixture.seed(1);
    let plan = fixture.plan(&before, true);
    let original = std::fs::read(&fixture.source).unwrap();
    let replaced = b"a different source at the exact same destination path";
    std::fs::write(&fixture.source, replaced).unwrap();

    let error = fixture
        .append(plan.plan_token, &before)
        .expect_err("changed source");
    assert!(error.to_string().contains("source changed"));
    assert_eq!(fixture.current_head(), before.working_commit_id);
    assert_eq!(
        fixture
            .session
            .catalog
            .recipe_commits(fixture.photo.parse().unwrap())
            .unwrap()
            .len(),
        1,
        "rejecting a stale semantic mask creates no Recipe commit",
    );
    assert_eq!(std::fs::read(&fixture.source).unwrap(), replaced);
    assert_ne!(original, replaced);
    fixture
        .session
        .close_semantic_recipe_import(plan.plan_token)
        .unwrap();
}

#[test]
fn cancelling_import_item_signals_attached_preview_and_retires_job() {
    let fixture = ImportFixture::new();
    let before = fixture.seed(1);
    let plan = fixture.plan(&before, false);
    let item = &plan.items[0].item_id;
    let job = fixture
        .session
        .begin_semantic_recipe_import_item(plan.plan_token, item)
        .unwrap();
    let preview = fixture.session.begin_basic_edit_preview();
    assert_ne!(preview, 0);
    fixture
        .session
        .subject_masks
        .attach_preview_render(job, preview)
        .unwrap();
    let native = fixture
        .session
        .edit_preview_render_tokens
        .cancellation(preview)
        .unwrap();
    let foundation = fixture
        .session
        .edit_preview_render_tokens
        .foundation_cancellation(preview)
        .unwrap();
    let result = fixture
        .session
        .cancel_semantic_recipe_import_item(plan.plan_token, item, job)
        .unwrap();
    assert_eq!(result.terminal, ffi::FfiRecipeImportItemTerminal::Cancelled);
    assert!(foundation.is_cancelled());
    assert!(!native.cancel(), "native stop handle was already signalled");
    assert!(fixture.session.subject_masks.cancellation(job).is_err());
    fixture
        .session
        .close_semantic_recipe_import(plan.plan_token)
        .unwrap();
}

#[test]
fn shared_link_commits_the_exact_imported_managed_mask_and_follow_policy() {
    let fixture = ImportFixture::new();
    let before = fixture.seed(1);
    let shared = fixture
        .session
        .publish_shared_grade_node(
            "Imported look",
            &crate::recipe_v1::new_basic_grade_node("Imported look").unwrap(),
        )
        .unwrap();
    let plan = fixture.plan(&before, true);
    let linked = fixture
        .session
        .commit_shared_grade_node_link(
            plan.plan_token,
            &shared.layer_id,
            &shared.revision_id,
            &fixture.photo,
            &fixture.source,
            &before.working_commit_id,
            &before.working_commit_id,
            &before.active_variant_id,
            &before.settings,
        )
        .expect("link finalized destination-specific semantic raster");
    let snapshot = fixture.snapshot(&linked.working_commit_id);
    let instance = &snapshot.layers()[1];
    assert!(instance.shared_follow_head());
    assert_eq!(
        snapshot
            .resolve_mask(instance.mask().unwrap())
            .unwrap()
            .definition(),
        &semantic_definition('b')
    );
    assert_eq!(
        linked.settings.grade_nodes[1].shared_revision_id,
        shared.revision_id
    );
    assert_eq!(linked.settings.grade_nodes[1].shared_revision_number, 1);
    assert!(linked.settings.grade_nodes[1].shared_follow_head);
    assert_eq!(linked.settings.grade_nodes[1].opacity, 0.42);
    assert!(!linked.settings.grade_nodes[1].enabled);
    assert_eq!(
        snapshot.layers()[0],
        fixture.snapshot(&before.working_commit_id).layers()[0]
    );
    let mut changed = shared.grade_node.clone();
    changed.basic.exposure_stops = 0.8;
    let latest = fixture
        .session
        .publish_shared_grade_node("Imported look", &changed)
        .unwrap();
    let target = fixture
        .session
        .shared_grade_linked_target(&shared.layer_id, &fixture.photo, &fixture.source)
        .unwrap();
    let updated = fixture
        .session
        .commit_shared_grade_link_update(&latest.layer_id, &latest.revision_id, &target)
        .unwrap();
    assert_eq!(updated.status, ffi::FfiSharedGradeUpdateStatus::Updated);
    let updated_snapshot = fixture.snapshot(&updated.state.working_commit_id);
    assert_eq!(updated_snapshot.layers()[1].id(), instance.id());
    assert_eq!(updated_snapshot.layers()[1].mask(), instance.mask());
    assert_eq!(
        updated_snapshot
            .resolve_mask(updated_snapshot.layers()[1].mask().unwrap())
            .unwrap()
            .definition(),
        &semantic_definition('b')
    );
    fixture
        .session
        .close_semantic_recipe_import(plan.plan_token)
        .unwrap();
}

#[test]
fn shared_link_rejects_a_new_library_head_and_wrong_parameters_without_mutation() {
    let fixture = ImportFixture::new();
    let before = fixture.seed(1);
    let shared = fixture
        .session
        .publish_shared_grade_node(
            "Imported look",
            &crate::recipe_v1::new_basic_grade_node("Imported look").unwrap(),
        )
        .unwrap();
    let plan = fixture.plan(&before, true);
    let mut changed = shared.grade_node.clone();
    changed.basic.exposure_stops = 0.8;
    let latest = fixture
        .session
        .publish_shared_grade_node("Imported look", &changed)
        .unwrap();
    assert!(
        fixture
            .session
            .commit_shared_grade_node_link(
                plan.plan_token,
                &shared.layer_id,
                &shared.revision_id,
                &fixture.photo,
                &fixture.source,
                &before.working_commit_id,
                &before.working_commit_id,
                &before.active_variant_id,
                &before.settings,
            )
            .is_err()
    );
    assert!(
        fixture
            .session
            .commit_shared_grade_node_link(
                plan.plan_token,
                &latest.layer_id,
                &latest.revision_id,
                &fixture.photo,
                &fixture.source,
                &before.working_commit_id,
                &before.working_commit_id,
                &before.active_variant_id,
                &before.settings,
            )
            .is_err()
    );
    assert_eq!(fixture.current_head(), before.working_commit_id);
    fixture
        .session
        .close_semantic_recipe_import(plan.plan_token)
        .unwrap();
}
