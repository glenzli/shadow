//! Desktop-session orchestration for AI subject-mask authoring.
//!
//! One job prepares an identity-geometry JPEG, maps prompt points from the
//! displayed final canvas into original-image space, and stages provider
//! output. Applying remains a separate generation-checked transaction so a
//! stale background result can never mutate the Recipe.

use anyhow::{Context, Result as AnyResult, bail};
use shadow_ai::{
    MAX_MASK_PROMPT_POINTS, MaskPointPolarity, MaskPrompt, MaskPromptPoint, RasterExtent,
    UnitInterval,
};
use shadow_domain::{
    EntityId, MaskComponentId, MaskComponentOperation, SemanticMaskAggregation, SemanticMaskIntent,
};

use super::{
    DesktopSession, ffi,
    recipe_v1::{
        CompositeMaskDraft, GradeStackDraft, MaskComponentDraft, MaskComponentDraftDefinition,
        decode_grade_stack_draft_recipe_v1,
    },
    subject_mask_people::{FaceRegionSet, prepare_person_candidates},
    subject_mask_runtime::{
        SemanticMaskInvocation, SemanticMaskStageOutcome, SubjectMaskInvocation,
        SubjectMaskSelection,
        geometry::{map_output_prompt_to_original, project_gray8_mask_to_output},
    },
    subject_mask_service::{
        SubjectMaskCompletion, SubjectMaskInputAdmission, SubjectMaskPersonSnapshot,
        SubjectMaskRefinement, SubjectMaskRenderInputIdentity, SubjectMaskServiceError,
    },
    wall_clock::current_time_ms,
};

const SUBJECT_MASK_INPUT_MAX_EDGE: u32 = 1_024;
const SUBJECT_MASK_INPUT_JPEG_QUALITY: u8 = 95;
const SUBJECT_MASK_CANDIDATE_PREVIEW_EDGE: u32 = 256;

impl DesktopSession {
    pub(crate) fn begin_subject_mask_input_session(&self) -> AnyResult<u64> {
        Ok(self.subject_masks.begin_input_session()?)
    }

    pub(crate) fn finish_subject_mask_input_session(
        &self,
        subject_mask_input_session_token: u64,
    ) -> AnyResult<()> {
        Ok(self
            .subject_masks
            .finish_input_session(subject_mask_input_session_token)?)
    }

    pub(crate) fn begin_subject_mask_job(&self) -> AnyResult<u64> {
        Ok(self.subject_masks.begin_job()?)
    }

    pub(crate) fn cancel_subject_mask_job(&self, subject_mask_job_token: u64) -> AnyResult<()> {
        if let Some(preview_render_token) = self.subject_masks.cancel_job(subject_mask_job_token)? {
            // Provider cancellation already won for the AI job. The preview
            // token may have reached its unique terminal state first, so this
            // best-effort signal intentionally does not redefine job success.
            let _ = self.cancel_basic_edit_preview(preview_render_token);
        }
        Ok(())
    }

    pub(crate) fn execute_subject_mask_job(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiSubjectMaskRequest,
    ) -> AnyResult<ffi::FfiSubjectMaskResult> {
        match self.execute_subject_mask_job_inner(photo_id, source_path, request) {
            Ok(result) => Ok(result),
            Err(error) => {
                // Invalid captured input must not leak a bounded job slot. If
                // an inner terminal path already consumed the job this is an
                // intentional no-op.
                let _ = self.subject_masks.finish_job(request.job_token);
                Err(error)
            }
        }
    }

