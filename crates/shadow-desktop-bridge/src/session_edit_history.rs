//! Desktop-session orchestration for non-destructive edit history.
//!
//! This module owns working drafts, named versions, checkout, and the atomic
//! publication of Recipe and Library edit-repository references.

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_bridge::query_raw_white_balance_presentation_from_metadata;
use shadow_catalog::{
    CatalogError, CommitEditRepository, CommitRecipe, CommitRecipeAndEditRepository,
    EditObjectPackWrite, EditRepositoryRefUpdate, RecipeCommitRecord, RecipeRefExpectation,
    RecipeRefKind, RecipeRefTarget,
};
use shadow_domain::{
    EditEntityMapV1, EditObject, EditObjectKind, EditObjectPack, EditRepositoryCommit,
    EditRepositoryCommitPayloadV1, EditRepositoryRefExpectation, EditRepositoryRefKind, EntityId,
    LibraryRootV1, PhotoId, RecipeCommit, RecipeCommitId, RecipeId, VersionName,
};

use super::{
    DesktopSession,
    edit_version_diff::{commit_record, ffi_edit_version},
    ffi,
    recipe_v1::{
        GradeStackDraft, decode_grade_stack_draft_from_recipe_v1_snapshot,
        decode_grade_stack_draft_recipe_v1, encode_grade_stack_draft_recipe_v1,
        grade_stack_recipe_v1_snapshot,
    },
    wall_clock::current_time_ms,
};

impl DesktopSession {
    pub(crate) fn photo_edit_state(
        &self,
        photo_id: &str,
        source_path: &str,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        self.photo_edit_state_for(photo_id, &source.location.display_path)
    }

    pub(crate) fn reset_incompatible_photo_edit_history(
        &self,
        photo_id: &str,
        source_path: &str,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        self.catalog.discard_recipe_history(photo_id)?;
        // Development recovery is destructive by design, so do not merely
        // assume the delete succeeded and fabricate an empty state. Re-read
        // the actual Catalog boundary first: a later reopen must observe the
        // same clean state this call returns.
        if self
            .catalog
            .recipe_ref(photo_id, WORKING_RECIPE_REF)?
            .is_some()
            || !self.catalog.recipe_commits(photo_id)?.is_empty()
        {
            bail!("development Recipe reset did not remove this photo's persisted edit history");
        }
        self.photo_edit_state_for(photo_id, &source.location.display_path)
    }

    pub(crate) fn save_basic_edit_version(
        &self,
        photo_id: &str,
        source_path: &str,
        base_commit_id: &str,
        expected_working_commit_id: &str,
        settings: &ffi::FfiEditSettings,
        version_name: &str,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        self.save_basic_edit_version_at_with_expected(
            photo_id,
            source_path,
            base_commit_id,
            expected_working_commit_id,
            settings,
            version_name,
            current_time_ms()?,
        )
    }

    pub(crate) fn autosave_basic_edit_working(
        &self,
        photo_id: &str,
        source_path: &str,
        base_commit_id: &str,
        expected_working_commit_id: &str,
        settings: &ffi::FfiEditSettings,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        self.autosave_basic_edit_working_at(
            photo_id,
            source_path,
            base_commit_id,
            expected_working_commit_id,
            settings,
            current_time_ms()?,
        )
    }

