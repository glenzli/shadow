//! Current-working shared-grade subscriptions with bounded discovery and CAS.
//!
//! Recipes always contain a pinned revision and a concrete executable graph.
//! This service updates those graphs at publication time; rendering never
//! consults a mutable shared Library head.

use std::collections::HashSet;

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_catalog::{
    CommitRecipeAndEditRepository, CommitSharedGradeLinkUpdate, SharedGradeLinkWriteResult,
    SharedGradeSourceGuard,
};
use shadow_domain::{EditCommitId, EntityId, LayerId, LayerInstanceId, LayerRevisionId, PhotoId};

use crate::{
    DesktopSession, ffi,
    recipe_v1::{
        GradeStackDraft, SharedGradeNodeReference, decode_grade_node_draft_recipe_v1,
        decode_grade_stack_draft_recipe_v1, encode_grade_stack_draft_recipe_v1,
        ffi_shared_grade_node, grade_node_draft_from_shared_revision,
    },
    session_edit_history::LIBRARY_EDIT_MAIN_REF,
    shared_grade_application::{replace_shared_grade_parameters, shared_grade_parameters_match},
    shared_grade_library,
    wall_clock::current_time_ms,
};

struct ReadyLinkUpdate {
    before: ffi::FfiPhotoEditState,
    grade_stack: GradeStackDraft,
    library_commit_id: EditCommitId,
    source: SharedGradeSourceGuard,
}

enum LinkInspection {
    Ready(Box<ReadyLinkUpdate>),
    Terminal(Box<ffi::FfiSharedGradeLinkUpdateReceipt>),
}

impl DesktopSession {
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn publish_shared_grade_node_from_photo(
        &self,
        photo_id: &str,
        source_path: &str,
        base_commit_id: &str,
        expected_working_commit_id: &str,
        expected_variant_id: &str,
        expected_representation_id: &str,
        settings: &ffi::FfiEditSettings,
        grade_node_id: &str,
        label: &str,
    ) -> AnyResult<ffi::FfiSharedGradePublishReceipt> {
        if expected_variant_id.is_empty() || expected_representation_id.is_empty() {
            bail!("shared Grade Node publication requires exact Variant and source identities");
        }
        let source = self.shared_grade_source_guard(photo_id, source_path)?;
        if source.representation_id.to_string() != expected_representation_id {
            bail!("shared Grade Node source representation changed");
        }
        let instance_id = grade_node_id
            .parse::<LayerInstanceId>()
            .context("parse publishing Grade Node instance")?;
        let mut grade_stack = decode_grade_stack_draft_recipe_v1(settings)?;
        let node = grade_stack
            .grade_nodes
            .iter_mut()
            .find(|node| node.recipe_v1_identity.grade_node_id == instance_id)
            .ok_or_else(|| anyhow!("publishing Grade Node instance is unavailable"))?;
        let layer_id = node
            .shared
            .map_or_else(LayerId::new_v7, |reference| reference.layer_id);
        let created_at_ms = current_time_ms()?;
        let publication = shared_grade_library::prepare_shared_grade_revision(
            &self.catalog,
            layer_id,
            label,
            shared_grade_library::canonical_shared_graph(node, layer_id)?,
            node.shared.map(|reference| reference.revision_id),
            created_at_ms,
        )?;
        node.shared = Some(SharedGradeNodeReference {
            layer_id,
            revision_id: publication.revision.id(),
            shared_follow_head: true,
        });
        let (request, _, variant_id) = self.prepare_grade_stack_working_for_variant_at(
            photo_id,
            source_path,
            base_commit_id,
            expected_working_commit_id,
            &grade_stack,
            created_at_ms,
            Some(expected_variant_id),
            Some(expected_representation_id),
        )?;
        // Both DTO projections finish before any movable ref is published.
        let shared_node = ffi_shared_grade_node(&publication.revision)?;
        let mut state =
            self.photo_edit_state_for_prepared_working(&request, variant_id, source_path)?;
        self.project_shared_grade_revision_numbers(&mut state.settings)?;
        for node in &mut state.settings.grade_nodes {
            if node.grade_node_id == grade_node_id {
                node.shared_revision_number = publication.revision.revision_number();
            }
        }
        self.require_shared_grade_source_guard(photo_id, &source)?;
        self.catalog.store_edit_object_pack(&publication.objects)?;
        if self
            .catalog
            .commit_shared_grade_publication(
                &CommitRecipeAndEditRepository {
                    recipe: request,
                    repository: publication.repository,
                    expected_active_variant_id: Some(variant_id),
                },
                &source,
            )?
            .is_none()
        {
            bail!("shared Grade Node source changed before publication");
        }
        Ok(ffi::FfiSharedGradePublishReceipt { shared_node, state })
    }