    // Render capture, prompt projection, runtime staging, cancellation, and
    // proposal publication are one bounded subject-mask job transaction.
    #[allow(clippy::too_many_lines)]
    fn execute_subject_mask_job_inner(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiSubjectMaskRequest,
    ) -> AnyResult<ffi::FfiSubjectMaskResult> {
        let cancellation = self.subject_masks.cancellation(request.job_token)?;
        if cancellation.is_cancelled() {
            self.subject_masks.finish_job(request.job_token)?;
            return Ok(subject_mask_terminal(
                request,
                ffi::FfiSubjectMaskTerminal::Cancelled,
                0,
                String::new(),
            ));
        }

        let grade_stack = decode_grade_stack_draft_recipe_v1(&request.settings)?;
        validate_subject_mask_target(
            &grade_stack,
            request.target_grade_node_index,
            &request.target_grade_node_id,
        )?;
        let display_points = match request.kind {
            ffi::FfiSubjectMaskKind::PromptedSubject => subject_mask_points(&request.points)?,
            ffi::FfiSubjectMaskKind::PeopleDiscovery
            | ffi::FfiSubjectMaskKind::PeopleRegions
            | ffi::FfiSubjectMaskKind::SemanticQuery => Vec::new(),
            _ => bail!("subject-mask selection kind is unsupported"),
        };
        let input_identity = SubjectMaskRenderInputIdentity {
            photo_id: photo_id.to_owned(),
            source_path: source_path.to_owned(),
            base_commit_id: request.base_commit_id.clone(),
            grade_stack: grade_stack.clone(),
        };
        let prepared_input = match self
            .subject_masks
            .admit_original_space_input(request.input_session_token, &input_identity)?
        {
            SubjectMaskInputAdmission::Reuse(input) => input,
            SubjectMaskInputAdmission::Prepare => {
                let prepared = (|| -> AnyResult<Option<_>> {
                    let render_token = self.begin_basic_edit_preview();
                    if render_token == 0 {
                        bail!("subject-mask input preview registry is full");
                    }
                    if let Err(error) = self
                        .subject_masks
                        .attach_preview_render(request.job_token, render_token)
                    {
                        let _ = self.cancel_basic_edit_preview(render_token);
                        if matches!(error, SubjectMaskServiceError::JobCancelled(_)) {
                            return Ok(None);
                        }
                        return Err(error.into());
                    }

                    let mut original_space_settings = request.settings.clone();
                    original_space_settings.geometry = identity_ffi_geometry();
                    let input_preview = self.render_subject_mask_input_preview(
                        photo_id,
                        source_path,
                        &ffi::FfiEditPreviewRequest {
                            base_commit_id: request.base_commit_id.clone(),
                            settings: original_space_settings,
                            render_token,
                            max_edge: SUBJECT_MASK_INPUT_MAX_EDGE,
                            jpeg_quality: SUBJECT_MASK_INPUT_JPEG_QUALITY,
                            // The subject-mask render entry replaces this
                            // public work class with its internal no-analysis
                            // JPEG policy while retaining the same working
                            // Recipe source.
                            policy: ffi::FfiEditPreviewPolicy::Settled,
                            use_working_recipe: true,
                            mask_coverage_requested: false,
                            mask_coverage_target_layer_index: 0,
                            mask_coverage_component_requested: false,
                            mask_coverage_target_component_index: 0,
                            mask_selection_revision: 0,
                        },
                    )?;
                    if input_preview.terminal == ffi::FfiEditPreviewTerminal::Cancelled
                        || cancellation.is_cancelled()
                    {
                        return Ok(None);
                    }
                    if input_preview.terminal != ffi::FfiEditPreviewTerminal::Completed
                        || input_preview.row_stride_bytes != 0
                    {
                        bail!("subject-mask input preview returned an invalid terminal payload");
                    }
                    let coordinate_extent =
                        RasterExtent::new(input_preview.width, input_preview.height)
                            .context("subject-mask input preview dimensions are invalid")?;
                    Ok(Some(
                        self.subject_masks
                            .complete_original_space_input_preparation(
                                request.input_session_token,
                                &input_identity,
                                input_preview.bytes,
                                coordinate_extent,
                            )?,
                    ))
                })();
                match prepared {
                    Ok(Some(input)) => input,
                    Ok(None) => {
                        let _ = self.subject_masks.abort_original_space_input_preparation(
                            request.input_session_token,
                            &input_identity,
                        );
                        self.subject_masks.finish_job(request.job_token)?;
                        return Ok(subject_mask_terminal(
                            request,
                            ffi::FfiSubjectMaskTerminal::Cancelled,
                            0,
                            String::new(),
                        ));
                    }
                    Err(error) => {
                        let _ = self.subject_masks.abort_original_space_input_preparation(
                            request.input_session_token,
                            &input_identity,
                        );
                        return Err(error);
                    }
                }
            }
        };
        let coordinate_extent = prepared_input.coordinate_extent;
        if request.kind == ffi::FfiSubjectMaskKind::PeopleDiscovery {
            let people = match self
                .subject_masks
                .people_snapshot(request.input_session_token, &input_identity)?
            {
                Some(people) => people,
                None => {
                    let detections = match self.subject_mask_runtime.detect_people(
                        &prepared_input.bytes,
                        &prepared_input.content_hash,
                        coordinate_extent,
                        &cancellation,
                    ) {
                        Ok(Some(detections)) => detections,
                        Ok(None) => {
                            self.subject_masks.finish_job(request.job_token)?;
                            return Ok(subject_mask_terminal(
                                request,
                                ffi::FfiSubjectMaskTerminal::Cancelled,
                                0,
                                String::new(),
                            ));
                        }
                        Err(error) => {
                            self.subject_masks.finish_job(request.job_token)?;
                            return Ok(subject_mask_terminal(
                                request,
                                ffi::FfiSubjectMaskTerminal::Unavailable,
                                0,
                                error.to_string(),
                            ));
                        }
                    };
                    let candidates = prepare_person_candidates(
                        &prepared_input.bytes,
                        coordinate_extent,
                        &detections,
                    )?;
                    self.subject_masks.cache_people(
                        request.input_session_token,
                        &input_identity,
                        candidates,
                    )?
                }
            };
            self.subject_masks.finish_job(request.job_token)?;
            return Ok(people_ready_result(request, people, String::new()));
        }
        let points = display_points
            .into_iter()
            .map(|point| {
                map_output_prompt_to_original(
                    point,
                    grade_stack.canvas.effective_geometry(),
                    coordinate_extent,
                )
            })
            .collect::<Vec<_>>();
        let selection = match request.kind {
            ffi::FfiSubjectMaskKind::PromptedSubject => {
                SubjectMaskSelection::PromptedSubject { points }
            }
            ffi::FfiSubjectMaskKind::PeopleRegions => {
                let regions = FaceRegionSet::from_bits(request.face_region_mask)?;
                let person = self.subject_masks.person_candidate(
                    request.input_session_token,
                    &input_identity,
                    request.person_index,
                )?;
                let parsed = match self.subject_masks.parsed_person(
                    request.input_session_token,
                    &input_identity,
                    request.person_index,
                )? {
                    Some(parsed) => parsed,
                    None => {
                        let parsed = match self.subject_mask_runtime.parse_person(
                            &prepared_input.bytes,
                            &prepared_input.content_hash,
                            coordinate_extent,
                            &person,
                            &cancellation,
                        ) {
                            Ok(Some(parsed)) => parsed,
                            Ok(None) => {
                                self.subject_masks.finish_job(request.job_token)?;
                                return Ok(subject_mask_terminal(
                                    request,
                                    ffi::FfiSubjectMaskTerminal::Cancelled,
                                    0,
                                    String::new(),
                                ));
                            }
                            Err(error) => {
                                self.subject_masks.finish_job(request.job_token)?;
                                return Ok(subject_mask_terminal(
                                    request,
                                    ffi::FfiSubjectMaskTerminal::Unavailable,
                                    0,
                                    error.to_string(),
                                ));
                            }
                        };
                        self.subject_masks.cache_parsed_person(
                            request.input_session_token,
                            &input_identity,
                            request.person_index,
                            parsed,
                        )?
                    }
                };
                if !regions.intersects(parsed.available_regions) {
                    self.subject_masks.finish_job(request.job_token)?;
                    let people = self
                        .subject_masks
                        .people_snapshot(request.input_session_token, &input_identity)?
                        .unwrap_or_default();
                    return Ok(people_ready_result(
                        request,
                        people,
                        "the selected facial regions are not visible for this person".into(),
                    ));
                }
                SubjectMaskSelection::FaceRegions {
                    person,
                    regions,
                    parsed,
                }
            }
            ffi::FfiSubjectMaskKind::SemanticQuery => {
                if !request.points.is_empty() {
                    bail!("semantic-mask request cannot also carry point prompts");
                }
                SubjectMaskSelection::SemanticQuery {
                    intent: SemanticMaskIntent::new(
                        request.semantic_query.clone(),
                        request.semantic_maximum_regions,
                        request.semantic_score_threshold_percent,
                        SemanticMaskAggregation::Union,
                    )?,
                }
            }
            _ => bail!("subject-mask selection kind is unsupported"),
        };
        let request_id = format!(
            "desktop-subject-mask-{}-{}",
            request.job_token, request.generation
        );
        let promotion_id = format!(
            "photo:{photo_id}:grade-node:{}:subject-mask:{}",
            request.target_grade_node_id, request.job_token
        );
        let staged = match selection {
            SubjectMaskSelection::SemanticQuery { intent } => {
                self.subject_mask_runtime.stage_semantic_intent(
                    self.subject_masks.store(),
                    SemanticMaskInvocation {
                        request_id,
                        promotion_id,
                        generation: request.generation,
                        photo_id: photo_id.to_owned(),
                        original_space_input: prepared_input.clone(),
                        intent,
                    },
                    &cancellation,
                )
            }
            selection => match self.subject_mask_runtime.stage(
                self.subject_masks.store(),
                SubjectMaskInvocation {
                    request_id,
                    promotion_id,
                    generation: request.generation,
                    photo_id: photo_id.to_owned(),
                    original_space_input_jpeg: prepared_input.bytes.clone(),
                    original_space_input_content_hash: prepared_input.content_hash.clone(),
                    coordinate_extent,
                    selection,
                },
                &cancellation,
            ) {
                Ok(receipt) => SemanticMaskStageOutcome::Staged(receipt),
                Err(error) => SemanticMaskStageOutcome::Unavailable(error),
            },
        };
        let receipt = match staged {
            SemanticMaskStageOutcome::Staged(receipt)
            | SemanticMaskStageOutcome::ProviderUnavailable(receipt)
            | SemanticMaskStageOutcome::ProviderFailed(receipt) => receipt,
            SemanticMaskStageOutcome::Cancelled => {
                self.subject_masks.finish_job(request.job_token)?;
                return Ok(subject_mask_terminal(
                    request,
                    ffi::FfiSubjectMaskTerminal::Cancelled,
                    0,
                    String::new(),
                ));
            }
            SemanticMaskStageOutcome::NotFound => {
                self.subject_masks.finish_job(request.job_token)?;
                return Ok(subject_mask_terminal(
                    request,
                    ffi::FfiSubjectMaskTerminal::Failed,
                    0,
                    "semantic query did not match a visible region".into(),
                ));
            }
            SemanticMaskStageOutcome::Unavailable(error) => {
                self.subject_masks.finish_job(request.job_token)?;
                let terminal = if cancellation.is_cancelled() {
                    ffi::FfiSubjectMaskTerminal::Cancelled
                } else {
                    ffi::FfiSubjectMaskTerminal::Unavailable
                };
                return Ok(subject_mask_terminal(
                    request,
                    terminal,
                    0,
                    error.to_string(),
                ));
            }
        };
        let completion = self
            .subject_masks
            .complete_job(request.job_token, receipt)?;
        Ok(match completion {
            SubjectMaskCompletion::Staged {
                request_id,
                generation,
                proposal_token,
            } => {
                let expected_request_id = format!(
                    "desktop-subject-mask-{}-{}",
                    request.job_token, request.generation
                );
                if validate_staged_subject_mask_identity(
                    &expected_request_id,
                    request.generation,
                    &request_id,
                    generation,
                )
                .is_err()
                {
                    let _ = self.subject_masks.discard_proposal(proposal_token);
                    bail!("subject-mask runtime returned mismatched request identity");
                }
                let preview_result = (|| -> AnyResult<_> {
                    let preview = self.subject_masks.proposal_preview(proposal_token)?;
                    if preview.generation != generation {
                        bail!("subject-mask proposal preview generation changed");
                    }
                    if preview.encoding != shadow_ai::SoftMaskEncoding::Gray8Unorm {
                        bail!("subject-mask candidate preview requires a Gray8 soft mask");
                    }
                    let output_extent = RasterExtent::new(
                        SUBJECT_MASK_CANDIDATE_PREVIEW_EDGE,
                        SUBJECT_MASK_CANDIDATE_PREVIEW_EDGE,
                    )
                    .context("subject-mask candidate preview extent is invalid")?;
                    let samples = project_gray8_mask_to_output(
                        &preview.samples,
                        preview.raster_extent,
                        preview.coordinate_extent,
                        grade_stack.canvas.effective_geometry(),
                        output_extent,
                    )
                    .context("subject-mask candidate preview projection is invalid")?;
                    Ok((output_extent, samples))
                })();
                let (output_extent, preview_samples) = match preview_result {
                    Ok(preview) => preview,
                    Err(error) => {
                        let _ = self.subject_masks.discard_proposal(proposal_token);
                        return Err(error);
                    }
                };
                let mut result = subject_mask_terminal(
                    request,
                    ffi::FfiSubjectMaskTerminal::Staged,
                    proposal_token,
                    String::new(),
                );
                result.preview_width = output_extent.width;
                result.preview_height = output_extent.height;
                result.preview_samples = preview_samples;
                result.people = project_people(
                    self.subject_masks
                        .people_snapshot(request.input_session_token, &input_identity)?
                        .unwrap_or_default(),
                );
                result
            }
            SubjectMaskCompletion::Unavailable { reason } => subject_mask_terminal(
                request,
                ffi::FfiSubjectMaskTerminal::Unavailable,
                0,
                format!("{reason:?}"),
            ),
            SubjectMaskCompletion::Cancelled => subject_mask_terminal(
                request,
                ffi::FfiSubjectMaskTerminal::Cancelled,
                0,
                String::new(),
            ),
            SubjectMaskCompletion::Failed { failure } => subject_mask_terminal(
                request,
                ffi::FfiSubjectMaskTerminal::Failed,
                0,
                format!("{failure:?}"),
            ),
        })
    }

