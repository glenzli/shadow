//! Verify the resource closure of every retained state, including hidden
//! nodes and abandoned branches. Content checks are memoized per save.
use anyhow::{Context, Result, ensure};
use shadow_domain::{MaskDefinition, RecipeSnapshot};
use std::{
    collections::BTreeSet,
    fs,
    path::{Path, PathBuf},
};

pub(super) struct ResourceClosure {
    root: PathBuf,
    seen: BTreeSet<String>,
    foundations: Option<Vec<shadow_cache::FoundationArtifactVerification>>,
}

impl ResourceClosure {
    pub fn new(root: &Path) -> Self {
        Self {
            root: root.into(),
            seen: BTreeSet::new(),
            foundations: None,
        }
    }
    pub fn validate(&mut self, snapshot: &RecipeSnapshot, source: &Path) -> Result<()> {
        snapshot.validate_for_commit()?;
        for mask in snapshot.masks() {
            self.mask(mask.definition())?;
        }
        for region in snapshot.image_completions() {
            let patch = region.patch();
            if self
                .seen
                .insert(format!("completion/{}", patch.store_object_id()))
            {
                let store = shadow_core::FilesystemDerivedRasterStore::open(
                    self.root.join("derived-rasters"),
                )?;
                store
                    .open_recipe_completion_patch(patch)
                    .context("document history completion resource is missing or corrupt")?;
            }
        }
        let draft = crate::recipe_v1::decode_grade_stack_draft_from_recipe_v1_snapshot(snapshot)?;
        for node in &draft.grade_nodes {
            let lut = &node.fine.lut;
            if !lut.resource_id.is_empty() && self.seen.insert(format!("lut/{}", lut.resource_id)) {
                crate::recipe_lut_resources::read_verified_lut(
                    Path::new(&lut.managed_path),
                    &lut.resource_id,
                    Some(&self.root),
                )
                .context("document history LUT resource is missing or corrupt")?;
            }
        }
        let denoise = snapshot.raw_ai_denoise_node();
        if denoise.is_enabled() && !denoise.is_bypassed() && denoise.amount_percent() > 0 {
            let key = format!("raw/{}", source.display());
            if self.seen.insert(key) {
                if self.foundations.is_none() {
                    let mut files = Vec::new();
                    walk(
                        &self.root.join("cache/ai/raw-foundations/foundations"),
                        0,
                        &mut files,
                    )?;
                    let mut values = Vec::new();
                    for file in files {
                        values.push(
                            shadow_cache::FoundationArtifactReader::open(file)?
                                .verification()
                                .clone(),
                        );
                    }
                    self.foundations = Some(values);
                }
                let sha = shadow_cache::sha256_file(source)?;
                ensure!(
                    self.foundations
                        .as_ref()
                        .expect("loaded foundation inventory")
                        .iter()
                        .any(|v| v.source_sha256 == sha
                            && v.model_package_sha256 == denoise.model().package_sha256()
                            && v.model_graph_sha256 == denoise.model().graph_sha256()),
                    "document history RAW denoise artifact is missing"
                );
            }
        }
        Ok(())
    }
    fn mask(&mut self, definition: &MaskDefinition) -> Result<()> {
        match definition {
            MaskDefinition::ManagedRaster { raster, .. } => {
                if self
                    .seen
                    .insert(format!("mask/{}", raster.store_object_id()))
                {
                    let store = shadow_core::FilesystemDerivedRasterStore::open(
                        self.root.join("derived-rasters"),
                    )?;
                    store
                        .open_recipe_mask(raster)
                        .context("document history mask resource is missing or corrupt")?;
                }
            }
            MaskDefinition::Composite { composite } => {
                for component in composite.components() {
                    self.mask(component.definition())?;
                }
            }
            _ => {}
        }
        Ok(())
    }
}

fn walk(root: &Path, depth: usize, out: &mut Vec<PathBuf>) -> Result<()> {
    if !root.exists() {
        return Ok(());
    }
    ensure!(
        depth < 8 && out.len() < 100_000,
        "RAW artifact inventory exceeds its limits"
    );
    let kind = fs::symlink_metadata(root)?.file_type();
    if kind.is_dir() {
        for entry in fs::read_dir(root)? {
            walk(&entry?.path(), depth + 1, out)?;
        }
    } else {
        ensure!(kind.is_file(), "RAW artifact is not a regular file");
        out.push(root.into());
    }
    Ok(())
}