    pub(crate) fn shared_grade_linked_targets(
        &self,
        layer_id: &str,
        revision_id: &str,
        after_photo_id: &str,
        limit: u32,
    ) -> AnyResult<ffi::FfiSharedGradeTargetPage> {
        let layer_id = layer_id
            .parse::<LayerId>()
            .context("parse linked shared Grade Node layer")?;
        let revision_id = revision_id
            .parse::<LayerRevisionId>()
            .context("parse linked shared Grade Node revision")?;
        let after = (!after_photo_id.is_empty())
            .then(|| after_photo_id.parse::<PhotoId>())
            .transpose()?;
        let page = self.catalog.shared_grade_linked_working_page(
            layer_id,
            revision_id,
            after,
            usize::try_from(limit)?,
        )?;
        let targets = page
            .records
            .into_iter()
            .map(|record| {
                let source = self.catalog.photo_source(record.photo_id)?;
                Ok(ffi::FfiSharedGradeLinkedTarget {
                    photo_id: record.photo_id.to_string(),
                    source_path: source
                        .as_ref()
                        .map_or_else(String::new, |source| source.location.display_path.clone()),
                    representation_id: source
                        .as_ref()
                        .map_or_else(String::new, |source| source.representation_id.to_string()),
                    source_byte_len: source.as_ref().map_or(0, |source| source.source.byte_len),
                    has_source_modified_at_ms: source
                        .as_ref()
                        .is_some_and(|source| source.source.modified_at_ms.is_some()),
                    source_modified_at_ms: source
                        .as_ref()
                        .and_then(|source| source.source.modified_at_ms)
                        .unwrap_or(0),
                    variant_id: record.variant_id.to_string(),
                    working_commit_id: record.commit_id.to_string(),
                    instances: record
                        .instances
                        .into_iter()
                        .map(|instance| ffi::FfiSharedGradeLinkedInstance {
                            grade_node_id: instance.instance_id.to_string(),
                            shared_revision_id: instance.revision_id.to_string(),
                        })
                        .collect(),
                })
            })
            .collect::<AnyResult<Vec<_>>>()?;
        Ok(ffi::FfiSharedGradeTargetPage {
            targets,
            has_more: page.next_photo_id.is_some(),
            next_photo_id: page
                .next_photo_id
                .map_or_else(String::new, |id| id.to_string()),
        })
    }

    /// Exact selected-photo lookup avoids walking the Library once per UI
    /// target. Every following instance of this layer is captured together.
    pub(crate) fn shared_grade_linked_target(
        &self,
        layer_id: &str,
        photo_id: &str,
        source_path: &str,
    ) -> AnyResult<ffi::FfiSharedGradeLinkedTarget> {
        let layer_id = layer_id.parse::<LayerId>()?;
        let photo = photo_id.parse::<PhotoId>()?;
        let source = self.catalog.photo_source(photo)?;
        if source
            .as_ref()
            .is_some_and(|source| source.location.display_path != source_path)
        {
            bail!("selected shared Grade Node target source changed");
        }
        let variant = self.active_photo_variant_id(photo)?;
        let head = self
            .catalog
            .recipe_ref(photo, crate::session_edit_history::WORKING_RECIPE_REF)?;
        let instances = if let Some(head) = &head {
            let record = self
                .catalog
                .recipe_commit(photo, head.commit_id)?
                .ok_or_else(|| anyhow!("selected shared Grade Node target head is unavailable"))?;
            record
                .commit
                .snapshot()
                .layers()
                .iter()
                .filter_map(|layer| match layer.content() {
                    shadow_domain::LayerContent::Shared {
                        layer_id: candidate,
                        revision: shadow_domain::LayerRevisionSelector::Pinned(revision),
                        ..
                    } if *candidate == layer_id && layer.shared_follow_head() => {
                        Some(ffi::FfiSharedGradeLinkedInstance {
                            grade_node_id: layer.id().to_string(),
                            shared_revision_id: revision.to_string(),
                        })
                    }
                    _ => None,
                })
                .collect()
        } else {
            Vec::new()
        };
        Ok(ffi::FfiSharedGradeLinkedTarget {
            photo_id: photo_id.into(),
            source_path: source
                .as_ref()
                .map_or_else(String::new, |source| source.location.display_path.clone()),
            representation_id: source
                .as_ref()
                .map_or_else(String::new, |source| source.representation_id.to_string()),
            source_byte_len: source.as_ref().map_or(0, |source| source.source.byte_len),
            has_source_modified_at_ms: source
                .as_ref()
                .is_some_and(|source| source.source.modified_at_ms.is_some()),
            source_modified_at_ms: source
                .as_ref()
                .and_then(|source| source.source.modified_at_ms)
                .unwrap_or(0),
            variant_id: variant.to_string(),
            working_commit_id: head.map_or_else(String::new, |head| head.commit_id.to_string()),
            instances,
        })
    }