    pub(crate) fn apply_subject_mask_proposal(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiSubjectMaskApplyRequest,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        // Reject a stale or forged photo/source pair before consuming the
        // move-only proposal authority.
        self.validated_photo_source(photo_id, source_path)?;
        let mut grade_stack = decode_grade_stack_draft_recipe_v1(&request.settings)?;
        validate_subject_mask_target(
            &grade_stack,
            request.target_grade_node_index,
            &request.target_grade_node_id,
        )?;
        let operation = subject_mask_operation(request.target_mask_operation)?;
        let target_component_count = subject_mask_target_component_count(
            &grade_stack.grade_nodes[request.target_grade_node_index as usize],
        );
        if (target_component_count == 0 && operation != MaskComponentOperation::Base)
            || (target_component_count > 0 && operation == MaskComponentOperation::Base)
            || target_component_count >= shadow_domain::MAX_MASK_COMPONENTS
        {
            bail!("subject-mask operation is incompatible with the target component stack");
        }
        let mask = self.subject_masks.promote_proposal(
            request.proposal_token,
            request.generation,
            SubjectMaskRefinement {
                expansion_percent: 0,
                feather_percent: 0,
                leaf_invert: request.invert,
                semantic_intent: if request.semantic_query.is_empty() {
                    None
                } else {
                    Some(SemanticMaskIntent::new(
                        request.semantic_query.clone(),
                        request.semantic_maximum_regions,
                        request.semantic_score_threshold_percent,
                        SemanticMaskAggregation::Union,
                    )?)
                },
            },
        )?;
        let target = grade_stack
            .grade_nodes
            .get_mut(request.target_grade_node_index as usize)
            .context("subject-mask target Grade Node index is unavailable")?;
        append_subject_mask_component(target, mask, operation)?;
        self.autosave_grade_stack_working_at(
            photo_id,
            source_path,
            &request.base_commit_id,
            &request.expected_working_commit_id,
            &grade_stack,
            current_time_ms()?,
        )
    }