    fn prepare_library_edit_version(
        &self,
        photo_id: PhotoId,
        recipe_commit: &RecipeCommit,
        version_name: &VersionName,
        created_at_ms: i64,
    ) -> AnyResult<(EditObjectPackWrite, CommitEditRepository)> {
        let head = self.catalog.edit_repository_ref(LIBRARY_EDIT_MAIN_REF)?;
        let (parent, root, photo_map) = if let Some(head) = head {
            if head.kind != EditRepositoryRefKind::Branch {
                bail!("Library edit ref {LIBRARY_EDIT_MAIN_REF} is not a branch");
            }
            let repository_commit = self
                .catalog
                .edit_repository_commit(head.commit_id)?
                .ok_or_else(|| anyhow!("Library edit head commit {} is missing", head.commit_id))?;
            let root_record = self
                .catalog
                .edit_object(repository_commit.commit.payload().root)?
                .ok_or_else(|| {
                    anyhow!(
                        "Library edit root {} is missing",
                        repository_commit.commit.payload().root
                    )
                })?;
            let root = LibraryRootV1::from_object(&root_record.object)
                .context("decode Library edit root")?;
            let photo_map = if let Some(map_id) = root.photo_recipes {
                let map_record = self
                    .catalog
                    .edit_object(map_id)?
                    .ok_or_else(|| anyhow!("Library photo edit map {map_id} is missing"))?;
                EditEntityMapV1::from_object(&map_record.object)
                    .context("decode Library photo edit map")?
            } else {
                EditEntityMapV1::new(Vec::new())?
            };
            (Some(head.commit_id), root, photo_map)
        } else {
            (
                None,
                LibraryRootV1 {
                    photo_recipes: None,
                    shared_grade_heads: None,
                    masks: None,
                    styles: None,
                    output_states: None,
                },
                EditEntityMapV1::new(Vec::new())?,
            )
        };

        // The render recipe is wrapped as an immutable leaf while the Library
        // repository remains the authoritative cross-entity history.
        let recipe_object =
            EditObject::from_canonical_json(EditObjectKind::LegacyRecipe, 1, recipe_commit)?;
        let recipe_pack = EditObjectPack::new(recipe_object, Vec::new())?;
        let photo_key = format!("{LIBRARY_PHOTO_EDIT_KEY_PREFIX}{photo_id}");
        let photo_map = photo_map.with_entry(photo_key, recipe_pack.object().id())?;
        let photo_map_pack = photo_map.into_object_pack()?;
        let root_pack = LibraryRootV1 {
            photo_recipes: Some(photo_map_pack.object().id()),
            shared_grade_heads: root.shared_grade_heads,
            masks: root.masks,
            styles: root.styles,
            output_states: root.output_states,
        }
        .into_object_pack()?;
        let repository_commit = EditRepositoryCommit::new(EditRepositoryCommitPayloadV1 {
            root: root_pack.object().id(),
            parents: parent.into_iter().collect(),
            message: Some(version_name.as_str().to_owned()),
            created_at_ms,
        })?;
        let expected = parent.map_or(
            EditRepositoryRefExpectation::Missing,
            EditRepositoryRefExpectation::At,
        );
        let version_ref = format!(
            "{LIBRARY_EDIT_VERSION_REF_PREFIX}{}",
            repository_commit.id()
        );
        Ok((
            EditObjectPackWrite {
                objects: vec![root_pack, photo_map_pack, recipe_pack],
                created_at_ms,
            },
            CommitEditRepository {
                commit: repository_commit,
                update_refs: vec![
                    EditRepositoryRefUpdate {
                        name: LIBRARY_EDIT_MAIN_REF.into(),
                        kind: EditRepositoryRefKind::Branch,
                        expected,
                        updated_at_ms: created_at_ms,
                    },
                    EditRepositoryRefUpdate {
                        name: version_ref,
                        kind: EditRepositoryRefKind::NamedVersion,
                        expected: EditRepositoryRefExpectation::Missing,
                        updated_at_ms: created_at_ms,
                    },
                ],
            },
        ))
    }