    /// Returns the exact durable BEFORE state for the desktop reservation.
    /// Inspection does not create a Recipe, proposal, render, or model job.
    pub(crate) fn prepare_shared_grade_link_update(
        &self,
        layer_id: &str,
        revision_id: &str,
        target: &ffi::FfiSharedGradeLinkedTarget,
    ) -> AnyResult<ffi::FfiSharedGradeLinkUpdateReceipt> {
        match self.inspect_shared_grade_link_update(layer_id, revision_id, target)? {
            LinkInspection::Ready(ready) => update_receipt(
                ffi::FfiSharedGradeUpdateStatus::Updated,
                String::new(),
                Some(ready.before),
            ),
            LinkInspection::Terminal(receipt) => Ok(*receipt),
        }
    }

    pub(crate) fn commit_shared_grade_link_update(
        &self,
        layer_id: &str,
        revision_id: &str,
        target: &ffi::FfiSharedGradeLinkedTarget,
    ) -> AnyResult<ffi::FfiSharedGradeLinkUpdateReceipt> {
        let ready = match self.inspect_shared_grade_link_update(layer_id, revision_id, target)? {
            LinkInspection::Ready(ready) => ready,
            LinkInspection::Terminal(receipt) => return Ok(*receipt),
        };
        let (request, _, variant_id) = self.prepare_grade_stack_working_for_variant_at(
            &target.photo_id,
            &target.source_path,
            &target.working_commit_id,
            &target.working_commit_id,
            &ready.grade_stack,
            current_time_ms()?,
            Some(&target.variant_id),
            Some(&target.representation_id),
        )?;
        let state =
            self.photo_edit_state_for_prepared_working(&request, variant_id, &target.source_path)?;
        if self
            .require_shared_grade_source_guard(&target.photo_id, &ready.source)
            .is_err()
        {
            return update_receipt(
                ffi::FfiSharedGradeUpdateStatus::Unavailable,
                "photo source changed before shared update".into(),
                Some(ready.before),
            );
        }
        let status =
            match self
                .catalog
                .commit_shared_grade_link_update(&CommitSharedGradeLinkUpdate {
                    recipe: request,
                    expected_variant_id: variant_id,
                    expected_library_commit_id: ready.library_commit_id,
                    library_ref_name: LIBRARY_EDIT_MAIN_REF.into(),
                    source: ready.source,
                })? {
                SharedGradeLinkWriteResult::Committed(_) => {
                    ffi::FfiSharedGradeUpdateStatus::Updated
                }
                SharedGradeLinkWriteResult::HeadChanged => {
                    ffi::FfiSharedGradeUpdateStatus::HeadChanged
                }
                SharedGradeLinkWriteResult::SourceChanged => {
                    ffi::FfiSharedGradeUpdateStatus::Unavailable
                }
            };
        if status == ffi::FfiSharedGradeUpdateStatus::Updated {
            update_receipt(status, String::new(), Some(state))
        } else {
            update_receipt(
                status,
                "shared update expectations changed before publication".into(),
                Some(ready.before),
            )
        }
    }