    pub(crate) fn discard_subject_mask_proposal(&self, proposal_token: u64) -> AnyResult<()> {
        Ok(self.subject_masks.discard_proposal(proposal_token)?)
    }
}

fn validate_staged_subject_mask_identity(
    expected_request_id: &str,
    expected_generation: u64,
    request_id: &str,
    generation: u64,
) -> AnyResult<()> {
    if request_id != expected_request_id || generation != expected_generation {
        bail!("subject-mask runtime returned mismatched request identity");
    }
    Ok(())
}

fn validate_subject_mask_target(
    grade_stack: &GradeStackDraft,
    target_grade_node_index: u32,
    target_grade_node_id: &str,
) -> AnyResult<()> {
    let target = grade_stack
        .grade_nodes
        .get(target_grade_node_index as usize)
        .context("subject-mask target Grade Node index is unavailable")?;
    if target.recipe_v1_identity.grade_node_id.to_string() != target_grade_node_id {
        bail!("subject-mask target Grade Node identity changed");
    }
    if subject_mask_target_component_count(target) >= shadow_domain::MAX_MASK_COMPONENTS {
        bail!("subject-mask target already contains the maximum number of mask components");
    }
    Ok(())
}

fn subject_mask_target_component_count(target: &super::recipe_v1::GradeNodeDraft) -> usize {
    target.composite_mask.as_ref().map_or_else(
        || usize::from(target.local_mask.is_some() || target.preserved_managed_raster.is_some()),
        |composite| composite.components.len(),
    )
}