    #[cfg(test)]
    pub(crate) fn save_basic_edit_version_at(
        &self,
        photo_id: &str,
        source_path: &str,
        base_commit_id: &str,
        settings: &ffi::FfiEditSettings,
        version_name: &str,
        created_at_ms: i64,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        self.save_basic_edit_version_at_with_expected(
            photo_id,
            source_path,
            base_commit_id,
            base_commit_id,
            settings,
            version_name,
            created_at_ms,
        )
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn save_basic_edit_version_at_with_expected(
        &self,
        photo_id: &str,
        source_path: &str,
        base_commit_id: &str,
        expected_working_commit_id: &str,
        settings: &ffi::FfiEditSettings,
        version_name: &str,
        created_at_ms: i64,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        let version_name =
            VersionName::new(version_name).context("validate basic edit version name")?;
        let grade_stack = decode_grade_stack_draft_recipe_v1(settings)?;
        let base_commit_id = if base_commit_id.is_empty() {
            None
        } else {
            Some(
                base_commit_id
                    .parse::<RecipeCommitId>()
                    .with_context(|| format!("parse save base commit id {base_commit_id}"))?,
            )
        };
        let expected_working_commit_id = if expected_working_commit_id.is_empty() {
            None
        } else {
            Some(
                expected_working_commit_id
                    .parse::<RecipeCommitId>()
                    .with_context(|| {
                        format!(
                            "parse expected working Recipe commit id {expected_working_commit_id}"
                        )
                    })?,
            )
        };
        // The content parent and the movable durable head are deliberately
        // independent. Loading a historical version uses the old commit as its
        // content base while CAS still guards the latest durable working ref.
        let base_record = base_commit_id
            .map(|commit_id| {
                self.catalog
                    .recipe_commit(photo_id, commit_id)?
                    .ok_or_else(|| anyhow!("save base Recipe commit {commit_id} is unavailable"))
            })
            .transpose()?;
        let snapshot = grade_stack_recipe_v1_snapshot(
            &grade_stack,
            base_record.as_ref().map(|record| record.commit.snapshot()),
        )?;
        let (recipe_id, parents) = if let Some(record) = base_record.as_ref() {
            (record.commit.recipe_id(), vec![record.commit.id()])
        } else {
            (RecipeId::new_v7(), Vec::new())
        };
        let commit_id = RecipeCommitId::new_v7();
        let commit = RecipeCommit::new(
            commit_id,
            recipe_id,
            parents,
            snapshot,
            Some(version_name.as_str().to_owned()),
            created_at_ms,
        )?;
        let recipe_request = CommitRecipe {
            photo_id,
            commit: commit.clone(),
            update_refs: vec![
                RecipeRefTarget {
                    name: WORKING_RECIPE_REF.to_owned(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(
                        expected_working_commit_id
                            .map_or(RecipeRefExpectation::Missing, RecipeRefExpectation::At),
                    ),
                },
                RecipeRefTarget {
                    name: format!("{NAMED_VERSION_REF_PREFIX}{commit_id}"),
                    kind: RecipeRefKind::NamedVersion,
                    expectation: Some(RecipeRefExpectation::Missing),
                },
            ],
        };
        let (object_pack, repository) =
            self.prepare_library_edit_version(photo_id, &commit, &version_name, created_at_ms)?;
        // Object insertion can safely precede publication: failed CAS leaves
        // only unreachable immutable objects. Both commits and both ref sets
        // are published in the following single SQLite transaction.
        self.catalog.store_edit_object_pack(&object_pack)?;
        self.catalog
            .commit_recipe_and_edit_repository(&CommitRecipeAndEditRepository {
                recipe: recipe_request,
                repository,
            })?;
        self.photo_edit_state_for(photo_id, &source.location.display_path)
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn autosave_basic_edit_working_at(
        &self,
        photo_id: &str,
        source_path: &str,
        base_commit_id: &str,
        expected_working_commit_id: &str,
        settings: &ffi::FfiEditSettings,
        created_at_ms: i64,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        let grade_stack = decode_grade_stack_draft_recipe_v1(settings)?;
        self.autosave_grade_stack_working_at(
            photo_id,
            source_path,
            base_commit_id,
            expected_working_commit_id,
            &grade_stack,
            created_at_ms,
        )
    }

    /// Publishes an already-decoded draft through the same bounded working-ref
    /// CAS transaction as ordinary desktop autosave.
    ///
    /// Internal authoring workflows use this boundary when their state cannot
    /// be represented losslessly by the public Qt edit DTO, such as a newly
    /// promoted managed-raster mask.
    ///
    /// The bounded CAS/rebase loop is one autosave publication transaction;
    /// extracting fragments would obscure which working-head observation each
    /// retry owns.
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub(crate) fn autosave_grade_stack_working_at(
        &self,
        photo_id: &str,
        source_path: &str,
        base_commit_id: &str,
        expected_working_commit_id: &str,
        grade_stack: &GradeStackDraft,
        created_at_ms: i64,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        let base_commit_id = if base_commit_id.is_empty() {
            None
        } else {
            Some(base_commit_id.parse::<RecipeCommitId>().with_context(|| {
                format!("parse autosave base Recipe commit id {base_commit_id}")
            })?)
        };
        let expected_working_commit_id = if expected_working_commit_id.is_empty() {
            None
        } else {
            Some(
                expected_working_commit_id
                    .parse::<RecipeCommitId>()
                    .with_context(|| {
                        format!(
                            "parse expected autosave working Recipe commit id {expected_working_commit_id}"
                        )
                    })?,
            )
        };
        // Normal editing carries the durable `working` head as both its content
        // base and its CAS expectation. A checked-out named Version is the one
        // exception: it remains the content base while the durable head is only
        // used to publish the new draft safely.
        let base_is_expected_working_head =
            base_commit_id.as_ref() == expected_working_commit_id.as_ref();
        // A historical named Version may be loaded as a transient draft. Its
        // content is the parent of a new autosave while the current durable
        // working head remains independently CAS-protected.
        let base_record = base_commit_id
            .map(|commit_id| {
                self.catalog
                    .recipe_commit(photo_id, commit_id)?
                    .ok_or_else(|| {
                        anyhow!("autosave base Recipe commit {commit_id} is unavailable")
                    })
            })
            .transpose()?;
        let autosave_request = |parent: Option<&RecipeCommitRecord>,
                                expected_working: Option<RecipeCommitId>|
         -> AnyResult<CommitRecipe> {
            let snapshot = grade_stack_recipe_v1_snapshot(
                &grade_stack,
                parent.map(|record| record.commit.snapshot()),
            )?;
            let (recipe_id, parents) = if let Some(record) = parent {
                (record.commit.recipe_id(), vec![record.commit.id()])
            } else {
                (RecipeId::new_v7(), Vec::new())
            };
            let commit = RecipeCommit::new(
                RecipeCommitId::new_v7(),
                recipe_id,
                parents,
                snapshot,
                None,
                created_at_ms,
            )?;
            Ok(CommitRecipe {
                photo_id,
                commit,
                update_refs: vec![RecipeRefTarget {
                    name: WORKING_RECIPE_REF.to_owned(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(
                        expected_working
                            .map_or(RecipeRefExpectation::Missing, RecipeRefExpectation::At),
                    ),
                }],
            })
        };

        // Every autosave is a complete immutable draft. A conflicting `working` ref means a
        // newer autosave (or another open Shadow session) published between this controller's
        // snapshot and the catalog transaction. Rebase the full draft repeatedly on the exact
        // observed head instead of surfacing a normal CAS race as a user-visible save failure.
        // The bounded loop preserves fail-closed behavior for a genuinely hot external writer.
        let mut expected_working = expected_working_commit_id;
        let mut rebased_working_record: Option<RecipeCommitRecord> = None;
        let mut published = false;
        for _ in 0..AUTOSAVE_WORKING_REF_REBASE_ATTEMPTS {
            let parent = if base_is_expected_working_head || base_record.is_none() {
                rebased_working_record.as_ref().or(base_record.as_ref())
            } else {
                // A checked-out named Version remains the content parent. Its durable working
                // ref only provides the CAS guard, so a concurrent autosave does not rewrite
                // the branch point selected by the photographer.
                base_record.as_ref()
            };
            let request = autosave_request(parent, expected_working)?;
            match self.catalog.commit_recipe(&request) {
                Ok(_) => {
                    published = true;
                    break;
                }
                Err(CatalogError::RecipeRefExpectationMismatch { name, actual, .. })
                    if name == WORKING_RECIPE_REF =>
                {
                    rebased_working_record = actual
                        .map(|commit_id| {
                            self.catalog.recipe_commit(photo_id, commit_id)?.ok_or_else(|| {
                                anyhow!(
                                    "autosave conflict refers to unavailable working Recipe commit {commit_id}"
                                )
                            })
                        })
                        .transpose()?;
                    expected_working = actual;
                }
                Err(error) => return Err(error.into()),
            }
        }
        if !published {
            bail!(
                "autosave could not publish after {AUTOSAVE_WORKING_REF_REBASE_ATTEMPTS} concurrent working-state updates"
            );
        }
        self.photo_edit_state_for(photo_id, &source.location.display_path)
    }

    pub(crate) fn checkout_basic_edit_version(
        &self,
        photo_id: &str,
        source_path: &str,
        commit_id: &str,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        let commit_id: RecipeCommitId = commit_id
            .parse()
            .with_context(|| format!("parse Recipe commit id {commit_id}"))?;
        let commits = self.catalog.recipe_commits(photo_id)?;
        let record = commit_record(&commits, commit_id)?;
        decode_grade_stack_draft_from_recipe_v1_snapshot(record.commit.snapshot())?;
        self.photo_edit_state_for_selected(
            photo_id,
            &source.location.display_path,
            Some(commit_id),
            true,
        )
    }

    pub(crate) fn photo_edit_state_for(
        &self,
        photo_id: PhotoId,
        source_path: &str,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        // Recipe v1 is deliberately fixed throughout pre-release work. A
        // previous experimental shape is not silently mutated or opened as a
        // half-valid edit: surface one recoverable, user-confirmed reset
        // instead. The photo and every non-edit Library fact remain intact.
        let working = self
            .catalog
            .recipe_ref(photo_id, WORKING_RECIPE_REF)
            .map_err(|error| {
                anyhow!(
                    "incompatible development Recipe: could not read the working edit reference: {error}"
                )
            })?;
        if let Some(reference) = working.as_ref() {
            let record = self
                .catalog
                .recipe_commit(photo_id, reference.commit_id)
                .map_err(|error| {
                    anyhow!(
                        "incompatible development Recipe: could not read working commit {}: {error}",
                        reference.commit_id
                    )
                })?
                .ok_or_else(|| {
                    anyhow!(
                        "incompatible development Recipe: working commit {} is unavailable",
                        reference.commit_id
                    )
                })?;
            if let Err(error) =
                decode_grade_stack_draft_from_recipe_v1_snapshot(record.commit.snapshot())
            {
                bail!("incompatible development Recipe: {error}");
            }
        }
        self.photo_edit_state_for_selected(
            photo_id,
            source_path,
            working.map(|reference| reference.commit_id),
            false,
        )
        .map_err(|error| {
            anyhow!(
                "incompatible development Recipe: could not load this photo's edit history: {error}"
            )
        })
    }

    fn photo_edit_state_for_selected(
        &self,
        photo_id: PhotoId,
        source_path: &str,
        selected_commit_id: Option<RecipeCommitId>,
        is_version_draft: bool,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        let commits = self.catalog.recipe_commits(photo_id)?;
        let selected_record = selected_commit_id
            .map(|commit_id| commit_record(&commits, commit_id))
            .transpose()?;
        let grade_stack = selected_record.map_or_else(
            || Ok(GradeStackDraft::default()),
            |record| decode_grade_stack_draft_from_recipe_v1_snapshot(record.commit.snapshot()),
        )?;
        let selected_id = selected_record.map(|record| record.commit.id());
        let recipe_id = selected_record.map(|record| record.commit.recipe_id());
        // The Version panel is intentionally a list of human-created named
        // checkpoints. Autosave commits are immutable and recoverable through
        // `working`, but must not turn every slider release into history UI.
        let versions = commits
            .iter()
            .filter(|record| record.commit.message().is_some())
            .map(|record| ffi_edit_version(record, &commits, selected_id))
            .collect::<AnyResult<Vec<_>>>()?;
        let mut settings = encode_grade_stack_draft_recipe_v1(grade_stack)
            .context("project persisted Recipe into the editable desktop contract")?;
        if let Some(metadata) = self
            .catalog
            .review_source(photo_id)?
            .and_then(|raw| raw.metadata)
            && let Some(presentation) =
                query_raw_white_balance_presentation_from_metadata(&metadata)
        {
            settings.foundation.as_shot_white_balance_available = true;
            settings.foundation.as_shot_temperature_kelvin = presentation.temperature_kelvin;
            settings.foundation.as_shot_tint = presentation.tint;
            if settings.foundation.raw_white_balance_mode == 0 {
                settings.foundation.temperature_kelvin = presentation.temperature_kelvin;
                settings.foundation.tint = presentation.tint;
            }
        }
        Ok(ffi::FfiPhotoEditState {
            photo_id: photo_id.to_string(),
            source_path: source_path.to_owned(),
            has_working_version: selected_id.is_some(),
            is_version_draft,
            working_commit_id: selected_id.map_or_else(String::new, |id| id.to_string()),
            recipe_id: recipe_id.map_or_else(String::new, |id| id.to_string()),
            settings,
            versions,
        })
    }
}

pub(crate) const WORKING_RECIPE_REF: &str = "working";
// Autosave submissions are complete snapshots, so a short sequence of CAS conflicts can safely
// be rebased without losing local work. This is not a spin lock: an actively contended external
// writer still becomes an explicit error after the bounded retry budget.
const AUTOSAVE_WORKING_REF_REBASE_ATTEMPTS: usize = 8;
pub(crate) const NAMED_VERSION_REF_PREFIX: &str = "versions/";
pub(crate) const LIBRARY_EDIT_MAIN_REF: &str = "heads/main";
pub(crate) const LIBRARY_EDIT_VERSION_REF_PREFIX: &str = "versions/";
pub(crate) const LIBRARY_PHOTO_EDIT_KEY_PREFIX: &str = "photo/";
