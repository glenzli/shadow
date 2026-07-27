//! Desktop-session CXX orchestration for publishing and applying shared Grade Nodes.

use anyhow::{Context, Result as AnyResult};
use shadow_domain::{EntityId, LayerId};

use super::{
    DesktopSession, ffi,
    recipe_v1::{
        decode_grade_node_draft_recipe_v1, decode_grade_stack_draft_recipe_v1,
        encode_grade_node_as_recipe_v1_layer, encode_grade_stack_draft_recipe_v1,
        ffi_shared_grade_node, grade_node_draft_from_shared_revision,
    },
    shared_grade_application, shared_grade_library,
    wall_clock::current_time_ms,
};

impl DesktopSession {
    pub(crate) fn shared_grade_nodes(&self) -> AnyResult<Vec<ffi::FfiSharedGradeNode>> {
        shared_grade_library::shared_grade_revisions(&self.catalog)?
            .iter()
            .map(ffi_shared_grade_node)
            .collect()
    }

    pub(crate) fn publish_shared_grade_node(
        &self,
        label: &str,
        grade_node: &ffi::FfiGradeNode,
    ) -> AnyResult<ffi::FfiSharedGradeNode> {
        let draft = decode_grade_node_draft_recipe_v1(grade_node, 0)?;
        let layer = encode_grade_node_as_recipe_v1_layer(&draft)?;
        let layer_id = draft.shared.map_or_else(
            || LayerId::from_uuid(draft.recipe_v1_identity.grade_node_id.as_uuid()),
            |shared| shared.layer_id,
        );
        let revision = shared_grade_library::publish_shared_grade_revision(
            &self.catalog,
            layer_id,
            label,
            layer.content().graph().clone(),
            current_time_ms()?,
        )?;
        ffi_shared_grade_node(&revision)
    }

    pub(crate) fn apply_shared_grade_node_to_photos(
        &self,
        layer_id: &str,
        targets: Vec<ffi::FfiBatchPhotoTarget>,
    ) -> AnyResult<ffi::FfiBatchGradeReceipt> {
        use shared_grade_application::{SharedGradeMerge, merge_shared_grade_node};

        let layer_id = layer_id
            .parse::<LayerId>()
            .with_context(|| format!("parse shared Grade Node layer id {layer_id:?}"))?;
        let revision = shared_grade_library::shared_grade_revision(&self.catalog, layer_id)?;
        let shared = grade_node_draft_from_shared_revision(&revision)?;
        let requested = u32::try_from(targets.len()).unwrap_or(u32::MAX);
        let mut receipt = ffi::FfiBatchGradeReceipt {
            requested,
            updated: 0,
            unchanged: 0,
            failed: 0,
            errors: Vec::new(),
        };
        for target in targets {
            let result = (|| -> AnyResult<SharedGradeMerge> {
                let state = self.photo_edit_state(&target.photo_id, &target.source_path)?;
                let mut grade_stack = decode_grade_stack_draft_recipe_v1(&state.settings)?;
                let merge = merge_shared_grade_node(&mut grade_stack, &shared)
                    .map_err(anyhow::Error::msg)?;
                if merge == SharedGradeMerge::Unchanged {
                    return Ok(merge);
                }
                let settings = encode_grade_stack_draft_recipe_v1(grade_stack);
                self.autosave_basic_edit_working_at(
                    &target.photo_id,
                    &target.source_path,
                    &state.working_commit_id,
                    &state.working_commit_id,
                    &settings,
                    current_time_ms()?,
                )?;
                Ok(merge)
            })();
            match result {
                Ok(SharedGradeMerge::Updated) => receipt.updated += 1,
                Ok(SharedGradeMerge::Unchanged) => receipt.unchanged += 1,
                Err(error) => {
                    receipt.failed += 1;
                    receipt.errors.push(format!("{}: {error}", target.photo_id));
                }
            }
        }
        Ok(receipt)
    }
}