fn subject_mask_operation(value: u8) -> AnyResult<MaskComponentOperation> {
    match value {
        0 => Ok(MaskComponentOperation::Base),
        1 => Ok(MaskComponentOperation::Add),
        2 => Ok(MaskComponentOperation::Subtract),
        3 => Ok(MaskComponentOperation::Intersect),
        other => bail!("subject-mask target uses unsupported component operation {other}"),
    }
}

fn append_subject_mask_component(
    target: &mut super::recipe_v1::GradeNodeDraft,
    mask: shadow_domain::MaskDefinition,
    operation: MaskComponentOperation,
) -> AnyResult<MaskComponentId> {
    let component_count = subject_mask_target_component_count(target);
    if (component_count == 0 && operation != MaskComponentOperation::Base)
        || (component_count > 0 && operation == MaskComponentOperation::Base)
        || component_count >= shadow_domain::MAX_MASK_COMPONENTS
    {
        bail!("subject-mask operation is incompatible with the target component stack");
    }
    let component_id = MaskComponentId::from_uuid(uuid::Uuid::new_v4());
    let new_component = MaskComponentDraft {
        id: component_id,
        operation,
        enabled: true,
        definition: MaskComponentDraftDefinition::Definition(mask),
    };
    if operation == MaskComponentOperation::Base {
        target.local_mask = None;
        target.preserved_managed_raster = None;
        target.composite_mask = Some(CompositeMaskDraft {
            components: vec![new_component],
            invert: false,
        });
    } else if let Some(composite) = &mut target.composite_mask {
        composite.components.push(new_component);
    } else {
        let base_definition = match (
            target.local_mask.take(),
            target.preserved_managed_raster.take(),
        ) {
            (Some(definition), None) => MaskComponentDraftDefinition::Definition(definition),
            (None, Some(settings)) => {
                MaskComponentDraftDefinition::PreservedManagedRaster(settings)
            }
            _ => bail!("subject-mask target has no unambiguous base component"),
        };
        target.composite_mask = Some(CompositeMaskDraft {
            components: vec![
                MaskComponentDraft {
                    id: MaskComponentId::from_uuid(uuid::Uuid::new_v4()),
                    operation: MaskComponentOperation::Base,
                    enabled: true,
                    definition: base_definition,
                },
                new_component,
            ],
            invert: false,
        });
    }
    Ok(component_id)
}