    fn inspect_shared_grade_link_update(
        &self,
        layer_id: &str,
        revision_id: &str,
        target: &ffi::FfiSharedGradeLinkedTarget,
    ) -> AnyResult<LinkInspection> {
        let layer_id = layer_id.parse::<LayerId>()?;
        let revision_id = revision_id.parse::<LayerRevisionId>()?;
        let Some((library_commit_id, revision)) =
            shared_grade_library::try_shared_grade_revision_head(&self.catalog, layer_id)?
        else {
            return terminal(
                ffi::FfiSharedGradeUpdateStatus::Unavailable,
                "shared Grade Node is unavailable",
                None,
            );
        };
        if revision.id() != revision_id {
            return terminal(
                ffi::FfiSharedGradeUpdateStatus::HeadChanged,
                "shared Grade Node Library head changed",
                None,
            );
        }
        if target.source_path.is_empty() || target.representation_id.is_empty() {
            return terminal(
                ffi::FfiSharedGradeUpdateStatus::Unavailable,
                "photo has no online source",
                None,
            );
        }
        let photo_id = target.photo_id.parse::<PhotoId>()?;
        let source = match self.shared_grade_source_guard(&target.photo_id, &target.source_path) {
            Ok(source) => source,
            Err(error) => {
                return terminal(
                    ffi::FfiSharedGradeUpdateStatus::Unavailable,
                    &error.to_string(),
                    None,
                );
            }
        };
        let captured_fingerprint = shadow_catalog::RepresentationFingerprint {
            byte_len: target.source_byte_len,
            modified_at_ms: target
                .has_source_modified_at_ms
                .then_some(target.source_modified_at_ms),
        };
        if source.representation_id.to_string() != target.representation_id
            || source.fingerprint != captured_fingerprint
        {
            return terminal(
                ffi::FfiSharedGradeUpdateStatus::Unavailable,
                "photo source identity changed",
                None,
            );
        }
        let before = self.photo_edit_state(&target.photo_id, &target.source_path)?;
        if before.active_variant_id != target.variant_id
            || before.working_commit_id != target.working_commit_id
        {
            return terminal(
                ffi::FfiSharedGradeUpdateStatus::HeadChanged,
                "photo working head or Variant changed",
                Some(before),
            );
        }
        if target.instances.is_empty() {
            return terminal(
                ffi::FfiSharedGradeUpdateStatus::Unchanged,
                "no subscribed instances remain",
                Some(before),
            );
        }
        let mut seen = HashSet::new();
        let mut grade_stack = decode_grade_stack_draft_recipe_v1(&before.settings)?;
        let mut replacement = grade_node_draft_from_shared_revision(&revision)?;
        replacement
            .shared
            .as_mut()
            .expect("published shared revision")
            .shared_follow_head = true;
        let mut changed = false;
        for captured in &target.instances {
            let instance_id = captured.grade_node_id.parse::<LayerInstanceId>()?;
            if !seen.insert(instance_id) {
                bail!("shared Grade Node target repeats an instance identity");
            }
            let old_revision_id = captured.shared_revision_id.parse::<LayerRevisionId>()?;
            let Some(instance) = grade_stack
                .grade_nodes
                .iter_mut()
                .find(|node| node.recipe_v1_identity.grade_node_id == instance_id)
            else {
                return terminal(
                    ffi::FfiSharedGradeUpdateStatus::HeadChanged,
                    "subscribed Grade Node instance changed",
                    Some(before),
                );
            };
            let Some(reference) = instance.shared else {
                return terminal(
                    ffi::FfiSharedGradeUpdateStatus::HeadChanged,
                    "Grade Node was unlinked",
                    Some(before),
                );
            };
            if reference.layer_id != layer_id
                || !reference.shared_follow_head
                || reference.revision_id != old_revision_id
            {
                return terminal(
                    ffi::FfiSharedGradeUpdateStatus::HeadChanged,
                    "Grade Node link changed",
                    Some(before),
                );
            }
            let Some(baseline) = self
                .catalog
                .shared_grade_revision_by_id(layer_id, old_revision_id)?
            else {
                return terminal(
                    ffi::FfiSharedGradeUpdateStatus::Unavailable,
                    "pinned shared revision is unavailable",
                    Some(before),
                );
            };
            let baseline = grade_node_draft_from_shared_revision(&baseline)?;
            if !shared_grade_parameters_match(instance, &baseline) {
                return terminal(
                    ffi::FfiSharedGradeUpdateStatus::LocalChanged,
                    "shared Grade Node has unpublished local adjustments",
                    Some(before),
                );
            }
            if reference.revision_id != revision_id {
                replace_shared_grade_parameters(instance, &replacement);
                changed = true;
            }
        }
        // Ref verification is independent from DTO projection; the captured
        // photo owns every exact instance through this immutable head.
        let current = self
            .catalog
            .recipe_ref(photo_id, crate::session_edit_history::WORKING_RECIPE_REF)?;
        if current.is_none_or(|head| head.commit_id.to_string() != target.working_commit_id) {
            return terminal(
                ffi::FfiSharedGradeUpdateStatus::HeadChanged,
                "photo working head changed during inspection",
                Some(before),
            );
        }
        if !changed {
            return terminal(
                ffi::FfiSharedGradeUpdateStatus::Unchanged,
                "shared revision already materialized",
                Some(before),
            );
        }
        Ok(LinkInspection::Ready(Box::new(ReadyLinkUpdate {
            before,
            grade_stack,
            library_commit_id,
            source,
        })))
    }

    pub(crate) fn shared_grade_source_guard(
        &self,
        photo_id: &str,
        source_path: &str,
    ) -> AnyResult<SharedGradeSourceGuard> {
        let (_, source) = self.validated_photo_source(photo_id, source_path)?;
        let path = shadow_core::native_path_from_location(&source.location)?;
        let fingerprint = shadow_core::fingerprint_source(&path)
            .context("read shared Grade Node source metadata")?;
        if fingerprint != source.source {
            bail!("shared Grade Node source changed since Catalog registration");
        }
        Ok(SharedGradeSourceGuard {
            representation_id: source.representation_id,
            source_path: source_path.into(),
            fingerprint,
        })
    }

    pub(crate) fn require_shared_grade_source_guard(
        &self,
        photo_id: &str,
        expected: &SharedGradeSourceGuard,
    ) -> AnyResult<()> {
        if self.shared_grade_source_guard(photo_id, &expected.source_path)? != *expected {
            bail!("shared Grade Node source changed during preparation");
        }
        Ok(())
    }

    pub(crate) fn project_shared_grade_revision_numbers(
        &self,
        settings: &mut ffi::FfiEditSettings,
    ) -> AnyResult<()> {
        for (index, node) in settings.grade_nodes.iter_mut().enumerate() {
            node.shared_revision_number = 0;
            node.shared_locally_modified = false;
            if node.shared_layer_id.is_empty() {
                continue;
            }
            let Some(revision) = self.catalog.shared_grade_revision_by_id(
                node.shared_layer_id.parse()?,
                node.shared_revision_id.parse()?,
            )?
            else {
                continue;
            };
            node.shared_revision_number = revision.revision_number();
            // A moved Library head says whether an update exists, not whether
            // this photo changed. Compare against its own immutable pinned
            // revision so saved private edits remain visible after reopening.
            let materialized = decode_grade_node_draft_recipe_v1(node, index)?;
            let pinned = grade_node_draft_from_shared_revision(&revision)?;
            node.shared_locally_modified = !shared_grade_parameters_match(&materialized, &pinned);
        }
        Ok(())
    }
}

fn terminal(
    status: ffi::FfiSharedGradeUpdateStatus,
    diagnostic: &str,
    state: Option<ffi::FfiPhotoEditState>,
) -> AnyResult<LinkInspection> {
    Ok(LinkInspection::Terminal(Box::new(update_receipt(
        status,
        diagnostic.into(),
        state,
    )?)))
}

fn update_receipt(
    status: ffi::FfiSharedGradeUpdateStatus,
    diagnostic: String,
    state: Option<ffi::FfiPhotoEditState>,
) -> AnyResult<ffi::FfiSharedGradeLinkUpdateReceipt> {
    let has_state = state.is_some();
    let state = match state {
        Some(state) => state,
        None => ffi::FfiPhotoEditState {
            photo_id: String::new(),
            source_path: String::new(),
            has_working_version: false,
            is_version_draft: false,
            working_commit_id: String::new(),
            recipe_id: String::new(),
            active_variant_id: String::new(),
            variants: Vec::new(),
            settings: encode_grade_stack_draft_recipe_v1(GradeStackDraft::default())?,
            versions: Vec::new(),
        },
    };
    Ok(ffi::FfiSharedGradeLinkUpdateReceipt {
        status,
        diagnostic,
        has_state,
        state,
    })
}

#[cfg(test)]
mod tests;