fn subject_mask_points(points: &[ffi::FfiSubjectMaskPoint]) -> AnyResult<Vec<MaskPromptPoint>> {
    if points.is_empty() || points.len() > MAX_MASK_PROMPT_POINTS {
        bail!("subject-mask prompt requires between 1 and {MAX_MASK_PROMPT_POINTS} points");
    }
    let points = points
        .iter()
        .enumerate()
        .map(|(index, point)| {
            Ok(MaskPromptPoint {
                x: UnitInterval::new(point.x)
                    .with_context(|| format!("subject-mask point {index} x is invalid"))?,
                y: UnitInterval::new(point.y)
                    .with_context(|| format!("subject-mask point {index} y is invalid"))?,
                polarity: if point.foreground {
                    MaskPointPolarity::Foreground
                } else {
                    MaskPointPolarity::Background
                },
            })
        })
        .collect::<AnyResult<Vec<_>>>()?;
    MaskPrompt::Points {
        points: points.clone(),
    }
    .validate()
    .context("subject-mask point prompt is invalid")?;
    Ok(points)
}

const fn identity_ffi_geometry() -> ffi::FfiPhotoGeometry {
    ffi::FfiPhotoGeometry {
        present: false,
        enabled: true,
        crop_left: 0.0,
        crop_top: 0.0,
        crop_right: 1.0,
        crop_bottom: 1.0,
        quarter_turn: 0,
        straighten_degrees: 0.0,
        perspective_vertical: 0.0,
        perspective_horizontal: 0.0,
        flip_horizontal: false,
        flip_vertical: false,
    }
}

fn subject_mask_terminal(
    request: &ffi::FfiSubjectMaskRequest,
    terminal: ffi::FfiSubjectMaskTerminal,
    proposal_token: u64,
    detail: String,
) -> ffi::FfiSubjectMaskResult {
    ffi::FfiSubjectMaskResult {
        terminal,
        job_token: request.job_token,
        generation: request.generation,
        proposal_token,
        detail,
        preview_width: 0,
        preview_height: 0,
        preview_samples: Vec::new(),
        people: Vec::new(),
    }
}

fn people_ready_result(
    request: &ffi::FfiSubjectMaskRequest,
    people: Vec<SubjectMaskPersonSnapshot>,
    detail: String,
) -> ffi::FfiSubjectMaskResult {
    let mut result =
        subject_mask_terminal(request, ffi::FfiSubjectMaskTerminal::PeopleReady, 0, detail);
    result.people = project_people(people);
    result
}

fn project_people(people: Vec<SubjectMaskPersonSnapshot>) -> Vec<ffi::FfiSubjectMaskPerson> {
    people
        .into_iter()
        .enumerate()
        .map(|(index, person)| ffi::FfiSubjectMaskPerson {
            index: index as u32,
            confidence: f64::from(person.candidate.confidence),
            thumbnail_jpeg: person.candidate.thumbnail_jpeg.as_ref().to_vec(),
            regions_analyzed: person.available_regions.is_some(),
            available_region_mask: person.available_regions.map_or(0, |regions| regions.bits()),
        })
        .collect()
}

#[cfg(test)]
mod tests;
